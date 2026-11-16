// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/mount_internal.h>
#include <sys/vnode_internal.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>

static mount_t marble_capacity_data_mount;
/* calls, aliases, errors, raw system bytes, Data total/available, exposed total, Data free. */
__attribute__((used)) volatile uint64_t marble_storage_capacity[8];

void native_marble_storage_register_data(mount_t);
void native_marble_storage_getattr(mount_t, struct vfs_attr *, vfs_context_t);
void native_marble_storage_register_data(mount_t data)
{
    unsigned enabled=0;
    if (!PE_parse_boot_argn("marble-ufs", &enabled, sizeof enabled) || enabled!=1 || !data) return;
    mount_t empty=NULL;
    mount_ref(data,0);
    if (!__atomic_compare_exchange_n(&marble_capacity_data_mount,&empty,data,false,
                                     __ATOMIC_RELEASE,__ATOMIC_RELAXED)) {
        mount_drop(data,0);
        return;
    }
    vfs_setmntsystemdata(data);
}

void native_marble_storage_getattr(mount_t mp, struct vfs_attr *result, vfs_context_t ctx)
{
    mount_t data=__atomic_load_n(&marble_capacity_data_mount,__ATOMIC_ACQUIRE);
    if (!data || !mp || mp==data) return;
    const uint64_t capacity_bits=VFSATTR_f_bsize|VFSATTR_f_blocks|VFSATTR_f_bfree|
        VFSATTR_f_bavail|VFSATTR_f_bused|VFSATTR_f_quota|VFSATTR_f_reserved;
    if (!(result->f_active & capacity_bits)) return;
    bool system=(mp->mnt_flag & MNT_ROOTFS)!=0;
    if (!system && (mp->mnt_flag & MNT_UNION) && mp->mnt_vnodecovered!=NULLVP) {
        vnode_t lower=mp->mnt_vnodecovered;
        system=(lower->v_flag & VROOT) && (lower->v_mount->mnt_flag & MNT_ROOTFS);
    }
    if (!system) return;
    __atomic_fetch_add(&marble_storage_capacity[0],1,__ATOMIC_RELAXED);
    struct vfs_attr actual;
    VFSATTR_INIT(&actual);
    actual.f_active=capacity_bits;
    if (vfs_busy(data,LK_NOWAIT)) {
        __atomic_fetch_add(&marble_storage_capacity[2],1,__ATOMIC_RELAXED);
        return;
    }
    const int error=vfs_getattr(data,&actual,ctx);
    vfs_unbusy(data);
    if (error || !VFSATTR_IS_SUPPORTED(&actual,f_bsize) ||
        !VFSATTR_IS_SUPPORTED(&actual,f_blocks) || !VFSATTR_IS_SUPPORTED(&actual,f_bfree) ||
        !VFSATTR_IS_SUPPORTED(&actual,f_bavail) || !actual.f_bsize ||
        actual.f_bfree>actual.f_blocks || actual.f_bavail>actual.f_bfree) {
        __atomic_fetch_add(&marble_storage_capacity[2],1,__ATOMIC_RELAXED);
        return;
    }
    uint64_t raw=mp->mnt_vfsstat.f_bsize*mp->mnt_vfsstat.f_blocks;
    if (VFSATTR_IS_SUPPORTED(result,f_bsize) && VFSATTR_IS_SUPPORTED(result,f_blocks))
        raw=result->f_bsize*result->f_blocks;
    marble_storage_capacity[3]=raw;
    marble_storage_capacity[4]=actual.f_bsize*actual.f_blocks;
    marble_storage_capacity[5]=actual.f_bsize*actual.f_bavail;
    marble_storage_capacity[7]=actual.f_bsize*actual.f_bfree;
    if (!VFSATTR_IS_SUPPORTED(&actual,f_bused))
        VFSATTR_RETURN(&actual,f_bused,actual.f_blocks-actual.f_bfree);
#define MARBLE_COPY_CAPACITY(field) do { \
    if (VFSATTR_IS_ACTIVE(result,field)) { \
        if (VFSATTR_IS_SUPPORTED(&actual,field)) VFSATTR_RETURN(result,field,actual.field); \
        else result->f_supported &= ~VFSATTR_ ## field; \
    } \
} while(0)
    MARBLE_COPY_CAPACITY(f_bsize);MARBLE_COPY_CAPACITY(f_blocks);
    MARBLE_COPY_CAPACITY(f_bfree);MARBLE_COPY_CAPACITY(f_bavail);
    MARBLE_COPY_CAPACITY(f_bused);MARBLE_COPY_CAPACITY(f_quota);MARBLE_COPY_CAPACITY(f_reserved);
#undef MARBLE_COPY_CAPACITY
    marble_storage_capacity[6]=actual.f_bsize*actual.f_blocks;
    if (__atomic_fetch_add(&marble_storage_capacity[1],1,__ATOMIC_RELAXED)==0)
        printf("MarbleStorage capacity system=%llu Data=%llu available=%llu free=%llu bytes\n",
               (unsigned long long)raw,(unsigned long long)marble_storage_capacity[4],
               (unsigned long long)marble_storage_capacity[5],(unsigned long long)marble_storage_capacity[7]);
}
