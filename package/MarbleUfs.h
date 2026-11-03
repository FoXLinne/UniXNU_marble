// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
class IOMemoryDescriptor;
extern "C" {
bool marble_ufs_start(void);
uint64_t marble_ufs_root_bytes(unsigned);
uint64_t marble_ufs_root_read(unsigned, IOMemoryDescriptor *, uint64_t, uint64_t);
bool marble_ufs_store_open(uint64_t);
bool marble_ufs_store_read(uint64_t, uint32_t, void *);
bool marble_ufs_store_write(uint64_t, uint32_t, const void *);
bool marble_ufs_store_sync(void);
}
