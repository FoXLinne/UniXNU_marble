// SPDX-License-Identifier: BSD-2-Clause-Patent
// Definitions for the diagnostic counters declared by UfsTicksShim.h.
#include <stdint.h>
volatile uint64_t native_ufs_verify_ticks = 0, native_ufs_read_ticks = 0;
volatile uint32_t native_ufs_integrity_failures = 0;
