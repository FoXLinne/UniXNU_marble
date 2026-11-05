// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/mount_internal.h>
#include <sys/vnode_internal.h>
#include <sys/sysproto.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/thread.h>
#include <arm/thread.h>

extern int native_root_block_authorized(dev_t);
extern int native_root_block_cache_authorized(dev_t);
extern int native_root_block_volume_device(const char *, dev_t *, char *, size_t);
extern int native_marble_data_volume(dev_t *, char *, size_t);
extern void commpage_update_dyld_flags(uint64_t);
extern uint64_t dyld_flags;
extern int sheng_boot_watchdog_policy(void *, uint64_t);
extern int native_marble_setup_policy(proc_t, user_addr_t);
extern void native_marble_staged_apps_prepare(proc_t, user_addr_t, mount_t);
/* CoreEntitlements retains its 16 KiB allocation and stack-window ABI. */
__attribute__((used)) const unsigned int native_ios18_ce_page_shift = 14;
int native_marble_prepare_systemos(proc_t, user_addr_t, user_addr_t *);
int native_marble_bindfs_allowed(void);
static int bootBindMount;

struct marble_ui_exit {
    uint32_t pid, reason_namespace;
    uint64_t reason_code;
    char name[32];
    unsigned char state[320];
    char description[512];
};
struct marble_ui_exit marble_ui_exits[16];
unsigned marble_ui_exit_count;
__attribute__((used)) const unsigned marble_ui_exit_bytes = sizeof(struct marble_ui_exit);
void native_marble_ui_exit(proc_t, const char *, uint32_t, uint64_t, const char *);

void native_marble_ui_exit(proc_t process, const char *name, uint32_t reason_namespace,
                          uint64_t reason_code, const char *description)
{
    if (strcmp(name, "SpringBoard") && strcmp(name, "backboardd")) return;
    unsigned index = __atomic_fetch_add(&marble_ui_exit_count, 1, __ATOMIC_RELAXED) % 16;
    struct marble_ui_exit *record = &marble_ui_exits[index];
    bzero(record, sizeof(*record));
    record->pid = (uint32_t)proc_getpid(process);
    strlcpy(record->name, name, sizeof(record->name));
    record->reason_namespace = reason_namespace;
    record->reason_code = reason_code;
    if (description) strlcpy(record->description, description, sizeof(record->description));
    struct arm_saved_state *state = get_user_regs(current_thread());
    if (state) memcpy(record->state, state, sizeof(record->state));
    uint64_t pc;
    memcpy(&pc, record->state + 264, sizeof(pc));
    printf("MarbleUI: %s[%u] exit namespace=%u code=%llx pc=%llx\n", record->name,
           record->pid, record->reason_namespace, record->reason_code, pc);
}

int native_marble_bindfs_allowed(void)
{
    return bootBindMount && proc_selfpid() == 1;
}

static int preparePreboot(proc_t process, user_addr_t scratch)
{
    static const char *const paths[] = {
        "/private/var/preboot", "/private/var/preboot/Cryptexes", "/private/var/preboot/Cryptexes/OS",
        "/private/var/preboot/Cryptexes/OS/System", "/private/var/preboot/Cryptexes/OS/usr"};
    int error;
    int32_t result = 0;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        struct mkdir_args request = {.path = scratch + 0x800, .mode = 0755};
        error = copyout(paths[i], request.path, strlen(paths[i]) + 1);
        if (!error) error = mkdir(process, &request, &result);
        if (error && error != EEXIST) return error;
    }
    struct mount_args request = {.type = scratch + 0x100, .path = scratch + 0x120,
                                .flags = MNT_RDONLY, .data = scratch + 0x200};
    if ((error = copyout("bindfs", request.type, 7)) ||
        (error = copyout("/private/preboot", request.path, 17)) ||
        (error = copyout(paths[0], request.data, strlen(paths[0]) + 1))) return error;
    bootBindMount = 1;
    error = mount(process, &request, &result);
    bootBindMount = 0;
    printf("MarbleStorage: preboot bind mount returned %d\n", error);
    return error;
}

static int bindCryptex(proc_t process, user_addr_t scratch, const char *source, const char *target)
{
    struct mount_args request = {.type = scratch + 0x100, .path = scratch + 0x120,
                                .flags = MNT_RDONLY, .data = scratch + 0x200};
    int error;
    int32_t result = 0;
    if ((error = copyout("bindfs", request.type, 7)) ||
        (error = copyout(target, request.path, strlen(target) + 1)) ||
        (error = copyout(source, request.data, strlen(source) + 1))) return error;
    bootBindMount = 1;
    error = mount(process, &request, &result);
    bootBindMount = 0;
    printf("MarbleStorage: cryptex bind %s returned %d\n", target, error);
    return error;
}

static int prepareData(proc_t process, user_addr_t scratch)
{
    dev_t selected = NODEV;
    char path[64] = {0};
    if (!native_marble_data_volume(&selected, path, sizeof(path))) return ENOENT;
    user64_addr_t spec = scratch + 0x160;
    uint8_t payload[0x150] = {0};
    uint16_t mode = 1;
    uint32_t owner = 99;
    memcpy(payload, &spec, sizeof(spec));
    memcpy(payload + 16, &mode, sizeof(mode));
    memcpy(payload + 0x144, &owner, sizeof(owner));
    memcpy(payload + 0x148, &owner, sizeof(owner));
    struct mount_args request = {.type = scratch + 0x100, .path = scratch + 0x120,
                                .flags = 0, .data = scratch + 0x200};
    int error;
    int32_t result = 0;
    if ((error = copyout("apfs", request.type, 5)) ||
        (error = copyout("/private/var", request.path, 13)) ||
        (error = copyout(path, spec, strlen(path) + 1)) ||
        (error = copyout(payload, request.data, sizeof(payload)))) return error;
    error = mount(process, &request, &result);
    printf("MarbleStorage: Data mount %s returned %d\n", path, error);
    if (error) return error;
    vnode_t data_root = NULLVP;
    error = vnode_lookup("/private/var", 0, &data_root, vfs_context_current());
    if (error) return error;
    mount_t mounted = vnode_mount(data_root);
    vnode_t device_vnode = vfs_devvp(mounted);
    dev_t actual = device_vnode ? vnode_specrdev(device_vnode) : NODEV;
    if (device_vnode) vnode_put(device_vnode);
    int accepted = actual == selected && !(vfs_flags(mounted) & MNT_RDONLY) &&
        !strcmp(vfs_statfs(mounted)->f_fstypename, "apfs");
    vnode_put(data_root);
    if (accepted) native_marble_staged_apps_prepare(process, scratch, mounted);
    return accepted ? 0 : EAUTH;
}

int native_marble_prepare_systemos(proc_t process, user_addr_t scratch, user_addr_t *environment)
{
    const char *uuid = "9F1E9F09-B2A0-4844-98CB-E1025B99C4CE";
    const char *mountpoint = "/";
    vnode_t original_root = rootvnode;
    mount_t root = vnode_mount(rootvnode);
    vnode_t device_vnode = vfs_devvp(root);
    dev_t root_device = device_vnode ? vnode_specrdev(device_vnode) : NODEV;
    if (device_vnode) vnode_put(device_vnode);
    if (!(vfs_flags(root) & MNT_RDONLY) || !native_root_block_authorized(root_device)) return EAUTH;
    int error = prepareData(process, scratch);
    if (!error) error = preparePreboot(process, scratch);
    if (error) return error;
    dev_t selected = NODEV;
    char path[64] = {0};
    if (!native_root_block_volume_device(uuid, &selected, path, sizeof(path))) return ENOENT;
    user64_addr_t spec = scratch + 0x160;
    uint8_t payload[0x150] = {0};
    uint64_t flags = MNT_RDONLY;
    uint16_t mode = 1;
    uint32_t owner = 99;
    memcpy(payload, &spec, sizeof(spec));
    memcpy(payload + 8, &flags, sizeof(flags));
    memcpy(payload + 16, &mode, sizeof(mode));
    memcpy(payload + 0x144, &owner, sizeof(owner));
    memcpy(payload + 0x148, &owner, sizeof(owner));
    struct mount_args request = {.type = scratch + 0x100, .path = scratch + 0x120,
                                .flags = MNT_RDONLY | MNT_UNION, .data = scratch + 0x200};
    int32_t result = 0;
    if ((error = copyout("apfs", request.type, 5)) ||
        (error = copyout(mountpoint, request.path, strlen(mountpoint) + 1)) ||
        (error = copyout(path, spec, strlen(path) + 1)) ||
        (error = copyout(payload, request.data, sizeof(payload)))) return error;
    error = mount(process, &request, &result);
    printf("MarbleStorage: SystemOS mount %s returned %d\n", path, error);
    if (error) return error;
    vnode_t mounted_root = NULLVP;
    error = vnode_lookup(mountpoint, 0, &mounted_root, vfs_context_current());
    if (error) return error;
    mount_t mounted = vnode_mount(mounted_root);
    device_vnode = vfs_devvp(mounted);
    dev_t actual = device_vnode ? vnode_specrdev(device_vnode) : NODEV;
    if (device_vnode) vnode_put(device_vnode);
    int accepted = (vfs_flags(mounted) & MNT_RDONLY) && actual == selected &&
        !strcmp(vfs_statfs(mounted)->f_fstypename, "apfs") && native_root_block_cache_authorized(actual);
    vnode_put(mounted_root);
    if (!accepted || !native_root_block_authorized(root_device) ||
        !(vfs_flags(mounted) & MNT_UNION) || vnode_mountedhere(original_root) != mounted) return EAUTH;
    error = bindCryptex(process, scratch, "/System", "/private/preboot/Cryptexes/OS/System");
    if (!error) error = bindCryptex(process, scratch, "/usr", "/private/preboot/Cryptexes/OS/usr");
    if (error) return error;
    vnode_t cache = NULLVP;
    error = vnode_lookup("/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e", 0,
                         &cache, vfs_context_current());
    if (error) return error;
    int cache_visible = vnode_mount(cache) == mounted;
    vnode_put(cache);
    if (!cache_visible) return EAUTH;
    error = sheng_boot_watchdog_policy(process, scratch);
    if (error) return error;
    error = native_marble_setup_policy(process, scratch);
    if (error) return error;
    *environment = USER_ADDR_NULL;
    dyld_flags |= UINT64_C(8);
    commpage_update_dyld_flags(dyld_flags);
    printf("MarbleStorage: SystemOS resident policy accepted, dyld flags %llx\n", dyld_flags);
    return 0;
}
