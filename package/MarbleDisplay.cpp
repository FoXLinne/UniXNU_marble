// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_marble_display_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" void native_marble_charging_start(void);
extern "C" void native_marble_hid_start(void);
extern "C" void native_keyevents_start(void);
extern "C" void native_marble_touch_report(char *, unsigned);

extern "C" {
volatile uint32_t marble_display_stage, marble_display_registered, marble_display_clients;
volatile uint32_t marble_display_transfers, marble_display_retirements, marble_display_frames;
volatile uint32_t marble_display_swap_stage, marble_display_validation, marble_display_rejections;
extern volatile uint32_t native_legacy_surfaces[14], native_legacy_direct[14];
volatile uint64_t marble_display_mapping, marble_display_bytes, marble_display_copy_ticks;
uint32_t *marble_display_pixels;
uint32_t marble_display_geometry[3];
uint32_t marble_display_snapshot_ready;
char marble_touch_status[256];
volatile uint64_t marble_display_frame[6], marble_display_surface[7];
volatile uint32_t marble_display_pm[3], marble_display_vsync[2];
volatile int marble_display_client_status, marble_display_transfer_status;
volatile uint32_t native_ufs_dpu_state;
struct MarbleDisplayTrace { uint32_t event, pid, count; uint64_t values[12]; };
MarbleDisplayTrace marble_display_trace[256];
uint32_t marble_display_trace_count;

int native_gpu_trace_enabled(void) { return 1; }
void native_gpu_trace_record(uint32_t event, uint32_t pid, const uint64_t *values, uint32_t count)
{
    unsigned index = __atomic_fetch_add(&marble_display_trace_count, 1U, __ATOMIC_RELAXED) % 256;
    MarbleDisplayTrace *record = &marble_display_trace[index];
    record->event = event; record->pid = pid; record->count = count < 12 ? count : 12;
    for (unsigned i = 0; i < record->count; ++i) record->values[i] = values[i];
}
bool native_adreno_fences(uint32_t *, uint32_t *) { return false; }
bool native_adreno_fences_detail(uint32_t *, uint32_t *, uint32_t *) { return false; }
uint32_t native_ufs_dpu_map(unsigned, const uint64_t *, unsigned) { return 0; }

__attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot, void *object, void *caller)
{
    panic("MarbleDisplay: backend slot %u object %p caller %p", slot, object, caller);
}

bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *mapping, uint32_t width, uint32_t height)
{
    marble_display_mapping = mapping->getVirtualAddress();
    marble_display_bytes = mapping->getLength();
    PE_Video video = {};
    if (PE_current_console(&video)) return false;
    marble_display_pixels = static_cast<uint32_t *>(IOMalloc(mapping->getLength()));
    if (!marble_display_pixels) return false;
    marble_display_geometry[0] = width;
    marble_display_geometry[1] = height;
    marble_display_geometry[2] = static_cast<uint32_t>(video.v_rowBytes);
    IOLog("MarbleDisplay: GOP %ux%u physical %llx bytes %llu\n", width, height,
          mapping->getPhysicalAddress(), mapping->getLength());
    return true;
}

void native_marble_display_snapshot(void)
{
    native_marble_touch_report(marble_touch_status, sizeof(marble_touch_status));
    if (!marble_display_mapping || !marble_display_pixels) return;
    const volatile uint32_t *source = reinterpret_cast<const volatile uint32_t *>(marble_display_mapping);
    for (uint64_t i = 0; i < marble_display_bytes / 4; ++i) marble_display_pixels[i] = source[i];
    marble_display_snapshot_ready = 1;
}

void native_legacy_registration_observed(IOService *display)
{
    ++marble_display_registered;
    IOLog("MarbleDisplay: registered %s\n", display->getName());
}

void native_legacy_swap_stage(unsigned stage) { marble_display_swap_stage = stage; }
void native_legacy_client_enter(task_t, uint32_t type)
{
    ++marble_display_clients;
    IOLog("MarbleDisplay: client type %u\n", type);
}
void native_legacy_client_observed(IOReturn status, IOUserClient *)
{
    marble_display_client_status = status;
    IOLog("MarbleDisplay: client status %x\n", status);
}
void native_legacy_pm_observed(unsigned stage, IOReturn status, unsigned long ordinal)
{
    marble_display_pm[0] = stage; marble_display_pm[1] = status; marble_display_pm[2] = static_cast<uint32_t>(ordinal);
}
void native_legacy_frame_observed(unsigned kind, uint64_t count, uint64_t minx, uint64_t miny, uint64_t maxx, uint64_t maxy)
{
    ++marble_display_frames;
    const uint64_t values[] = {kind, count, minx, miny, maxx, maxy};
    for (unsigned i = 0; i < 6; ++i) marble_display_frame[i] = values[i];
}
void native_legacy_frame_colors_observed(const uint32_t *colors, const uint64_t *counts, uint64_t other)
{
    IOLog("MarbleDisplay: frame %u color %x pixels %llu other %llu\n",
          marble_display_frames, colors[0], counts[0], other);
}
void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *, const void *)
{
    marble_display_validation = reason;
    if (!accepted) ++marble_display_rejections;
}
void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format, uint32_t planes,
                                    uint64_t stride, uint64_t allocation, uint64_t bytes)
{
    const uint64_t values[] = {width, height, format, planes, stride, allocation, bytes};
    for (unsigned i = 0; i < 7; ++i) marble_display_surface[i] = values[i];
    IOLog("MarbleDisplay: rejected surface %llux%llu format %x stride %llu\n", width, height, format, stride);
}
bool native_legacy_prepare_scanout(void)
{
    PE_initialize_console(nullptr, kPEDisableScreen);
    return true;
}
void native_legacy_transfer_observed(IOReturn status, const void *, uint64_t bytes)
{
    ++marble_display_transfers; marble_display_transfer_status = status;
    IOLog("MarbleDisplay: transfer %u status %x bytes %llu\n", marble_display_transfers, status, bytes);
}
void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t) { marble_display_copy_ticks += ticks; }
void native_legacy_retirement_observed(IOService *) { ++marble_display_retirements; }
bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t, uint32_t)
{
    return false;
}
void native_legacy_gpu_fallback_observed(int status) { native_legacy_direct[0] = status; }
void native_legacy_vsync_observed(unsigned event, unsigned hz)
{
    marble_display_vsync[0] = event; marble_display_vsync[1] = hz;
}

void native_marble_display_start(void)
{
    marble_display_stage = 1;
    kern_return_t status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
    if (status) panic("MarbleDisplay: family load %x", status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("MarbleDisplay: class initialization unavailable");
    native_marble_display_init();
    status = OSMetaClass::postModLoad(handle);
    if (status) panic("MarbleDisplay: class registration %x", status);
    status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    if (status) panic("MarbleDisplay: IOSurface load %x", status);
    OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (!surface) panic("MarbleDisplay: IOSurface root unavailable");
    surface->release(); matching->release();
    PE_Video video = {};
    if (PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height ||
        video.v_depth != 32 || video.v_rotate || video.v_rowBytes < video.v_width * 4 ||
        video.v_rowBytes > UINT64_MAX / video.v_height)
        panic("MarbleDisplay: GOP handoff invalid");
    marble_display_stage = 2;
    IOPlatformDevice *nub = new IOPlatformDevice;
    IOService *parent = IOService::getPlatform();
    IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
    OSArray *ranges = OSArray::withCapacity(1);
    if (!nub || !parent || !memory || !ranges || !nub->init()) panic("MarbleDisplay: provider allocation");
    ranges->setObject(memory); nub->setDeviceMemory(ranges); nub->setName("NativeGOPFramebuffer");
    ranges->release(); memory->release();
    if (!nub->attach(parent) || !nub->start(parent)) panic("MarbleDisplay: provider start");
    IORegistryEntry *armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    const char type[] = "display-subsystem";
    OSData *data = OSData::withBytes(type, sizeof(type));
    if (!armio || !data || !nub->setProperty("device_type", data)) panic("MarbleDisplay: device tree provider");
    nub->setName("gop-display", gIODTPlane);
    if (!nub->attachToParent(armio, gIODTPlane)) panic("MarbleDisplay: device tree attach");
    data->release(); armio->release();
    IOService *display = native_legacy_display_allocate();
    if (!display || !display->init()) panic("MarbleDisplay: display initialization");
    OSString *grant = OSString::withCString("backboardd,SpringBoard,mediaserverd");
    if (!grant || !display->setProperty("NativeSandboxGrantProcess", grant)) panic("MarbleDisplay: access policy");
    grant->release();
    marble_display_stage = 3;
    SInt32 score = 0;
    if (display->probe(nub, &score) != display || !display->attach(nub)) panic("MarbleDisplay: probe");
    marble_display_stage = 4;
    if (!display->start(nub)) panic("MarbleDisplay: start");
    marble_display_stage = 5;
    IOLog("MarbleDisplay: display started\n");
    native_keyevents_start();
    native_marble_charging_start();
    native_marble_hid_start();
}
}
