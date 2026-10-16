// 前端（Mac 键盘 / iOS 触屏）→ nn::hid 的输入状态。按钮位与 nn::hid::NpadButton 一致：
// A0 B1 X2 Y3 StickL4 StickR5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15；摇杆 ±32767。
// Front end (Mac keyboard / iOS touchscreen) -> nn::hid input state. Button bits match nn::hid::NpadButton:
// A0 B1 X2 Y3 StickL4 StickR5 L6 R7 ZL8 ZR9 Plus10 Minus11 Left12 Up13 Right14 Down15; sticks ±32767.
#pragma once
#include <stdint.h>
// 按钮位（= nn::hid::NpadButton 位序；hle_hid.c 的 DM_KEYS 键名表由这里生成）。前端一律用这些名字组 buttons。
// Button bits (= nn::hid::NpadButton bit order; the DM_KEYS key-name table in hle_hid.c is generated from here). Frontends always compose buttons from these names.
enum {
    DM_BTN_A = 1u << 0, DM_BTN_B = 1u << 1, DM_BTN_X = 1u << 2, DM_BTN_Y = 1u << 3,
    DM_BTN_LS = 1u << 4, DM_BTN_RS = 1u << 5, DM_BTN_L = 1u << 6, DM_BTN_R = 1u << 7,
    DM_BTN_ZL = 1u << 8, DM_BTN_ZR = 1u << 9, DM_BTN_PLUS = 1u << 10, DM_BTN_MINUS = 1u << 11,
    DM_BTN_LEFT = 1u << 12, DM_BTN_UP = 1u << 13, DM_BTN_RIGHT = 1u << 14, DM_BTN_DOWN = 1u << 15,
};
#define DM_STICK_MAX 32767
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
uint64_t dm_input_samples(void);   // 游戏已取手柄的累计次数 | Cumulative number of times the game has sampled the controller
// 实体 / 虚拟手柄与键盘 / 触屏、脚本分别存储；三路按钮及待消费按下沿按位或。
// 每轴取绝对值最大者，等幅优先级 input > pad > script；各源全零只释放本源，不覆盖其他源。
// Physical / virtual pad, keyboard / touch, and script are stored separately; buttons and pending press edges are ORed across all three.
// Each axis uses the greatest absolute deflection, with input > pad > script on ties; zeroing one source only releases that source.
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
// 已授权的模拟 / DM_KEYS 专用第三源；真实发布和游戏收样走同一宿主 HID 路径，不代表蓝牙硬件输入。
// Independent third source for authorized simulation / DM_KEYS; real publication and guest sampling use the host HID path, without representing Bluetooth hardware input.
void dm_script_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
// 断线 / 失焦专用：只清本源 buttons、四轴和未消费按下沿，并递增追踪 reset_epoch。
// Disconnect / focus-loss reset: clear only this source's buttons, four axes and pending press edges, advancing its trace reset_epoch.
void dm_input_reset(void);
void dm_pad_reset(void);
// 优雅退出使用 _exit，必须显式写出诊断缓冲；重复调用无操作。
// Graceful shutdown uses _exit, so explicitly flush diagnostic buffers; repeated calls are no-ops.
void dm_input_trace_flush(void);
// Mac 手柄触觉桥：仅真实设备引擎及连续 player 初始化成功才发布能力；未实现前端保持 0。
// Mac controller-haptics bridge: capabilities are published only after real device engines and continuous players initialize; unimplemented front ends remain at 0.
// 强度有限幅，频率只近似映射 CoreHaptics sharpness；WHOLE 是单个真实 default 通道，不承诺独立左右或精确 HD 振动。
// Amplitudes are bounded and frequencies only approximate CoreHaptics sharpness; WHOLE is one real default channel, without independent-side or exact HD-rumble claims.
enum { DM_HAPTIC_LEFT=1, DM_HAPTIC_RIGHT=2, DM_HAPTIC_WHOLE=4 };
typedef struct { float low_amp, low_freq, high_amp, high_freq; } dm_vibration_value;
void dm_haptic_bind(void *controller, int active); // Mac 主线程：controller 为 GCController；NULL / 失焦强停止 | Mac main thread: controller is a GCController; NULL / focus loss force stop
// 终止型清理：封住新提交，零强度→停止 player→停止引擎；最多等待 timeout_ms，返回 1 仅说明全部 API 完成且无已记录错误。
// Terminal cleanup: reject new submissions, zero intensity, stop players, then stop engines; wait at most timeout_ms, returning 1 only for completed APIs without recorded errors.
// 任意宿主线程可调用；工作队列调用不等待自身；超时 / API 错误返回 0，不代表物理电机状态。
// Callable from any host thread; worker calls do not wait on themselves; timeout / API errors return 0, without claiming physical motor state.
int dm_haptic_shutdown(const char *reason, unsigned timeout_ms);
unsigned dm_haptic_capabilities(void);
int dm_haptic_initialize(unsigned side);
void dm_haptic_send(unsigned side, const dm_vibration_value *value);
// 软件成功提交值，不是电机遥测；发送异步合并，停止 / 断线 / 失焦 / 故障清零。
// Successfully submitted software state, not motor telemetry; sends coalesce asynchronously, and stop / disconnect / focus loss / faults clear it.
void dm_haptic_actual(unsigned side, dm_vibration_value *out);
// 可选 DM_KEYS_FIFO=/已存在FIFO绝对路径：实际 POSIX 控制线程，每行复用 DM_KEYS 的 秒:键[+键][~时长],… 语法。
// 每行最多 1022 字节 / 64 步；各行及固定脚本整组串行，时间相对该组开始执行，行末主动归零。
// 例：向 FIFO 写入一行 0:B+LSRight~0.4；模拟仅走独立 script 源，不代表实体手柄或无线链路。
// Optional DM_KEYS_FIFO=/absolute/path/to/existing/fifo: a real POSIX control thread accepts the existing seconds:key[+key][~hold],… DM_KEYS grammar per line.
// Each line is limited to 1022 bytes / 64 steps; lines and the fixed script execute serially as whole groups, with times relative to group execution and an explicit zero at line end.
// Example line: 0:B+LSRight~0.4; simulation only uses the independent script source and does not represent a physical controller or wireless link.

// 可选 DM_INPUT_TRACE=/绝对路径/input.jsonl（进程启动前设置）：65536 条预分配缓冲，在正常 exit 或有序宿主退出时写出；崩溃 / 直接 _exit / 强杀不会写出。
// publish 每次真实调用递增 publication_seq；read 仅状态、发布序列或 id/style 变化时记录，sampling/frame 自增不单独触发。
// begin_ns/time_ns 使用宿主 now_ns；time_ns 是锁内发布完成 / HID 填写完成时刻，不是客体下一条指令或硬件事件时刻。
// source_seq 顺序 input/pad/script；read 的 source_latch 是实际消费的按下锁存，可能来自较早发布，须结合发布历史匹配。
// reset 行及 reset_epoch 同顺序标记显式丢弃锁存；不能把 reset 前未消费按下关联到 reset 后读取。
// 实际 read 记录 sampling/id/style/attributes/buttons/四轴与原子读取的 NVN frame。缓冲满保留前缀，summary.dropped 明示丢样，不能据缺行断言未收样。
// 发布→实际 HID 填写可测宿主收样延迟；关联随后 frame/上屏只能估计管线延迟，不能证明游戏已使用输入，更不是光子响应或硬件无线延迟。
// Optional DM_INPUT_TRACE=/absolute/path/input.jsonl (set before process startup): 65536 preallocated records, written on normal exit or orderly host shutdown; crashes / direct _exit / forced termination do not flush.
// Every actual publication increments publication_seq; read records require a state, publication-sequence or id/style change, not sampling/frame increments alone.
// begin_ns/time_ns use host now_ns; time_ns marks locked publication completion / HID fill completion, not the guest's next instruction or a hardware event.
// source_seq is ordered input/pad/script; a read's source_latch contains consumed press latches that may originate in earlier publications, requiring publication-history matching.
// reset rows and reset_epoch use the same source order and mark explicit latch cancellation; do not associate unconsumed pre-reset presses with post-reset reads.
// Actual reads record sampling/id/style/attributes/buttons/four axes and an atomic NVN frame snapshot. Full buffers retain a prefix and report summary.dropped; missing rows do not prove missing samples.
// Publication to actual HID fill measures host sampling latency; subsequent frame/presentation correlation only estimates pipeline latency, without proving game use, photon response or hardware radio latency.

// 私有skill-aim-v1：只取已实际GetNpad返回的右轴成对收样，非前端尚未消费值。
// Private skill-aim-v1: takes only paired received samples of the right axis that GetNpad has actually returned, not values the front end has not yet had consumed.
void dm_input_sampled_right(int32_t *rx,int32_t *ry);

// 手柄状态注入点：GetNpadState 填完原状态后、伪键覆盖前调用。Mac 宿主触点脚本（touch_mac.c，Mac 专用、按 _mac 约定不进 iOS 包）启动时注册；iOS 不带该文件，保持空
// Controller state injection point: called after GetNpadState has filled in the original state and before the pseudo-key override. The Mac host touch point script (touch_mac.c, Mac only, kept out of the iOS app by the _mac convention) registers at startup; iOS does not carry that file and it stays empty
extern void (*dm_pad_hook)(uint64_t now, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
