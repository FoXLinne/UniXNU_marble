// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_kern_xnu.h>
#include <mach/mach_vm.h>

// The source-private map API uses (set_max, protection) and unsafe-address
// wrappers. The public Mach API uses the actual address and byte length.
static inline kern_return_t qemu_vm_protect_readonly(vm_map_t map, vm_offset_t start,
                                                    vm_offset_t end,
                                                    vm_prot_t protection,
                                                    boolean_t maximum)
{
    if (end < start) return KERN_INVALID_ARGUMENT;
    return mach_vm_protect(map, start, end - start, maximum, protection);
}
#define vm_map_protect qemu_vm_protect_readonly
