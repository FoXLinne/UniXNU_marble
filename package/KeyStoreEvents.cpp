// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" IOService *native_keyevents_allocate(void);
extern "C" bool native_root_block_has_data(void);
static uint32_t refusedOpens, openedClients, clientCalls;

extern "C" void native_keyevents_open_refused(void) { refusedOpens++; }
extern "C" bool native_aks_state_enabled(void) {
    unsigned enabled = 0;
    return PE_parse_boot_argn("sheng-aks", &enabled, sizeof(enabled)) && enabled == 1;
}
extern "C" uint32_t native_aks_lock_state(void) {
    unsigned state = 0;
    PE_parse_boot_argn("sheng-aks-sls", &state, sizeof(state));
    return state <= 7 ? state : 0;
}
extern "C" size_t native_aks_device_id(uint8_t *out, size_t capacity) {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    size_t n = 0;
    if (id && id->getLength() >= 8 && id->getLength() <= capacity) {
        const uint8_t *bytes = static_cast<const uint8_t *>(id->getBytesNoCopy());
        bool nonzero = false;
        for (unsigned i = 0; i < id->getLength(); ++i) { out[i] = bytes[i]; nonzero |= bytes[i] != 0; }
        if (nonzero) n = id->getLength();
    }
    if (chosen) chosen->release();
    return n;
}
extern "C" void native_aks_opened(void) { openedClients++; }
extern "C" void native_aks_call(uint32_t, int32_t) { clientCalls++; }
extern "C" void native_keyevents_report(char *out, unsigned size) {
    snprintf(out, size, "KeyStoreEvents refused=%u opened=%u calls=%u", refusedOpens, openedClients, clientCalls);
}

extern "C" void native_keyevents_start(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("offline-key-events", &enabled, sizeof(enabled)) || enabled != 1) return;
    if (!native_root_block_has_data()) panic("RootStage: KeyStoreEvents Data volume unavailable");
    const OSSymbol *name = OSSymbol::withCString("AppleKeyStore");
    bool present = name && OSMetaClass::getMetaClassWithName(name);
    OSSafeReleaseNULL(name);
    if (present) panic("RootStage: AppleKeyStore class already registered");
    IOService *provider = IOService::getResourceService();
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("RootStage: KeyStoreEvents metaclass registration unavailable");
    native_keyevents_static_init();
    kern_return_t status = OSMetaClass::postModLoad(handle);
    if (status) panic("RootStage: KeyStoreEvents metaclass registration failed %x", status);
    IOService *service = native_keyevents_allocate();
    if (!service || !service->init() || !service->attach(provider) || !service->start(provider))
        panic("RootStage: KeyStoreEvents start failed");
    IOLog("RootStage: KeyStoreEvents published, state client=%u\n", native_aks_state_enabled());
}
