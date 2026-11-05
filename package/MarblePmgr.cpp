// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>

extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_marble_pmgr_start(void);
extern "C" void native_marble_watchdog_publish(void);
extern "C" bool native_resident_prepare(void);
extern "C" void native_marble_cpu_start(void);
extern "C" void native_marble_aes_init(void) __asm__("__GLOBAL__sub_I_NativeAES.cpp");
extern "C" kmod_info_t g_kernel_kmod_info;

extern "C" void native_marble_pmgr_start(void)
{
    native_resident_prepare();
    native_marble_cpu_start();
    kern_return_t status = OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR");
    if (status) panic("MarblePMGR: module load failed: %d", status);
    OSObject *object = OSMetaClass::allocClassWithName("ApplePMGRNub");
    IOService *nub = OSDynamicCast(IOService, object);
    IOService *platform = IOService::getPlatform();
    if (!nub || !platform || nub->getMetaClass()->getClassSize() < 0x90)
        panic("MarblePMGR: original nub allocation failed");
    // The original nub defines absent-provider AON reads as UINT32_MAX and writes as no-ops.
    // Its private provider at +0x88 stays absent on this Qualcomm platform.
    uintptr_t provider;
    memcpy(&provider, reinterpret_cast<const char *>(nub) + 0x88, sizeof(provider));
    if (provider || !nub->init() || !nub->attach(platform) || !nub->IOService::start(platform))
        panic("MarblePMGR: original nub initialization failed");
    memcpy(&provider, reinterpret_cast<const char *>(nub) + 0x88, sizeof(provider));
    if (provider) panic("MarblePMGR: unexpected Apple hardware provider");
    nub->setName("marble-platform-power");
    nub->registerService(kIOServiceSynchronous);
    IOLog("MarblePMGR: original ApplePMGRNub published; Apple AON provider absent\n");
    native_marble_watchdog_publish();
    status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOCryptoAcceleratorFamily");
    if (status) panic("MarbleAES: module load failed: %d", status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("MarbleAES: class initialization unavailable");
    native_marble_aes_init();
    status = OSMetaClass::postModLoad(handle);
    if (status) panic("MarbleAES: class registration failed: %d", status);
    OSObject *aesObject = OSMetaClass::allocClassWithName("NativeAES");
    IOService *aes = OSDynamicCast(IOService, aesObject);
    if (!aes || !aes->init() || !aes->attach(platform) || !aes->start(platform))
        panic("MarbleAES: software provider initialization failed");
    IOLog("MarbleAES: software IOAESAccelerator published\n");
}
