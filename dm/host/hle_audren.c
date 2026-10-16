// nn::audio AudioRenderer（audren）——进程内渲染：用游戏 SDK 自带的整套渲染器，不重写混音。
// 较新的 SDK 把渲染器服务端（nn::audio::server：InfoUpdater / CommandGenerator …）和 DSP 命令处理的 CPU 版（nn::audio::dsp::CommandListProcessor：
// ADPCM / PCM 解码、重采样、混音、混响 / 延迟 / 双二阶 / 压限 …）都链进了应用，供「CPU 渲染 + 手动执行」模式用（AudioRendererParameter
// +0x2c renderingDevice = 1 CPU，+0x30 executionMode = 1 手动）。这个模式下 OpenAudioRenderer 走进程内的 DFC 管理器（CreateAudioRendererManagerByDfc），
// RequestUpdate 直接在进程内解析，渲染由调用 nn::audio::ExecuteAudioRendererRendering 的线程在 CPU 上做，做完触发游戏的渲染器事件。
// 宿主做三件事：
//   1. 接管 GetAudioRendererWorkBufferSize / OpenAudioRenderer：参数改成 CPU + 手动后调 SDK 原函数（工作区大小按改后的参数算）；
//   2. 渲染线程（宿主起的客体线程）：输出端每放回一块 5 ms 缓冲就渲一帧，节奏跟着声卡时钟走；
//   3. 接管 AudioRendererImpl::ExecuteAudioRendererRendering（经 PLT 调到）：照原函数生成命令、交 CommandListProcessor 处理，
//      再从命令表里找 DeviceSink 命令（CPU 处理器对它是空操作），把它的输入混音缓冲（int32，16 位范围）夹成 int16 立体声交给 AudioOut 输出（hle_audio.c）。
// 命令表格式（SDK 16）：头 +0x04 命令数、+0x08 混音缓冲地址、+0x10 缓冲数、+0x14 每帧采样数、+0x18 采样率，命令从 +0x20 起；
//   每条命令 +0x00 魔数 0xcafebabe、+0x04 启用、+0x05 命令号（DeviceSink = 0x13）、+0x06 大小；DeviceSink +0x128 上采样缓冲、+0x130 输入数、+0x134 输入下标（s16）。
// nn::audio AudioRenderer (audren) -- in-process rendering: use the complete renderer the game's SDK ships with, no mixer of our own.
// Newer SDKs link the renderer server side (nn::audio::server: InfoUpdater / CommandGenerator ...) and the CPU version of the DSP command processing (nn::audio::dsp::CommandListProcessor:
// ADPCM / PCM decoding, resampling, mixing, reverb / delay / biquad / limiter / compressor ...) into the application, for the "CPU rendering + manual execution" mode (AudioRendererParameter
// +0x2c renderingDevice = 1 CPU, +0x30 executionMode = 1 manual). In this mode OpenAudioRenderer goes through the in-process DFC manager (CreateAudioRendererManagerByDfc),
// RequestUpdate is parsed in-process, and rendering happens on the CPU of whichever thread calls nn::audio::ExecuteAudioRendererRendering, which then signals the game's renderer event.
// The host does three things:
//   1. Takes over GetAudioRendererWorkBufferSize / OpenAudioRenderer: switches the parameter to CPU + manual and calls the SDK original (the work buffer size follows the modified parameter);
//   2. A render thread (a guest thread started by the host): renders one frame whenever the output hands back a 5 ms buffer, so pacing follows the sound card clock;
//   3. Takes over AudioRendererImpl::ExecuteAudioRendererRendering (reached through the PLT): generates commands and runs CommandListProcessor like the original,
//      then finds the DeviceSink command in the command list (a no-op for the CPU processor) and clamps its input mix buffers (int32 holding 16-bit range) to int16 stereo for AudioOut (hle_audio.c).
// Command list format (SDK 16): header +0x04 command count, +0x08 mix buffer address, +0x10 buffer count, +0x14 samples per frame, +0x18 sample rate, commands from +0x20;
//   each command +0x00 magic 0xcafebabe, +0x04 enabled, +0x05 command ID (DeviceSink = 0x13), +0x06 size; DeviceSink +0x128 upsampler buffer, +0x130 input count, +0x134 input indices (s16).
#include "hle.h"
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size);
void *dm_pcmout_open(int ch);
void dm_pcmout_start(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);

enum { P_DEVICE = 0x2c, P_MODE = 0x30, P_SIZE = 0x38, FRAME_MAX = 1024, NBUF = 4 };
static void *gsym(const char *n) { void *p = guest_sym(n); if (!p) die("audren：缺 SDK 符号 %s", n); return p; }
static void cpu_manual(uint8_t *dst, const uint8_t *src) { memcpy(dst, src, P_SIZE); *(int32_t *)(dst + P_DEVICE) = 1; *(int32_t *)(dst + P_MODE) = 1; }

HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "渲染器工作区大小：按 CPU 渲染 + 手动执行的参数算",
          size_t, h_ar_WorkSize, const uint8_t *param) {
    static size_t (*orig)(const uint8_t *); if (!orig) orig = gsym("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
    uint8_t p[P_SIZE]; cpu_manual(p, param); return orig(p);
}
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm", "开渲染器：改成 CPU 渲染 + 手动执行后走 SDK 进程内渲染器",
          Result, h_ar_Open0, void **handle, const uint8_t *param, void *work, size_t sz) {
    static Result (*orig)(void **, const uint8_t *, void *, size_t);
    if (!orig) orig = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm");
    uint8_t p[P_SIZE]; cpu_manual(p, param); return orig(handle, p, work, sz);
}

// ---------- 渲染与输出 | rendering and output ----------
static void *g_handle[2];                    // AudioRendererHandle（渲染器对象、客户端状态）| AudioRendererHandle (renderer object, client state)
static _Atomic int g_open;
static int16_t g_frame[FRAME_MAX * 2]; static int g_frame_n;   // 最近一帧（立体声 int16）| the latest frame (stereo int16)
static _Atomic uint64_t g_frames, g_nonsilent;
enum { CQ = 4 }; static int16_t g_cq[CQ][FRAME_MAX * 2]; static int g_cq_n[CQ]; static _Atomic uint32_t g_cq_head, g_cq_tail;   // 追帧的输出队列 | output queue of catch-up frames

typedef struct { void *ptr; uint64_t cap, size; } gbuf;       // 与 hle_audio.c 的 AudioOutBuffer 前部一致 | same as the front of hle_audio.c's AudioOutBuffer
static void render_main(void *arg) {
    (void)arg;
    void (*exec)(void *, void *) = gsym("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE");
    void *out = dm_pcmout_open(2);
    static int16_t data[NBUF][FRAME_MAX * 2]; static gbuf bufs[NBUF]; gbuf *free_list[NBUF]; int nfree = NBUF;
    for (int i = 0; i < NBUF; i++) { bufs[i] = (gbuf){data[i], sizeof data[i], 0}; free_list[i] = &bufs[i]; }
    dm_pcmout_start(out);
    LOG("audren：渲染线程开跑（%d 块缓冲在途，节奏跟输出端）", NBUF);
    while (atomic_load(&g_open)) {
        while (!nfree) {   // 等输出端放回一块（静音接收端 / 声卡每 5 ms 一块）| wait for the output to hand a block back (silent sink / sound card, one every 5 ms)
            gbuf *b = dm_pcmout_released(out);
            if (b) { free_list[nfree++] = b; break; }
            usleep(1000);
            if (!atomic_load(&g_open)) return;
        }
        const int16_t *src; int got;
        uint32_t h = atomic_load(&g_cq_head);
        if (h != atomic_load(&g_cq_tail)) { src = g_cq[h % CQ]; got = g_cq_n[h % CQ]; atomic_store(&g_cq_head, h + 1); }   // 先放追帧的输出 | play queued catch-up output first
        else { g_frame_n = 0; exec(g_handle[0], g_handle[1]); src = g_frame; got = g_frame_n; }   // SDK：生成命令 → CPU 处理（宿主接管处取出输出）→ 触发游戏的渲染器事件 | SDK: generate commands → CPU processing (output taken in the host takeover) → signal the game's renderer event
        int n = got ? got : 240;
        gbuf *b = free_list[--nfree];
        if (got) memcpy(b->ptr, src, (size_t)n * 4); else memset(b->ptr, 0, (size_t)n * 4);
        b->size = (uint64_t)n * 4;
        if (!dm_pcmout_append(out, b)) free_list[nfree++] = b;
    }
}

HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "开渲染器（带事件）：CPU 渲染 + 手动执行，宿主起渲染线程",
          Result, h_ar_Open, void **handle, void *ev, const uint8_t *param, void *work, size_t sz) {
    static Result (*orig)(void **, void *, const uint8_t *, void *, size_t);
    if (!orig) orig = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    uint8_t p[P_SIZE]; cpu_manual(p, param);
    LOG("audren：OpenAudioRenderer 采样率 %d 每帧 %d 混音缓冲 %d 子混音 %d 声部 %d 输出 %d 效果 %d 分路 %d（原设备 %d 模式 %d → CPU / 手动）",
        *(int32_t *)param, *(int32_t *)(param + 4), *(int32_t *)(param + 8), *(int32_t *)(param + 0xc), *(int32_t *)(param + 0x10), *(int32_t *)(param + 0x14),
        *(int32_t *)(param + 0x18), *(int32_t *)(param + 0x24), *(int32_t *)(param + P_DEVICE), *(int32_t *)(param + P_MODE));
    Result r = orig(handle, ev, p, work, sz);
    if (r) { LOG("【红】audren：SDK OpenAudioRenderer 失败 %#x", r); return r; }
    if (atomic_load(&g_open)) { LOG("【红】audren：第二个渲染器（宿主只驱动一个）"); return r; }
    g_handle[0] = handle[0]; g_handle[1] = handle[1]; atomic_store(&g_open, 1);
    dm_guest_thread_spawn(render_main, NULL, 9, 0x10000);
    return r;
}
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "关渲染器：先停宿主渲染线程再交 SDK", void, h_ar_Close, void *obj, void *state) {
    static void (*orig)(void *, void *); if (!orig) orig = gsym("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE");
    if (obj == g_handle[0]) { atomic_store(&g_open, 0); usleep(20000); }   // 渲染线程每轮查一次，最多等一帧多 | the render thread checks every round; wait a little over one frame
    orig(obj, state);
}

// 取 DeviceSink 的输入混音缓冲 → int16 立体声 | DeviceSink input mix buffers → int16 stereo
static int capture(const uint8_t *cl, int16_t *dst) {
    uint32_t ncmd = *(const uint32_t *)(cl + 4), samples = *(const uint32_t *)(cl + 0x14), rate = *(const uint32_t *)(cl + 0x18);
    const int32_t *mix = *(const int32_t *const *)(cl + 8);
    static int logged;
    if (!logged && ncmd > 4) {   // 第一帧完整命令表：命令号分布 | first full command list: command ID histogram
        int hist[64] = {0}; const uint8_t *q = cl + 0x20; char line[512]; size_t o = 0;
        for (uint32_t i = 0; i < ncmd && *(const uint32_t *)q == 0xcafebabeu && *(const int16_t *)(q + 6) > 0; i++) { hist[q[5] & 63]++; q += *(const int16_t *)(q + 6); }
        for (int k = 0; k < 64; k++) if (hist[k]) o += snprintf(line + o, sizeof line - o, " %d×%d", k, hist[k]);
        LOG("audren：首帧命令 %u 条，按命令号：%s", ncmd, line);
    }
    const uint8_t *c = cl + 0x20;
    for (uint32_t i = 0; i < ncmd; i++) {
        uint32_t magic = *(const uint32_t *)c; int16_t size = *(const int16_t *)(c + 6);
        if (magic != 0xcafebabeu || size <= 0) { if (!logged++) LOG("【红】audren：命令表第 %u 条格式不对（魔数 %#x 大小 %d）", i, magic, size); return 0; }
        if (c[5] == 0x13 && c[4]) {
            uint32_t nin = *(const uint32_t *)(c + 0x130); const int16_t *in = (const int16_t *)(c + 0x134);
            const int32_t *up = *(const int32_t *const *)(c + 0x128);
            if (!logged++) LOG("audren：DeviceSink 输入 %u 个（%d %d …）采样率 %u 每帧 %u 混音缓冲 %p 上采样缓冲 %p", nin, nin ? in[0] : -1, nin > 1 ? in[1] : -1, rate, samples, (void *)mix, (void *)up);
            if (!nin || samples > FRAME_MAX) return 0;
            // 输入缓冲：48 kHz 读混音缓冲；32 kHz 渲染时 DeviceSink 读上采样后的缓冲（按输入顺序连排）
            // Input buffers: at 48 kHz the mix buffers; with 32 kHz rendering DeviceSink reads the upsampled buffer (inputs laid out in order)
            const int32_t *ch[6]; uint32_t k = nin > 6 ? 6 : nin;
            for (uint32_t j = 0; j < k; j++) ch[j] = up && rate != 48000 ? up + (size_t)j * samples : mix + (size_t)in[j] * samples;
            // 声道顺序 前左 前右 中 低音 后左 后右；6 → 2 按系统缺省下混系数（前 1.0、中 0.707、低音 0.251、后 0.707，Q15）
            // Channel order front-left front-right center LFE back-left back-right; 6 → 2 with the system default downmix coefficients (front 1.0, center 0.707, LFE 0.251, back 0.707, Q15)
            enum { CF = 32768, CC = 23167, CL = 8224, CB = 23167 };
            uint64_t loud = 0;
            for (uint32_t s = 0; s < samples; s++) {
                int32_t l, r;
                if (k >= 6) { int64_t c = (int64_t)ch[2][s] * CC + (int64_t)ch[3][s] * CL;
                              l = (int32_t)(((int64_t)ch[0][s] * CF + c + (int64_t)ch[4][s] * CB) >> 15); r = (int32_t)(((int64_t)ch[1][s] * CF + c + (int64_t)ch[5][s] * CB) >> 15); }
                else { l = ch[0][s]; r = k > 1 ? ch[1][s] : l; }
                l = l > 32767 ? 32767 : l < -32768 ? -32768 : l; r = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
                dst[2 * s] = (int16_t)l; dst[2 * s + 1] = (int16_t)r; loud |= (uint64_t)(l | r);
            }
            atomic_fetch_add(&g_frames, 1); if (loud) atomic_fetch_add(&g_nonsilent, 1);
            uint64_t f = atomic_load(&g_frames); if (f == 200 || f % 12000 == 0) LOG("audren：已渲 %llu 帧，其中有声 %llu 帧", (unsigned long long)f, (unsigned long long)atomic_load(&g_nonsilent));
            return (int)samples;
        }
        c += size;
    }
    return 0;
}
// 渲染一帧（生成命令 + CPU 处理）；dst = 输出（立体声 int16，*nout 采样数）。两处调用（渲染线程、追帧）由宿主锁串行：共用同一块命令缓冲
// Render one frame (generate commands + CPU processing); dst = output (stereo int16, *nout samples). The two callers (render thread, catch-up) are serialized by a host lock: they share one command buffer
static pthread_mutex_t g_render_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_updates, g_renders, g_catchups;
static Result render_once(uint8_t *sys, int16_t *dst, int *nout) {
    static int (*mode)(void *), (*device)(void *); static uint8_t *(*cbuf)(void *); static size_t (*cbsz)(void *), (*gen)(void *, void *, size_t);
    static void (*tmax)(void *, uint32_t), (*setup)(void *, const void *, size_t), (*process)(void *), (*lock)(void *), (*unlock)(void *);
    if (!process) {
        lock = gsym("_ZN2nn2os12SdkMutexType4LockEv"); unlock = gsym("_ZN2nn2os12SdkMutexType6UnlockEv");
        mode = gsym("_ZNK2nn5audio6server17AudioRenderSystem16GetExecutionModeEv"); device = gsym("_ZNK2nn5audio6server17AudioRenderSystem18GetRenderingDeviceEv");
        cbuf = gsym("_ZNK2nn5audio6server17AudioRenderSystem26GetCommandBufferWorkBufferEv"); cbsz = gsym("_ZNK2nn5audio6server17AudioRenderSystem30GetCommandBufferWorkBufferSizeEv");
        gen = gsym("_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandEPvm");
        tmax = gsym("_ZN2nn5audio3dsp20CommandListProcessor17SetProcessTimeMaxEj"); setup = gsym("_ZN2nn5audio3dsp20CommandListProcessor5SetupEPKvm");
        process = gsym("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv");
    }
    if (mode(sys) != 1 || device(sys) != 1) return 0x40299;   // 同原函数：非 CPU 手动模式不能手动渲染 | same as the original: manual rendering needs CPU + manual mode
    pthread_mutex_lock(&g_render_mu);
    uint64_t t0 = now_ns();
    // 生成命令要与游戏线程的 RequestUpdate（AudioRenderSystem::Update，持 +0x1f0 的互斥锁）互斥：原版的系统线程（SendCommandToDsp）也是持同一把锁生成。处理（CPU 版 DSP）照原版在锁外。
    // Command generation must exclude the game thread's RequestUpdate (AudioRenderSystem::Update, which holds the mutex at +0x1f0): the original system thread (SendCommandToDsp) generates under the same lock. Processing (the CPU DSP) stays outside the lock, as in the original.
    uint8_t *cl = cbuf(sys);
    lock(sys + 0x1f0); size_t n = gen(sys, cl, cbsz(sys)); unlock(sys + 0x1f0);
    atomic_fetch_add(&g_renders, 1);
    uint64_t proc[16] __attribute__((aligned(16))) = {0};   // CommandListProcessor（0x58 字节）| CommandListProcessor (0x58 bytes)
    tmax(proc, 0xffffffffu); setup(proc, cl, n); process(proc);
    if (dst) *nout = capture(cl, dst);
    // 每帧渲染耗时（5 ms 一帧的预算）：每 2000 帧报均值 / 最大 | render time per frame (budget: 5 ms per frame): mean / max every 2000 frames
    static uint64_t sum, mx, cnt; uint64_t d = now_ns() - t0; sum += d; if (d > mx) mx = d;
    if (++cnt == 2000) { LOG("audren：渲染耗时 均 %.2f ms / 最大 %.2f ms（每帧 5 ms 预算，最近 2000 帧）；追帧累计 %llu 次", sum / 2000 / 1e6, mx / 1e6, (unsigned long long)atomic_load(&g_catchups)); sum = mx = cnt = 0; }
    pthread_mutex_unlock(&g_render_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio6server24AudioRendererManagerImpl17AudioRendererImpl29ExecuteAudioRendererRenderingEv", "手动渲染一帧：SDK 生成命令 + CPU 处理，宿主取 DeviceSink 输出",
          Result, h_ar_ImplExec, uint8_t *impl) { return render_once(impl + 0x60, g_frame, &g_frame_n); }   // AudioRendererImpl 里的 AudioRenderSystem 在 +0x60 | the AudioRenderSystem sits at +0x60 inside AudioRendererImpl

// 两次 RequestUpdate 之间至少渲一帧。效果的「待初始化」只在游戏标「新加」的那次更新里置上（如 LightLimiterInfo::Update：isNew → 参数状态 0），
// 下一次更新照游戏的值覆盖；中间没生成过命令，DSP 就永远收不到初始化，之后 ApplyLightLimiterEffect 读空指针。真机渲染器每 5 ms 自己出一帧、
// 游戏又等渲染器事件，正常是一更新一帧；但开局游戏连着更新两次（相隔 0.04 ms），真出声时声卡打开慢、第一帧来得晚，两次之间没帧。
// 这里遇到「上次更新后没渲过」就先在当前线程追渲一帧；输出进队列，渲染线程下一轮放它、不再另渲。
// At least one rendered frame between two RequestUpdates. An effect's "needs initialization" is set only by the update in which the game flags it as new (e.g. LightLimiterInfo::Update: isNew → parameter status 0);
// the next update overwrites it with the game's value; if no commands were generated in between, the DSP never receives the initialization and ApplyLightLimiterEffect later reads a null pointer. The real renderer produces a frame every 5 ms on its own
// and the game waits on the renderer event, so normally it is one update per frame; but at start-up the game updates twice in a row (0.04 ms apart), and with real audio output the sound card opens slowly and the first frame comes late, leaving no frame in between.
// Here, when nothing has been rendered since the last update, one catch-up frame is rendered on the current thread first; its output is queued and played by the render thread in place of its next render.
HLE_FORCE("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm", "渲染器更新：上次更新后没渲过就先追渲一帧（保证效果初始化），再交 SDK",
          Result, h_ars_Update, uint8_t *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn) {
    static Result (*orig)(void *, void *, size_t, void *, size_t, const void *, size_t); if (!orig) orig = gsym("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm");
    static uint64_t last_r = ~0ull;
    uint64_t u = atomic_fetch_add(&g_updates, 1) + 1, r = atomic_load(&g_renders);
    if (last_r != ~0ull && r == last_r) {
        uint64_t c = atomic_fetch_add(&g_catchups, 1) + 1;
        if (c <= 3) LOG("audren：第 %llu 次更新前没渲过帧，先追渲一帧（累计 %llu 次）", (unsigned long long)u, (unsigned long long)c);
        uint32_t t = atomic_load(&g_cq_tail);
        if (t - atomic_load(&g_cq_head) < CQ) { render_once(sys, g_cq[t % CQ], &g_cq_n[t % CQ]); atomic_store(&g_cq_tail, t + 1); }
        else render_once(sys, NULL, NULL);   // 队列满（不该发生）：照渲，输出丢弃 | queue full (should not happen): render anyway, output discarded
    }
    static int tr = -1; if (tr < 0) tr = getenv("DM_AUDREN_TRACE") != NULL;
    if (tr && u < 200) LOG("audren 更新 #%llu t=%.3f ms 距上次更新渲了 %llu 帧", (unsigned long long)u, now_ns() / 1e6, (unsigned long long)(atomic_load(&g_renders) - (last_r == ~0ull ? 0 : last_r)));
    Result res = orig(sys, o, on, p, pn, in, inn);
    last_r = atomic_load(&g_renders);
    return res;
}

// 输出设备：一个内置扬声器，立体声（音频设备服务仍走 IPC，宿主直接答）
// Output device: one built-in speaker, stereo (the audio device service still goes through IPC; the host answers directly)
enum { DEVNAME = 0x100 };
static void out_devname(char *out) { memset(out, 0, DEVNAME); strlcpy(out, "AudioBuiltInSpeakerOutput", DEVNAME); }
HLE_FORCE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", "输出设备列表：内置扬声器 1 个", int, h_ad_List, char *names, int n) { if (n <= 0 || !names) return 0; out_devname(names); return 1; }
HLE_FORCE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", "当前输出设备：内置扬声器", void, h_ad_Active, char *name) { out_devname(name); }
// SDK 12 的旧名（Engage）：同一服务、同样回答 | The SDK 12 names (Engage): the same service, the same answers
HLE_FORCE("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", "输出设备列表（SDK 12 名）：内置扬声器 1 个", int, h_ad_List12, char *names, int n) { return h_ad_List(names, n); }
HLE_FORCE("_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", "当前输出设备（SDK 12 名）：内置扬声器", void, h_ad_Active12, char *name) { out_devname(name); }
HLE_FORCE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", "输出声道数：2", int, h_ad_Channels, void) { return 2; }
HLE_FORCE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "设备音量；宿主忽略", void, h_ad_SetVol, const char *name, float v) { (void)name; (void)v; }
HLE_FORCE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", "输出设备变化通知：绑定、永不触发", void, h_ad_Notify, void *ev) { hle_bind_system_event(ev, 0); }
