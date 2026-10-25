// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOService.h>
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSUnserialize.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHomeInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" void native_marble_touch_start(void);
extern "C" void native_marble_gpu_start(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" volatile uint32_t native_legacy_transfer_calls;
static uint32_t events[8], ticks, sent;
static int results[8], status = -1;
static uint64_t nextHome, frequency;
static unsigned homeDelay, homeCount, homeInterval;
static bool configured;
static unsigned requestedHome;
static bool registerTouchDriver(void) {
    const char personality[] =
        "<array><dict>"
        "<key>IOClass</key><string>IOHIDEventDriver</string>"
        "<key>IOProviderClass</key><string>IOHIDInterface</string>"
        "<key>CFBundleIdentifier</key><string>com.apple.iokit.IOHIDFamily</string>"
        "<key>HIDDefaultBehavior</key><string></string>"
        "<key>DeviceUsagePairs</key><array><dict>"
        "<key>DeviceUsagePage</key><integer>13</integer>"
        "<key>DeviceUsage</key><integer>4</integer>"
        "</dict></array></dict></array>";
    OSArray *drivers = OSDynamicCast(OSArray, OSUnserializeXML(personality));
    bool result = drivers && gIOCatalogue->addDrivers(drivers, true);
    OSSafeReleaseNULL(drivers);
    return result;
}
extern "C" void native_hid_request_home(void) {
    __atomic_fetch_add(&requestedHome, 1, __ATOMIC_RELAXED);
}

extern "C" void native_hid_input_observed(unsigned event, int result) {
    if (event < 8) { events[event]++; results[event] = result; }
}
extern "C" void native_hid_live_checkpoint(IOService *) { ticks++; }
extern "C" bool native_hid_input_requested(void) {
    if (native_legacy_transfer_calls < 2) return false;
    if (__atomic_load_n(&requestedHome, __ATOMIC_RELAXED)) {
        __atomic_fetch_sub(&requestedHome, 1, __ATOMIC_RELAXED);
        return true;
    }
    uint64_t now;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));
    if (!configured) {
        configured = true; homeCount = 1; homeInterval = 20;
        PE_parse_boot_argn("offline-hid-auto-home", &homeDelay, sizeof(homeDelay));
        PE_parse_boot_argn("offline-hid-auto-home-count", &homeCount, sizeof(homeCount));
        PE_parse_boot_argn("offline-hid-auto-home-interval", &homeInterval, sizeof(homeInterval));
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
        nextHome = now + (uint64_t)homeDelay * frequency;
    }
    if (!homeDelay || sent >= homeCount || now < nextHome) return false;
    sent++; nextHome += (uint64_t)homeInterval * frequency;
    return true;
}
extern "C" void native_hid_report(char *out, unsigned size) {
    snprintf(out, size, "HID status=%x ticks=%u home=%u reports=%u/%u results=%x/%x matched=%d touch=%u result=%x touch_matched=%d",
        status, ticks, sent, events[4], events[5], results[4], results[5], results[3], events[6], results[6], results[7]);
}
extern "C" void native_hid_home_start(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("offline-hid-home", &enabled, sizeof(enabled)) || enabled != 1) return;
    status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if (!status) status = OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver", true, false,
        kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
    if (status) panic("RootStage: HID family load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    bool valid = meta && meta->getClassSize() == 208;
    OSSafeReleaseNULL(name);
    if (!valid) panic("RootStage: HID device ABI differs");
    OSDictionary *matching = IOService::serviceMatching("IOHIDResource");
    IOService *resource = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!resource) panic("RootStage: HID resource unavailable");
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("RootStage: HID metaclass registration unavailable");
    native_hid_static_init(); status = OSMetaClass::postModLoad(handle);
    if (status) panic("RootStage: HID metaclass registration failed %x", status);
    IOService *device = native_hid_input_allocate();
    if (!device || !device->init() || !device->attach(resource) || !device->start(resource))
        panic("RootStage: HID input start failed");
    unsigned touch = 0;
    if (PE_parse_boot_argn("marble-touch", &touch, sizeof(touch)) && touch) {
        IOLog("MarbleTouch: event driver personality=%u\n", registerTouchDriver());
        IOService *digitizer = native_hid_touch_allocate();
        if (digitizer && digitizer->init() && digitizer->attach(resource) && digitizer->start(resource)) {
            if (touch == 1) native_marble_touch_start();
            else IOLog("MarbleTouch: HID ready, awaiting runtime start\n");
        } else IOLog("MarbleTouch: HID digitizer start failed\n");
    }
    resource->release();
    unsigned gpu = 0;
    if (PE_parse_boot_argn("marble-gpu", &gpu, sizeof(gpu)) && gpu == 1)
        native_marble_gpu_start();
    IOLog("RootStage: virtual HID consumer input published\n");
}
