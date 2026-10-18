// Switch 2 Pro 原生 BLE 桥；仅 DM_NS2_PRO=ble 启用，不经过虚拟 HID。
// Native Switch 2 Pro BLE bridge; opt in with DM_NS2_PRO=ble, without virtual HID.
#pragma once
#include "dm_input.h"

int dm_ns2_enabled(void);
void dm_ns2_start(void);
void dm_ns2_set_active(int active);
unsigned dm_ns2_capabilities(void);
unsigned dm_ns2_device_capabilities(void);
int dm_ns2_initialize(unsigned side);
void dm_ns2_send(unsigned side, const dm_vibration_value *value);
void dm_ns2_actual(unsigned side, dm_vibration_value *out);
// 成功仅表示零包交给 CoreBluetooth；无写响应通道不提供电机停止确认。
// Success only means CoreBluetooth accepted the zero packet; this channel has no motor-stop acknowledgement.
int dm_ns2_shutdown(const char *reason, unsigned timeout_ms);

// Hz / 强度编码由独立 codec 提供；BLE 传输始终使用完整的 42-byte report。
// A separate codec handles Hz / amplitude encoding; BLE always transports a complete 42-byte report.
void ns2_encode_rumble(const dm_vibration_value values[2], uint8_t sequence, float gain, uint8_t out[42]);
