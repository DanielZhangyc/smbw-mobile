// 编码回归使用实机已测的频码和幅码，覆盖双频、右侧、第十位和停止包。
// Encoding regression uses physically measured frequency/amplitude codes, covering dual bands, right side, bit10 and stop packets.
#include "../ns2.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    dm_vibration_value values[2] = {{.5f,160,.5f,320}, {0,0,.5f,640}};
    uint8_t packet[42]; ns2_encode_rumble(values, 31, 1, packet);
    // 160/320 Hz → 385/481；0.5 → 已实测幅码96；两个40-bit字段独立。
    // 160/320 Hz map to 385/481; 0.5 maps to measured amplitude code96, with independent 40-bit words.
    const uint8_t left[] = {0x81,0x81,0x11,0x1e,0x18};
    const uint8_t right[] = {0,0,0x10,0x24,0x18};
    assert(packet[0] == 0 && packet[1] == 0x5f && packet[17] == 0x5f);
    assert(!memcmp(packet + 2, left, sizeof left));
    assert(!memcmp(packet + 18, right, sizeof right));
    for (unsigned i = 7; i < 17; i++) assert(packet[i] == 0);
    for (unsigned i = 23; i < 42; i++) assert(packet[i] == 0);
    values[0] = (dm_vibration_value){0}; values[1] = (dm_vibration_value){0};
    ns2_encode_rumble(values, 16, 2, packet);
    for (unsigned i = 0; i < 42; i++) assert(packet[i] == (i == 1 || i == 17 ? 0x10 : 0));
    values[0] = (dm_vibration_value){NAN,160,1,NAN};
    ns2_encode_rumble(values, 0, 1, packet);
    assert(packet[1] == 0x10);
    for (unsigned i = 2; i < 17; i++) assert(packet[i] == 0);
    values[0] = (dm_vibration_value){.5f,160,0,0};
    ns2_encode_rumble(values, 0, 2, packet);
    const uint8_t stronger[] = {0x81,0x01,0x03,0,0};
    assert(!memcmp(packet + 2, stronger, sizeof stronger));
    ns2_encode_rumble(values, 0, 0, packet);
    assert(packet[1] == 0x10);
    puts("NS2 measured-code, dual-band, sequence and stop regression passed");
    return 0;
}
