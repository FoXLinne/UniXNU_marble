// SPDX-License-Identifier: BSD-2-Clause-Patent
// LaunchServices rebuild policy adapted from UniXNU-sheng 5ef48217d479bdf3adaf909032bc3d56d410b7de.
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/vnode_internal.h>
#include <sys/sysproto.h>
#include <sys/systm.h>
#include <sys/uio_internal.h>
#include <sys/dirent.h>
#include <sys/fcntl.h>
#include <kern/kalloc.h>

volatile int marble_ls_store_status = -1;
volatile int marble_ls_store_error;
void native_marble_ls_store_rebuild(proc_t, user_addr_t);

static int names(const char *path, const char *prefix, char (*result)[64], unsigned capacity, unsigned *count)
{
    *count = 0;
    vfs_context_t context = vfs_context_current();
    vnode_t directory = NULLVP;
    int error = vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW, &directory, context);
    if (error) return error;
    error = VNOP_OPEN(directory, FREAD, context);
    if (error) { vnode_put(directory); return error; }
    const unsigned size = 8192;
    uint8_t *buffer = kalloc_data_tag(size, Z_WAITOK, VM_KERN_MEMORY_FILE);
    uio_t uio = buffer ? uio_create(1, 0, UIO_SYSSPACE, UIO_READ) : NULL;
    if (!uio) error = ENOMEM;
    int eof = 0;
    size_t prefix_length = strlen(prefix);
    while (!error && !eof && *count < capacity) {
        uio_reset(uio, uio_offset(uio), UIO_SYSSPACE, UIO_READ);
        uio_addiov(uio, CAST_USER_ADDR_T(buffer), size);
        int entries = 0;
        error = vnode_readdir64(directory, uio, VNODE_READDIR_EXTENDED, &eof, &entries, context);
        unsigned used = size - (unsigned)uio_resid(uio);
        if (error || !used) break;
        for (unsigned at = 0; at + offsetof(struct direntry, d_name) < used && *count < capacity;) {
            const struct direntry *entry = (const struct direntry *)(buffer + at);
            if (!entry->d_reclen || at + entry->d_reclen > used) { error = EINVAL; break; }
            int dot = entry->d_name[0] == '.' && (entry->d_namlen == 1 ||
                (entry->d_namlen == 2 && entry->d_name[1] == '.'));
            if (!dot && entry->d_namlen < 64 && !strncmp(entry->d_name, prefix, prefix_length)) {
                memcpy(result[*count], entry->d_name, entry->d_namlen);
                result[(*count)++][entry->d_namlen] = 0;
            }
            at += entry->d_reclen;
        }
    }
    if (uio) uio_free(uio);
    if (buffer) kfree_data(buffer, size);
    VNOP_CLOSE(directory, FREAD, context);
    vnode_put(directory);
    return error;
}

void native_marble_ls_store_rebuild(proc_t process, user_addr_t scratch)
{
    /* lsd container UUIDs belong to each Data installation. Discover them;
     * remove the generated database before lsd scans the published apps. */
    static const char *parents[] = {
        "/private/var/mobile/Containers/Data/InternalDaemon",
        "/private/var/containers/Data/InternalDaemon",
        "/private/var/containers/Data/System"
    };
    char (*containers)[64] = kalloc_data_tag(128 * 64, Z_WAITOK, VM_KERN_MEMORY_FILE);
    char (*files)[64] = kalloc_data_tag(8 * 64, Z_WAITOK, VM_KERN_MEMORY_FILE);
    char *path = kalloc_data_tag(256, Z_WAITOK, VM_KERN_MEMORY_FILE);
    unsigned removed = 0, looked = 0;
    if (!containers || !files || !path) {
        marble_ls_store_error = ENOMEM;
        goto done;
    }
    for (unsigned parent = 0; parent < sizeof(parents) / sizeof(parents[0]); ++parent) {
        unsigned count = 0;
        if (names(parents[parent], "", containers, 128, &count)) continue;
        for (unsigned container = 0; container < count; ++container) {
            snprintf(path, 256, "%s/%s/Library/Caches", parents[parent], containers[container]);
            unsigned found = 0;
            if (names(path, "com.apple.LaunchServices-", files, 8, &found)) continue;
            ++looked;
            for (unsigned file = 0; file < found; ++file) {
                snprintf(path, 256, "%s/%s/Library/Caches/%s", parents[parent], containers[container], files[file]);
                int error = copyout(path, scratch + 0xc00, strlen(path) + 1);
                struct unlink_args request = {.path = scratch + 0xc00};
                int32_t result = 0;
                if (!error) error = unlink(process, &request, &result);
                if (!error) ++removed;
                else marble_ls_store_error = error;
            }
        }
    }
done:
    marble_ls_store_status = (int)(removed * 1000 + looked);
    printf("MarbleApps: LaunchServices databases=%u containers=%u error=%d\n", removed, looked, marble_ls_store_error);
    if (containers) kfree_data(containers, 128 * 64);
    if (files) kfree_data(files, 8 * 64);
    if (path) kfree_data(path, 256);
}
