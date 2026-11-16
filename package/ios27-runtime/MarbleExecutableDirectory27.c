// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/vnode_internal.h>
#include <sys/codesign.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/unistd.h>
#include <sys/kauth.h>

volatile uint64_t marble_metadata_directory_open_count;
volatile uint64_t marble_metadata_directory_access_count;

static boolean_t marble_own_executable_directory(proc_t process, const char *path)
{
    if (!process || strcmp(path, "/usr/libexec") ||
        !csproc_get_platform_binary(process)) return FALSE;
    const char *name = proc_best_name(process);
    const char *expected;
    if (!strcmp(name, "driverkitd")) expected = "/usr/libexec/driverkitd";
    else if (!strcmp(name, "audiomxd")) expected = "/usr/libexec/audiomxd";
    else return FALSE;
    vnode_t executable = proc_getexecutablevnode(process);
    if (!executable) return FALSE;
    char executable_path[MAXPATHLEN];
    int length = sizeof(executable_path);
    int error = vn_getpath(executable, executable_path, &length);
    vnode_put(executable);
    return !error && !strcmp(executable_path, expected);
}

/* CoreFoundation resolves the standalone daemon through a directory descriptor.
 * Give that descriptor search capability, keeping FREAD/FWRITE clear. Existing
 * vnode and sandbox checks authorize the search; directory enumeration keeps
 * its ordinary descriptor read-mode requirement.
 */
int native_marble_metadata_directory_open(proc_t process, user_addr_t user_path, int flags)
{
    if ((flags & (O_ACCMODE | O_DIRECTORY | O_CREAT | O_TRUNC | O_APPEND)) != O_DIRECTORY)
        return flags;
    char path[MAXPATHLEN];
    size_t copied;
    if (copyinstr(user_path, path, sizeof(path), &copied) ||
        !marble_own_executable_directory(process, path)) return flags;
    __atomic_fetch_add(&marble_metadata_directory_open_count, 1, __ATOMIC_RELAXED);
    return flags | O_EXEC;
}

int native_marble_metadata_directory_access(proc_t process, vnode_t directory, int mode)
{
    if (mode != R_OK || !directory || !vnode_isdir(directory)) return mode;
    char path[MAXPATHLEN];
    int length = sizeof(path);
    if (vn_getpath(directory, path, &length) ||
        !marble_own_executable_directory(process, path)) return mode;
    __atomic_fetch_add(&marble_metadata_directory_access_count, 1, __ATOMIC_RELAXED);
    return X_OK;
}

/* PosterBoard authenticates its same-user platform clients through csops.
 * The syscall verifies the supplied audit token before this compatibility rule.
 * Keep mutation operations and clients outside the signed system group on the
 * ordinary MAC policy path.
 */
volatile uint64_t marble_poster_signature_queries;
int native_marble_poster_signature_query(proc_t actor, proc_t target, unsigned operation)
{
    if (!actor || !target ||
        (operation != CS_OPS_STATUS && operation != CS_OPS_IDENTITY &&
         operation != CS_OPS_ENTITLEMENTS_BLOB &&
         operation != CS_OPS_DER_ENTITLEMENTS_BLOB) ||
        strcmp(proc_best_name(actor), "PosterBoard") ||
        !csproc_get_platform_binary(actor) || !csproc_get_platform_binary(target)) return 0;
    vnode_t executable = proc_getexecutablevnode(actor);
    if (!executable) return 0;
    char path[MAXPATHLEN];
    int length = sizeof(path);
    int error = vn_getpath(executable, path, &length);
    vnode_put(executable);
    if (error || strcmp(path, "/Applications/PosterBoard.app/PosterBoard")) return 0;
    kauth_cred_t actor_cred = kauth_cred_proc_ref(actor);
    kauth_cred_t target_cred = kauth_cred_proc_ref(target);
    int same_user = kauth_cred_getuid(actor_cred) == kauth_cred_getuid(target_cred);
    kauth_cred_unref(&actor_cred);
    kauth_cred_unref(&target_cred);
    if (!same_user) return 0;
    __atomic_fetch_add(&marble_poster_signature_queries, 1, __ATOMIC_RELAXED);
    return 1;
}
