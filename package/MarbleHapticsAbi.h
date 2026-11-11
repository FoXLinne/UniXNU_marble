// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MARBLE_HAPTICS_ABI_H
#define MARBLE_HAPTICS_ABI_H
#include <stdint.h>

#define MARBLE_HAPTICS_DEVICE "/dev/marble-haptics"
#define MARBLE_HAPTICS_MAGIC 0x4d484150u
#define MARBLE_HAPTICS_VERSION 1u
#define MARBLE_HAPTICS_RATE 48000u
#define MARBLE_HAPTICS_MAX_FRAMES 2048u
#define MARBLE_HAPTICS_STOP 1u
#define MARBLE_HAPTICS_START 2u

// One atomic write: this header followed by frames signed little-endian PCM16
// samples. host_time is the presentation time of sample zero, in Mach ticks.
// START and STOP carry zero frames and establish/terminate an IO generation.
typedef struct {
    uint32_t magic, version, flags, frames;
    uint64_t host_time;
    uint32_t rate, reserved;
} MarbleHapticsPacket;

#endif
