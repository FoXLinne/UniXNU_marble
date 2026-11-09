// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include "MarbleScanoutWire.h"

namespace {
using UINT8=uint8_t;using UINT32=uint32_t;using UINT64=uint64_t;
using UINTN=uintptr_t;using BOOLEAN=bool;
#define STATIC static
#define CONST const
#define VOID void
#define TRUE true
#define FALSE false
volatile MARBLE_SCANOUT *mDisplay;
IOMemoryMap *nativePanelMapping;
volatile UINT32 *nativeMdp,*nativeDsi;
UINT64 mDisplayFrequency,mPanelReadAt;
UINT32 mPanelBrightness=0x133,mPanelHz=60;
BOOLEAN mPanelAsleep;
UINT64 DisplayCounter() {
    UINT64 value;__asm__ volatile("mrs %0,cntpct_el0":"=r"(value));return value;
}
void NativePanelDelay(unsigned usec) {
    if (usec>=1000) IOSleep((usec+999)/1000);
    else IODelay(usec);
}
BOOLEAN MarbleRootReadRegister(UINT32 *out,const void *address,UINTN,UINT64 *) {
    const UINTN at=reinterpret_cast<UINTN>(address),base=reinterpret_cast<UINTN>(nativeMdp);
    if (at<base || at-base>0xa0000-4 || (at&3)) return false;
    *out=*reinterpret_cast<const volatile UINT32 *>(address);return true;
}
// Generated from the device's firmware panel commands so packet encoding and
// timing have one implementation for firmware and kernel ownership.
#include "MarblePanelCommands.inc"
}

extern "C" IOReturn native_marble_kernel_panel_control(uint32_t operation,uint32_t value,
                                                      volatile MARBLE_SCANOUT *display) {
    if (!nativePanelMapping) {
        auto *memory=IODeviceMemory::withRange(0xae00000,0xa0000);
        nativePanelMapping=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        OSSafeReleaseNULL(memory);
        if (!nativePanelMapping) return kIOReturnNoMemory;
        nativeMdp=reinterpret_cast<volatile UINT32 *>(nativePanelMapping->getVirtualAddress());
        nativeDsi=nativeMdp+0x94000/4;
        __asm__ volatile("mrs %0,cntfrq_el0":"=r"(mDisplayFrequency));
        mDisplay=display;PanelImportState();
    }
    mDisplay=display;
    bool ok=false;
    if (operation==MARBLE_PANEL_BRIGHTNESS) ok=PanelBrightness(value);
    else if (operation==MARBLE_PANEL_POWER) ok=PanelPower(value!=0);
    else if (operation==MARBLE_PANEL_REFRESH) ok=PanelRefresh(value);
    else return kIOReturnBadArgument;
    PanelPublishState();
    __asm__ volatile("dc cvac,%0\n\tdc cvac,%1\n\tdsb sy"::
        "r"(&display->panel_mode_status[0]),"r"(&display->panel_status[0]):"memory");
    return ok?kIOReturnSuccess:kIOReturnIOError;
}
