// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MARBLE_SPEAKER_ABI_H
#define MARBLE_SPEAKER_ABI_H
#include <stdint.h>
#define MARBLE_SPEAKER_DEVICE "/dev/marble-speaker"
#define MARBLE_SPEAKER_MAGIC 0x4d53504bu
#define MARBLE_SPEAKER_RATE 96000u
#define MARBLE_SPEAKER_MAX_FRAMES 2048u
// Same command header as Actuator, stereo signed PCM32 at 96 kHz follows.
// Q31 samples have 16 significant bits from the system mix. START=2 STOP=1.
#endif
