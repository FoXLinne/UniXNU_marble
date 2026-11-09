// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/mount_internal.h>
#include <sys/vnode_internal.h>
#include <sys/sysproto.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <mach/task_info.h>
#include <pexpert/pexpert.h>
#include <arm/thread.h>
#include <vm/vm_map.h>

extern vm_map_t current_map(void);
extern kern_return_t vm_map_region_recurse_64(vm_map_t, vm_map_offset_t *, vm_map_size_t *,
    natural_t *, vm_region_submap_info_64_t, mach_msg_type_number_t *);

extern int native_root_block_authorized(dev_t);
extern int native_root_block_cache_authorized(dev_t);
extern int native_root_block_volume_device(const char *, dev_t *, char *, size_t);
extern int native_marble_data_volume(dev_t *, char *, size_t);
extern void commpage_update_dyld_flags(uint64_t);
extern uint64_t dyld_flags;
extern int sheng_boot_watchdog_policy(void *, uint64_t);
extern int native_marble_setup_policy(proc_t, user_addr_t);
extern void native_marble_ls_store_rebuild(proc_t, user_addr_t);
extern int native_marble_metal_prepare(proc_t, user_addr_t);
extern volatile int native_bindfs_boot_mount;
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
char marble_ui_exit_reasons[16][1024];
unsigned marble_ui_exit_count;
struct marble_ui_exit marble_primary_ui_exits[2];
char marble_primary_ui_exit_reasons[2][1024];
unsigned marble_primary_ui_exit_count[2];
__attribute__((used)) const unsigned marble_ui_exit_bytes = sizeof(struct marble_ui_exit);
void native_marble_ui_exit(proc_t, const char *, uint32_t, uint64_t, const char *);

static void captureAbortReason(char *out, size_t capacity, user_addr_t image_info,
    uint64_t image_info_size, uint64_t cache_slide)
{
#ifdef MARBLE_IOS18_GPU
    /* 22H384 shared cache: Foundation, CoreFoundation, Swift, libc++abi and libc __crash_info v5.
     * swift/Runtime/Debug.h defines message and message2. Read the recorded
     * strings at exit instead of faulting process memory through a heap scan. */
    static const uint8_t expected_uuid[16] = {0x52, 0xfd, 0xdb, 0xe8, 0x0e, 0x6e, 0x3d, 0x06, 0xb7, 0xc9, 0xc3, 0x62, 0x95, 0x52, 0x92, 0xe7};
    static const uint64_t annotations[] = {
        0x1eb08fe80, 0x1ee9bd838, 0x1eb070c88, 0x26a0e2c10, 0x1eb2e0840
    };
    uint8_t uuid[16];
    if (!capacity || !image_info || image_info_size < 176 ||
        copyin(image_info + 160, uuid, sizeof(uuid)) || memcmp(uuid, expected_uuid, sizeof(uuid))) return;
    char message[1024];
    /* libobjc publishes its legacy crash string through this separate slot. */
    uint64_t objc_message = 0;
    if (!copyin(0x1eb068488 + cache_slide, &objc_message, sizeof(objc_message)) && objc_message) {
        size_t length = 0;
        int result = copyinstr(objc_message & ((UINT64_C(1) << 39) - 1),
            message, sizeof(message), &length);
        message[sizeof(message) - 1] = 0;
        if (!result || result == ENAMETOOLONG) strlcat(out, message, capacity);
    }
    for (unsigned i = 0; i < sizeof(annotations) / sizeof(annotations[0]); ++i) {
        uint64_t fields[8];
        if (copyin(annotations[i] + cache_slide, fields, sizeof(fields)) || fields[0] != 5) continue;
        const unsigned messages[] = {1, 4};
        for (unsigned j = 0; j < 2; ++j) {
            user_addr_t pointer = fields[messages[j]] & ((UINT64_C(1) << 39) - 1);
            size_t length = 0;
            if (!pointer) continue;
            int result = copyinstr(pointer, message, sizeof(message), &length);
            message[sizeof(message) - 1] = 0;
            if (result && result != ENAMETOOLONG) continue;
            if (out[0]) strlcat(out, "\n", capacity);
            strlcat(out, message, capacity);
        }
    }
#else
    (void)out; (void)capacity; (void)image_info; (void)image_info_size; (void)cache_slide;
#endif
}

void native_marble_ui_exit(proc_t process, const char *name, uint32_t reason_namespace,
                          uint64_t reason_code, const char *description)
{
    int primary_ui = !strcmp(name, "SpringBoard") || !strcmp(name, "backboardd");
    if (!primary_ui && ((!reason_namespace && !reason_code) ||
        (reason_namespace == 2 && (reason_code == 9 || reason_code == 15)))) return;
    unsigned index = __atomic_fetch_add(&marble_ui_exit_count, 1, __ATOMIC_RELAXED) % 16;
    struct marble_ui_exit *record = &marble_ui_exits[index];
    bzero(record, sizeof(*record));
    bzero(marble_ui_exit_reasons[index], sizeof(marble_ui_exit_reasons[index]));
    record->pid = (uint32_t)proc_getpid(process);
    strlcpy(record->name, name, sizeof(record->name));
    record->reason_namespace = reason_namespace;
    record->reason_code = reason_code;
    struct arm_saved_state *state = get_user_regs(current_thread());
    if (state) memcpy(record->state, state, sizeof(record->state));
    task_dyld_info_data_t dyld_info = {0};
    mach_msg_type_number_t info_count = TASK_DYLD_INFO_COUNT;
    uint64_t cache_slide = 0;
    if (current_proc() == process && task_info(proc_task(process), TASK_DYLD_INFO,
        (task_info_t)&dyld_info, &info_count) == KERN_SUCCESS && dyld_info.all_image_info_size >= 160)
        (void)copyin(dyld_info.all_image_info_addr + 152, &cache_slide, sizeof(cache_slide));
    size_t used = (size_t)snprintf(record->description, sizeof(record->description), "slide=%llx", cache_slide);
    uint64_t frame = 0;
    if (state && current_proc() == process) memcpy(&frame, record->state + 240, sizeof(frame));
    const uint64_t user_mask = (UINT64_C(1) << 39) - 1;
    frame &= user_mask;
    for (unsigned depth = 0; depth < 12 && frame && !(frame & 15) &&
         used + 20 < sizeof(record->description); ++depth) {
        uint64_t links[2];
        if (copyin(frame, links, sizeof(links))) break;
        used += (size_t)snprintf(record->description + used, sizeof(record->description) - used,
                                 " %llx", links[1] & user_mask);
        uint64_t next = links[0] & user_mask;
        if (next <= frame || next - frame > UINT64_C(1048576)) break;
        frame = next;
    }
    if (description && used + 2 < sizeof(record->description))
        snprintf(record->description + used, sizeof(record->description) - used, " %s", description);
    if (reason_namespace == 2 && reason_code == 6 && current_proc() == process)
        captureAbortReason(marble_ui_exit_reasons[index], sizeof(marble_ui_exit_reasons[index]),
            dyld_info.all_image_info_addr, dyld_info.all_image_info_size, cache_slide);
    if (primary_ui) {
        unsigned primary_index = !strcmp(name, "backboardd");
        memcpy(&marble_primary_ui_exits[primary_index], record, sizeof(*record));
        memcpy(marble_primary_ui_exit_reasons[primary_index], marble_ui_exit_reasons[index],
               sizeof(marble_primary_ui_exit_reasons[primary_index]));
        __atomic_fetch_add(&marble_primary_ui_exit_count[primary_index], 1, __ATOMIC_RELEASE);
    }
    uint64_t pc;
    memcpy(&pc, record->state + 264, sizeof(pc));
    printf("MarbleUI: %s[%u] exit namespace=%u code=%llx pc=%llx\n", record->name,
           record->pid, record->reason_namespace, record->reason_code, pc);
}

int native_marble_bindfs_allowed(void)
{
    return (bootBindMount || native_bindfs_boot_mount) && proc_selfpid() == 1;
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

volatile int marble_app_union_status = -1;
volatile unsigned marble_app_union_visible;

static void publishSystemApps(proc_t process, user_addr_t scratch)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("owner-staged-apps", &enabled, sizeof(enabled)) || enabled != 1) return;
    struct mount_args request = {.type = scratch + 0x100, .path = scratch + 0x120,
                                .flags = MNT_RDONLY | MNT_UNION, .data = scratch + 0x200};
    int error = copyout("bindfs", request.type, sizeof("bindfs"));
    if (!error) error = copyout("/Applications", request.path, sizeof("/Applications"));
    if (!error) error = copyout("/private/var/staged_system_apps", request.data, sizeof("/private/var/staged_system_apps"));
    int32_t result = 0;
    if (!error) {
        bootBindMount = 1;
        error = mount(process, &request, &result);
        bootBindMount = 0;
    }
    marble_app_union_status = error;
    if (!error) {
        static const char *paths[] = {"/Applications/Preferences.app/Info.plist", "/Applications/Weather.app/Info.plist",
                                     "/Applications/MobileSafari.app/Info.plist", "/Applications/Calculator.app/Info.plist"};
        for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
            vnode_t node = NULLVP;
            if (!vnode_lookup(paths[i], 0, &node, vfs_context_current())) {
                marble_app_union_visible |= 1U << i;
                vnode_put(node);
            }
        }
    }
    printf("MarbleApps: sealed Applications union status=%d visible=%x\n", error, marble_app_union_visible);
}

static int prepareData(proc_t process, user_addr_t scratch)
{
    publishSystemApps(process, scratch);
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
    if (!marble_app_union_status) native_marble_ls_store_rebuild(process, scratch);
    error = sheng_boot_watchdog_policy(process, scratch);
    if (error) return error;
    error = native_marble_setup_policy(process, scratch);
    if (error) return error;
    error = native_marble_metal_prepare(process, scratch);
    if (error) return error;
    static const char *const graphics_environment[] = {
        "CA_DISABLE_FRAMEBUFFER_COMPRESSION=1", "CA_DISPLAY_EDR_HEADROOM=1",
        "CA_FP16_MIN_HEADROOM=1e30", "CA_FORCE_FP16_FRAMEBUFFER=0", "CA_ALLOW_EDR_MIRRORING=0"};
    user64_addr_t pointers[6] = {0};
    user_addr_t cursor = scratch + 0x900;
    for (unsigned i = 0; i < sizeof(graphics_environment) / sizeof(graphics_environment[0]); ++i) {
        pointers[i] = cursor;
        size_t bytes = strlen(graphics_environment[i]) + 1;
        error = copyout(graphics_environment[i], cursor, bytes);
        if (error) return error;
        cursor += bytes;
    }
    *environment = scratch + 0xc00;
    error = copyout(pointers, *environment, sizeof(pointers));
    if (error) return error;
    dyld_flags |= UINT64_C(8);
    commpage_update_dyld_flags(dyld_flags);
    printf("MarbleStorage: SystemOS resident policy accepted, dyld flags %llx\n", dyld_flags);
    return 0;
}
