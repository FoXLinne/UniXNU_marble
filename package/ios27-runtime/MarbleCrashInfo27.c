// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/vnode_internal.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/kalloc.h>
#include <mach/task_info.h>
#include <arm/thread.h>

extern int native_root_block_authorized(dev_t);

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

/* CPU mapping history for a live Metal resource whose cached pointer faults. */
#define MARBLE_CPU_MAP_RECORDS 1024U
uint64_t (*marble_buffer_mapping_events)[16];
uint32_t marble_buffer_mapping_count;
uint32_t marble_glass_mapping_history_count;
uint64_t marble_glass_mapping_history[32][16];
void native_marble_buffer_mapping_event(uint32_t pid, uint32_t kind, uint64_t owner,
    uint64_t address, uint64_t bytes, uint64_t handle, uint64_t token)
{
    if (!pid || !address || !bytes) return;
    uint64_t (*events)[16] = __atomic_load_n(&marble_buffer_mapping_events, __ATOMIC_ACQUIRE);
    if (!events) {
        uint64_t (*made)[16] = kalloc_data(MARBLE_CPU_MAP_RECORDS * 16 * sizeof(uint64_t), Z_WAITOK | Z_ZERO);
        if (!made) return;
        uint64_t (*expected)[16] = NULL;
        if (!__atomic_compare_exchange_n(&marble_buffer_mapping_events, &expected, made, false,
                __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            kfree_data(made, MARBLE_CPU_MAP_RECORDS * 16 * sizeof(uint64_t));
            events = expected;
        } else events = made;
    }
    unsigned ordinal = __atomic_fetch_add(&marble_buffer_mapping_count, 1, __ATOMIC_RELAXED);
    uint64_t *row = events[ordinal % MARBLE_CPU_MAP_RECORDS];
    bzero(row, 16 * sizeof(uint64_t));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(row[1]));
    row[2] = pid; row[3] = kind; row[4] = owner; row[5] = address;
    row[6] = bytes; row[7] = handle; row[8] = token;
    struct arm_saved_state *state = get_user_regs(current_thread());
    if (state && (uint32_t)proc_selfpid() == pid) {
        const unsigned char *raw = (const unsigned char *)state;
        memcpy(&row[9], raw + 264, 8);
        memcpy(&row[10], raw + 248, 8);
        memcpy(&row[11], raw + 240, 8);
        uint64_t frame = row[11] & ((UINT64_C(1) << 39) - 1);
        for (unsigned i = 12; i < 16 && frame && !(frame & 15); ++i) {
            uint64_t links[2];
            if (copyin(frame, links, sizeof(links))) break;
            row[i] = links[1] & ((UINT64_C(1) << 39) - 1);
            uint64_t next = links[0] & ((UINT64_C(1) << 39) - 1);
            if (next <= frame || next - frame > 1048576) break;
            frame = next;
        }
    }
    __atomic_store_n(&row[0], ordinal + 1, __ATOMIC_RELEASE);
}
static void captureGlassMappingHistory(uint32_t pid, uint64_t address)
{
    uint64_t (*events)[16] = __atomic_load_n(&marble_buffer_mapping_events, __ATOMIC_ACQUIRE);
    unsigned end = __atomic_load_n(&marble_buffer_mapping_count, __ATOMIC_ACQUIRE);
    unsigned count = end < MARBLE_CPU_MAP_RECORDS ? end : MARBLE_CPU_MAP_RECORDS;
    if (!events) return;
    for (unsigned i = 0; i < count && marble_glass_mapping_history_count < 32; ++i) {
        const uint64_t *row = events[(end - 1 - i) % MARBLE_CPU_MAP_RECORDS];
        uint64_t sequence = __atomic_load_n(&row[0], __ATOMIC_ACQUIRE), saved[16];
        memcpy(saved, row, sizeof(saved));
        if (!sequence || sequence != __atomic_load_n(&row[0], __ATOMIC_ACQUIRE) ||
            saved[2] != pid || saved[5] > address || address - saved[5] >= saved[6]) continue;
        memcpy(marble_glass_mapping_history[marble_glass_mapping_history_count++], saved, sizeof(saved));
    }
}

/* Retain the 24B119 glass buffer graph before user-map teardown. NBBuffer's
 * handle ivar is +24 in the selected NativeAdrenoMetal payload. */
struct marble_glass_part {
    uint64_t address;
    uint32_t bytes;
    int32_t error;
    char label[16];
    unsigned char data[512];
};
struct marble_glass_capture {
    uint64_t magic, pid, pc, far, esr, slide;
    uint32_t count, reserved;
    struct marble_glass_part parts[16];
};
struct marble_glass_capture marble_glass_fault;
static struct marble_glass_part *glassPart(const char *label, uint64_t address, unsigned bytes)
{
    address &= (UINT64_C(1) << 39) - 1;
    if (marble_glass_fault.count >= 16 || !address || bytes > 512) return NULL;
    struct marble_glass_part *part = &marble_glass_fault.parts[marble_glass_fault.count++];
    part->address = address;
    part->bytes = bytes;
    strlcpy(part->label, label, sizeof(part->label));
    part->error = copyin(address, part->data, bytes);
    return part;
}
static void captureGlassFault(const struct marble_ui_exit *record, uint64_t slide)
{
    uint64_t pc, far, esr = 0, context, buffer, pointer;
    memcpy(&pc, record->state + 264, 8);
    if (marble_glass_fault.magic || record->reason_namespace != 2 || record->reason_code != 11 ||
        strcmp(record->name, "backboardd") || pc != slide + UINT64_C(0x1843743fc)) return;
    memcpy(&far, record->state + 280, 8);
    memcpy(&esr, record->state + 288, 4);
    memcpy(&context, record->state + 160, 8);
    memcpy(&buffer, record->state + 192, 8);
    memcpy(&pointer, record->state + 232, 8);
    marble_glass_fault.pid = record->pid;
    marble_glass_fault.pc = pc; marble_glass_fault.far = far;
    marble_glass_fault.esr = esr; marble_glass_fault.slide = slide;
    struct marble_glass_part *node = glassPart("CA-buffer", buffer, 64);
    glassPart("fault-pointer", pointer, 64);
    glassPart("CA-context", context, 64);
    uint64_t pool = 0;
    if (!copyin((context & ((UINT64_C(1) << 39) - 1)) + 0x1428, &pool, 8))
        glassPart("CA-pool", pool, 384);
    if (node && !node->error) {
        uint64_t owner, metal;
        memcpy(&owner, node->data + 24, 8);
        memcpy(&metal, node->data + 32, 8);
        glassPart("CA-pool-block", owner, 64);
        struct marble_glass_part *object = glassPart("NBBuffer", metal, 96);
        if (object && !object->error) {
            uint64_t handle;
            memcpy(&handle, object->data + 24, 8);
            struct marble_glass_part *resource = glassPart("NmbBuffer", handle, 128);
            if (resource && !resource->error) {
                uint64_t words[8];
                memcpy(words, resource->data, sizeof(words));
                for (unsigned i = 0; i < 8; ++i) {
                    uint64_t address = words[i] & ((UINT64_C(1) << 39) - 1);
                    if (address < UINT64_C(0x100000000) || (address & 7)) continue;
                    char label[16];
                    snprintf(label, sizeof(label), "resource-%u", i);
                    struct marble_glass_part *child = glassPart(label, address, 512);
                    if (i == 1 && words[0] == 1 && child && !child->error) {
                        uint64_t runtime;
                        memcpy(&runtime, child->data + 56, 8);
                        struct marble_glass_part *shared = glassPart("RuntimeBuffer", runtime, 512);
                        if (shared && !shared->error) {
                            uint64_t links[12];
                            memcpy(links, shared->data, sizeof(links));
                            for (unsigned j = 2; j < 12; ++j) {
                                uint64_t pointer = links[j] & ((UINT64_C(1) << 39) - 1);
                                if (pointer < UINT64_C(0x100000000) || (pointer & 7)) continue;
                                snprintf(label, sizeof(label), "runtime-%u", j);
                                glassPart(label, pointer, 512);
                            }
                        }
                    }
                }
            }
        }
    }
    captureGlassMappingHistory(record->pid, far);
    __atomic_store_n(&marble_glass_fault.magic, UINT64_C(0x314655424c47434d), __ATOMIC_RELEASE);
}

static void captureAbortReason(char *out, size_t capacity, user_addr_t image_info,
    uint64_t image_info_size, uint64_t cache_slide)
{
#ifdef MARBLE_IOS27_GPU
    /* 24B119 shared cache (UUID 555BFDAA-D930-31AC-A915-D472C273C527):
     * Foundation, CoreFoundation, libswiftCore, libc++abi and libsystem_c
     * __crash_info v5 section bases. swift/Runtime/Debug.h defines message and
     * message2. Read the recorded strings at exit instead of faulting process
     * memory through a heap scan. */
    static const uint8_t expected_uuid[16] = {0x55, 0x5b, 0xfd, 0xaa, 0xd9, 0x30, 0x31, 0xac, 0xa9, 0x15, 0xd4, 0x72, 0xc2, 0x73, 0xc5, 0x27};
    static const uint64_t annotations[] = {
        0x1eaf990a8, 0x1eaf8a188, 0x1eb00e958, 0x2da9270e8, 0x1eb1c5408
    };
    uint8_t uuid[16];
    if (!capacity || !image_info || image_info_size < 176 ||
        copyin(image_info + 160, uuid, sizeof(uuid)) || memcmp(uuid, expected_uuid, sizeof(uuid))) return;
    char message[1024];
    /* libobjc publishes its legacy crash string through its own
     * __crash_info+8 message pointer, written by _objc_crashlog. */
    uint64_t objc_message = 0;
    if (!copyin(0x1eaf700c8 + cache_slide, &objc_message, sizeof(objc_message)) && objc_message) {
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
    if (current_proc() == process) captureGlassFault(record, cache_slide);
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
    if (reason_namespace == 2 && (reason_code == 6 || reason_code == 5) && current_proc() == process)
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

/* Retained syscall results for the DriverKit daemon's executable-path lookup. */
#include <sys/syscall.h>
#include <sys/fcntl.h>
struct marble_path_call {
    uint64_t ordinal, arguments[8];
    uint32_t pid, call;
    int32_t error, result0, result1;
    uint32_t reserved;
    char input[192], output[192];
};
struct marble_path_call *marble_path_calls;
unsigned marble_path_call_count;
__attribute__((used)) const unsigned marble_path_call_bytes = sizeof(struct marble_path_call);
void native_marble_path_trace(proc_t process, unsigned code, int error,
    const uint64_t *arguments, const int32_t *results)
{
    const char *name = proc_best_name(process);
    if (!strcmp(name, "backboardd")) {
        if (!error && code == SYS_munmap)
            native_marble_buffer_mapping_event((uint32_t)proc_getpid(process), 4, 0,
                arguments[0], arguments[1], 0, 0);
        if (!error && code == SYS_mmap && arguments[1] >= 65536)
            native_marble_buffer_mapping_event((uint32_t)proc_getpid(process), 5, 0,
                (uint32_t)results[0] | (uint64_t)(uint32_t)results[1] << 32, arguments[1], 0, 0);
        return;
    }
    bool driver = !strcmp(name, "driverkitd");
    bool poster = !strcmp(name, "PosterBoard") || !strcmp(name, "Preferences") ||
        !strcmp(name, "GradientPosterExtension") || !strcmp(name, "NullPoster");
    if (!driver && !poster) return;
    /* Preserve file operations and failures for the wallpaper save transaction. */
    if (poster && !error) {
        switch (code) {
        case SYS_open: case SYS_open_nocancel: case SYS_openat: case SYS_openat_nocancel:
        case SYS_mkdir: case SYS_mkdirat: case SYS_rename: case SYS_renameat:
        case SYS_unlink: case SYS_unlinkat: case SYS_write: case SYS_write_nocancel:
        case SYS_fsync: case SYS_fcntl: case SYS_fchmod: case SYS_fchown:
            break;
        default: return;
        }
    }
    struct marble_path_call *calls = __atomic_load_n(&marble_path_calls, __ATOMIC_ACQUIRE);
    if (!calls) {
        struct marble_path_call *allocated = kalloc_data(sizeof(*calls) * 1024, Z_WAITOK | Z_ZERO);
        if (!allocated) return;
        struct marble_path_call *expected = NULL;
        if (!__atomic_compare_exchange_n(&marble_path_calls, &expected, allocated, false,
                __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            kfree_data(allocated, sizeof(*calls) * 1024);
            calls = expected;
        } else calls = allocated;
    }
    unsigned ordinal = __atomic_fetch_add(&marble_path_call_count, 1, __ATOMIC_RELAXED);
    struct marble_path_call *record = &calls[ordinal % 1024];
    bzero(record, sizeof(*record));
    memcpy(record->arguments, arguments, sizeof(record->arguments));
    record->pid = (uint32_t)proc_getpid(process);
    record->call = code; record->error = error;
    record->result0 = results[0]; record->result1 = results[1];
    user_addr_t input = 0, output = 0;
    bool binary = false;
    size_t count = 0;
    switch (code) {
    case SYS_open: case SYS_open_nocancel: case SYS_stat64: case SYS_lstat64:
    case SYS_access: case SYS_getattrlist: case SYS_readlink:
    case SYS_mkdir: case SYS_rename: case SYS_unlink:
        input = arguments[0]; break;
    case SYS_openat: case SYS_openat_nocancel: case SYS_fstatat64: case SYS_readlinkat:
    case SYS_mkdirat: case SYS_renameat: case SYS_unlinkat:
        input = arguments[1]; break;
    }
    if (input) (void)copyinstr(input, record->input, sizeof(record->input), &count);
    if (!error) {
        if (code == SYS_fsgetpath) output = arguments[0];
        if (code == SYS_fcntl && arguments[1] == F_GETPATH) output = arguments[2];
        if (code == SYS_proc_info && arguments[0] == 2 && arguments[2] == 11) output = arguments[4];
        if (code == SYS_getattrlist) { output = arguments[2]; binary = true; }
        if (output) {
            if (binary) (void)copyin(output, record->output, MIN(arguments[3], sizeof(record->output)));
            else (void)copyinstr(output, record->output, sizeof(record->output), &count);
        }
    }
    __atomic_store_n(&record->ordinal, ordinal + 1, __ATOMIC_RELEASE);
}
