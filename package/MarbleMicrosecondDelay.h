// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// UniXNU-sheng TouchProbe.cpp ioDelayUs: short SPI/SPMI polls stay on the CPU.
static inline void marbleMicrosecondDelay(unsigned us) {
    if (us > 50) { IODelay(us); return; }
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * us / 1000000 + 1;
    do { __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now)); } while (now - start < ticks);
}
