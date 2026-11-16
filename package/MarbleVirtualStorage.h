// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <sys/types.h>

extern "C" {
void native_qemu_storage_metaclass_init(void);
void native_qemu_storage_start(void);
int native_qemu_root_authorized(dev_t);
int native_qemu_cache_authorized(dev_t);
int native_qemu_volume_device(const char *, dev_t *, char *, size_t);
}
