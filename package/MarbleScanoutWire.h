// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MARBLE_SCANOUT_WIRE_H
#define MARBLE_SCANOUT_WIRE_H
#define MARBLE_SCANOUT_OFFSET 4096
#define MARBLE_SCANOUT_MAGIC 0x31544f554e43534dULL
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
} MARBLE_SCANOUT;
#endif
