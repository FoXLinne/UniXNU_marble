// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/vnode_internal.h>
#include <os/atomic_private.h>

// 24B119 queries bit 15 of the context process's VFS policy. Keep the
// actual predicate even when the older source kernel has no named setter.
__attribute__((used)) boolean_t
native_ios27_vfs_context_iopolicy_0x8000(vfs_context_t context)
{
    proc_t process = vfs_context_proc(context);
    return process && (os_atomic_load(&process->p_vfs_iopolicy, relaxed) & 0x8000u);
}
