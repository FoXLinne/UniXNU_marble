// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MARBLE_SCANOUT_WIRE_H
#define MARBLE_SCANOUT_WIRE_H
#define MARBLE_SCANOUT_OFFSET 4096
#define MARBLE_SCANOUT_MAGIC 0x31544f554e43534dULL
#define MARBLE_PANEL_DCS_CAP 0x315343444c4e504dULL
#define MARBLE_PANEL_BRIGHTNESS 0xfffffffeU
#define MARBLE_PANEL_POWER 0xfffffffdU
#define MARBLE_PANEL_REFRESH 0xfffffffcU
#define MARBLE_PANEL_REFRESH_CAP 0x3153485a4c4e504dULL
typedef struct {
    unsigned long long magic;
    unsigned error, ready, completed, shown;
    unsigned long long longest_us, timeouts, presented, checkpoint;
    unsigned pipe_mask, flush_mask;
    /* Firmware owns line 0; XNU owns line 1. */
    unsigned requested, slot;
    unsigned long long base, span, reserved[5];
    unsigned width, height, stride, version;
    unsigned registers[8][8];
    unsigned panel_mode_status[12];
    /* Firmware-owned cache line, separate from XNU's request at offset 64. */
    unsigned long long panel_status[8];
} MARBLE_SCANOUT;
#endif
