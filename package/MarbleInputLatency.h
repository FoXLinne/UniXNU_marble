// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
struct MarbleInputLatency {
    uint64_t sequence, irq, spiBegin, spiEnd, hidBegin, hidEnd, swap, present, request;
    uint32_t headerRetries, payloadDrops, hidStatus, reserved;
};
extern "C" {
void native_marble_input_profile_init(void);
void native_marble_input_begin(uint64_t, uint64_t, uint64_t, uint32_t, uint32_t);
void native_marble_input_hid_begin(void);
void native_marble_input_hid_end(uint32_t);
void native_marble_input_swap_begin(const void *);
void native_marble_input_swap_presented(const void *);
void native_marble_input_swap_cancelled(void);
}
