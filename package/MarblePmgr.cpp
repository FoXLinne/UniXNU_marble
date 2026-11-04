// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSMetaClass.h>

extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_marble_pmgr_start(void);

extern "C" void native_marble_pmgr_start(void)
{
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
}
