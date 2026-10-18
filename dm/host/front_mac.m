// Mac 前端：DM_WINDOW=1 时开窗口（CAMetalLayer，供图形后端出图）+ 键盘 → dm_input；客体改在大栈线程上跑，主线程归 AppKit。
// 不开 DM_WINDOW：客体直接在当前线程跑，行为与无前端时相同（无窗口测试用这个）。
// 键位（任天堂布局）：WASD 左摇杆、方向键十字键、J=A K=B U=X I=Y、Q=L E=R、Z=ZL C=ZR、F=按左摇杆 G=按右摇杆、回车=+ Tab=−。
// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: the guest runs directly on the current thread, behaving the same as with no front end (windowless tests use this).
// Key map (Nintendo layout): WASD left stick, arrow keys D-pad, J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick, Return=+ Tab=−.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATextLayer.h>
#import <Metal/Metal.h>
#include "dm.h"
#include "dm_input.h"
#include "dm_uiscreen.h"
void mtl_set_ui_rect_fn(void (*fn)(double *y, double *h));
static void ui_rect_frac(double *fy, double *fh) { int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); if (dm_screen_real(&rw, &rh) && rh > 0) { *fy = (double)y / rh; *fh = (double)h / rh; } }   // 同 front_ios.m：界面矩形的纵向位置与高（占画面高的比例） | Same as front_ios.m: vertical position and height of the UI rectangle (as fractions of the frame height)
__attribute__((constructor)) static void ui_shift_reg(void) { mtl_set_ui_rect_fn(ui_rect_frac); }   // 不开窗口的跑法（截图 / 闸门）也要生效，所以不放在建图层那里 | Must also take effect when running without a window (screenshots / gate checks), so it is not placed where the layer is created
#include "front.h"
#include "ns2.h"
#include "hle.h"
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>

static CAMetalLayer *g_layer;
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;

// 可选统计只在后台每秒增量读取，不进入逐帧渲染路径；文字图层不接收鼠标或焦点。
// Optional statistics read incrementally once per second off-thread; the text layer receives neither mouse nor focus.
static void stats_hud_start(NSView *view) {
    const char *enabled = getenv("DM_STATS_HUD"); if (!enabled || strcmp(enabled, "1")) return;
    const char *path = getenv("DM_NVN_TSV");
    if (!path || !*path) die("DM_STATS_HUD=1 requires DM_NVN_TSV");
    NSString *filePath = [NSString stringWithUTF8String:path];
    CATextLayer *text = [CATextLayer layer]; text.frame = CGRectMake(12, view.bounds.size.height - 116, 560, 104);
    text.autoresizingMask = kCALayerMinYMargin; text.contentsScale = view.window.backingScaleFactor;
    text.font = (__bridge CFTypeRef)[NSFont monospacedSystemFontOfSize:14 weight:NSFontWeightMedium]; text.fontSize = 14;
    text.foregroundColor = NSColor.whiteColor.CGColor; text.backgroundColor = [NSColor colorWithWhite:0 alpha:.75].CGColor;
    text.cornerRadius = 5; text.string = @"Stats: waiting for real TSV…\nCPU process: 100% = 1 core\nGPU time: N/A";
    [view.layer addSublayer:text];
    dispatch_queue_t queue = dispatch_queue_create("dm.stats-hud", DISPATCH_QUEUE_SERIAL);
    static dispatch_source_t timer;
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, 100 * NSEC_PER_MSEC);
    __block int fd = -1, format = 0, failed = 0;
    __block off_t offset = 0;
    __block NSMutableData *pending = [NSMutableData data];
    __block NSMutableDictionary<NSNumber *, NSMutableDictionary *> *frames = [NSMutableDictionary dictionary];
    __block double previousWall = 0, previousCPU = 0, latestSeen = 0, latestAdvanceWall = 0;
    dispatch_source_set_event_handler(timer, ^{
        @autoreleasepool {
            if (failed) return;
            NSString *failure = nil;
            if (fd < 0) { fd = open(filePath.fileSystemRepresentation, O_RDONLY); if (fd < 0 && errno != ENOENT) failure = @"TSV open failed"; }
            struct stat st = {0}; BOOL backlog = NO;
            if (fd >= 0 && !failure) {
                if (fstat(fd, &st) || st.st_size < offset) failure = @"TSV changed/truncated";
                else {
                    // 每秒最多读取 8 MiB；积压时不把旧窗口冒充实时统计。
                    // Read at most 8 MiB per second; never present a lagging window as live statistics.
                    size_t wanted = (size_t)MIN((off_t)(8u << 20), st.st_size - offset);
                    NSMutableData *chunk = [NSMutableData dataWithLength:wanted];
                    ssize_t n = wanted ? read(fd, chunk.mutableBytes, wanted) : 0;
                    if (n < 0) failure = @"TSV read failed";
                    else {
                        chunk.length = (NSUInteger)n; [pending appendData:chunk]; offset += n; backlog = offset < st.st_size;
                        const uint8_t *bytes = pending.bytes; NSUInteger begin = 0;
                        for (NSUInteger i = 0; i < pending.length; i++) if (bytes[i] == '\n') {
                            NSString *line = [[NSString alloc] initWithBytes:bytes + begin length:i - begin encoding:NSUTF8StringEncoding]; begin = i + 1;
                            if (!line) { failure = @"TSV UTF-8 error"; break; }
                            if (!line.length || [line hasPrefix:@"#"]) continue;
                            if (!format) {
                                if ([line isEqualToString:@"frame\tname\tvalue"]) format = 1;
                                else if ([line isEqualToString:@"frame\\tname\\tvalue"]) format = 2;
                                else { failure = @"TSV header error"; break; }
                                continue;
                            }
                            NSArray<NSString *> *cols = [line componentsSeparatedByString:[line containsString:@"\t"] ? @"\t" : @"\\t"];
                            if (cols.count != 3) { failure = @"TSV column error"; break; }
                            NSString *name = cols[1];
                            if (![@[@"@pres_at_ms", @"@presented_ms", @"@gpu_ms"] containsObject:name]) continue;
                            NSScanner *fs = [NSScanner scannerWithString:cols[0]], *vs = [NSScanner scannerWithString:cols[2]];
                            unsigned long long frame; double value;
                            if (![fs scanUnsignedLongLong:&frame] || !fs.atEnd || ![vs scanDouble:&value] || !vs.atEnd || !isfinite(value)) { failure = @"TSV numeric error"; break; }
                            NSNumber *key = @(frame); NSMutableDictionary *row = frames[key];
                            if (!row) frames[key] = row = [NSMutableDictionary dictionary];
                            row[name] = @(value);
                        }
                        if (begin) [pending replaceBytesInRange:NSMakeRange(0, begin) withBytes:NULL length:0];
                        if (pending.length > 4096) failure = @"TSV row exceeds limit";
                    }
                }
            }
            if (failure) {
                failed = 1; if (fd >= 0) { close(fd); fd = -1; }
                LOG("Stats HUD: %s", failure.UTF8String);
                dispatch_async(dispatch_get_main_queue(), ^{ text.string = [@"Stats HUD error: " stringByAppendingString:failure]; }); return;
            }
            NSArray<NSNumber *> *keys = [[frames allKeys] sortedArrayUsingSelector:@selector(compare:)];
            // 8192 帧固定上限，延迟 16 帧结算以等待真实异步上屏反馈；不跨未知帧连线。
            // Bound storage to 8192 frames and settle 16 frames late for asynchronous presentation feedback; never bridge unknown frames.
            if (keys.count > 8192) { for (NSUInteger i = 0; i < keys.count - 8192; i++) [frames removeObjectForKey:keys[i]]; keys = [[frames allKeys] sortedArrayUsingSelector:@selector(compare:)]; }
            uint64_t maximum = keys.lastObject.unsignedLongLongValue, settled = maximum > 16 ? maximum - 16 : 0;
            double latest = 0, gpu = NAN;
            for (NSNumber *key in keys) if (key.unsignedLongLongValue <= settled) {
                NSDictionary *row = frames[key]; if (row[@"@pres_at_ms"]) latest = MAX(latest, [row[@"@pres_at_ms"] doubleValue]);
                if (row[@"@gpu_ms"] && [row[@"@gpu_ms"] doubleValue] >= 0) gpu = [row[@"@gpu_ms"] doubleValue];
            }
            double observedWall = CACurrentMediaTime();
            if (latest > latestSeen) { latestSeen = latest; latestAdvanceWall = observedWall; }
            BOOL stale = !latestAdvanceWall || observedWall - latestAdvanceWall > 2;
            NSMutableArray<NSNumber *> *gaps = [NSMutableArray array]; uint64_t previousFrame = 0; double previousTime = 0; NSUInteger unknown = 0;
            for (NSNumber *key in keys) {
                uint64_t frame = key.unsignedLongLongValue; NSDictionary *row = frames[key];
                double at = [row[@"@pres_at_ms"] doubleValue]; if (frame > settled || at <= 0 || at < latest - 60000) continue;
                if (previousTime) {
                    BOOL confirmed = at > previousTime;
                    for (uint64_t f = previousFrame + 1; confirmed && f < frame; f++) confirmed = [frames[@(f)][@"@presented_ms"] isEqual:@(-1)];
                    if (confirmed) [gaps addObject:@(at - previousTime)]; else unknown++;
                }
                previousFrame = frame; previousTime = at;
            }
            NSArray<NSNumber *> *sorted = [gaps sortedArrayUsingSelector:@selector(compare:)]; double sum = 0, tail = 0;
            for (NSNumber *gap in gaps) sum += gap.doubleValue;
            NSUInteger count = sorted.count, slow = (count + 99) / 100;
            for (NSUInteger i = count - slow; i < count; i++) tail += sorted[i].doubleValue;
            struct rusage usage; double wall = CACurrentMediaTime(), cpu = 0, percent = NAN;
            if (!getrusage(RUSAGE_SELF, &usage)) {
                cpu = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
                if (previousWall && wall > previousWall) percent = 100 * (cpu - previousCPU) / (wall - previousWall);
                previousWall = wall; previousCPU = cpu;
            }
            NSString *fps = count && !unknown && !backlog && !stale ? [NSString stringWithFormat:@"%.1f", 1000 * count / sum] : @"N/A";
            NSString *low = count && !unknown && !backlog && !stale ? [NSString stringWithFormat:@"%.1f", 1000 * slow / tail] : @"N/A";
            NSString *cpuLabel = isfinite(percent) ? [NSString stringWithFormat:@"%.0f%%", percent] : @"N/A";
            NSString *gpuLabel = isfinite(gpu) && !backlog && !stale ? [NSString stringWithFormat:@"%.2f ms", gpu] : @"N/A";
            NSString *label = [NSString stringWithFormat:@"Observed FPS %@   1%% low %@\nCPU process %@ (100%% = 1 core)\nGPU time %@ (not utilization)\n<=60s / %lu intervals / unknown %lu%@", fps, low, cpuLabel, gpuLabel, (unsigned long)count, (unsigned long)unknown, backlog ? @" / catching up" : stale ? @" / stale" : @""];
            dispatch_async(dispatch_get_main_queue(), ^{ text.string = label; });
        }
    });
    dispatch_resume(timer);
}

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
// 原版 16:9 常数，行为不变；DM_GAME_RES=宽x高（主机模式，掌机按 720/900 同比）只供与 iOS 宽屏对拍
// The original game's 16:9 constants, behavior unchanged; DM_GAME_RES=<width>x<height> (docked mode; handheld scaled by 720/900) is only for side-by-side comparison with iOS widescreen
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}

static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    dm_input_set(g_btn, lx, ly, 0, 0);
}
// 失焦时同时清状态和待消费按下，避免恢复窗口后继续移动或补发旧按键。
// Clear state and pending presses on focus loss so returning cannot keep moving or replay an old key.
static void release_keyboard(void) {
    g_w = g_a = g_s = g_d = 0; g_btn = 0; dm_input_reset();
}
// 窗口关闭与 AppKit 退出共用宿主收尾，确保触觉、音频、GPU 和诊断都完成清理。
// Window close and AppKit termination share host shutdown to clean up haptics, audio, GPU and diagnostics.
static void front_shutdown(const char *reason) {
    release_keyboard(); dm_touch_set(NULL, 0); front_controller_stop();
    dm_host_shutdown(reason);
}
// App 与窗口通知的顺序不固定；每次按真实组合状态同步游戏、键盘和手柄。
// App and window notifications may arrive in either order; synchronize guest, keyboard and controller from their actual combined state.
static void sync_focus(NSWindow *win, const char *event) {
    int active = NSApp.active, key = win.keyWindow, minimized = win.miniaturized;
    int state = minimized ? 3 : active && key ? 1 : 2;
    if (state != 1) { release_keyboard(); dm_touch_set(NULL, 0); }
    front_controller_set_active(state == 1);
    if (dm_focus_now() != state) dm_focus_set(state);
    LOG("Mac focus t=%.6f event=%s active=%d key=%d minimized=%d visible=%d fullscreen=%d guest=%d",
        CACurrentMediaTime(), event, active, key, minimized, win.visible,
        (win.styleMask & NSWindowStyleMaskFullScreen) != 0, state);
}
static int key_bit(unsigned short code) {
    switch (code) {
        case 38: return 0;  case 40: return 1;  case 32: return 2;  case 34: return 3;    // J K U I → A B X Y
        case 3: return 4;   case 5: return 5;                                            // F G → 按摇杆 | F G → press stick
        case 12: return 6;  case 14: return 7;  case 6: return 8;   case 8: return 9;    // Q E Z C → L R ZL ZR
        case 36: return 10; case 48: return 11;                                          // 回车 Tab → + − | Return Tab → + −
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15; // ← ↑ → ↓
    }
    return -1;
}
static void on_key(NSEvent *e, int down) {
    unsigned short c = e.keyCode;
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    int b = key_bit(c);
    if (b >= 0) g_btn = down ? (g_btn | 1ull << b) : (g_btn & ~(1ull << b));
    push();
}

typedef struct { void (*fn)(void); NSWindow *window; NSString *title; } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];                       // 信号备用栈是每线程的；崩溃打印要在客体线程上也生效 | The signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    if (dm_ns2_enabled()) {
        // 先等真实校准与订阅完成，再让客体做一次性设备枚举；AppKit 主循环继续处理权限、焦点和退出。
        // Wait for real calibration and subscriptions before one-shot guest enumeration; AppKit still handles permissions, focus and quit.
        LOG("NS2 Pro BLE：等待连接就绪后启动游戏，请按手柄同步键；可从原生菜单退出");
        while (dm_ns2_capabilities() != (DM_HAPTIC_LEFT | DM_HAPTIC_RIGHT)) usleep(10000);
        guest_arg *arg = p;
        dispatch_async(dispatch_get_main_queue(), ^{ arg->window.title = arg->title; });
        dm_pad_reset(); LOG("NS2 Pro BLE：双侧通道已就绪，启动客体设备枚举");
    }
    ((guest_arg *)p)->fn(); LOG("客体线程结束"); exit(0);
}

void front_run(void (*guest)(void)) {
    if (!getenv("DM_WINDOW")) {
        if (dm_ns2_enabled()) die("【红】DM_NS2_PRO=ble 需要 DM_WINDOW=1，以处理蓝牙权限和窗口生命周期");
        guest(); return;
    }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        // 手工启动 AppKit 时没有 nib 菜单；显式注册原生退出动作，Command-Q 才能由菜单处理。
        // A manually started AppKit app has no nib menu; register native termination so the menu handles Command-Q.
        NSMenu *menu = [NSMenu new], *appMenu = [NSMenu new];
        NSMenuItem *appItem = [NSMenuItem new]; appItem.submenu = appMenu; [menu addItem:appItem];
        NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:@DM_GAME_APP_NAME]
                                                   action:@selector(terminate:) keyEquivalent:@"q"];
        quit.target = NSApp; [appMenu addItem:quit]; NSApp.mainMenu = menu;
        NSRect r = NSMakeRect(0, 0, 1280, 720);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @DM_GAME_APP_NAME;   // 游戏档案 app_name | game profile app_name
        const char *window_title = getenv("DM_WINDOW_TITLE");
        if (window_title && *window_title) win.title = [NSString stringWithUTF8String:window_title];
        NSView *v = win.contentView;
        v.wantsLayer = YES;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        // 双缓冲可限制排队显示延迟；保留三缓冲供同一 App 做真实对照。
        // Double buffering limits queued display latency; retain triple buffering for actual same-app comparisons.
        const char *drawable_count = getenv("DM_MAC_DRAWABLES");
        if (drawable_count && strcmp(drawable_count, "2") && strcmp(drawable_count, "3")) die("DM_MAC_DRAWABLES 只接受 2 或 3");
        g_layer.maximumDrawableCount = drawable_count ? (NSUInteger)atoi(drawable_count) : 3;
        LOG("Mac 显示 drawable 数 %lu", (unsigned long)g_layer.maximumDrawableCount);
        g_layer.contentsScale = win.backingScaleFactor;
        v.layer = g_layer;
        stats_hud_start(v);
        [win center];
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        front_controller_start();
        front_controller_set_active(NSApp.active && win.keyWindow && !win.miniaturized);
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;   // 只吃游戏窗口的键；软键盘输入框等其他窗口照常收字 | Only swallow keys for the game window; other windows such as the software keyboard input box receive typing as usual
            unsigned short code = e.keyCode;
            if (key_bit(code) < 0 && code != 13 && code != 0 && code != 1 && code != 2) return e;
            BOOL focused = NSApp.active && win.keyWindow && !win.miniaturized;
            BOOL command = (e.modifierFlags & NSEventModifierFlagCommand) != 0;
            // 松开先清状态，即使此时按着 Command 或焦点已变；系统快捷键与未映射键正常交给 AppKit。
            // Clear releases even with Command held or focus changed; route system shortcuts and unmapped keys normally through AppKit.
            if (e.type == NSEventTypeKeyUp) { on_key(e, 0); return command || !focused ? e : nil; }
            if (command || !focused) return e;
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // 吃掉，免得系统提示音 | Swallow it to avoid the system beep
        }];
        // 鼠标当单点触屏（本机测触屏用，front.h dm_touch_set）：左键按下 / 拖动 = 手指 0 在归一化坐标处，松开 = 抬起
        // Mouse as a single-point touchscreen (for testing touch on the local machine, front.h dm_touch_set): left button down / drag = finger 0 at the normalized coordinates, release = lifted
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;
            // 观察点击而不截断派发，让 AppKit 正常激活窗口和处理标题栏；只有内容区点击生成触点。
            // Observe clicks without stopping dispatch so AppKit can activate windows and handle title bars; only content-area clicks generate touches.
            if (e.type == NSEventTypeLeftMouseUp) { dm_touch_set(NULL, 0); return e; }
            if (!NSApp.active || !win.keyWindow || win.miniaturized) return e;
            NSPoint p = [v convertPoint:e.locationInWindow fromView:nil]; NSSize sz = v.bounds.size;
            if (sz.width <= 0 || sz.height <= 0 || !NSPointInRect(p, v.bounds)) return e;
            dm_touch t = {0, (float)(p.x / sz.width), (float)(1.0 - p.y / sz.height)};   // AppKit 视图原点在左下，触屏原点在左上 | The AppKit view origin is at bottom left, the touchscreen origin is at top left
            dm_touch_set(&t, 1); return e;
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; front_shutdown("window-close"); }];
        // 生命周期（hle.h / dm.h）：失焦 → 2、拿回焦点 → 1；最小化 → 3 + 音频暂停 + GPU 挂起，还原 → 恢复（随后拿回焦点 → 1）
        // Lifecycle (hle.h / dm.h): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resume (followed by focus regained → 1)
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; sync_focus(win, "resign-key"); }];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; sync_focus(win, "become-key"); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; sync_focus(win, "minimize"); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); sync_focus(win, "restore"); }];
        // App 失活也释放手柄；应用重新激活时仍要求游戏窗口有焦点。
        // Also release controllers on app deactivation; reactivation still requires focus on the game window.
        [nc addObserverForName:NSApplicationDidResignActiveNotification object:NSApp queue:nil usingBlock:^(NSNotification *n) { (void)n; sync_focus(win, "resign-active"); }];
        [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:NSApp queue:nil usingBlock:^(NSNotification *n) { (void)n; sync_focus(win, "become-active"); }];
        [nc addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; sync_focus(win, "enter-fullscreen"); }];
        [nc addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; sync_focus(win, "exit-fullscreen"); }];
        [nc addObserverForName:NSApplicationWillTerminateNotification object:NSApp queue:nil usingBlock:^(NSNotification *n) { (void)n; front_shutdown("AppKit-terminate"); }];
        sync_focus(win, "startup");
        // 客体线程：栈 64 MiB（Switch 主线程栈由 npdm 指定，宿主给足）
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest; ga.window = win; ga.title = win.title;
        if (dm_ns2_enabled()) win.title = [NSString stringWithFormat:@"%@ — Waiting for Switch 2 Pro", ga.title];
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
        front_shutdown("AppKit-run-return");
    }
}

// 软键盘输入框（front.h）：主线程弹 NSAlert + 文本框，客体线程同步等结果。无窗口模式不弹（返回 -1，调用方按取消）
// Software keyboard input box (front.h): the main thread shows an NSAlert + text field and the guest thread waits synchronously for the result. Not shown in windowless mode (returns -1, the caller treats it as cancel)
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    __block int r = 0;
    __block NSString *text = nil;
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSAlert *a = [NSAlert new];
        a.messageText = title && *title ? @(title) : @"输入";
        NSString *info = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
        a.informativeText = info;
        [a addButtonWithTitle:@"确定"]; [a addButtonWithTitle:@"取消"];
        NSTextField *f = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
        f.stringValue = initial ? @(initial) : @"";
        if (guide && *guide) f.placeholderString = @(guide);
        a.accessoryView = f; a.window.initialFirstResponder = f;
        r = [a runModal] == NSAlertFirstButtonReturn;
        text = f.stringValue;
    });
    if (!r) return 0;
    (void)max_chars;                              // 截断按 UTF-16 单元在 hle_swkbd.c 统一做 | Truncation is done uniformly in hle_swkbd.c in UTF-16 units
    snprintf(out, out_cap, "%s", text.UTF8String);
    return 1;
}
