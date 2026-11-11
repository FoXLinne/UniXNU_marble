// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ShengGpr.h"
#include "ShengAudioReach.h"
static uint32_t read32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
int main(void) {
    uint8_t packet[512], guard[16];
    memset(guard, 0xa5, sizeof(guard));
    assert(ar_open_marble_tdm_device(guard, 8, 0x3001, 1) == 0);
    for (unsigned i = 8; i < sizeof(guard); ++i) assert(guard[i] == 0xa5);
    uint32_t length = ar_open_marble_tdm_device(packet, sizeof(packet), 0x3001, 1);
    GprPacket decoded;
    assert(length && length <= 512 && gpr_decode(packet, length, &decoded) == GprOk);
    assert(decoded.opcode == AR_OP_GRAPH_OPEN && decoded.dst_port == AR_APM_IID);
    assert(decoded.src_port == 0x3001 && decoded.token == 1);
    unsigned modules = 0;
    for (unsigned i = 0; i + 4 <= length; i += 4) if (read32(packet + i) == 0x0700100e) ++modules;
    assert(modules == 1);
    const ArFormat format = {96000, 32, 2};
    length = ar_config_marble_tdm_device(packet, sizeof(packet), 0x3001, 2, &format);
    assert(gpr_decode(packet, length, &decoded) == GprOk && decoded.opcode == AR_OP_SET_CFG);
    unsigned found = 0;
    for (unsigned i = 0; i + 44 <= length; i += 4) {
        if (read32(packet + i) != 0x0800101b) continue;
        const uint8_t *p = packet + i + 12;
        assert(read32(p) == 0 && read32(p + 4) == 2); // LPAIF, tertiary
        assert(read32(p + 12) == 3 && read32(p + 16) == (32u << 16 | 4));
        assert(read32(p + 20) == 0 && read32(p + 24) == 1);
        ++found;
    }
    assert(found == 1);
    length = ar_write(packet, sizeof(packet), 0x3001, 3, 0x130000000ULL, 1, 7680);
    assert(gpr_decode(packet, length, &decoded) == GprOk && decoded.opcode == AR_OP_WRITE);
    assert(decoded.token == 3 && decoded.dst_port == AR_IID_PLAY_SHMEM);
    puts("PASS: bounded GPR packets, tertiary TDM module/slots/timing and stereo DMA write packet");
}
