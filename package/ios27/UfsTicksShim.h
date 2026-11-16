// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diagnostic tick counters used unconditionally by NativeUfs.cpp even when
// NATIVE_VERIFIED_BLOCK is off; the source defines them inside the feature
// block. Force-included only for that TU; self-disables if the feature is on.
#ifndef UNIXNU_UFS_TICKS_SHIM_H
#define UNIXNU_UFS_TICKS_SHIM_H
#if !defined(NATIVE_VERIFIED_BLOCK) || !NATIVE_VERIFIED_BLOCK
#include <stdint.h>
extern volatile uint64_t native_ufs_verify_ticks, native_ufs_read_ticks;
extern volatile uint32_t native_ufs_integrity_failures;
static inline uint64_t timerTicks(void) {
    uint64_t v;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v));
    return v;
}
#endif
#endif
