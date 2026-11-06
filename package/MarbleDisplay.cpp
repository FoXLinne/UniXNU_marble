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
#include <kern/thread.h>

extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_marble_display_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" void native_marble_charging_start(void);
extern "C" void native_marble_hid_start(void);
extern "C" void native_marble_gpu_start(void);
extern "C" int native_marble_gpu_wait(void);
extern "C" IOService *native_gop_presenter_create(void);
extern "C" void native_gop_presenter_set_source(IOService *, IOService *);
extern "C" bool native_gop_gpu_offer(IOService *, void *, IOMemoryDescriptor *, uint64_t,
                                     uint32_t, uint32_t, uint32_t);
extern "C" void marble_gop_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");
static IOService *gpuPresenter;
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
uint64_t marble_display_timing[17];
volatile uint32_t marble_display_timing_state, marble_display_timing_error;
char marble_touch_status[256];
volatile uint64_t marble_display_frame[6], marble_display_surface[7];
volatile uint32_t marble_display_pm[3], marble_display_vsync[2];
volatile int marble_display_client_status, marble_display_transfer_status;
volatile uint32_t native_ufs_dpu_state;
struct MarbleDisplayTrace { uint32_t event, pid, count; uint64_t values[12]; };
MarbleDisplayTrace marble_display_trace[256];
uint32_t marble_display_trace_count;

#ifndef MARBLE_IOS18_GPU
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
#endif
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

static void displayTimingWorker(void *, wait_result_t)
{
    // Marble's DT places the two DSI interfaces at MDSS+0x36000/0x37000.
    // A live frame transfer establishes the display path before this request.
    auto *memory = IODeviceMemory::withRange(0xae00000, 0x38000);
    auto *mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!mapping) {
        marble_display_timing_error = kIOReturnNoMemory;
        __atomic_store_n(&marble_display_timing_state, 3U, __ATOMIC_RELEASE);
        thread_terminate(current_thread());
        return;
    }
    static constexpr uint32_t offsets[] = {0x26c, 0, 0xa8, 0xac, 0xb0, 8, 0xc};
    const auto *registers = reinterpret_cast<const volatile uint32_t *>(mapping->getVirtualAddress());
    uint64_t result[17] = {};
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0\n\tmrs %1, cntfrq_el0" : "=r"(result[0]), "=r"(result[1]));
    for (unsigned interface = 0; interface < 2; ++interface)
        for (unsigned index = 0; index < 7; ++index)
            result[2 + interface * 7 + index] = registers[(0x36000 + interface * 0x1000 + offsets[index]) / 4];
    __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0" : "=r"(result[16]) :: "memory");
    memcpy(marble_display_timing, result, sizeof(result));
    mapping->release();
    __atomic_store_n(&marble_display_timing_state, 2U, __ATOMIC_RELEASE);
    thread_terminate(current_thread());
}

void native_marble_display_timing_request(void)
{
    unsigned authorized = 0;
    if (!PE_parse_boot_argn("marble-display-mmio", &authorized, sizeof(authorized)) || authorized != 1) {
        marble_display_timing_error = kIOReturnNotPermitted;
        __atomic_store_n(&marble_display_timing_state, 3U, __ATOMIC_RELEASE);
        return;
    }
    if (!marble_display_transfers) {
        marble_display_timing_error = kIOReturnNotReady;
        __atomic_store_n(&marble_display_timing_state, 3U, __ATOMIC_RELEASE);
        return;
    }
    uint32_t previous = __atomic_load_n(&marble_display_timing_state, __ATOMIC_ACQUIRE);
    if (previous == 1 || !__atomic_compare_exchange_n(&marble_display_timing_state, &previous, 1U,
        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    marble_display_timing_error = 0;
    thread_t worker;
    kern_return_t status = kernel_thread_start(displayTimingWorker, nullptr, &worker);
    if (!status) thread_deallocate(worker);
    else {
        marble_display_timing_error = status;
        __atomic_store_n(&marble_display_timing_state, 3U, __ATOMIC_RELEASE);
    }
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
    if (status || marble_display_transfers == 1)
        IOLog("MarbleDisplay: transfer %u status %x bytes %llu\n", marble_display_transfers, status, bytes);
}
void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t) { marble_display_copy_ticks += ticks; }
void native_legacy_retirement_observed(IOService *) { ++marble_display_retirements; }
bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *memory, uint64_t stride,
                             uint32_t width, uint32_t height, uint32_t format)
{
    return gpuPresenter && native_gop_gpu_offer(gpuPresenter, display, memory, stride, width, height, format);
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
    void *module = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!module) panic("MarbleGPU: presenter registration");
    marble_gop_static_init();
    if (OSMetaClass::postModLoad(module)) panic("MarbleGPU: presenter metaclass");
    IOService *presenter = native_gop_presenter_create();
    IOService *home = IOService::getResourceService();
    if (!presenter || !home || !presenter->init()) panic("MarbleGPU: presenter allocation");
    native_gop_presenter_set_source(presenter, nub);
    if (!presenter->attach(home) || !presenter->start(home)) panic("MarbleGPU: presenter start");
    gpuPresenter = presenter;
    native_marble_gpu_start();
    int gpuStatus = native_marble_gpu_wait();
    if (gpuStatus) panic("MarbleGPU: accelerator startup status=%x", gpuStatus);
    native_keyevents_start();
    native_marble_charging_start();
    native_marble_hid_start();
}
}
