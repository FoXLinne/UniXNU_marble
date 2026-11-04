// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register the power source class after IOKit initialization.
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_battery_static_init(void) __asm__("__GLOBAL__sub_I_MarbleBattery.cpp");
extern "C" void marble_battery_start(void);
// See MarbleBattery.cpp for the meaning of each entry.
extern "C" {volatile int32_t native_battery_stats[16];}

extern "C" void marble_battery_probe(void) {
    if (native_battery_stats[0]) return;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {native_battery_stats[0] = -10; return;}
    native_battery_static_init();
    if (OSMetaClass::postModLoad(handle)) {native_battery_stats[0] = -11; return;}
    marble_battery_start();
}
