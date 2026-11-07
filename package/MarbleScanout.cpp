// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware owns the inherited display MMIO mapping. XNU exchanges completed
// buffer slots through separately owned cache lines in the RootStage mailbox.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <Protocol/RootStage.h>
#include "MarbleScanout.h"
#include "MarbleScanoutWire.h"

extern "C" {
volatile uint64_t marble_scanout_state[17];
uint32_t marble_scanout_registers[8][8];
}
namespace {
IOMemoryMap *mailboxMap;
volatile MARBLE_SCANOUT *mailbox;
MarbleScanoutLayout layout;
MarbleScanoutOwnership ownership;
bool ready, failed;
IOLock *scanoutLock;
void refresh() {
    __asm__ volatile("dc ivac,%0\n\tdsb sy"::"r"(mailbox):"memory");
    marble_scanout_state[1]=mailbox->error;
    marble_scanout_state[4]=mailbox->presented;
    marble_scanout_state[5]=mailbox->timeouts;
    marble_scanout_state[6]=mailbox->longest_us;
    marble_scanout_state[9]=mailbox->checkpoint;
}
bool configure() {
    if (ready) return true;
    if (failed) return false;
    failed=true;
    unsigned enabled=0;PE_parse_boot_argn("marble-display-flip",&enabled,sizeof(enabled));
    if (enabled!=1) return false;
    marble_scanout_state[0]=1;
    IORegistryEntry *node=IORegistryEntry::fromPath("/chosen/memory-map",gIODTPlane);
    OSData *data=node ? OSDynamicCast(OSData,node->getProperty("RootDisks")) : nullptr;
    uint64_t range[2]={};
    if (data && data->getLength()==sizeof(range)) memcpy(range,data->getBytesNoCopy(),sizeof(range));
    OSSafeReleaseNULL(node);
    if (!range[0] || range[1]!=sizeof(ROOT_STAGE_TABLE)) {marble_scanout_state[1]=1;return false;}
    auto *memory=IOMemoryDescriptor::withAddressRange(range[0],range[1],kIODirectionIn|kIOMemoryMapperNone,nullptr);
    auto *mapping=memory ? memory->map(kIOMapAnywhere|kIOMapCopybackCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!mapping) {marble_scanout_state[1]=2;return false;}
    ROOT_STAGE_TABLE stage;memcpy(&stage,reinterpret_cast<void *>(mapping->getVirtualAddress()),sizeof(stage));mapping->release();
    const auto &kernel=stage.Disks[2];
    if (stage.Magic!=ROOT_STAGE_MAGIC || kernel.Count!=1 || kernel.Bytes<0x100000 || kernel.Bytes>kernel.Extents[0].Bytes) {marble_scanout_state[1]=3;return false;}
    const uint64_t address=kernel.Extents[0].Address+kernel.Bytes-0x100000+MARBLE_SCANOUT_OFFSET;
    memory=IOMemoryDescriptor::withAddressRange(address,sizeof(MARBLE_SCANOUT),kIODirectionInOut|kIOMemoryMapperNone,nullptr);
    mailboxMap=memory ? memory->map(kIOMapAnywhere|kIOMapCopybackCache) : nullptr;OSSafeReleaseNULL(memory);
    if (!mailboxMap) {marble_scanout_state[1]=4;return false;}
    mailbox=reinterpret_cast<volatile MARBLE_SCANOUT *>(mailboxMap->getVirtualAddress());refresh();
    if (mailbox->magic!=MARBLE_SCANOUT_MAGIC || !mailbox->ready || mailbox->error) return false;
    PE_Video video={};
    if (PE_current_console(&video) || video.v_depth!=32 || video.v_rotate ||
        !marbleScanoutLayout(video.v_baseAddr,video.v_width,video.v_height,video.v_rowBytes,&layout) ||
        mailbox->base!=video.v_baseAddr || mailbox->span!=layout.slotBytes ||
        mailbox->width!=video.v_width || mailbox->height!=video.v_height || mailbox->stride!=video.v_rowBytes) {marble_scanout_state[1]=5;return false;}
    for (unsigned i=0;i<8;i++) {
        for (unsigned j=0;j<8;j++) marble_scanout_registers[i][j]=mailbox->registers[i][j];
        if ((mailbox->pipe_mask&(1U<<i)) && !marbleScanoutPipeValid(i,marble_scanout_registers[i],video.v_width,video.v_height,video.v_rowBytes)) {marble_scanout_state[1]=6;return false;}
    }
    scanoutLock=IOLockAlloc();if (!scanoutLock) {marble_scanout_state[1]=7;return false;}
    marble_scanout_state[2]=mailbox->pipe_mask;marble_scanout_state[3]=0x16000;
    marble_scanout_state[10]=mailbox->version;marble_scanout_state[11]=video.v_baseAddr;
    marble_scanout_state[12]=video.v_rowBytes;marble_scanout_state[13]=layout.slotBytes;
    marble_scanout_state[16]=mailbox->flush_mask;
    ready=true;failed=false;marble_scanout_state[0]=2;
    IOLog("MarbleScanout: firmware buffers=%u pipes=%x span=%llu\n",MarbleScanoutSlots,mailbox->pipe_mask,layout.slotBytes);
    return true;
}
IOReturn flip(uint32_t slot) {
    refresh();
    if (mailbox->requested!=mailbox->completed) return kIOReturnBusy;
    const uint32_t sequence=mailbox->requested+1;
    mailbox->slot=slot;__asm__ volatile("dmb ishst":::"memory");mailbox->requested=sequence;
    __asm__ volatile("dc cvac,%0\n\tdsb sy\n\tsev"::"r"(reinterpret_cast<uintptr_t>(mailbox)+64):"memory");
    const uint64_t began=mach_absolute_time();uint64_t ns=0;
    do {
        refresh();
        if (mailbox->completed==sequence) return mailbox->error ? kIOReturnIOError : kIOReturnSuccess;
        IOSleep(1);__asm__ volatile("sev":::"memory");
        absolutetime_to_nanoseconds(mach_absolute_time()-began,&ns);
    } while (ns<100000000);
    return kIOReturnTimeout;
}
}
extern "C" IOMemoryDescriptor *native_gop_scanout_ring(uint32_t *slots,uint64_t *slotBytes) {
    if (!slots || !slotBytes || !configure()) return nullptr;
    auto *memory=IODeviceMemory::withRange(layout.address,layout.bytes);
    if (!memory) return nullptr;
    *slots=MarbleScanoutSlots;*slotBytes=layout.slotBytes;return memory;
}
extern "C" IOReturn native_gop_scanout_present(uint32_t slot) {
    if (!ready) return kIOReturnNotReady;
    IOLockLock(scanoutLock);
    if (failed || !ownership.begin(slot)) {IOLockUnlock(scanoutLock);return kIOReturnNotReady;}
    marble_scanout_state[8]=ownership.pending;const IOReturn status=flip(slot);
    if (!status) {ownership.latched(slot);marble_scanout_state[7]=ownership.shown;marble_scanout_state[8]=ownership.pending;}
    else failed=true;
    IOLockUnlock(scanoutLock);return status;
}
extern "C" void native_gop_scanout_restore() {
    if (!ready) return;
    IOLockLock(scanoutLock);const IOReturn status=flip(0xffffffffU);
    if (!status) {ownership.restored();marble_scanout_state[7]=ownership.shown;marble_scanout_state[8]=ownership.pending;}
    else failed=true;
    IOLockUnlock(scanoutLock);
}
