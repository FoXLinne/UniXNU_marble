// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "MarbleVirtualStorage.h"

// The VM build links a strong implementation from its signed-storage archive.
// Board builds retain a diagnostic for an unconfigured virtual platform.
extern "C" __attribute__((weak, noinline)) void native_qemu_storage_metaclass_init(void)
{
    panic("QEMU storage: select a build with the virtual storage component");
}
extern "C" __attribute__((weak, noinline)) void native_qemu_storage_start(void)
{
    panic("QEMU storage: virtual storage component unavailable");
}
extern "C" __attribute__((weak)) int native_qemu_root_authorized(dev_t) { return 0; }
extern "C" __attribute__((weak)) int native_qemu_cache_authorized(dev_t) { return 0; }
extern "C" __attribute__((weak)) int native_qemu_volume_device(const char *, dev_t *, char *, size_t) { return 0; }
