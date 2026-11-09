// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU submits scanout addresses through its display MMIO mapping. Panel power
// and brightness use the retained firmware mailbox.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task.h>
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
IOReturn native_marble_kernel_panel_control(uint32_t,uint32_t,volatile MARBLE_SCANOUT *);
volatile uint64_t marble_scanout_state[17];
volatile uint64_t marble_panel_dcs_state[8];
volatile uint32_t marble_panel_mode_state[12];
volatile uint64_t marble_scanout_latch_ticks[2048];
volatile uint32_t marble_scanout_latch_count;
uint32_t marble_scanout_registers[8][8];
volatile uint32_t native_ufs_dpu_smmu[24];
volatile uint32_t native_ufs_dpu_state;
volatile uint32_t native_ufs_dpu_check[8];
}
namespace {
// Sheng display translation, with marble stream IDs from marble_live.dts.
IOMemoryMap *dpuSmmuMap;
static uint32_t read32(uintptr_t base,unsigned offset) {return *reinterpret_cast<volatile uint32_t *>(base+offset);}
static void write32(uintptr_t base,unsigned offset,uint32_t value) {*reinterpret_cast<volatile uint32_t *>(base+offset)=value;}
// The display bank's stage 1 table: L1, then L2 for the first GiB, then kDpuSlots * kDpuSlotTables L3 tables (one per 2 MiB of a slot).
constexpr uint32_t kDpuWindow = 0x10000000U, kDpuSlots = 4, kDpuSlotBytes = 32U << 20, kDpuSlotTables = kDpuSlotBytes >> 21;
static uint64_t *gDpuTables;
static uintptr_t gDpuCb;
static IOLock *gDpuLock;
static uintptr_t gDpuLiveCb;
// FSR fault bits (TF .. UUT, SS, MULTI); 0x600 is the format field.
constexpr uint32_t kDpuFaults = 0xc00001feU;
static void dpuClean (const void *start, size_t length)
{
  for (size_t off = 0; off < length; off += 64)
    __asm__ volatile ("dc civac, %0" :: "r" (static_cast<const uint8_t *> (start) + off) : "memory");
  __asm__ volatile ("dsb sy" ::: "memory");
}
static bool dpuTlbFlush (uintptr_t cb)
{
  write32 (cb, 0x618, 0); write32 (cb, 0x7f0, 0);   // TLBIALL, TLBSYNC
  __asm__ volatile ("dsb sy" ::: "memory");
  for (unsigned spin = 0; spin < 100000; ++spin) if (!(read32 (cb, 0x7f4) & 1)) return true;
  return false;
}
static void dpuEnable (uintptr_t smmu, uintptr_t cb, uint32_t bank, uint32_t oldSctlr)
{
  (void) smmu;
  const size_t bytes = (2 + kDpuSlots * kDpuSlotTables) * 4096;
  IOBufferMemoryDescriptor *tables = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone), bytes, 4096);
  gDpuLock = IOLockAlloc ();
  if (!tables || !gDpuLock) {native_ufs_dpu_state = 0xdead0002U; return;}
  uint64_t *l1 = static_cast<uint64_t *> (tables->getBytesNoCopy ()), *l2 = l1 + 512;
  const uint64_t pa = tables->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
  bzero (l1, bytes);
  // Normal non-cacheable (attribute 0 = 0x44), not shareable, access flag, readable and writable at both privilege levels.
  const uint64_t attrs = (1ULL << 10) | (1ULL << 6);
  // i564: the first fetch faulted at once with FSYNR0 level 0, so the walk may start at level 0 although T0SZ 32 means level 1. One table
  // serves every level: [0] points back at itself (level 0 -> level 1, level 1 -> level 2 for the window), [2] and [3] are the 1 GiB
  // blocks, [128 + t] the window's level 3 tables. The second page stays unused.
  (void) l2;
  l1[0] = pa | 3;
  l1[2] = 0x80000000ULL | attrs | 1;
  l1[3] = 0xc0000000ULL | attrs | 1;
  for (uint32_t t = 0; t < kDpuSlots * kDpuSlotTables; ++t) l1[(kDpuWindow >> 21) + t] = (pa + (2 + t) * 4096ULL) | 3;
  dpuClean (l1, bytes);
  const uint32_t old[5] = {read32 (cb, 0x20), read32 (cb, 0x24), read32 (cb, 0x10), read32 (cb, 0x30), read32 (cb, 0x38)};
  gDpuLiveCb = cb;
  write32 (cb, 0x20, static_cast<uint32_t> (pa)); write32 (cb, 0x24, static_cast<uint32_t> (pa >> 32));
  // TCR2 (PASIZE bits 2:0, SEP bits 17:15): 48-bit output and SEP 7 (the sign extension comes from the upstream address, as arm-smmu's AArch64 contexts set it). SEP 0 takes
  // IA[31] as the sign bit, so every address with bit 31 set but zero above it (the display's own framebuffer at 0xb90f7500: i564, and the
  // first fetch of every earlier try) is out of TTBR0's range and faults at level 0.
  write32 (cb, 0x10, 5U | (7U << 15));
  write32 (cb, 0x30, 32U | (1U << 23));               // T0SZ 32, non-cacheable walks, 4 KiB granule, no TTBR1
  write32 (cb, 0x38, 0x44);                           // MAIR attribute 0: normal non-cacheable
  __asm__ volatile ("dsb sy" ::: "memory");
  volatile uint32_t *c = native_ufs_dpu_check;
  c[0] = read32 (cb, 0x20); c[1] = read32 (cb, 0x30); c[2] = read32 (cb, 0x58);
  const auto undo = [&] (uint32_t why) {
    write32 (cb, 0x0, oldSctlr);
    __asm__ volatile ("dsb sy; isb" ::: "memory");
    write32 (cb, 0x20, old[0]); write32 (cb, 0x24, old[1]); write32 (cb, 0x10, old[2]); write32 (cb, 0x30, old[3]); write32 (cb, 0x38, old[4]);
    const uint32_t fsr = read32 (cb, 0x58);
    if (fsr & (1U << 30)) write32 (cb, 0x8, 1);        // RESUME: terminate a stalled transaction
    if (fsr & kDpuFaults) write32 (cb, 0x58, fsr);     // write one to clear
    dpuTlbFlush (cb);
    native_ufs_dpu_state = why;
  };
  if (c[0] != static_cast<uint32_t> (pa) || c[1] != (32U | (1U << 23))) {undo (0xdead0003U); return;}   // the bank ignores our writes
  if (!dpuTlbFlush (cb)) {undo (0xdead0004U); return;}
  write32 (cb, 0x0, 1U | (1U << 5) | (1U << 6));      // SCTLR: M, CFRE, CFIE; a fault terminates instead of stalling the display
  __asm__ volatile ("dsb sy; isb" ::: "memory");
  // No check translation here (i563: IDR0 bit 26 ATOSNS, the SMMU has no ATOS), so watch the display's own fetches for 50 ms (6 frames
  // at 120 Hz) and turn translation off again at the first fault.
  uint32_t fsr = 0, us = 0;
  for (; us < 50000; us += 500) {
    fsr = read32 (cb, 0x58);
    if (fsr & kDpuFaults) break;
    IODelay (500);
  }
  c[3] = fsr; c[7] = us;
  if (fsr & kDpuFaults) {
    c[4] = read32 (cb, 0x60); c[5] = read32 (cb, 0x64); c[6] = read32 (cb, 0x68);
    undo (0xdead0005U); return;
  }
  gDpuTables = l1; gDpuCb = cb;
  native_ufs_dpu_state = 0x80000000U | bank;
}
static void startDpu() {
    if(dpuSmmuMap)return;
    auto *memory=IODeviceMemory::withRange(0x15000000ULL,0xd0000);
    dpuSmmuMap=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    OSSafeReleaseNULL(memory);
    if(!dpuSmmuMap)return;
    const unsigned scanout=2;
    const uintptr_t s = dpuSmmuMap->getVirtualAddress();
    const uint32_t idr0 = read32 (s, 0x20), idr1 = read32 (s, 0x24);
    const unsigned groups = (idr0 & 255) > 128 ? 128 : (idr0 & 255), banks = idr1 & 255;
    const unsigned pages = 1U << (((idr1 >> 28) & 7) + 1);
    native_ufs_dpu_smmu[1] = read32 (s, 0x0); native_ufs_dpu_smmu[2] = idr0; native_ufs_dpu_smmu[3] = idr1;
    unsigned valid = 0;
    for (unsigned k = 0; k < 2; ++k) native_ufs_dpu_smmu[4 + 9 * k] = 0xffffffffU;
    for (unsigned i = 0; i < groups; ++i) {
      const uint32_t smr = read32 (s, 0x800 + 4 * i);
      if (!(smr & 0x80000000U)) continue;
      ++valid;
      const uint32_t mask = (smr >> 16) & 0x7fff;
      for (unsigned k = 0; k < 2; ++k) {
        volatile uint32_t *r = &native_ufs_dpu_smmu[4 + 9 * k];
        if (r[0] != 0xffffffffU || ((smr ^ (0x2800U + 0x400U * k)) & 0x7fff & ~mask)) continue;
        const uint32_t s2cr = read32 (s, 0xc00 + 4 * i), bank = s2cr & 255;
        r[0] = i | (bank << 16); r[1] = smr; r[2] = s2cr;
        if (((s2cr >> 16) & 3) == 0 && bank < banks && (pages + banks) * 4096 <= 0xd0000) {
          const uintptr_t cb = s + (pages + bank) * 4096;
          r[3] = read32 (s, 0x1000 + 4 * bank); r[4] = read32 (s, 0x1800 + 4 * bank);
          r[5] = read32 (cb, 0x0); r[6] = read32 (cb, 0x20); r[7] = read32 (cb, 0x24); r[8] = read32 (cb, 0x30);
          native_ufs_dpu_smmu[22 + k] = read32 (cb, 0x58);
        }
      }
    }
    native_ufs_dpu_smmu[0] = 0x80000000U | groups | (valid << 8) | (banks << 16);
    const uint32_t found = native_ufs_dpu_smmu[4], bank = found >> 16;
    if (scanout && found != 0xffffffffU && bank < banks && ((native_ufs_dpu_smmu[6] >> 16) & 3) == 0 && !(native_ufs_dpu_smmu[9] & 1) &&
        ((native_ufs_dpu_smmu[7] >> 16) & 3) == 1) {
      dpuEnable (s, s + (pages + bank) * 4096, bank, native_ufs_dpu_smmu[9]);
    } else if (scanout) {
      native_ufs_dpu_state = 0xdead0001U;
    }
}
IOMemoryMap *mailboxMap;
IOMemoryMap *scanoutMdpMap;
volatile uint32_t *scanoutMdp;
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
    // Firmware DCS short responses retain the packet data type in byte zero.
    __asm__ volatile("dc ivac,%0\n\tdsb sy"::"r"(&mailbox->panel_status[0]):"memory");
    for (unsigned i=0;i<8;++i) marble_panel_dcs_state[i]=mailbox->panel_status[i];
    __asm__ volatile("dc ivac,%0\n\tdsb sy"::"r"(&mailbox->panel_mode_status[0]):"memory");
    for (unsigned i=0;i<12;++i) marble_panel_mode_state[i]=mailbox->panel_mode_status[i];
    marble_scanout_state[14]=mailbox->panel_status[0];
    marble_scanout_state[15]=mailbox->panel_status[1]|(mailbox->panel_status[2]<<32);
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
    auto *mdpMemory=IODeviceMemory::withRange(0xae00000,0x30000);
    scanoutMdpMap=mdpMemory ? mdpMemory->map(kIOMapAnywhere|kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(mdpMemory);
    if (!scanoutMdpMap) {marble_scanout_state[1]=8;return false;}
    scanoutMdp=reinterpret_cast<volatile uint32_t *>(scanoutMdpMap->getVirtualAddress());
    marble_scanout_state[2]=mailbox->pipe_mask;marble_scanout_state[3]=0x16000;
    marble_scanout_state[10]=mailbox->version;marble_scanout_state[11]=video.v_baseAddr;
    marble_scanout_state[12]=video.v_rowBytes;marble_scanout_state[13]=layout.slotBytes;
    marble_scanout_state[16]=mailbox->flush_mask;
    ready=true;failed=false;marble_scanout_state[0]=2;
    IOLog("MarbleScanout: kernel MMIO buffers=%u pipes=%x span=%llu\n",MarbleScanoutSlots,mailbox->pipe_mask,layout.slotBytes);
    return true;
}
IOReturn scanoutFlip(uint32_t slot) {
    if (!scanoutMdp || (slot>=MarbleScanoutSlots && slot!=0xffffffffU)) return kIOReturnBadArgument;
    constexpr uint32_t pipes[8]={0x5000,0x7000,0x9000,0xb000,0x25000,0x27000,0x29000,0x2b000};
    const uint32_t mask=mailbox->pipe_mask, flush=mailbox->flush_mask;
    if (!mask || !flush || (mask&~0xffU)) return kIOReturnBadArgument;
    const uint64_t address=slot==0xffffffffU ? mailbox->base : layout.address+slot*layout.slotBytes;
    if (address>UINT32_MAX || scanoutMdp[0x16018/4]&flush) return kIOReturnBusy;
    // Validate the complete transaction before changing any pipe address.
    for (unsigned i=0;i<8;++i) if (mask&(1U<<i)) {
        const uint64_t source=marble_scanout_registers[i][2];
        if (source<mailbox->base || source-mailbox->base>=layout.slotBytes ||
            address+source-mailbox->base>UINT32_MAX) return kIOReturnBadArgument;
    }
    for (unsigned i=0;i<8;++i) if (mask&(1U<<i))
        scanoutMdp[(pipes[i]+0x14)/4]=uint32_t(address+marble_scanout_registers[i][2]-mailbox->base);
    __asm__ volatile("dsb sy" ::: "memory");
    scanoutMdp[0x16018/4]=flush;
    __asm__ volatile("dsb sy" ::: "memory");
    const uint64_t began=mach_absolute_time();uint64_t ns=0;
    while (scanoutMdp[0x16018/4]&flush) {
        IOSleep(1);
        absolutetime_to_nanoseconds(mach_absolute_time()-began,&ns);
        if (ns>=50000000) return kIOReturnTimeout;
    }
    return kIOReturnSuccess;
}
IOReturn flip(uint32_t slot, uint64_t timeoutNs=100000000) {
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
    } while (ns<timeoutNs);
    return kIOReturnTimeout;
}
}
extern "C" bool native_gop_scanout_active(void) {
    if (!ready) return false;
    IOLockLock(scanoutLock);
    const bool active = !failed && ownership.shown < MarbleScanoutSlots;
    IOLockUnlock(scanoutLock);
    return active;
}
extern "C" bool native_marble_panel_control_available(void) {
    return configure() && mailbox->reserved[4]==MARBLE_PANEL_DCS_CAP;
}
static IOReturn panelControl(uint32_t operation,uint32_t value) {
    if (!native_marble_panel_control_available()) return kIOReturnUnsupported;
    IOLockLock(scanoutLock);
    unsigned native=0;PE_parse_boot_argn("marble-native-services",&native,sizeof(native));
    if (native) {
        const IOReturn result=native_marble_kernel_panel_control(operation,value,mailbox);
        IOLockUnlock(scanoutLock);
        return result;
    }
    mailbox->reserved[0]=value;
    const IOReturn result=flip(operation,operation==MARBLE_PANEL_REFRESH?1000000000:500000000);
    IOLockUnlock(scanoutLock);
    return result;
}
extern "C" IOReturn native_marble_panel_set_brightness(uint32_t value) {
    return panelControl(MARBLE_PANEL_BRIGHTNESS,value<4?4:value>4095?4095:value);
}
extern "C" IOReturn native_marble_panel_set_refresh(uint32_t hz) {
    if (!native_marble_panel_control_available() || mailbox->reserved[3]!=MARBLE_PANEL_REFRESH_CAP)
        return kIOReturnUnsupported;
    return panelControl(MARBLE_PANEL_REFRESH,hz);
}
extern "C" IOReturn native_legacy_panel_power(unsigned long state) {
    return panelControl(MARBLE_PANEL_POWER,state!=0);
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
    marble_scanout_state[8]=ownership.pending;const IOReturn status=scanoutFlip(slot);
    if (!status) {
        ownership.latched(slot);marble_scanout_state[7]=ownership.shown;marble_scanout_state[8]=ownership.pending;
        const uint32_t count = marble_scanout_latch_count;
        marble_scanout_latch_ticks[count % 2048] = mach_absolute_time();
        __atomic_store_n(&marble_scanout_latch_count, count + 1, __ATOMIC_RELEASE);
    }
    else failed=true;
    IOLockUnlock(scanoutLock);return status;
}
extern "C" bool native_gop_scanout_snapshot(void *output, uint64_t capacity) {
    if (!ready || !output) return false;
    const uint64_t pictureBytes = uint64_t(mailbox->stride) * mailbox->height;
    if (!pictureBytes || pictureBytes > layout.slotBytes || capacity < pictureBytes) return false;
    IOLockLock(scanoutLock);
    const uint32_t slot = ownership.shown;
    if (slot >= MarbleScanoutSlots) { IOLockUnlock(scanoutLock); return false; }
    auto *memory = IODeviceMemory::withRange(layout.address + uint64_t(slot) * layout.slotBytes,
                                            layout.slotBytes);
    auto *mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (mapping) {
        const volatile uint32_t *source = reinterpret_cast<const volatile uint32_t *>(mapping->getVirtualAddress());
        auto *destination = static_cast<uint32_t *>(output);
        for (uint64_t word = 0; word < pictureBytes / 4; ++word) destination[word] = source[word];
        mapping->release();
    }
    IOLockUnlock(scanoutLock);
    return mapping != nullptr;
}
extern "C" void native_gop_scanout_restore() {
    if (!ready) return;
    IOLockLock(scanoutLock);const IOReturn status=scanoutFlip(0xffffffffU);
    if (!status) {failed=false;ownership.restored();marble_scanout_state[7]=ownership.shown;marble_scanout_state[8]=ownership.pending;}
    else failed=true;
    IOLockUnlock(scanoutLock);
}

extern "C" uint32_t native_ufs_dpu_map (unsigned slot, const uint64_t *pages, unsigned count)
{
  if (!gDpuTables || slot >= kDpuSlots || count > kDpuSlotBytes / 4096) return 0;
  uint64_t *l3 = gDpuTables + 1024 + static_cast<size_t> (slot) * kDpuSlotTables * 512;
  const uint64_t attrs = (1ULL << 10) | (1ULL << 6);
  for (unsigned i = 0; i < kDpuSlotTables * 512; ++i) {
    if (i < count && (pages[i] & 0xfff)) return 0;
    l3[i] = i < count ? (pages[i] | attrs | 3) : 0;
  }
  dpuClean (l3, kDpuSlotTables * 4096);
  IOLockLock (gDpuLock);
  const bool synced = dpuTlbFlush (gDpuCb);
  IOLockUnlock (gDpuLock);
  return synced ? kDpuWindow + slot * kDpuSlotBytes : 0;
}

// Board binding for Sheng's native scanout; firmware supplies the active pipes.
extern "C" bool native_gop_direct_config(uint32_t *pipes,uint32_t *offsets,
                                         uint32_t *mask,uint32_t *flush,
                                         uint32_t *base,uint32_t *stride) {
    if(!configure())return false;
    startDpu();
    if((native_ufs_dpu_state&0xffff0000U)!=0x80000000U)return false;
    constexpr uint32_t pipeOffsets[8]={0x4000,0x6000,0x8000,0xa000,0x24000,0x26000,0x28000,0x2a000};
    for(unsigned i=0;i<8;++i) {
        pipes[i]=pipeOffsets[i];
        offsets[i]=(mailbox->pipe_mask&(1U<<i))?marble_scanout_registers[i][2]-uint32_t(mailbox->base):0;
    }
    *mask=mailbox->pipe_mask;*flush=mailbox->flush_mask;
    *base=uint32_t(mailbox->base);*stride=mailbox->stride;
    return true;
}
