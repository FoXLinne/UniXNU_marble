// SPDX-License-Identifier: BSD-2-Clause-Patent
// Marble UFS transport. Stream ID 0xe0 comes from the device tree.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <vm/vm_kern.h>
#include <kern/task.h>
#include "RootDiagnostics.h"
#include "TransferPoll.h"
#include "SmmuTakeover.h"
#include "MarbleUfs.h"
#include "MarbleRtc.h"
extern "C" { uint8_t marble_rtc_header_before[4096]; }
namespace {
UfsRootDiagnostics gRootDiagnostic;
bool gRootDiagnosticEnabled = true;
void rootDiagnosticFailure(const char *why) {
  char report[2048];
  formatUfsRootDiagnostics(report, sizeof(report), gRootDiagnostic, why, "MarbleUfs");
  IOLog("%s", report);
}
static uint32_t read32(uintptr_t base, unsigned offset) { return *(volatile uint32_t *)(base+offset); }
static void write32(uintptr_t base, unsigned offset, uint32_t value) { *(volatile uint32_t *)(base+offset)=value; }
static uintptr_t mapPhys(uint64_t address, uint64_t bytes, OSSharedPtr<IOMemoryMap> &keep) {
  auto *memory=IODeviceMemory::withRange(address, bytes);
  if (!memory) return 0;
  keep=OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere|kIOMapInhibitCache), OSNoRetain);
  memory->release();
  return keep ? keep->getVirtualAddress() : 0;
}
static void smmuBypassStream (uint16_t stream, OSSharedPtr<IOMemoryMap> &keep, uint32_t *report,
                              uint64_t dma, uint64_t bytes)
{
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, keep);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  // hostStopQueue has quiesced the firmware's UFS requests. Release its exact
  // stream entry before installing the native physical-address context.
  unsigned groups=MIN(read32(smmu,0x20)&255U,128U);
  for (unsigned index=0;index<groups;index++) {
    unsigned offset=0x800+4*index;
    if (read32(smmu,offset)==(0x80000000U|stream)) write32(smmu,offset,0);
  }
  __asm__ volatile("dsb sy" ::: "memory");
  UfsSmmuPlan plan = planUfsSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });
  gRootDiagnostic.smmuId1 = read32 (smmu, 0x24);
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.smmuSlot = plan.slot;
  if (plan.error) {
    gRootDiagnostic.stage = "SMMU planning";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    gRootDiagnostic.smr = plan.smr; gRootDiagnostic.s2cr = plan.s2cr;
    rootDiagnosticFailure (plan.error);
    panic ("NATIVE_UFS smmu: %s slot=%u smr=%x s2cr=%x", plan.error, plan.slot, plan.smr, plan.s2cr);
  }
  unsigned bank = plan.bank, index = plan.slot;
  uintptr_t cb = smmu + (plan.pages + bank) * 4096;
  const char *failure = configureUfsSmmu (plan, stream,
    [smmu](unsigned offset) { return read32 (smmu, offset); },
    [smmu](unsigned offset, uint32_t value) {
      write32 (smmu, offset, value);
      __asm__ volatile ("dsb sy" ::: "memory");
    });
  uint32_t smr = read32 (smmu, 0x800 + 4 * index), s2cr = read32 (smmu, 0xc00 + 4 * index);
  uint32_t cbar = read32 (smmu, 0x1000 + 4 * bank), sctlr = read32 (cb, 0x0);
  gRootDiagnostic.smr = smr; gRootDiagnostic.s2cr = s2cr;
  gRootDiagnostic.cbar = cbar; gRootDiagnostic.sctlr = sctlr;
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.cba2r = read32 (smmu, 0x1800 + 4 * bank);
  if (failure) {
    gRootDiagnostic.stage = "SMMU readback";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    rootDiagnosticFailure (failure);
    panic ("NATIVE_UFS smmu: %s idx=%u bank=%u cba2r=%x", failure, index, bank,
           gRootDiagnostic.cba2r);
  }
  report[0] = index | (bank << 16);
  report[1] = cbar;
  report[2] = sctlr;
}
#include "RootTransportLayout.h"

struct UfsHost {
  OSSharedPtr<IOMemoryMap> hcMap, smmuMap;
  uintptr_t hc;
  IOBufferMemoryDescriptor *dma;
  uint8_t *virt;
  uint64_t phys;
  IOLock *lock;
  bool ready;
  bool poisoned = false;
  uintptr_t smmu = 0, cb = 0;
};
UfsHost gHost = { OSSharedPtr<IOMemoryMap> (), OSSharedPtr<IOMemoryMap> (), 0, nullptr, nullptr, 0, nullptr, false };

static IOReturn recordTransport (IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  gRootDiagnostic.ocs = ocs;
  if (result != kIOReturnSuccess && gRootDiagnosticEnabled) {
    // Read only the already mapped HC's status/registers used by this driver.
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
    gRootDiagnostic.doorbell = read32 (gHost.hc, 0x58);
    gRootDiagnostic.interrupt = read32 (gHost.hc, 0x20);
    gRootDiagnostic.list = uint64_t (read32 (gHost.hc, 0x50)) |
                          (uint64_t (read32 (gHost.hc, 0x54)) << 32);
    gRootDiagnostic.runStop = read32 (gHost.hc, 0x60);
    gRootDiagnostic.ahit = read32 (gHost.hc, 0x18);
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32 (gHost.hc, 0x38 + 4 * i);
    if (gHost.smmu && gHost.cb) {
      gRootDiagnostic.gfsr = read32 (gHost.smmu, 0x48);
      gRootDiagnostic.gsyn = read32 (gHost.smmu, 0x54);
      gRootDiagnostic.fsr = read32 (gHost.cb, 0x58);
      gRootDiagnostic.fsyn = read32 (gHost.cb, 0x68);
      gRootDiagnostic.far = uint64_t (read32 (gHost.cb, 0x60)) |
                            (uint64_t (read32 (gHost.cb, 0x64)) << 32);
    }
  }
  return result;
}

static void cacheCivac (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Device-owned cache lines must not be written back over DMA results. All
// callers use complete 64-byte lines from the dedicated page-aligned buffer.
static void cacheInvalidate (const void *start, size_t length)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  uintptr_t address = reinterpret_cast<uintptr_t> (start);
  for (uintptr_t end = address + length; address < end; address += 64) {
    __asm__ volatile ("dc ivac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void hostWrite (unsigned offset, uint32_t value)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gHost.hc, offset, value);
  __asm__ volatile ("dsb sy" ::: "memory");
}

#include "RootQueue.h"

#include "RootCommands.h"

// Bring the live controller up for reading: map, SMMU pass-through for the UFS stream, take over
// the request list, wake the device from UFS-PowerDown and wait for LUN 0 to be ready.
// `why` receives a short reason on failure.
static bool hostBringUp (const char **why)
{
  if (gHost.poisoned) {
    *why = "transport quarantined";
    return false;
  }
  if (gHost.ready) {
    return true;
  }
  gRootDiagnostic.stage = "controller map";
  gHost.hc = mapPhys (0x01d84000ULL, 0x3000ULL, gHost.hcMap);
  if (!gHost.hc) {
    *why = "controller map";
    return false;
  }
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.cap = read32 (gHost.hc, 0x00);
    gRootDiagnostic.version = read32 (gHost.hc, 0x08);
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
  }
  gRootDiagnostic.stage = "DMA allocation";
  gHost.dma = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone),
    kDmaBytes, 4096);
  gHost.lock = IOLockAlloc ();
  if (!gHost.dma || !gHost.lock) {
    *why = "dma buffer";
    return false;
  }
  gHost.virt = static_cast<uint8_t *> (gHost.dma->getBytesNoCopy ());
  IOByteCount contiguous = 0;
  gHost.phys = gHost.dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  gRootDiagnostic.dma = gHost.phys;
  gRootDiagnostic.contiguous = contiguous;
  if (!gHost.virt || !gHost.phys || (gHost.phys & 4095) || contiguous < kDmaBytes ||
      (!(read32 (gHost.hc, 0) & 0x01000000) && gHost.phys + kDmaBytes > 0x100000000ULL)) {
    *why = "DMA address/contiguity";
    return false;
  }
  bzero (gHost.virt, kDmaBytes);
  cacheCivac (gHost.virt, kDmaBytes);
  uint32_t report[3] = { 0, 0, 0 };
  gRootDiagnostic.stage = "SMMU and request list";
  if (!hostStopQueue (why)) return false;
  smmuBypassStream (0xe0, gHost.smmuMap, report, gHost.phys, kDmaBytes);
  gHost.smmu = gHost.smmuMap->getVirtualAddress ();
  uint32_t idr1 = read32 (gHost.smmu, 0x24);
  gHost.cb = gHost.smmu + ((1U << (((idr1 >> 28) & 7) + 1)) + (report[0] >> 16)) * 4096;
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.gfsrBefore = read32 (gHost.smmu, 0x48);
    gRootDiagnostic.fsrBefore = read32 (gHost.cb, 0x58);
    gRootDiagnostic.smmuId2 = read32 (gHost.smmu, 0x28);
    gRootDiagnostic.cba2r = read32 (gHost.smmu, 0x1800 + 4 * (report[0] >> 16));
  }
  if (!hostStartQueue (why)) return false;
  gRootDiagnostic.stage = "NOP OUT";
  // NOP OUT proves the transport before touching the SCSI layer.
  if (hostNop () != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "NOP OUT";
    return false;
  }
  uint32_t power = 0;
  gRootDiagnostic.stage = "power mode query";
  if (hostQueryRead (0x03, 0x02, &power) != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "power mode query";
    return false;
  }
  gRootDiagnostic.power = power;
  if ((power & 0xff) != 0x11) {
    gRootDiagnostic.stage = "device wake";
    // START STOP UNIT, power condition ACTIVE, to the UFS Device well-known LU (as Linux resumes).
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0xd0, ssu, 6, false, 0, 500000, &scsi) != kIOReturnSuccess) {
      gHost.ready = false;
      *why = "wake (START STOP UNIT)";
      return false;
    }
    IOSleep (50);
  }
  gRootDiagnostic.stage = "LUN ready";
  for (unsigned attempt = 0; attempt < 20; attempt++) {
    uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0, tur, 6, false, 0, 200000, &scsi) == kIOReturnSuccess) {
      gHost.ready = true;
      return true;
    }
    if (gHost.poisoned) break;
    IOSleep (10);
  }
  gHost.ready = false;
  *why = "LUN 0 not ready";
  return false;
}
#define NATIVE_UFS_ROOT_PARTITION "unixnu"
#include "RootLayout.h"
RootLayout layout;
uint64_t storeFirst, storeBlocks;
uint8_t *readScratch;
IOLock *readLock;
}
extern "C" bool marble_ufs_start(void) {
  const char *why="";
  if (!hostBringUp(&why) || !findRootLayout(&layout,&why) || layout.manifestStatus) {
    rootDiagnosticFailure(why); return false;
  }
  if (layout.windows!=3 || strcmp(layout.window[0].name,"OS") ||
      strcmp(layout.window[1].name,"SystemOS") || strcmp(layout.window[2].name,"KernelSeed")) return false;
  // The bounded metadata-write slot covers the manifest sector for RTC calibration.
  if (!hostLogOpen(layout.userdataFirst,1)) return false;
  readLock=IOLockAlloc(); readScratch=(uint8_t *)IOMalloc(kDataMax);
  return readLock && readScratch;
}
extern "C" uint64_t marble_ufs_root_bytes(unsigned index) {
  return index<2 ? layout.window[index].size : 0;
}
extern "C" uint64_t marble_ufs_root_read(unsigned index, IOMemoryDescriptor *buffer,
                                         uint64_t offset, uint64_t bytes) {
  if (index>=2 || offset>layout.window[index].size || bytes>layout.window[index].size-offset) return 0;
  uint64_t base=layout.userdataFirst*4096+layout.window[index].offset, done=0;
  IOLockLock(readLock);
  while (done<bytes) {
    uint64_t at=base+offset+done; unsigned within=at&4095;
    uint32_t size=(uint32_t)MIN(bytes-done,uint64_t(kDataMax-within));
    uint32_t blocks=(within+size+4095)/4096;
    if (hostRead4k(0,at/4096,blocks,readScratch)!=kIOReturnSuccess) break;
    auto copied=buffer->writeBytes(done,readScratch+within,size); done+=copied;
    if (copied!=size) break;
  }
  IOLockUnlock(readLock);
  return done;
}
extern "C" bool marble_ufs_store_open(uint64_t blocks) {
  uint64_t partition=layout.userdataLast-layout.userdataFirst+1;
  uint64_t image=(layout.imageBytes+4095)/4096;
  uint64_t reserve=256; // Last MiB stays clear of the end of the partition.
  if (!gHost.ready || !blocks || blocks>partition || reserve>partition-blocks ||
      image+1024>partition-blocks-reserve) return false;
  storeFirst=layout.userdataLast+1-reserve-blocks;
  storeBlocks=blocks;
  return hostStoreOpen(storeFirst,blocks);
}
extern "C" bool marble_ufs_store_read(uint64_t block, uint32_t count, void *data) {
  return block<=storeBlocks && count<=storeBlocks-block &&
    hostRead4k(0,storeFirst+block,count,data)==kIOReturnSuccess;
}
extern "C" bool marble_ufs_store_write(uint64_t block, uint32_t count, const void *data) {
  return block<=storeBlocks && count<=storeBlocks-block &&
    hostWrite4k(0,storeFirst+block,count,data,true)==kIOReturnSuccess;
}
extern "C" bool marble_ufs_store_sync(void) { return hostSyncCache()==kIOReturnSuccess; }

extern "C" bool marble_ufs_rtc_store(int64_t offset) {
  if (!gHost.ready || !readLock || layout.manifestStatus) return false;
  IOLockLock(readLock);
  bool result=false;
  if (hostRead4k(0,layout.userdataFirst,1,readScratch)==kIOReturnSuccess &&
      !memcmp(readScratch,"SHNGROOT",8) && le32(readScratch+8)==1 && le32(readScratch+12)==3) {
    SHA256_CTX context; uint8_t digest[32];
    SHA256_Init(&context); SHA256_Update(&context,readScratch,4064); SHA256_Final(digest,&context);
    if (!memcmp(digest,readScratch+4064,32)) {
      uint64_t calibration[3]={MARBLE_RTC_CAL_MAGIC,uint64_t(offset),~uint64_t(offset)};
      if (!memcmp(readScratch+MARBLE_RTC_CAL_OFFSET,calibration,sizeof(calibration))) result=true;
      else {
        memcpy(marble_rtc_header_before,readScratch,4096);
        memcpy(readScratch+MARBLE_RTC_CAL_OFFSET,calibration,sizeof(calibration));
        SHA256_Init(&context); SHA256_Update(&context,readScratch,4064); SHA256_Final(readScratch+4064,&context);
        result=hostWrite4k(0,layout.userdataFirst,1,readScratch,true)==kIOReturnSuccess &&
               hostSyncCache()==kIOReturnSuccess;
      }
    }
  }
  IOLockUnlock(readLock);
  return result;
}
