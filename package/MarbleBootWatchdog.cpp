// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/thread.h>
#include <kern/startup.h>
#include <mach/kmod.h>
#include "OriginalWatchdogABI.h"

extern "C" void native_marble_watchdog_start(void);
extern "C" void native_marble_watchdog_publish(void);
extern "C" struct vnode *rootvnode;
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" bool marble_original_watchdog_start(IOWatchdog *, IOService *, void *, uint64_t);
extern "C" void marble_original_watchdog_properties(IOWatchdog *);
extern "C" void marble_watchdog_static_init(void) __asm__("__GLOBAL__sub_I_MarbleBootWatchdog.cpp");
extern "C" {
volatile uint64_t marble_watchdog_checkins;
volatile uint32_t marble_watchdog_opens;
volatile uint32_t marble_watchdog_monitoring;
}
static IOWatchdog *activeWatchdog;
extern "C" bool native_resident_prepare(void);
static bool residentWatchdog;
static uint64_t watchdogCall(uint64_t, uint64_t * = nullptr, uint64_t = 0);

class MarbleWatchdog : public IOWatchdog {
    OSDeclareDefaultStructors(MarbleWatchdog)
public:
    bool start(IOService *provider) override;
    IOReturn setProperties(OSObject *properties) override;
    IOReturn newUserClient(task_t task, void *security, UInt32 type,
                          OSDictionary *properties, IOUserClient **handler) override;
    void enable3234(void) override;
    void disable13331(void) override;
    void pet65562(void) override;
    void properties34074(void) override;
};
OSDefineMetaClassAndStructors(MarbleWatchdog, IOWatchdog)

bool MarbleWatchdog::start(IOService *provider) {
    residentWatchdog=native_resident_prepare();
    if (!marble_original_watchdog_start(this, provider, nullptr, 0)) return false;
    activeWatchdog = this;
    IOLog("MarbleWatchdog: original IOWatchdog lifecycle; Gunyah backend\n");
    return true;
}
void MarbleWatchdog::enable3234(void) {
    if (residentWatchdog) return;
    uint64_t control = 0;
    uint64_t status = watchdogCall(0x86000006, &control);
    if (!status && (control & 1)) return;
    status = watchdogCall(0x86000005, nullptr, 3);
    if (status || watchdogCall(0x86000006, &control) || !(control & 1))
        panic("MarbleWatchdog: enable failed: %llx", status);
}
void MarbleWatchdog::disable13331(void) {
    if (residentWatchdog) return;
    uint64_t control = 0;
    uint64_t status = watchdogCall(0x86000006, &control);
    if (!status && !(control & 1)) return;
    status = watchdogCall(0x86000005, nullptr, 2);
    if (status || watchdogCall(0x86000006, &control) || (control & 1))
        panic("MarbleWatchdog: disable failed: %llx", status);
}
void MarbleWatchdog::pet65562(void) {
    if (residentWatchdog) return;
    uint64_t status = watchdogCall(0x86000007);
    if (status) panic("MarbleWatchdog: checkin failed: %llx", status);
}
void MarbleWatchdog::properties34074(void) {
    marble_original_watchdog_properties(this);
    setProperty("NativeSandboxGrantProcess", "watchdogd");
    setProperty("IOPMUPrimary", 1ULL, 32);
    setProperty("IOPMUBootStage", 0ULL, 32);
}
IOReturn MarbleWatchdog::setProperties(OSObject *properties) {
    OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
    if (!dictionary) return kIOReturnBadArgument;
    OSObject *stage = dictionary->getObject("IOPMUBootStage");
    OSObject *name = dictionary->getObject("IOPMUBootAppName");
    if ((!stage && !name) || (stage && !OSDynamicCast(OSNumber, stage)) ||
        (name && !OSDynamicCast(OSString, name)) ||
        dictionary->getCount() != unsigned(bool(stage)) + unsigned(bool(name)))
        return kIOReturnBadArgument;
    if (stage) {
        setProperty("IOPMUBootStage", stage);
        IOLog("MarbleWatchdog: boot stage %llu\n", OSDynamicCast(OSNumber, stage)->unsigned64BitValue());
    }
    if (name) setProperty("IOPMUBootAppName", name);
    return kIOReturnSuccess;
}
IOReturn MarbleWatchdog::newUserClient(task_t task, void *security, UInt32 type,
                                     OSDictionary *properties, IOUserClient **handler) {
    IOReturn status = IOWatchdog::newUserClient(task, security, type, properties, handler);
    if (status == kIOReturnSuccess) ++marble_watchdog_opens;
    IOLog("MarbleWatchdog: client type=%u status=%x\n", unsigned(type), status);
    return status;
}
extern "C" void native_marble_watchdog_publish(void) {
    kern_return_t status = OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMWatchdogTimer");
    if (status) panic("MarbleWatchdog: module load failed: %d", status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("MarbleWatchdog: class initialization unavailable");
    marble_watchdog_static_init();
    status = OSMetaClass::postModLoad(handle);
    if (status) panic("MarbleWatchdog: class registration failed: %d", status);
    MarbleWatchdog *service = new MarbleWatchdog;
    IOService *platform = IOService::getPlatform();
    if (!service || !platform || !service->init() || !service->attach(platform) || !service->start(platform))
        panic("MarbleWatchdog: service initialization failed");
}

static uint64_t watchdogCall(uint64_t function, uint64_t *control, uint64_t argument) {
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = argument;
    register uint64_t x2 __asm__("x2") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2) ::
        "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
        "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    if (control) *control = x1;
    return x0;
}

[[noreturn]] static void watchdogWorker(void *, wait_result_t) {
    uint64_t control = 0;
    uint64_t status = watchdogCall(0x86000006, &control);
    IOLog("MarbleWatchdog: startup active=%u status=%llx\n", !status && (control & 1), status);
    for (;;) {
        if (activeWatchdog) {
            const char *state = reinterpret_cast<const char *>(activeWatchdog);
            memcpy(const_cast<uint64_t *>(&marble_watchdog_checkins), state + 0xb8, 8);
            marble_watchdog_monitoring = static_cast<unsigned char>(state[0xa8]);
        }
        watchdogCall(0x86000007);
        IOSleep(1000);
    }
}

extern "C" void native_marble_watchdog_start(void) {
    static int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    thread_t thread;
    if (kernel_thread_start(watchdogWorker, nullptr, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}

STARTUP(THREAD_CALL, STARTUP_RANK_LAST, native_marble_watchdog_start);
