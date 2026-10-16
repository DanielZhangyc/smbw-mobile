// nn::hid（手柄）：真机经 hid 服务读共享内存；宿主从前端写入的当前状态取（Mac 键盘 / 手柄，iOS 虚拟手柄 / 手柄）。
// 与游戏无关的通用版：一台标准手柄。DM_OPMODE=0（掌机）→ 掌机手柄（Npad 号 0x20）；否则 → 1P 全键手柄（号 0）。
// 振动按 Mac 真实设备能力桥接；六轴、鼠标、键盘不提供（设备数 0 / 未连接）；手柄小程序直接报「1 人、已选当前手柄」。
// nn::hid (controller): on the real device shared memory is read through the hid service; the host takes the current state written by the front end (Mac keyboard / controller, iOS virtual gamepad / controller).
// Generic, game-independent version: one standard controller. DM_OPMODE=0 (handheld) → handheld controller (Npad id 0x20); otherwise → 1P full-key controller (id 0).
// Vibration bridges actual Mac device capabilities; six-axis, mouse and keyboard are unavailable (device count 0 / not connected); the controller applet reports "1 player, current controller selected".
#include "hle.h"
#include "dm_input.h"
#include <stdatomic.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>

enum { NPAD_NO1 = 0, NPAD_HANDHELD = 0x20 };
enum { STYLE_FULLKEY = 1u << 0, STYLE_HANDHELD = 1u << 1, STYLE_JOYDUAL = 1u << 2, STYLE_JOYLEFT = 1u << 3, STYLE_JOYRIGHT = 1u << 4 };
enum { ATTR_CONNECTED = 1u << 0, ATTR_WIRED = 1u << 1, ATTR_LEFT_CONN = 1u << 2, ATTR_LEFT_WIRED = 1u << 3, ATTR_RIGHT_CONN = 1u << 4, ATTR_RIGHT_WIRED = 1u << 5 };
// 摇杆方向伪键（NpadButton 16..23）：由摇杆值导出，阈值约半程
// Stick direction pseudo-buttons (NpadButton 16..23): derived from the stick values, threshold about half travel
enum { STICK_THRESH = 0x4000 };

// nn::hid::NpadFullKeyState / Handheld / JoyDual / JoyLeft / JoyRight 布局相同（0x28）
// nn::hid::NpadFullKeyState / Handheld / JoyDual / JoyLeft / JoyRight share one layout (0x28)
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, _pad; } NpadState;
_Static_assert(sizeof(NpadState) == 0x28, "NpadState");

// 三路独立发布：键盘 / 触屏、实体手柄、脚本；同锁保证状态、按下沿和序列成组更新。
// Three independent publishers: keyboard / touch, physical pad, script; one lock keeps state, press edges and sequences coherent.
enum { INPUT_SOURCE, PAD_SOURCE, SCRIPT_SOURCE, SOURCE_COUNT };
typedef struct { uint64_t buttons, axes, latch, seq, reset_epoch; } input_source;
static input_source g_sources[SOURCE_COUNT];
static pthread_mutex_t g_input_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_publication_seq;
static _Atomic int64_t g_sampling;
// 可选追踪启动时预分配并触及全部页；热路径只追加内存，正常 exit 时才写 JSONL。
// Optional tracing allocates and touches every page at startup; the hot path only appends memory, and normal exit writes JSONL.
enum { INPUT_TRACE_CAPACITY = 65536 };
typedef struct {
    uint64_t event_seq, begin_ns, time_ns, frame, publication_seq, source_seq[SOURCE_COUNT], source_latch[SOURCE_COUNT], reset_epoch[SOURCE_COUNT], buttons;
    int64_t sampling;
    int32_t lx, ly, rx, ry;
    uint32_t source, id, style, attributes, reset;
} input_trace_record;
static input_trace_record *g_input_trace;
static FILE *g_input_trace_file;
static size_t g_input_trace_n;
static uint64_t g_input_trace_events, g_input_trace_dropped;
static int g_input_trace_on, g_input_trace_have_read;
static NpadState g_input_trace_last_read;
static uint32_t g_input_trace_last_id, g_input_trace_last_style;
static uint64_t g_input_trace_last_publication;

static void input_trace_append(unsigned source, uint64_t begin, const NpadState *state, uint32_t id, uint32_t style, const uint64_t *consumed, int reset) {
    if (!g_input_trace_on) return;
    uint64_t event = ++g_input_trace_events;
    if (g_input_trace_n == INPUT_TRACE_CAPACITY) { g_input_trace_dropped++; return; }
    input_trace_record *r = &g_input_trace[g_input_trace_n++];
    *r = (input_trace_record){.event_seq = event, .begin_ns = begin, .time_ns = now_ns(),
        .frame = dm_nvn_frame_count(), .publication_seq = g_publication_seq, .source = source,
        .sampling = state->sampling, .buttons = state->buttons, .lx = state->lx, .ly = state->ly,
        .rx = state->rx, .ry = state->ry, .id = id, .style = style, .attributes = state->attributes, .reset = reset};
    for (unsigned i = 0; i < SOURCE_COUNT; i++) {
        r->source_seq[i] = g_sources[i].seq;
        r->source_latch[i] = consumed ? consumed[i] : g_sources[i].latch;
        r->reset_epoch[i] = g_sources[i].reset_epoch;
    }
}

void dm_input_trace_flush(void) {
    pthread_mutex_lock(&g_input_mu);
    g_input_trace_on = 0;
    FILE *file = g_input_trace_file; g_input_trace_file = NULL;
    size_t n = g_input_trace_n; uint64_t dropped = g_input_trace_dropped, events = g_input_trace_events;
    pthread_mutex_unlock(&g_input_mu);
    if (!file) return;
    // source_seq 对应 input/pad/script；锁存可能来自较早的按下，须结合 source_latch 与发布历史，不能把松开序列当按下。
    // source_seq is ordered input/pad/script; latches may come from earlier presses, so use source_latch and publication history instead of treating a release sequence as a press.
    extern uint64_t g_t0_ns;
    fprintf(file, "{\"type\":\"meta\",\"schema\":1,\"clock\":\"host_now_ns\",\"frame_origin_ns\":%" PRIu64 ",\"capacity\":%u,\"source_order\":[\"input\",\"pad\",\"script\"],\"axis_tie_priority\":[\"input\",\"pad\",\"script\"],\"read_endpoint\":\"host_state_fill_complete\",\"render_frame_relation\":\"pipeline_estimate_only\",\"hardware_radio_latency_measured\":false}\n", g_t0_ns, (unsigned)INPUT_TRACE_CAPACITY);
    static const char *sources[] = {"input", "pad", "script", "guest"};
    for (size_t i = 0; i < n; i++) {
        const input_trace_record *r = &g_input_trace[i];
        fprintf(file, "{\"type\":\"%s\",\"source\":\"%s\",\"event_seq\":%" PRIu64 ",\"begin_ns\":%" PRIu64 ",\"time_ns\":%" PRIu64 ",\"nvn_frame\":%" PRIu64 ",\"publication_seq\":%" PRIu64 ",\"source_seq\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],\"source_latch\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],\"reset_epoch\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],\"sampling\":%" PRId64 ",\"id\":%u,\"style\":%u,\"buttons\":%" PRIu64 ",\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"attributes\":%u}\n",
            r->reset ? "reset" : r->source < SOURCE_COUNT ? "publish" : "read", sources[r->source], r->event_seq, r->begin_ns, r->time_ns,
            r->frame, r->publication_seq, r->source_seq[0], r->source_seq[1], r->source_seq[2],
            r->source_latch[0], r->source_latch[1], r->source_latch[2], r->reset_epoch[0], r->reset_epoch[1], r->reset_epoch[2], r->sampling,
            r->id, r->style, r->buttons, r->lx, r->ly, r->rx, r->ry, r->attributes);
    }
    fprintf(file, "{\"type\":\"summary\",\"records\":%zu,\"events\":%" PRIu64 ",\"dropped\":%" PRIu64 ",\"normal_exit\":true}\n", n, events, dropped);
    int failed = ferror(file); if (fclose(file)) failed = 1;
    if (failed) LOG("【红】DM_INPUT_TRACE 写出失败，追踪结果不可视为完整");
    else LOG("DM_INPUT_TRACE：写出 %zu 条，缓冲已满丢弃 %" PRIu64 " 条", n, dropped);
}

__attribute__((constructor)) static void input_trace_init(void) {
    const char *path = getenv("DM_INPUT_TRACE"); if (!path || !*path) return;
    g_input_trace = malloc(sizeof(*g_input_trace) * INPUT_TRACE_CAPACITY);
    if (!g_input_trace) die("DM_INPUT_TRACE 内存分配失败");
    // volatile 逐页触及，避免把首次缺页成本留到输入事件中。
    // Touch through volatile storage so first-touch page faults do not occur in input events.
    volatile unsigned char *bytes = (volatile unsigned char *)g_input_trace;
    size_t size = sizeof(*g_input_trace) * INPUT_TRACE_CAPACITY;
    for (size_t i = 0; i < size; i += (size_t)getpagesize()) bytes[i] = 0;
    bytes[size - 1] = 0;
    g_input_trace_file = fopen(path, "w"); if (!g_input_trace_file) die("DM_INPUT_TRACE 无法打开 %s", path);
    if (atexit(dm_input_trace_flush)) die("DM_INPUT_TRACE 无法登记退出写出");
    g_input_trace_on = 1;
}

static uint64_t pack(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    return (uint64_t)(uint16_t)lx | (uint64_t)(uint16_t)ly << 16 | (uint64_t)(uint16_t)rx << 32 | (uint64_t)(uint16_t)ry << 48;
}
static void input_publish(unsigned source, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t begin = g_input_trace ? now_ns() : 0;
    pthread_mutex_lock(&g_input_mu);
    input_source *s = &g_sources[source];
    s->latch |= buttons & ~s->buttons; s->buttons = buttons; s->axes = pack(lx, ly, rx, ry);
    s->seq = ++g_publication_seq;
    NpadState state = {.buttons = buttons, .lx = (int16_t)lx, .ly = (int16_t)ly, .rx = (int16_t)rx, .ry = (int16_t)ry};
    input_trace_append(source, begin, &state, 0, 0, NULL, 0);
    pthread_mutex_unlock(&g_input_mu);
}
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { input_publish(INPUT_SOURCE, buttons, lx, ly, rx, ry); }
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    input_publish(PAD_SOURCE, buttons, lx, ly, rx, ry);
}
void dm_script_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { input_publish(SCRIPT_SOURCE, buttons, lx, ly, rx, ry); }
static void input_reset(unsigned source) {
    uint64_t begin = g_input_trace ? now_ns() : 0;
    pthread_mutex_lock(&g_input_mu);
    input_source *s = &g_sources[source];
    s->buttons = s->axes = s->latch = 0; s->seq = ++g_publication_seq; s->reset_epoch++;
    NpadState state = {0}; input_trace_append(source, begin, &state, 0, 0, NULL, 1);
    pthread_mutex_unlock(&g_input_mu);
}
// 失焦 / 断线丢弃待消费按下；普通 set(0) 仍保留两次读取之间发生的短按。
// Focus loss / disconnect discard pending presses; ordinary set(0) still preserves taps between two reads.
void dm_input_reset(void) { input_reset(INPUT_SOURCE); }
void dm_pad_reset(void) { input_reset(PAD_SOURCE); }
uint64_t dm_input_samples(void) { return (uint64_t)atomic_load(&g_sampling); }
// DM_KEYS 脚本输入（测试用）："秒:键[+键][~按住秒],…"，例 "8:A,12:A,15:Down~0.5,16:A+B"。键名 A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down；
//   缺省按住 0.15 s。走独立脚本源（dm_script_set），实体手柄断连清零不覆盖脚本。时间从脚本线程启动算起
// DM_KEYS scripted input (for tests): "seconds:key[+key][~hold seconds],...", e.g. "8:A,12:A,15:Down~0.5,16:A+B". Key names A B X Y LS RS L R ZL ZR Plus Minus Left Up Right Down;
//   held 0.15 s by default. Uses the independent script source (dm_script_set), unaffected by physical-pad disconnect resets. Times are counted from script-thread start
// LSLeft / LSRight / LSUp / LSDown 是左摇杆满幅轴值，可与按钮组合；同轴相反方向同时出现时归零。
// LSLeft / LSRight / LSUp / LSDown are full-travel left-stick axes and combine with buttons; opposite directions on the same axis cancel.
typedef struct { double at, hold; uint64_t btn; int32_t lx, ly; } key_step;
static key_step g_steps[64]; static int g_nsteps;
static pthread_mutex_t g_script_run_mu = PTHREAD_MUTEX_INITIALIZER;
static double keys_seconds(const char *text) {
    char *end; errno = 0; double value = strtod(text, &end);
    if (end == text || *end || errno || !isfinite(value) || value < 0) die("DM_KEYS 非法秒数：%s", text);
    return value;
}
static int keys_parse(const char *e, key_step steps[64]) {
    int count = 0;
    static const char *names[20] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down", "LSLeft", "LSRight", "LSUp", "LSDown"};
    char buf[1024]; if (strlen(e) >= sizeof buf) die("DM_KEYS 文本超过 1023 字节");
    snprintf(buf, sizeof buf, "%s", e);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (count == 64) die("DM_KEYS 步骤超过 64 条");
        char *c = strchr(tok, ':'); if (!c) die("DM_KEYS 步骤缺「秒:」：%s", tok);
        *c = 0; key_step st = {.at = keys_seconds(tok), .hold = 0.15}; unsigned directions = 0;
        char *h = strchr(c + 1, '~'); if (h) { *h = 0; st.hold = keys_seconds(h + 1); }
        for (char *k = c + 1; k && *k;) { char *plus = strchr(k, '+'); if (plus) *plus = 0;
            int found = 0; for (int b = 0; b < 20; b++) if (!strcasecmp(k, names[b])) { if (b < 16) st.btn |= 1ull << b; else directions |= 1u << (b - 16); found = 1; }
            if (!found) die("DM_KEYS 不认识的键名：%s", k);
            k = plus ? plus + 1 : NULL; }
        st.lx = 32767 * (!!(directions & 2) - !!(directions & 1));
        st.ly = 32767 * (!!(directions & 4) - !!(directions & 8));
        steps[count++] = st;
    }
    return count;
}
static void keys_execute(const key_step *steps, int count) {
    // 固定脚本与 FIFO 行按整组串行；相对时间从该组取得执行权时起算。
    // Serialize the fixed script and FIFO lines as complete groups; relative time starts when each group begins execution.
    pthread_mutex_lock(&g_script_run_mu);
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < count; i++) {
        for (;;) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9; if (el >= steps[i].at) break; usleep(10000); }
        LOG("DM_KEYS：%.2f s 按 %#llx 左摇杆 %d,%d（按住 %.2f s）", steps[i].at, (unsigned long long)steps[i].btn, steps[i].lx, steps[i].ly, steps[i].hold);
        dm_script_set(steps[i].btn, steps[i].lx, steps[i].ly, 0, 0);
        // 单独等待真实按住区间，避免 useconds_t 转换溢出；被信号打断后继续剩余时长。
        // Wait the actual hold interval separately, avoiding useconds_t overflow and resuming after interrupted sleeps.
        struct timespec hold_start; clock_gettime(CLOCK_MONOTONIC, &hold_start);
        for (;;) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double el = (t.tv_sec - hold_start.tv_sec) + (t.tv_nsec - hold_start.tv_nsec) / 1e9;
            double left = steps[i].hold - el; if (left <= 0) break;
            struct timespec delay = {0, (long)(fmin(left, 0.01) * 1e9)};
            while (nanosleep(&delay, &delay)) if (errno != EINTR) die("DM_KEYS nanosleep 失败"); }
        dm_script_set(0, 0, 0, 0, 0);
    }
    dm_script_set(0, 0, 0, 0, 0);
    pthread_mutex_unlock(&g_script_run_mu);
}
static void *keys_main(void *arg) { (void)arg; keys_execute(g_steps, g_nsteps); return NULL; }
static void *keys_fifo_main(void *arg) {
    const char *path = arg;
    for (;;) {
        int fd; do { fd = open(path, O_RDONLY | O_NOFOLLOW); } while (fd < 0 && errno == EINTR);
        if (fd < 0) die("DM_KEYS_FIFO 打开失败 %s", path);
        struct stat st; if (fstat(fd, &st) || !S_ISFIFO(st.st_mode)) die("DM_KEYS_FIFO 打开后不是 FIFO：%s", path);
        FILE *file = fdopen(fd, "r"); if (!file) die("DM_KEYS_FIFO fdopen 失败");
        char line[1024];
        for (;;) {
            errno = 0;
            if (!fgets(line, sizeof line, file)) {
                if (ferror(file)) { if (errno == EINTR) { clearerr(file); continue; } die("DM_KEYS_FIFO 读取失败"); }
                break;
            }
            size_t len = strlen(line);
            if (len == sizeof(line) - 1 && line[len - 1] != '\n') die("DM_KEYS_FIFO 行超过 1022 字节");
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            key_step steps[64]; int count = keys_parse(line, steps); keys_execute(steps, count);
        }
        if (fclose(file)) die("DM_KEYS_FIFO 关闭失败");
    }
    return NULL;
}
void hid_keys_init(void) {
    const char *fifo = getenv("DM_KEYS_FIFO");
    if (fifo && *fifo) {
        struct stat st;
        if (*fifo != '/' || lstat(fifo, &st) || !S_ISFIFO(st.st_mode)) die("DM_KEYS_FIFO 必须是已存在 named FIFO 的绝对路径：%s", fifo);
    }
    const char *e = getenv("DM_KEYS");
    if (e && *e) g_nsteps = keys_parse(e, g_steps);
    pthread_t th;
    if (g_nsteps) { int error = pthread_create(&th, NULL, keys_main, NULL); if (error) die("DM_KEYS pthread_create：%d", error); pthread_detach(th); LOG("DM_KEYS：%d 步", g_nsteps); }
    if (fifo && *fifo) {
        char *path = strdup(fifo); if (!path) die("DM_KEYS_FIFO 路径分配失败");
        int error = pthread_create(&th, NULL, keys_fifo_main, path); if (error) die("DM_KEYS_FIFO pthread_create：%d", error);
        pthread_detach(th); LOG("DM_KEYS_FIFO：监听 %s（模拟脚本输入，不是无线硬件）", path);
    }
}

static uint32_t connected_id(void) { return dm_opmode() == 0 ? NPAD_HANDHELD : NPAD_NO1; }
uint32_t hle_hid_connected_npad(void) { return connected_id(); }   // nfp 的 NFC 设备挂在这个手柄上（hle_sdk16.c） | nfp's NFC device sits on this controller (hle_sdk16.c)
static uint32_t style_of(uint32_t id) { return id != connected_id() ? 0 : id == NPAD_HANDHELD ? STYLE_HANDHELD : STYLE_FULLKEY; }
static int32_t ax(uint64_t v, int i) { return (int16_t)(v >> (16 * i)); }
static int32_t pick(int32_t a, int32_t b) { return (a < 0 ? -a : a) >= (b < 0 ? -b : b) ? a : b; }   // 两路里偏得多的那个 | whichever of the two paths is deflected more

static void fill(NpadState *s, uint32_t id, uint32_t style) {
    uint64_t begin = g_input_trace ? now_ns() : 0;
    memset(s, 0, sizeof *s);
    s->sampling = atomic_fetch_add(&g_sampling, 1) + 1;
    pthread_mutex_lock(&g_input_mu);
    uint64_t consumed[SOURCE_COUNT] = {0};
    if (!(style_of(id) & style)) goto trace_read;   // 未连接的号 / 样式不消费任何锁存 | Unconnected ids / styles consume no latches
    uint64_t b = 0;
    // 各轴选绝对值最大者；相等时保留原 input 优先于 pad，再优先于新增 script。
    // Choose the greatest absolute deflection per axis; ties preserve input over pad, then the new script source.
    for (unsigned i = 0; i < SOURCE_COUNT; i++) {
        input_source *source = &g_sources[i];
        consumed[i] = source->latch; b |= source->buttons | source->latch; source->latch = 0;
        s->lx = pick(s->lx, ax(source->axes, 0)); s->ly = pick(s->ly, ax(source->axes, 1));
        s->rx = pick(s->rx, ax(source->axes, 2)); s->ry = pick(s->ry, ax(source->axes, 3));
    }
    b &= 0xffffull;
    if (s->lx < -STICK_THRESH) b |= 1u << 16;
    if (s->ly > STICK_THRESH) b |= 1u << 17;
    if (s->lx > STICK_THRESH) b |= 1u << 18;
    if (s->ly < -STICK_THRESH) b |= 1u << 19;
    if (s->rx < -STICK_THRESH) b |= 1u << 20;
    if (s->ry > STICK_THRESH) b |= 1u << 21;
    if (s->rx > STICK_THRESH) b |= 1u << 22;
    if (s->ry < -STICK_THRESH) b |= 1u << 23;
    s->buttons = b;
    s->attributes = ATTR_CONNECTED | (id == NPAD_HANDHELD ? ATTR_WIRED | ATTR_LEFT_CONN | ATTR_LEFT_WIRED | ATTR_RIGHT_CONN | ATTR_RIGHT_WIRED : ATTR_LEFT_CONN | ATTR_RIGHT_CONN);
trace_read:
    // 仅状态、发布序列或查询上下文变化时记录；采样号 / 帧号递增本身不产生逐帧日志。
    // Record only state, publication-sequence or query-context changes; increasing sampling / frame numbers alone do not create per-frame logs.
    if (g_input_trace_on && (!g_input_trace_have_read || g_input_trace_last_publication != g_publication_seq ||
        g_input_trace_last_id != id || g_input_trace_last_style != style ||
        memcmp((const char *)&g_input_trace_last_read + sizeof(s->sampling), (const char *)s + sizeof(s->sampling), sizeof(*s) - sizeof(s->sampling)))) {
        input_trace_append(SOURCE_COUNT, begin, s, id, style, consumed, 0);
        g_input_trace_have_read = 1; g_input_trace_last_read = *s;
        g_input_trace_last_publication = g_publication_seq; g_input_trace_last_id = id; g_input_trace_last_style = style;
    }
    pthread_mutex_unlock(&g_input_mu);
}
// GetNpadStates(out, n, id)：返回写入条数，新的在前。宿主不保留历史：只给 1 条当前状态（调用方按返回数处理）
// GetNpadStates(out, n, id): returns the number written, newest first. The host keeps no history: only 1 current state (callers handle the returned count)
static int states(NpadState *out, int n, uint32_t id, uint32_t style) { if (n <= 0) return 0; fill(out, id, style); return 1; }

HLE_FORCE("_ZN2nn3hid14InitializeNpadEv", "连 hid 服务；宿主从前端状态取输入", void, h_hid_InitNpad, void) {}
HLE_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "支持的手柄号；宿主固定", void, h_hid_SetIds, const uint32_t *ids, uint64_t n) { (void)ids; (void)n; }
static uint32_t g_styles_supported = 0xffffffffu;
HLE_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "支持的手柄样式；宿主只记下", void, h_hid_SetStyles, uint32_t styles) { g_styles_supported = styles; }
HLE_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "手柄样式查询；跟 DM_OPMODE 走", uint32_t, h_hid_GetStyle, const uint32_t *id) { return style_of(*id); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", "全键手柄状态", int, h_hid_StatesFullKey, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", "掌机手柄状态", int, h_hid_StatesHandheld, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_HANDHELD); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", "双 Joy-Con 状态（宿主不接）", int, h_hid_StatesJoyDual, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYDUAL); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", "左 Joy-Con 状态（宿主不接）", int, h_hid_StatesJoyLeft, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYLEFT); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", "右 Joy-Con 状态（宿主不接）", int, h_hid_StatesJoyRight, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYRIGHT); }
HLE_FORCE("_ZN2nn3hid14DisconnectNpadERKj", "断开手柄；宿主忽略", void, h_hid_Disconnect, const uint32_t *id) { (void)id; }
static uint32_t g_hold_type;
HLE_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "Joy-Con 握持方向；宿主只记下", void, h_hid_HoldType, int t) { g_hold_type = (uint32_t)t; }
HLE_FORCE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", "Joy-Con 分配：双持", int, h_hid_GetJoyAssign, const uint32_t *id) { (void)id; return 0; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", "Joy-Con 单持；宿主忽略", void, h_hid_JoySingle, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", "交换手柄号；宿主忽略", void, h_hid_Swap, const uint32_t *a, const uint32_t *b) { (void)a; (void)b; }
HLE_FORCE("_ZN2nn3hid21StartLrAssignmentModeEv", "L/R 分配模式；宿主忽略", void, h_hid_LrStart, void) {}
HLE_FORCE("_ZN2nn3hid20StopLrAssignmentModeEv", "L/R 分配模式；宿主忽略", void, h_hid_LrStop, void) {}
// 手柄小程序（系统 UI）：直接报「1 人、选中当前手柄」。ControllerSupportResultInfo = { int playerCount; uint32_t selectedId; }
// Controller applet (system UI): report "1 player, current controller selected" directly. ControllerSupportResultInfo = { int playerCount; uint32_t selectedId; }
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", "手柄小程序；宿主直接报已选好",
          Result, h_hid_ShowCtrl, int32_t *info, const void *arg) { (void)arg; if (info) { info[0] = 1; info[1] = (int32_t)connected_id(); } return RES_OK; }
// 六轴：给出句柄，但读不到状态（0 条）
// Six-axis: handles are handed out, but no states can be read (0 entries)
HLE_FORCE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "六轴句柄",
          int, h_hid_SixHandles, uint32_t *h, int n, const uint32_t *id, uint32_t style) { (void)style; int k = n < 2 ? n : 2; for (int i = 0; i < k; i++) h[i] = (*id << 8) | (uint32_t)i; return k; }
HLE_FORCE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "六轴；宿主不提供", void, h_hid_SixStart, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", "六轴状态；宿主不提供（0 条）",
          int, h_hid_SixStates, void *out, int n, const uint32_t *h) { (void)out; (void)n; (void)h; return 0; }
// 振动 ABI：handle 四字节为 style_index/player/device_idx/pad；值为 lowAmp/lowFreq/highAmp/highFreq 四 float。
// Vibration ABI: handle bytes are style_index/player/device_idx/pad; values are four floats lowAmp/lowFreq/highAmp/highFreq.
// 依据 libnx hid.h/hid.c 的公开布局；Mac 强实现只在真实 GameController 能力可用时枚举，其他前端明确无设备。
// Uses the public libnx hid.h/hid.c layout; the strong Mac implementation enumerates only real available GameController capabilities, while other front ends explicitly have no devices.
_Static_assert(sizeof(dm_vibration_value)==16, "VibrationValue ABI");
#include <TargetConditionals.h>
#if TARGET_OS_OSX
// Mac 必须链接真实后端，缺失时由链接器报错；不以无设备实现掩盖构建遗漏。
// Mac must link the actual backend: let missing symbols fail at link time instead of hiding a missing source as an absent device.
unsigned dm_haptic_device_capabilities(void);
#else
// 尚未接入触觉的其他前端显式不支持，不引入 Mac Objective-C 源或未定义符号。
// Other front ends without a haptics bridge explicitly report unsupported, with no Mac Objective-C sources or undefined symbols.
unsigned dm_haptic_capabilities(void) { return 0; }
int dm_haptic_shutdown(const char *reason, unsigned timeout_ms) { (void)reason; (void)timeout_ms; return 1; }
// 硬件声明先于异步引擎就绪：枚举 / 初始化据真实声明，提交结果仍据已就绪引擎。
// Hardware declarations precede async engine readiness: enumerate / initialize real declarations, while applied state still requires ready engines.
unsigned dm_haptic_device_capabilities(void) { return 0; }
int dm_haptic_initialize(unsigned side) { (void)side; return 0; }
void dm_haptic_send(unsigned side, const dm_vibration_value *value) { (void)side; (void)value; }
void dm_haptic_actual(unsigned side, dm_vibration_value *out) { (void)side; memset(out,0,sizeof *out); }
#endif
static int vibration_side(const void *handle) {
    if (!handle) return -1;
    uint32_t h; memcpy(&h,handle,sizeof h);
    uint32_t id=(h>>8)&255, style_index=h&255, side=(h>>16)&255;
    if ((h>>24) || id!=connected_id() || style_index!=(id==NPAD_HANDHELD?4u:3u) || side>1) return -1;
    unsigned caps=dm_haptic_device_capabilities();
    return (side ? caps&DM_HAPTIC_RIGHT : caps&(DM_HAPTIC_LEFT|DM_HAPTIC_WHOLE)) ? (int)side : -1;
}
HLE_FORCE("_ZN2nn3hid20IsVibrationPermittedEv", "震动许可；按当前真实设备能力", unsigned, h_hid_VibPermit, void) { return dm_haptic_device_capabilities()!=0; }
HLE_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "枚举实际触觉通道",
          int, h_hid_VibHandles, uint32_t *handles, int n, const uint32_t *id, uint32_t style) {
    if (n<=0 || !handles || !(style_of(*id)&style)) return 0;
    unsigned caps=dm_haptic_device_capabilities(); int count=0;
    uint32_t base=(*id<<8)|(*id==NPAD_HANDHELD?4u:3u);
    if (caps&(DM_HAPTIC_LEFT|DM_HAPTIC_WHOLE)) handles[count++]=base;
    if (count<n && (caps&DM_HAPTIC_RIGHT)) handles[count++]=base|(1u<<16);
    return count;
}
HLE_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "接受真实触觉通道异步初始化", void, h_hid_InitVib, const void *h) {
    int side=vibration_side(h); if (side>=0 && !dm_haptic_initialize((unsigned)side)) LOG("Mac 触觉：初始化时设备已不可用");
}
HLE_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "异步发送触觉值", void, h_hid_SendVib, const void *h, const void *v) {
    int side=vibration_side(h); if(side>=0 && v) { dm_vibration_value value; memcpy(&value,v,sizeof value); dm_haptic_send((unsigned)side,&value); }
}
// SDK VibrationNode 的原生 mixer / enqueue / flush 保留执行；flush 经 PLT 调用此批量服务入口，不经过公开单值接口。
// Keep the SDK's native VibrationNode mixer / enqueue / flush; flush calls this batch service entry through the PLT, bypassing the public single-value interface.
// 真实 SDK 0x49bd50 的 ABI 为 handles(x0), values(x1), count(w2)，返回 Result(w0)。仅替代其 HID IPC，逐项接现有真实后端。
// The actual SDK entry at 0x49bd50 takes handles(x0), values(x1), count(w2), returning Result(w0). Replace only its HID IPC, routing every entry to the existing real backend.
HLE_FORCE("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", "SDK 振动批量服务接真实触觉后端",
          Result, h_hid_SendVibBatch, const uint32_t *handles, const dm_vibration_value *values, int count) {
    if (count<0 || (count>0 && (!handles || !values))) return RES_FAIL;
    for (int i=0;i<count;i++) h_hid_SendVib(&handles[i],&values[i]);
    return RES_OK;
}
// 鼠标 / 键盘：未连接
// Mouse / keyboard: not connected
HLE_FORCE("_ZN2nn3hid15InitializeMouseEv", "鼠标；宿主不提供", void, h_hid_InitMouse, void) {}
HLE_FORCE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", "鼠标状态：0 条", int, h_hid_MouseStates, void *out, int n) { (void)out; (void)n; return 0; }
HLE_FORCE("_ZN2nn3hid18InitializeKeyboardEv", "键盘；宿主不提供", void, h_hid_InitKbd, void) {}
HLE_FORCE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", "键盘状态：未连接（KeyboardState 0x30 字节清零）", void, h_hid_KbdState, void *s) {
    memset(s, 0, 0x30); ((int64_t *)s)[0] = atomic_fetch_add(&g_sampling, 1) + 1;
}

// ---------- SDK 12 的接口（Engage）：单条 GetNpadState、模式查询 / 设置、触屏、手势、六轴细项、手柄颜色等 ----------
// 原版这些都读 hid 共享内存或走 hid 服务；宿主没有共享内存，按上面同一份前端状态作答。只改游戏自己缓冲的库函数（震动文件解析 / 生成、ControllerSupportArg 的 SetDefault / SetExplainText）留在 SDK 原样跑
// ---------- SDK 12 interfaces (Engage): single GetNpadState, mode queries / setters, touch screen, gestures, six-axis details, controller colors, etc. ----------
// In the original all of these read hid shared memory or go through the hid service; the host has no shared memory and answers from the same front-end state as above. Library functions that only modify the game's own buffers (vibration file parse / generate, ControllerSupportArg SetDefault / SetExplainText) keep running in the SDK
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", "全键手柄当前状态", void, h_hid_StateFullKey, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", "掌机手柄当前状态", void, h_hid_StateHandheld, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_HANDHELD); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", "双 Joy-Con 当前状态（宿主不接）", void, h_hid_StateJoyDual, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_JOYDUAL); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", "左 Joy-Con 当前状态（宿主不接）", void, h_hid_StateJoyLeft, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_JOYLEFT); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", "右 Joy-Con 当前状态（宿主不接）", void, h_hid_StateJoyRight, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_JOYRIGHT); }

static uint32_t g_comm_mode, g_hh_mode;
HLE_FORCE("_ZN2nn3hid18GetNpadJoyHoldTypeEv", "Joy-Con 握持方向：回报设过的值", int, h_hid_GetHold, void) { return (int)g_hold_type; }
HLE_FORCE("_ZN2nn3hid24GetSupportedNpadStyleSetEv", "支持的手柄样式：全部", uint32_t, h_hid_GetStyles, void) { return g_styles_supported; }
HLE_FORCE("_ZN2nn3hid24GetNpadCommunicationModeEv", "无线通信模式：回报设过的值", int, h_hid_GetComm, void) { return (int)g_comm_mode; }
HLE_FORCE("_ZN2nn3hid24SetNpadCommunicationModeENS0_21NpadCommunicationModeE", "无线通信模式；宿主只记下", void, h_hid_SetComm, int m) { g_comm_mode = (uint32_t)m; }
HLE_FORCE("_ZN2nn3hid29GetNpadHandheldActivationModeEv", "掌机激活模式：回报设过的值", int, h_hid_GetHh, void) { return (int)g_hh_mode; }
HLE_FORCE("_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", "掌机激活模式；宿主只记下", void, h_hid_SetHh, int m) { g_hh_mode = (uint32_t)m; }
HLE_FORCE("_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", "Joy-Con 双持；宿主忽略", void, h_hid_JoyDual, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE", "Joy-Con 单持（指定左右）；宿主忽略", void, h_hid_JoySingleT, const uint32_t *id, int t) { (void)id; (void)t; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleEPjRKjNS0_17NpadJoyDeviceTypeE", "Joy-Con 单持（取回另一半）；宿主忽略，没有另一半", int, h_hid_JoySingleOut, uint32_t *out, const uint32_t *id, int t) { (void)out; (void)id; (void)t; return 0; }
HLE_FORCE("_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_", "合并单 Joy-Con；宿主忽略，报成功", Result, h_hid_Merge, const uint32_t *a, const uint32_t *b) { (void)a; (void)b; return RES_OK; }
HLE_FORCE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", "手柄样式变化事件：绑定、永不触发", void, h_hid_BindStyleEv,
          const uint32_t *id, void *ev, int clear) { (void)id; hle_bind_system_event(ev, clear); }
HLE_FORCE("_ZN2nn3hid19GetPlayerLedPatternERKj", "玩家灯：1 号灯", uint8_t, h_hid_Led, const uint32_t *id) { (void)id; return 1; }
// 手柄颜色 NpadControllerColor = { 主色 RGBA, 按键色 RGBA }：给灰底黑键 | NpadControllerColor = { main RGBA, button RGBA }: grey body, black buttons
static void ctrl_color(uint32_t *c) { c[0] = 0xff828282u; c[1] = 0xff0f0f0fu; }
HLE_FORCE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj", "手柄颜色", Result, h_hid_Color, uint32_t *c, const uint32_t *id) { (void)id; ctrl_color(c); return RES_OK; }
HLE_FORCE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", "左右 Joy-Con 颜色", Result, h_hid_Color2, uint32_t *l, uint32_t *r, const uint32_t *id) { (void)id; ctrl_color(l); ctrl_color(r); return RES_OK; }
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", "手柄小程序（不要结果）；宿主直接报已选好", Result, h_hid_ShowCtrl0, const void *arg) { (void)arg; return RES_OK; }
HLE_FORCE("_ZN2nn3hid24ShowControllerStrapGuideEv", "腕带说明小程序；宿主不显示", Result, h_hid_Strap, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3hid28ShowControllerFirmwareUpdateERKNS0_27ControllerFirmwareUpdateArgE", "手柄固件更新小程序；宿主不显示", Result, h_hid_FwUpd, const void *arg) { (void)arg; return RES_OK; }

// 触屏：TouchScreenState<N> = { int64 采样号; int32 点数; int32 保留; TouchState[N] }。先报 0 个触点（只写头 16 字节）
// Touch screen: TouchScreenState<N> = { int64 sampling number; int32 count; int32 reserved; TouchState[N] }. Report 0 touch points for now (only the 16-byte header is written)
static void touch_head(void *s) { int64_t *p = s; p[0] = atomic_fetch_add(&g_sampling, 1) + 1; ((int32_t *)s)[2] = 0; ((int32_t *)s)[3] = 0; }
HLE_FORCE("_ZN2nn3hid21InitializeTouchScreenEv", "连 hid 触屏；宿主暂报无触点", void, h_hid_InitTouch, void) {}
#define TOUCH(N) \
    HLE_FORCE("_ZN2nn3hid19GetTouchScreenStateILm" #N "EEEvPNS0_16TouchScreenStateIXT_EEE", "触屏当前状态：0 个触点", void, h_hid_Touch1_##N, void *s) { touch_head(s); } \
    HLE_FORCE("_ZN2nn3hid20GetTouchScreenStatesILm" #N "EEEiPNS0_16TouchScreenStateIXT_EEEi", "触屏状态：1 条、0 个触点", int, h_hid_TouchN_##N, void *s, int n) { if (n <= 0) return 0; touch_head(s); return 1; }
TOUCH(1) TOUCH(2) TOUCH(3) TOUCH(4) TOUCH(5) TOUCH(6) TOUCH(7) TOUCH(8) TOUCH(9) TOUCH(10) TOUCH(11) TOUCH(12) TOUCH(13) TOUCH(14) TOUCH(15) TOUCH(16)
HLE_FORCE("_ZN2nn3hid17InitializeGestureEv", "手势；宿主不提供", void, h_hid_InitGesture, void) {}
HLE_FORCE("_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi", "手势状态：0 条", int, h_hid_Gestures, void *out, int n) { (void)out; (void)n; return 0; }

// 六轴细项：静止、未开融合；SixAxisSensorState 0x60 字节清零（attributes = 0：未连接） | Six-axis details: at rest, fusion off; SixAxisSensorState 0x60 bytes zeroed (attributes = 0: not connected)
HLE_FORCE("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", "六轴；宿主不提供", void, h_hid_SixStop, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", "六轴当前状态：未连接", void, h_hid_SixState, void *s, const uint32_t *h) { (void)h; memset(s, 0, 0x60); }
HLE_FORCE("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", "六轴静止：是", unsigned, h_hid_SixRest, const uint32_t *h) { (void)h; return 1; }
HLE_FORCE("_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", "六轴融合；宿主忽略", void, h_hid_SixFusionOn, const uint32_t *h, uint32_t on) { (void)h; (void)on; }
HLE_FORCE("_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE", "六轴融合：未开", unsigned, h_hid_SixFusionQ, const uint32_t *h) { (void)h; return 0; }
HLE_FORCE("_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", "陀螺零漂模式：标准", int, h_hid_GyroGet, const uint32_t *h) { (void)h; return 1; }
HLE_FORCE("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", "陀螺零漂模式；宿主忽略", void, h_hid_GyroSet, const uint32_t *h, const int *m) { (void)h; (void)m; }
HLE_FORCE("_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE", "六轴融合参数：0", void, h_hid_FusionGet, float *a, float *b, const uint32_t *h) { (void)h; *a = 0; *b = 0; }
HLE_FORCE("_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", "六轴融合参数；宿主忽略", void, h_hid_FusionSet, const uint32_t *h, float a, float b) { (void)h; (void)a; (void)b; }
HLE_FORCE("_ZN2nn3hid34ResetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleE", "六轴融合参数；宿主忽略", void, h_hid_FusionReset, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid41IsFirmwareUpdateAvailableForSixAxisSensorERKNS0_19SixAxisSensorHandleE", "六轴固件更新：无", unsigned, h_hid_SixFw, const uint32_t *h) { (void)h; return 0; }
// type=1 是客体 LRA 振动接口类型，不推断实体电机结构；position=0 表示真实 default 聚合通道。
// type=1 identifies the guest LRA vibration interface, without inferring physical motor construction; position=0 denotes the real default aggregate channel.
HLE_FORCE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", "触觉通道信息", void, h_hid_VibInfo, uint32_t *info, const void *h) {
    int side=vibration_side(h); info[0]=side>=0?1:0; info[1]=side<0?0:(dm_haptic_device_capabilities()&DM_HAPTIC_WHOLE)?0:(uint32_t)side+1;
}
// 返回后台成功提交的软件值；CoreHaptics 无电机遥测，不能把此值当物理验收。
// Return the worker's successfully submitted software value; CoreHaptics has no motor telemetry, so this is not physical validation.
HLE_FORCE("_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", "已提交触觉状态", void, h_hid_VibActual, dm_vibration_value *v, const void *h) {
    int side=vibration_side(h); memset(v,0,sizeof *v); if(side>=0) dm_haptic_actual((unsigned)side,v);
}
