// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <sys/msgbuf.h>
#include <sys/param.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>

extern "C" {
void native_runtime_usb_start(void);
void native_runtime_usb_report(char *, unsigned);
void native_marble_touch_report(char *, unsigned);
unsigned native_marble_charging_report(char *, unsigned);
void native_marble_gpu_report(char *, unsigned);
void native_marble_display_snapshot(void);
extern uint32_t *marble_display_pixels;
extern uint32_t marble_display_geometry[3];
extern volatile uint32_t marble_display_transfers;
extern uint64_t marble_data_bytes;
extern volatile uint64_t marble_ufs_reads, marble_ufs_writes;
void marble_runtime18_start(void);
void native_marble_reboot_bootloader(void);
void native_root_block_capture_log(void);
void native_root_block_request_snapshot(void);
unsigned native_root_runtime_read(unsigned, unsigned, void *, unsigned);
}

struct RuntimeLog {
    char magic[8];
    uint32_t version, length;
    char text[65520];
};
static RuntimeLog residentLog;
static RuntimeLog *const runtimeLog = &residentLog;
struct RuntimeScreen {
    char magic[8];
    uint32_t width, height, stride, bytes;
    uint64_t frames;
    uint8_t pixels[270 * 600 * 3];
};
static RuntimeScreen *runtimeScreen;
static IOLock *gpuExportLock;
static uint8_t *gpuExport;
static uint32_t gpuExportBytes, gpuExportFilled, gpuExportGeneration;

extern "C" int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset,
                                    const void *data, uint32_t size)
{
    if (!gpuExportLock || !generation || !total || !data || !size || offset > total || size > total - offset)
        return -1;
    IOLockLock(gpuExportLock);
    int status = 0;
    if (!offset) {
        uint8_t *next = static_cast<uint8_t *>(IOMalloc(total));
        if (!next) status = -2;
        else {
            if (gpuExport) IOFree(gpuExport, gpuExportBytes);
            gpuExport = next; gpuExportBytes = total; gpuExportFilled = 0; gpuExportGeneration = generation;
        }
    }
    if (!status && (generation != gpuExportGeneration || total != gpuExportBytes || offset != gpuExportFilled))
        status = -3;
    if (!status) {
        memcpy(gpuExport + offset, data, size);
        gpuExportFilled += size;
    }
    IOLockUnlock(gpuExportLock);
    return status;
}

extern "C" void native_root_block_capture_log(void)
{
    memcpy(runtimeLog->magic, "ROOTLOG1", 8);
    runtimeLog->version = 1;
    unsigned used = 0;
    if (msgbufp && msgbufp->msg_size) {
        unsigned count = MIN(static_cast<unsigned>(msgbufp->msg_size), unsigned(sizeof(runtimeLog->text) - 4096));
        unsigned start = (msgbufp->msg_bufx + msgbufp->msg_size - count) % msgbufp->msg_size;
        for (unsigned i = 0; i < count; ++i) {
            char c = msgbufp->msg_bufc[(start + i) % msgbufp->msg_size];
            if (c) runtimeLog->text[used++] = c;
        }
    }
    used += native_marble_charging_report(runtimeLog->text + used, 2048);
    char status[512];
    native_runtime_usb_report(status, sizeof(status));
    used += snprintf(runtimeLog->text + used, sizeof(runtimeLog->text) - used, "%s\n", status);
    native_marble_touch_report(status, sizeof(status));
    used += snprintf(runtimeLog->text + used, sizeof(runtimeLog->text) - used, "%s\nUFS reads=%llu writes=%llu Data=%llu\n",
        status, marble_ufs_reads, marble_ufs_writes, marble_data_bytes);
    native_marble_gpu_report(status, sizeof(status));
    used += snprintf(runtimeLog->text + used, sizeof(runtimeLog->text) - used, "%s\n", status);
    runtimeLog->length = used;
    // The firmware transport runs on a reserved core after the kernel returns.
    // Publish the diagnostic bytes to the point of coherency before cache/MMU handoff.
    auto publish = [](const void *address, size_t length) {
        uintptr_t first = reinterpret_cast<uintptr_t>(address) & ~uintptr_t(63);
        uintptr_t end = reinterpret_cast<uintptr_t>(address) + length;
        for (uintptr_t line = first; line < end; line += 64)
            __asm__ volatile("dc civac, %0" :: "r"(line) : "memory");
    };
    if (msgbufp && msgbufp->msg_size) {
        publish(msgbufp->msg_bufc, msgbufp->msg_size);
        publish(msgbufp, sizeof(*msgbufp));
    }
    publish(&msgbufp, sizeof(msgbufp));
    publish(runtimeLog, sizeof(*runtimeLog));
    __asm__ volatile("dsb sy" ::: "memory");
}

extern "C" int native_boot_gpu_addon(const uint8_t **data, uint64_t *bytes)
{
    static IOMemoryMap *mapping;
    *data = nullptr; *bytes = 0;
    if (!mapping) {
        IORegistryEntry *node = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
        OSData *range = node ? OSDynamicCast(OSData, node->getProperty("GpuAddon")) : nullptr;
        uint64_t location[2] = {};
        if (range && range->getLength() == sizeof(location))
            memcpy(location, range->getBytesNoCopy(), sizeof(location));
        OSSafeReleaseNULL(node);
        if (!location[0] || !location[1] || location[1] > (UINT64_C(64) << 20)) return 0;
        IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(location[0], location[1],
            kIODirectionIn | kIOMemoryMapperNone, nullptr);
        mapping = memory ? memory->map(kIOMapAnywhere | kIOMapCopybackCache) : nullptr;
        OSSafeReleaseNULL(memory);
    }
    if (!mapping) return 0;
    *data = reinterpret_cast<const uint8_t *>(mapping->getVirtualAddress());
    *bytes = mapping->getLength();
    return 1;
}

extern "C" void native_root_block_request_snapshot(void)
{
    native_marble_display_snapshot();
}

extern "C" unsigned native_root_runtime_read(unsigned kind, unsigned offset, void *out, unsigned capacity)
{
    if (kind == 16) {
        if (!gpuExportLock) return 0;
        IOLockLock(gpuExportLock);
        unsigned count = gpuExportFilled == gpuExportBytes && offset < gpuExportBytes ?
            MIN(capacity, gpuExportBytes - offset) : 0;
        if (count) memcpy(out, gpuExport + offset, count);
        IOLockUnlock(gpuExportLock);
        return count;
    }
    const void *source;
    unsigned bytes;
    struct {
        char magic[8];
        uint64_t uptime, firstFrame, reads, frames, resident, logical;
    } status = {};
    if (kind == 5) {
        memcpy(status.magic, "ROOTRUN1", 8);
        absolutetime_to_nanoseconds(mach_absolute_time(), &status.uptime);
        status.reads = marble_ufs_reads;
        status.frames = marble_display_transfers;
        status.logical = marble_data_bytes;
        source = &status;
        bytes = sizeof(status);
    } else if (kind == 1) {
        if (!offset) native_root_block_capture_log();
        source = runtimeLog;
        bytes = 16 + runtimeLog->length;
    } else if (kind == 2) {
        if (!offset) {
            native_marble_display_snapshot();
            if (!marble_display_pixels) return 0;
            memcpy(runtimeScreen->magic, "ROOTRGB1", 8);
            runtimeScreen->width = marble_display_geometry[0] / 4;
            runtimeScreen->height = marble_display_geometry[1] / 4;
            runtimeScreen->stride = runtimeScreen->width * 3;
            runtimeScreen->bytes = runtimeScreen->stride * runtimeScreen->height;
            runtimeScreen->frames = marble_display_transfers;
            if (runtimeScreen->bytes > sizeof(runtimeScreen->pixels)) return 0;
            for (unsigned y = 0; y < runtimeScreen->height; ++y)
                for (unsigned x = 0; x < runtimeScreen->width; ++x) {
                    uint32_t pixel = marble_display_pixels[y * marble_display_geometry[2] + x * 4];
                    unsigned at = y * runtimeScreen->stride + x * 3;
                    runtimeScreen->pixels[at] = pixel >> 16;
                    runtimeScreen->pixels[at + 1] = pixel >> 8;
                    runtimeScreen->pixels[at + 2] = pixel;
                }
        }
        source = runtimeScreen;
        bytes = 32 + runtimeScreen->bytes;
    } else return 0;
    if (offset >= bytes) return 0;
    unsigned count = MIN(capacity, bytes - offset);
    memcpy(out, static_cast<const uint8_t *>(source) + offset, count);
    return count;
}

extern "C" void marble_runtime18_start(void)
{
    unsigned reboot = 0;
    if (PE_parse_boot_argn("marble-reboot-bootloader", &reboot, sizeof(reboot)) && reboot)
        native_marble_reboot_bootloader();
    gpuExportLock = IOLockAlloc();
    if (!gpuExportLock) panic("MarbleRuntime: GPU export lock allocation failed");
    runtimeScreen = static_cast<RuntimeScreen *>(IOMallocZero(sizeof(RuntimeScreen)));
    if (!runtimeScreen) panic("MarbleRuntime: USB screen allocation failed");
    native_runtime_usb_start();
}
