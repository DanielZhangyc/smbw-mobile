// 手柄触觉只在独立串行队列操作；HID 调用只更新有界的双通道邮箱，不等待引擎。
// Controller haptics run only on a private serial queue; HID calls update a bounded two-channel mailbox without waiting for engines.
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#include "dm.h"
#include "dm_input.h"
#include "ns2.h"
#include <math.h>
#include <errno.h>
#include <ctype.h>

static pthread_mutex_t g_haptic_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_haptic_epoch;
static unsigned g_haptic_caps, g_haptic_device_caps, g_haptic_initialized, g_haptic_dirty;
static BOOL g_haptic_active, g_haptic_queued, g_haptic_shutdown;
static unsigned g_haptic_stop_errors;
static char g_haptic_queue_key;
static GCController *g_haptic_controller;
static dm_vibration_value g_haptic_pending[2], g_haptic_actual[2];
typedef struct {
    uint64_t received, applied, nonzero, coalesced;
    uint64_t last_received_nonzero_ns, last_received_zero_ns, last_nonzero_ns, last_zero_ns;
    float last_nonzero_intensity;
    float raw_low_peak, raw_high_peak, rss_peak, intensity_peak;
} haptic_audit_side;
@interface DMHapticAudit : NSObject {
@public
    uint64_t epoch;
    haptic_audit_side sides[2];
    BOOL logged;
}
@end
@implementation DMHapticAudit
@end
static DMHapticAudit *g_haptic_audit; // 字段由 g_haptic_mu 保护；旧 session 保留旧统计 | Fields use g_haptic_mu; old sessions retain their own statistics
static void haptic_exit(void) { (void)dm_haptic_shutdown("atexit", 600); }
static dispatch_group_t haptic_stops(void) {
    static dispatch_once_t once; static dispatch_group_t group;
    dispatch_once(&once, ^{ group=dispatch_group_create(); });
    return group;
}
static dispatch_queue_t haptic_queue(void) {
    static dispatch_once_t once; static dispatch_queue_t queue;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("dm.controller.haptics", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(queue, &g_haptic_queue_key, &g_haptic_queue_key, NULL);
        if (atexit(haptic_exit)) die("Mac 触觉无法登记退出清理");
    });
    return queue;
}
static unsigned haptic_bit(unsigned side, unsigned caps) { return side ? caps & DM_HAPTIC_RIGHT : caps & (DM_HAPTIC_LEFT | DM_HAPTIC_WHOLE); }
static float haptic_bound(float v, float low, float high, float invalid) { return isfinite(v) ? fmaxf(low, fminf(high, v)) : invalid; }
typedef struct { BOOL perceptual; float gain; } haptic_calibration;
static haptic_calibration haptic_config(void) {
    static dispatch_once_t once; static haptic_calibration config;
    dispatch_once(&once, ^{
        config=(haptic_calibration){YES,1};
        const char *curve=getenv("DM_RUMBLE_CURVE"), *gain=getenv("DM_RUMBLE_GAIN");
        if (curve) {
            if (!strcmp(curve,"linear")) config.perceptual=NO;
            else if (strcmp(curve,"perceptual")) die("【红】DM_RUMBLE_CURVE 必须为 linear 或 perceptual：%s",curve);
        }
        if (gain) {
            char *end; errno=0; double value=strtod(gain,&end);
            if (!*gain || isspace((unsigned char)*gain) || end==gain || *end || errno || !isfinite(value) || value<0 || value>4)
                die("【红】DM_RUMBLE_GAIN 必须为有限数值 0..4：%s",gain);
            config.gain=(float)value;
        }
        LOG("Mac 触觉校准 curve=%s gain=%.6g sharpnessControl=0（强度控制映射，非物理频率 / 电机遥测）",config.perceptual?"perceptual":"linear",config.gain);
    });
    return config;
}
// 保留 raw RSS 的原归一化；curve 仅校准控制强度，零始终严格为零。
// Preserve the raw RSS normalization; the curve calibrates only control intensity, with exact zero remaining zero.
static float haptic_raw_rss(dm_vibration_value value) {
    return fminf(1,hypotf(haptic_bound(value.low_amp,0,1,0),haptic_bound(value.high_amp,0,1,0)));
}
static float haptic_intensity(float raw_rss) {
    haptic_calibration config=haptic_config();
    float raw=haptic_bound(raw_rss,0,1,0);
    if (raw==0) return 0;
    return fminf(1,config.gain*(config.perceptual?sqrtf(raw):raw));
}
static dm_vibration_value haptic_value(dm_vibration_value v) {
    // 频率原样保留为诊断，不用于生成 sharpness，不宣称能复现 Switch 双频 HD 振动。
    // Preserve frequencies verbatim for diagnostics, without deriving sharpness or claiming Switch dual-band HD-rumble reproduction.
    return (dm_vibration_value){haptic_bound(v.low_amp,0,1,0),v.low_freq,haptic_bound(v.high_amp,0,1,0),v.high_freq};
}
static NSString *haptic_error_text(NSError *error) {
    if (!error) return @"API 未提供错误详情";
    NSError *underlying=error.userInfo[NSUnderlyingErrorKey];
    return [NSString stringWithFormat:@"%@ code=%ld: %@%@",error.domain,(long)error.code,error.localizedDescription,
        underlying ? [NSString stringWithFormat:@"; underlying %@ code=%ld",underlying.domain,(long)underlying.code] : @""];
}
static void haptic_stop_error(void) {
    pthread_mutex_lock(&g_haptic_mu); g_haptic_stop_errors++; pthread_mutex_unlock(&g_haptic_mu);
}
static void haptic_stop_engine(CHHapticEngine *engine, uint64_t epoch, unsigned side) {
    if (!engine) return;
    // stop 是异步完成契约；保留引擎到 completion，避免调用方清空 ivar 后提前拆掉 XPC 连接。
    // Stop has an asynchronous completion contract; retain the engine until completion so clearing the caller's ivar cannot tear down XPC prematurely.
    dispatch_group_t group=haptic_stops(); dispatch_group_enter(group);
    LOG("Mac 触觉 stop-request t_ns=%llu session=%llu side=%u",(unsigned long long)now_ns(),(unsigned long long)epoch,side);
    __block CHHapticEngine *held=engine;
    [held stopWithCompletionHandler:^(NSError *error) {
        if (error) { haptic_stop_error(); LOG("【红】Mac 触觉 stop-completion t_ns=%llu session=%llu side=%u error=%s",(unsigned long long)now_ns(),(unsigned long long)epoch,side,haptic_error_text(error).UTF8String); }
        else LOG("Mac 触觉 stop-completion t_ns=%llu session=%llu side=%u result=API-completed（非电机遥测）",(unsigned long long)now_ns(),(unsigned long long)epoch,side);
        held=nil;
        dispatch_group_leave(group);
    }];
}
static unsigned haptic_device_caps(GCController *controller) {
    GCDeviceHaptics *haptics=controller.haptics;
    if (!haptics) return 0;
    NSSet *localities=haptics.supportedLocalities; unsigned caps=0;
    if ([localities containsObject:GCHapticsLocalityLeftHandle]) caps |= DM_HAPTIC_LEFT;
    if ([localities containsObject:GCHapticsLocalityRightHandle]) caps |= DM_HAPTIC_RIGHT;
    if (!caps && [localities containsObject:GCHapticsLocalityDefault]) caps=DM_HAPTIC_WHOLE;
    return caps;
}
static void haptic_drain(void);
static void haptic_enqueue_locked(void) {
    if (g_haptic_dirty && !g_haptic_queued) {
        g_haptic_queued=YES;
        dispatch_async(haptic_queue(), ^{ haptic_drain(); });
    }
}

@interface DMHapticSession : NSObject {
@public
    uint64_t epoch;
    CHHapticEngine *engines[2];
    id<CHHapticPatternPlayer> players[2];
    BOOL playing[2];
    BOOL logged_accepted[2], logged_stopped[2], logged_zero[2];
    DMHapticAudit *audit;
    unsigned caps;
}
- (void)shutdown;
- (void)disable:(unsigned)side reason:(NSString *)reason;
- (unsigned)prepare:(GCController *)controller;
- (BOOL)apply:(dm_vibration_value)value side:(unsigned)side;
- (void)recordApply:(unsigned)side rss:(float)rss intensity:(float)intensity;
- (BOOL)zero:(unsigned)side reason:(const char *)reason forceLog:(BOOL)forceLog;
@end
static DMHapticSession *g_haptic_session; // 只在工作队列访问 | Accessed only on the worker queue

@implementation DMHapticSession
- (void)recordApply:(unsigned)side rss:(float)rss intensity:(float)intensity {
    pthread_mutex_lock(&g_haptic_mu);
    if (audit) {
        haptic_audit_side *s=&audit->sides[side];
        s->applied++; s->nonzero += intensity>0; s->rss_peak=fmaxf(s->rss_peak,rss); s->intensity_peak=fmaxf(s->intensity_peak,intensity);
        if (intensity>0) { s->last_nonzero_ns=now_ns(); s->last_nonzero_intensity=intensity; }
    }
    pthread_mutex_unlock(&g_haptic_mu);
}
- (BOOL)zero:(unsigned)side reason:(const char *)reason forceLog:(BOOL)forceLog {
    if (!players[side]) return YES;
    NSError *error=nil;
    CHHapticDynamicParameter *zero=[[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:0 relativeTime:0];
    BOOL accepted=[players[side] sendParameters:@[zero] atTime:CHHapticTimeImmediate error:&error];
    uint64_t time=now_ns();
    if (accepted) {
        pthread_mutex_lock(&g_haptic_mu); if (audit) audit->sides[side].last_zero_ns=time; pthread_mutex_unlock(&g_haptic_mu);
    } else haptic_stop_error();
    if (forceLog || !logged_zero[side] || !accepted) {
        LOG("%sMac 触觉 zero t_ns=%llu session=%llu side=%u reason=%s playing=%d result=%s（软件 API，非电机遥测）%s",
            accepted?"":"【红】",(unsigned long long)time,(unsigned long long)epoch,side,reason,playing[side],accepted?"accepted":"failed",accepted?"":haptic_error_text(error).UTF8String);
    }
    if (accepted) logged_zero[side]=YES;
    return accepted;
}
- (void)shutdown {
    for (unsigned side=0; side<2; side++) {
        // SDK 将回调标为 nonnull；解绑时不捕获对象，stop completion 不再转派到主队列或触觉工作队列。
        // The SDK marks handlers nonnull; detach without object captures, and do not redispatch stop completion to the main or haptics queue.
        engines[side].stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; }; engines[side].resetHandler = ^{};
        [self zero:side reason:"session-shutdown" forceLog:YES];
        NSError *error=nil;
        if (players[side]) {
            // cancel 会清除待执行参数；使用 stopAtTime，避免刚提交的零参数被主动撤销。
            // Cancel removes pending parameters; use stopAtTime to avoid explicitly cancelling the zero just submitted.
            BOOL stopped=[players[side] stopAtTime:CHHapticTimeImmediate error:&error];
            if (!stopped) haptic_stop_error();
            LOG("%sMac 触觉 player-stop t_ns=%llu session=%llu side=%u result=%s（软件 API，非电机遥测）%s",stopped?"":"【红】",(unsigned long long)now_ns(),(unsigned long long)epoch,side,stopped?"accepted":"failed",stopped?"":haptic_error_text(error).UTF8String);
        }
        haptic_stop_engine(engines[side],epoch,side);
        players[side]=nil; engines[side]=nil; playing[side]=NO;
    }
    haptic_audit_side summary[2]={{0},{0}}; BOOL print=NO;
    pthread_mutex_lock(&g_haptic_mu);
    if (audit && !audit->logged) { audit->logged=YES; memcpy(summary,audit->sides,sizeof summary); print=YES; }
    pthread_mutex_unlock(&g_haptic_mu);
    // 只在 session 结束汇总；received 是有效已初始化通道的收包，applied 是成功 apply（含空闲零值）。
    // Summarize only at session end: received counts initialized valid-channel requests; applied counts successful apply calls, including idle zero values.
    for (unsigned side=0;print && side<2;side++) if (haptic_bit(side,caps) || summary[side].received || summary[side].applied) {
        haptic_audit_side s=summary[side];
        LOG("Mac 触觉 summary session=%llu side=%u received=%llu applied=%llu nonzero=%llu coalesced=%llu raw_low_peak=%.6f raw_high_peak=%.6f raw_rss_peak=%.6f mapped_intensity_peak=%.6f last_received_nonzero_ns=%llu last_received_zero_ns=%llu last_nonzero_ns=%llu last_zero_ns=%llu last_nonzero_intensity=%.6f（软件统计，非电机遥测）",
            (unsigned long long)epoch,side,(unsigned long long)s.received,(unsigned long long)s.applied,
            (unsigned long long)s.nonzero,(unsigned long long)s.coalesced,s.raw_low_peak,s.raw_high_peak,s.rss_peak,s.intensity_peak,
            (unsigned long long)s.last_received_nonzero_ns,(unsigned long long)s.last_received_zero_ns,
            (unsigned long long)s.last_nonzero_ns,(unsigned long long)s.last_zero_ns,s.last_nonzero_intensity);
    }
    caps=0;
}
- (void)disable:(unsigned)side reason:(NSString *)reason {
    unsigned bit=haptic_bit(side,caps); caps &= ~bit;
    engines[side].stoppedHandler=^(CHHapticEngineStoppedReason stoppedReason) { (void)stoppedReason; }; engines[side].resetHandler=^{};
    [self zero:side reason:"disable" forceLog:YES];
    NSError *error=nil;
    if (players[side] && ![players[side] stopAtTime:CHHapticTimeImmediate error:&error]) { haptic_stop_error(); LOG("【红】Mac 触觉故障 player-stop 失败 t_ns=%llu：%s",(unsigned long long)now_ns(),haptic_error_text(error).UTF8String); }
    haptic_stop_engine(engines[side],epoch,side); players[side]=nil; engines[side]=nil; playing[side]=NO;
    pthread_mutex_lock(&g_haptic_mu);
    if (epoch==g_haptic_epoch) {
        g_haptic_caps &= ~bit; g_haptic_device_caps &= ~bit; g_haptic_initialized &= ~bit;
        g_haptic_dirty &= ~(1u<<side); g_haptic_pending[side]=g_haptic_actual[side]=(dm_vibration_value){0};
    }
    pthread_mutex_unlock(&g_haptic_mu);
    LOG("【红】Mac 触觉通道 %u 停用：%s；重新聚焦或重连后重新探测",side,reason.UTF8String ?: "API 未提供错误详情");
}
- (unsigned)prepare:(GCController *)controller {
    GCDeviceHaptics *haptics=controller.haptics;
    if (!haptics) { LOG("Mac 触觉：%s 未提供 GameController haptics，振动设备数 0",controller.vendorName.UTF8String ?: "未命名"); return 0; }
    NSSet *localities=haptics.supportedLocalities;
    NSString *locality[2]={nil,nil}; unsigned bits[2]={DM_HAPTIC_LEFT,DM_HAPTIC_RIGHT};
    if ([localities containsObject:GCHapticsLocalityLeftHandle]) locality[0]=GCHapticsLocalityLeftHandle;
    if ([localities containsObject:GCHapticsLocalityRightHandle]) locality[1]=GCHapticsLocalityRightHandle;
    // 没有分侧能力时只暴露一个真实 default 聚合通道，不假造左右独立电机。
    // With no side-specific locality, expose one real default aggregate channel rather than inventing two independent motors.
    if (!locality[0] && !locality[1] && [localities containsObject:GCHapticsLocalityDefault]) { locality[0]=GCHapticsLocalityDefault; bits[0]=DM_HAPTIC_WHOLE; }
    for (unsigned side=0; side<2; side++) if (locality[side]) {
        engines[side]=[haptics createEngineWithLocality:locality[side]];
        NSError *error=nil;
        engines[side].playsHapticsOnly=YES; engines[side].autoShutdownEnabled=NO;
        if (!engines[side] || ![engines[side] startAndReturnError:&error]) {
            LOG("【红】Mac 触觉引擎 %s 初始化失败：%s",locality[side].UTF8String,haptic_error_text(error).UTF8String);
            haptic_stop_engine(engines[side],epoch,side); engines[side]=nil; continue;
        }
        // Apple 的控制器持续 rumble 示例使用普通 player + GCHapticDurationInfinite，无需高级循环接口。
        // Apple's controller rumble example uses a standard player with GCHapticDurationInfinite, without advanced looping APIs.
        CHHapticEvent *event=[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1],
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5]] relativeTime:0 duration:GCHapticDurationInfinite];
        CHHapticPattern *pattern=[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
        if (pattern) players[side]=[engines[side] createPlayerWithPattern:pattern error:&error];
        if (!players[side]) { LOG("【红】Mac 连续触觉 player 创建失败：%s",haptic_error_text(error).UTF8String); haptic_stop_engine(engines[side],epoch,side); engines[side]=nil; continue; }
        __weak DMHapticSession *weakSelf=self; unsigned channel=side;
        engines[side].stoppedHandler=^(CHHapticEngineStoppedReason reason) {
            dispatch_async(haptic_queue(), ^{ DMHapticSession *session=weakSelf; if(session && session->engines[channel]) [session disable:channel reason:[NSString stringWithFormat:@"engine stopped %ld",(long)reason]]; });
        };
        engines[side].resetHandler=^{
            dispatch_async(haptic_queue(), ^{ DMHapticSession *session=weakSelf; if(session && session->engines[channel]) [session disable:channel reason:@"engine reset; old players invalidated, no stale replay"]; });
        };
        caps |= bits[side];
    }
    LOG("Mac 触觉：%s 可用 mask=%u（CoreHaptics 近似映射，未测物理电机）",controller.vendorName.UTF8String ?: "未命名",caps);
    return caps;
}
- (BOOL)apply:(dm_vibration_value)value side:(unsigned)side {
    if (!haptic_bit(side,caps) || !players[side]) return NO;
    NSError *error=nil;
    float raw_rss=haptic_raw_rss(value), intensity=haptic_intensity(raw_rss);
    if (intensity==0) {
        BOOL wasPlaying=playing[side];
        if (!wasPlaying) { [self recordApply:side rss:raw_rss intensity:0]; return YES; }
        if (![self zero:side reason:"guest-zero" forceLog:NO]) { [self disable:side reason:@"zero intensity submission failed"]; return NO; }
        if (![players[side] stopAtTime:CHHapticTimeImmediate error:&error]) { haptic_stop_error(); [self disable:side reason:haptic_error_text(error)]; return NO; }
        // 每个 session/通道只见证首次 player-stop 接受；空闲零值不冒充停止电机。
        // Witness only the first accepted player-stop per session/channel; an idle zero is not reported as a motor stop.
        if (wasPlaying && !logged_stopped[side]) {
            logged_stopped[side]=YES;
            LOG("Mac 触觉 player-stop t_ns=%llu session=%llu side=%u reason=guest-zero result=accepted（软件 API，非电机遥测）",(unsigned long long)now_ns(),(unsigned long long)epoch,side);
        }
        playing[side]=NO; [self recordApply:side rss:raw_rss intensity:0]; return YES;
    }
    // sharpnessControl=0 保持事件的中性 0.5 基准；Nintendo Hz 仅诊断，不冒充控制器实际频率。
    // sharpnessControl=0 preserves the event's neutral 0.5 baseline; Nintendo Hz remain diagnostic and do not claim actual controller frequencies.
    NSArray *parameters=@[
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:intensity relativeTime:0],
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:0 relativeTime:0]];
    if (![players[side] sendParameters:parameters atTime:CHHapticTimeImmediate error:&error] ||
        (!playing[side] && ![players[side] startAtTime:CHHapticTimeImmediate error:&error])) { [self disable:side reason:haptic_error_text(error)]; return NO; }
    // 每个 session/通道只记录首次非零成功提交，不在后续连续更新中高频写日志。
    // Log only the first successful nonzero submission per session/channel, without high-frequency logging during continuous updates.
    if (!logged_accepted[side]) {
        logged_accepted[side]=YES;
        LOG("Mac 触觉 accepted t_ns=%llu session=%llu side=%u low=%.3f@%.1fHz high=%.3f@%.1fHz rawRSS=%.6f mappedIntensity=%.6f sharpnessControl=0（CoreHaptics 接受提交，非电机遥测）",
            (unsigned long long)now_ns(),(unsigned long long)epoch,side,value.low_amp,value.low_freq,value.high_amp,value.high_freq,raw_rss,intensity);
    }
    playing[side]=YES; [self recordApply:side rss:raw_rss intensity:intensity]; return YES;
}
@end

void dm_haptic_bind(void *ptr, int active) {
    // opt-in 在入口完整路由；未连接或协议失败时不改走 CoreHaptics。
    // Route the complete opt-in at the boundary; disconnection or protocol failure never selects CoreHaptics.
    if (dm_ns2_enabled()) { (void)ptr; dm_ns2_set_active(active); return; }
    (void)haptic_config();
    GCController *controller=(__bridge GCController *)ptr;
    // 真实设备声明与引擎就绪分开：先让 HID 识别硬件通道，避免一次查询把异步准备窗口缓存为无设备。
    // Separate real device declarations from engine readiness: HID can identify hardware channels before async preparation, avoiding a cached one-shot no-device result.
    unsigned device_caps=active ? haptic_device_caps(controller) : 0;
    dispatch_queue_t queue=haptic_queue();
    DMHapticAudit *newAudit=[DMHapticAudit new];
    pthread_mutex_lock(&g_haptic_mu);
    if (g_haptic_shutdown) { pthread_mutex_unlock(&g_haptic_mu); return; }
    if (controller!=g_haptic_controller) g_haptic_initialized=0;
    g_haptic_controller=controller; g_haptic_active=active!=0;
    uint64_t epoch=++g_haptic_epoch;
    newAudit->epoch=epoch; g_haptic_audit=newAudit;
    g_haptic_caps=g_haptic_dirty=0;
    g_haptic_device_caps=device_caps;
    memset(g_haptic_pending,0,sizeof g_haptic_pending); memset(g_haptic_actual,0,sizeof g_haptic_actual);
    // 在公开新状态的同一把锁内先排入 prepare；HID 的 initialize/send 不可能排到它前面。
    // Queue prepare under the lock that publishes new state, so HID initialize/send cannot precede it.
    dispatch_async(queue, ^{
        @autoreleasepool {
            [g_haptic_session shutdown]; g_haptic_session=nil;
            pthread_mutex_lock(&g_haptic_mu); BOOL current=epoch==g_haptic_epoch && g_haptic_active; pthread_mutex_unlock(&g_haptic_mu);
            if (!current || !controller) return;
            DMHapticSession *session=[DMHapticSession new]; session->epoch=epoch; session->audit=newAudit;
            unsigned caps=[session prepare:controller];
            pthread_mutex_lock(&g_haptic_mu); current=epoch==g_haptic_epoch && g_haptic_active;
            if (current) {
                g_haptic_session=session; g_haptic_caps=g_haptic_device_caps=caps;
                g_haptic_initialized &= caps;
                for (unsigned side=0;side<2;side++) if (!haptic_bit(side,caps)) {
                    g_haptic_dirty &= ~(1u<<side); g_haptic_pending[side]=(dm_vibration_value){0};
                }
                haptic_enqueue_locked();
            }
            pthread_mutex_unlock(&g_haptic_mu);
            if (!current) [session shutdown];
        }
    });
    pthread_mutex_unlock(&g_haptic_mu);
}
int dm_haptic_shutdown(const char *reason, unsigned timeout_ms) {
    if (dm_ns2_enabled()) return dm_ns2_shutdown(reason, timeout_ms);
    dispatch_queue_t queue=haptic_queue(); dispatch_group_t group=haptic_stops();
    uint64_t begin=now_ns();
    dispatch_time_t deadline=dispatch_time(DISPATCH_TIME_NOW,(int64_t)timeout_ms*NSEC_PER_MSEC);
    BOOL onWorker=dispatch_get_specific(&g_haptic_queue_key)!=NULL;
    pthread_mutex_lock(&g_haptic_mu);
    BOOL first=!g_haptic_shutdown;
    if (first) {
        g_haptic_shutdown=YES; g_haptic_active=NO; ++g_haptic_epoch;
        g_haptic_caps=g_haptic_device_caps=g_haptic_initialized=g_haptic_dirty=0;
        memset(g_haptic_pending,0,sizeof g_haptic_pending); memset(g_haptic_actual,0,sizeof g_haptic_actual);
        // 先 enter 再排队；等待者不会在排队清理和异步 stop 登记之间误见空 group。
        // Enter before enqueueing so waiters cannot observe an empty group before queued cleanup registers asynchronous stops.
        dispatch_group_enter(group);
        if (!onWorker) dispatch_async(queue, ^{
            @autoreleasepool { [g_haptic_session shutdown]; g_haptic_session=nil; }
            dispatch_group_leave(group);
        });
    }
    pthread_mutex_unlock(&g_haptic_mu);
    if (first) LOG("Mac 触觉 shutdown-request t_ns=%llu reason=%s timeout_ms=%u",(unsigned long long)begin,reason?reason:"unknown",timeout_ms);
    if (first && onWorker) {
        @autoreleasepool { [g_haptic_session shutdown]; g_haptic_session=nil; }
        dispatch_group_leave(group);
    }
    // 工作队列不等待自身之前尚未执行的清理；其余调用只有限时等待，不向主队列同步派发。
    // The worker never waits for cleanup queued behind itself; other callers wait only to a deadline, without synchronous main-queue dispatch.
    long pending=dispatch_group_wait(group,onWorker?DISPATCH_TIME_NOW:deadline);
    pthread_mutex_lock(&g_haptic_mu); unsigned errors=g_haptic_stop_errors; pthread_mutex_unlock(&g_haptic_mu);
    if (first || pending || errors) LOG("%sMac 触觉 shutdown-barrier t_ns=%llu reason=%s elapsed_ms=%.3f pending=%ld api_errors=%u result=%s（仅 API completion，非电机遥测）",
        pending||errors?"【红】":"",(unsigned long long)now_ns(),reason?reason:"unknown",(now_ns()-begin)/1e6,pending,errors,pending?"incomplete":errors?"completed-with-errors":"completed");
    return !pending && !errors;
}
unsigned dm_haptic_capabilities(void) {
    if (dm_ns2_enabled()) return dm_ns2_capabilities();
    pthread_mutex_lock(&g_haptic_mu); unsigned caps=g_haptic_caps; pthread_mutex_unlock(&g_haptic_mu); return caps;
}
// HID 枚举 / 初始化只用真实设备能力：GCDeviceHaptics 声明或 NS2 完整握手后的通道；故障立即撤销。
// HID enumeration / initialization uses actual capabilities: GCDeviceHaptics declarations or fully initialized NS2 channels, revoked on failure.
unsigned dm_haptic_device_capabilities(void) {
    if (dm_ns2_enabled()) return dm_ns2_device_capabilities();
    pthread_mutex_lock(&g_haptic_mu); unsigned caps=g_haptic_device_caps; pthread_mutex_unlock(&g_haptic_mu); return caps;
}
int dm_haptic_initialize(unsigned side) {
    if (dm_ns2_enabled()) return dm_ns2_initialize(side);
    if (side>1) return 0;
    pthread_mutex_lock(&g_haptic_mu); unsigned bit=haptic_bit(side,g_haptic_device_caps); g_haptic_initialized |= bit; pthread_mutex_unlock(&g_haptic_mu);
    return bit!=0;
}
static void haptic_drain(void) {
    @autoreleasepool {
        pthread_mutex_lock(&g_haptic_mu);
        unsigned dirty=g_haptic_dirty; uint64_t epoch=g_haptic_epoch;
        g_haptic_queued=NO;
        DMHapticSession *session=g_haptic_session;
        if (!dirty || !session || session->epoch!=epoch) { pthread_mutex_unlock(&g_haptic_mu); return; }
        dm_vibration_value values[2]={g_haptic_pending[0],g_haptic_pending[1]}; g_haptic_dirty=0;
        pthread_mutex_unlock(&g_haptic_mu);
        for (unsigned side=0; side<2; side++) if (dirty & (1u<<side)) {
                // 生命周期在游戏线程之外变化时，尽量在发给引擎前丢弃旧代命令；排队的 shutdown 最终强停止。
                // If lifecycle changes outside the game thread, discard old-generation commands before engine submission; queued shutdown provides the final hard stop.
                pthread_mutex_lock(&g_haptic_mu); BOOL current=epoch==g_haptic_epoch && g_haptic_active; pthread_mutex_unlock(&g_haptic_mu);
                if (!current) break;
                if ([session apply:values[side] side:side]) {
                    pthread_mutex_lock(&g_haptic_mu); if(epoch==g_haptic_epoch) g_haptic_actual[side]=values[side]; pthread_mutex_unlock(&g_haptic_mu);
                }
        }
        // 每个任务最多处理两个通道后让出队列，停止任务不会被持续发送饿死，临时对象也及时释放。
        // Yield after at most two channels so continuous sends cannot starve stop tasks, and temporary objects are released promptly.
        pthread_mutex_lock(&g_haptic_mu); haptic_enqueue_locked(); pthread_mutex_unlock(&g_haptic_mu);
    }
}
void dm_haptic_send(unsigned side, const dm_vibration_value *value) {
    if (dm_ns2_enabled()) { dm_ns2_send(side, value); return; }
    if (side>1 || !value) return;
    dm_vibration_value raw=*value, bounded=haptic_value(raw);
    pthread_mutex_lock(&g_haptic_mu);
    unsigned bit=haptic_bit(side,g_haptic_device_caps);
    if (!g_haptic_active || !(bit & g_haptic_initialized)) { pthread_mutex_unlock(&g_haptic_mu); return; }
    if (g_haptic_audit) {
        haptic_audit_side *s=&g_haptic_audit->sides[side]; s->received++;
        if (haptic_raw_rss(bounded)>0) s->last_received_nonzero_ns=now_ns(); else s->last_received_zero_ns=now_ns();
        s->coalesced += (g_haptic_dirty & (1u<<side))!=0;
        if (isfinite(raw.low_amp)) s->raw_low_peak=fmaxf(s->raw_low_peak,raw.low_amp);
        if (isfinite(raw.high_amp)) s->raw_high_peak=fmaxf(s->raw_high_peak,raw.high_amp);
    }
    g_haptic_pending[side]=bounded; g_haptic_dirty |= 1u<<side;
    haptic_enqueue_locked();
    pthread_mutex_unlock(&g_haptic_mu);
}
void dm_haptic_actual(unsigned side, dm_vibration_value *out) {
    if (dm_ns2_enabled()) { dm_ns2_actual(side, out); return; }
    memset(out,0,sizeof *out); if(side>1) return;
    pthread_mutex_lock(&g_haptic_mu); *out=g_haptic_actual[side]; pthread_mutex_unlock(&g_haptic_mu);
}
