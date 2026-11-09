// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCatalogue.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSUnserialize.h>
#include <mach/kmod.h>

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_marble_hid_start(void);
extern "C" void native_marble_touch_start(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" IOService *native_hid_input_allocate(void);
extern "C" void native_marble_buttons_start(void);
extern "C" void native_marble_haptics_start(void);
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHomeInput.cpp");
extern "C" void native_hid_input_observed(unsigned, int);
extern "C" void native_hid_live_checkpoint(IOService *);
extern "C" bool native_hid_input_requested(void);
extern "C" {
volatile uint32_t marble_hid_stage, marble_hid_events[8], marble_hid_ticks;
volatile int marble_hid_results[8];
}

extern "C" void native_hid_input_observed(unsigned event, int result)
{
    if (event < 8) {
        marble_hid_events[event] = marble_hid_events[event] + 1;
        marble_hid_results[event] = result;
    }
}
extern "C" void native_hid_live_checkpoint(IOService *) { marble_hid_ticks = marble_hid_ticks + 1; }
extern "C" bool native_hid_input_requested(void) { return false; }

extern "C" void native_marble_hid_start(void)
{
    marble_hid_stage = 1;
    kern_return_t status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if (status) panic("MarbleHID: family load %x", status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("MarbleHID: class initialization unavailable");
    native_hid_static_init();
    status = OSMetaClass::postModLoad(handle);
    if (status) panic("MarbleHID: class registration %x", status);
    const char personality[] =
        "<array><dict><key>IOClass</key><string>IOHIDEventDriver</string>"
        "<key>IOProviderClass</key><string>IOHIDInterface</string>"
        "<key>CFBundleIdentifier</key><string>com.apple.iokit.IOHIDFamily</string>"
        "<key>HIDDefaultBehavior</key><string></string>"
        "<key>DeviceUsagePairs</key><array><dict>"
        "<key>DeviceUsagePage</key><integer>13</integer>"
        "<key>DeviceUsage</key><integer>4</integer></dict><dict>"
        "<key>DeviceUsagePage</key><integer>1</integer>"
        "<key>DeviceUsage</key><integer>6</integer></dict><dict>"
        "<key>DeviceUsagePage</key><integer>12</integer>"
        "<key>DeviceUsage</key><integer>1</integer></dict></array></dict></array>";
    OSArray *drivers = OSDynamicCast(OSArray, OSUnserializeXML(personality));
    if (!drivers || !gIOCatalogue->addDrivers(drivers, true)) panic("MarbleHID: digitizer personality");
    drivers->release();
    OSDictionary *matching = IOService::serviceMatching("IOHIDResource");
    IOService *resource = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!resource) panic("MarbleHID: resource unavailable");
    IOService *digitizer = native_hid_touch_allocate();
    if (!digitizer || !digitizer->init() || !digitizer->attach(resource) || !digitizer->start(resource))
        panic("MarbleHID: digitizer start");
    IOService *buttons = native_hid_input_allocate();
    if (!buttons || !buttons->init() || !buttons->attach(resource) || !buttons->start(resource))
        panic("MarbleHID: buttons start");
    resource->release();
    marble_hid_stage = 2;
    native_marble_touch_start();
    native_marble_buttons_start();
    native_marble_haptics_start();
    IOLog("MarbleHID: Goodix digitizer started\n");
}
