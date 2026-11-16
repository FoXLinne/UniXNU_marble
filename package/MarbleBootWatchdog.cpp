// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/thread.h>
#include <kern/startup.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include "OriginalWatchdogABI.h"
#include "MarblePlatformMatch.h"

extern "C" void native_marble_watchdog_start(void);
extern "C" void native_marble_boot_record(unsigned,uint64_t,uint64_t);
extern "C" void native_marble_watchdog_publish(void);
extern "C" struct vnode *rootvnode;
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void marble_original_watchdog_properties(IOWatchdog *);
extern "C" bool marble_original_watchdog_initialize(IOWatchdog *, IOService *, uint64_t, uint32_t);
extern "C" void marble_watchdog_static_init(void) __asm__("__GLOBAL__sub_I_MarbleBootWatchdog.cpp");
extern "C" {
volatile uint64_t marble_watchdog_checkins;
volatile uint32_t marble_watchdog_opens;
volatile uint32_t marble_watchdog_monitoring;
volatile uint64_t marble_sgi_probe[10];
}
extern "C" void armcpu_sgi_selfprobe(uint64_t *);
static IOWatchdog *activeWatchdog;
extern "C" bool native_resident_prepare(void);
extern "C" void native_marble_usb_health_check(void);
extern "C" void native_marble_rtc_kernel_service(void);
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
    native_marble_boot_record(0x307,0,0);
    residentWatchdog=native_resident_prepare();
    native_marble_boot_record(0x308,residentWatchdog,0);
    if (!marble_original_watchdog_initialize(this, provider, 0, 0)) return false;
    native_marble_boot_record(0x309,0,0);
    activeWatchdog = this;
    unsigned probe = 0;
    if (PE_parse_boot_argn("marble-ipi-probe", &probe, sizeof(probe)) && probe == 1) {
        uint64_t result[10];
        armcpu_sgi_selfprobe(result);
        memcpy(const_cast<uint64_t *>(marble_sgi_probe), result, sizeof(result));
    }

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
    native_marble_boot_record(0x300,0,0);
    if (!marble_platform_matches()) return;
    kern_return_t status = OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMWatchdogTimer");
    native_marble_boot_record(0x301,status,0);
    if (status) panic("MarbleWatchdog: module load failed: %d", status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("MarbleWatchdog: class initialization unavailable");
    native_marble_boot_record(0x302,0,0);
    marble_watchdog_static_init();
    native_marble_boot_record(0x303,0,0);
    status = OSMetaClass::postModLoad(handle);
    native_marble_boot_record(0x304,status,0);
    if (status) panic("MarbleWatchdog: class registration failed: %d", status);
    native_marble_boot_record(0x305,0,0);
    MarbleWatchdog *service = new MarbleWatchdog;
    native_marble_boot_record(0x306,reinterpret_cast<uint64_t>(service),0);
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
        native_marble_usb_health_check();
        native_marble_rtc_kernel_service();
        if (activeWatchdog) {
            const char *state = reinterpret_cast<const char *>(activeWatchdog);
            memcpy(const_cast<uint64_t *>(&marble_watchdog_checkins), state + 0xb8, 8);
            marble_watchdog_monitoring = static_cast<unsigned char>(state[0xa8]);
        }
        // Mode 3 leaves the recovery timer lease with the firmware USB owner.
        // Kernel health sampling continues while host-command expiry can reset.
        unsigned transport=0;
        PE_parse_boot_argn("rootstage-runtime-usb",&transport,sizeof(transport));
        if (transport!=3) watchdogCall(0x86000007);
        IOSleep(1000);
    }
}

extern "C" void native_marble_watchdog_start(void) {
    if (!marble_platform_matches()) return;
    static int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    thread_t thread;
    if (kernel_thread_start(watchdogWorker, nullptr, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}

STARTUP(THREAD_CALL, STARTUP_RANK_LAST, native_marble_watchdog_start);

// CPU initialization records use kernel static memory and no I/O allocation.
// Firmware derives this ring's window offset from the selected kernel symbol.
extern "C" { __attribute__((used,aligned(4096))) volatile uint64_t native_marble_boot_events[512]; }
extern "C" void native_marble_boot_record(unsigned event, uint64_t address, uint64_t value)
{
    uint64_t affinity,counter;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(affinity));
    if ((affinity&0xff00ffffffULL)!=0) return;
    volatile uint64_t *log=native_marble_boot_events;
    const uint64_t magic=UINT64_C(0x314543415254424d);
    if (log[0]!=magic) {
        log[1]=0;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(affinity));
        log[2]=affinity;log[0]=magic;
    }
    uint64_t index=log[1];
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(counter));
    volatile uint64_t *entry=log+4+4*(index%120);
    entry[0]=counter;entry[1]=event;entry[2]=address;entry[3]=value;
    __asm__ volatile("dc cvac, %0\n\tdsb sy" :: "r"(entry) : "memory");
    log[1]=index+1;
    __asm__ volatile("dc cvac, %0\n\tdsb sy" :: "r"(log) : "memory");
}
