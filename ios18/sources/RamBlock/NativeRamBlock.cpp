// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <IOKit/IOBSD.h>
#include <sys/param.h>
#include <libkern/crypto/sha2.h>
#include <mach/machine.h>
#include <machine/machine_routines.h>
#include <vm/vm_kern.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include "SparseDisk.h"
#include "PersistOverlay.h"
#include "DirectDisk.h"

extern "C" {
extern unsigned long gPhysBase, gPhysSize;
// The kernel's release of loader memory (payload-addon-release.patch); absent in a kernel without it.
bool ml_static_mfree_prepare(vm_offset_t, vm_size_t) __attribute__((weak_import));
void IODTFreeLoaderInfo(const char *, void *, int);
int native_ram_block_volume_device(const char *, dev_t *, char *, size_t);
volatile uint32_t native_ram_block_ready = 0;
volatile uint64_t native_ram_block_bytes = 0;
volatile uint64_t native_ram_block_reads = 0;
volatile uint64_t native_ram_block_writes = 0;
volatile uint64_t native_ram_block_failures = 0;
volatile uint64_t native_ram_block_resident_bytes = 0;
volatile uint32_t native_ram_block_sparse = 0;
volatile uint32_t native_ram_block_seed_released = 0;   // pages of the loader's packed seed returned to the VM
uint8_t native_ram_block_seed_sha256[32];
// Optional persistence of the changed pages through a board-provided store window (see PersistOverlay.h). All weak: a build
// without the provider keeps the volatile disk.
unsigned native_ufs_store_mode(void) __attribute__((weak_import));
int native_ufs_store_open(uint64_t, uint64_t *, const char **) __attribute__((weak_import));
int native_ufs_store_read(uint64_t, uint32_t, void *) __attribute__((weak_import));
int native_ufs_store_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
// Write without forced unit access (the device's write cache may hold it until native_ufs_store_sync). Present: write back.
int native_ufs_store_write_cached(uint64_t, uint32_t, const void *) __attribute__((weak_import));
int native_ufs_store_sync(void) __attribute__((weak_import));
unsigned native_ufs_store_gpt_check(void) __attribute__((weak_import));
// Diagnostics. state: 0 off, 1 blank (read-only probe), 2 resumed, 3 fresh (Install and boot), 4 refused/failed, 5 degraded after an I/O
// error, 6 resumed read-only (probe), 7 kept: the store holds no Data of this seed and no reset was asked, so it is left untouched and
// this boot's Data runs from the seed alone (header: why). header: persist::checkHeader result.
volatile uint32_t native_data_persist_state = 0, native_data_persist_header = 9, native_data_persist_gpt = 0xff, native_data_persist_gpt_after = 0xff;
// Data disk I/O, in mach_absolute_time ticks: requests and bytes by direction, time from entry to completion (lock waits included),
// time waiting for locks (the gate; in direct mode also a staging buffer, the page range of a write and a running flush), the longest
// request; the disk's synchronize (flushes, their time, bitmap blocks written, those the
// idle timer started). native_data_write_back: 1 when the store runs write back (a write cache, made durable by synchronize).
volatile uint64_t native_data_io_reads = 0, native_data_io_read_bytes = 0, native_data_io_read_ticks = 0,
                  native_data_io_writes = 0, native_data_io_write_bytes = 0, native_data_io_write_ticks = 0,
                  native_data_io_gate_ticks = 0, native_data_io_max_ticks = 0,
                  native_data_flushes = 0, native_data_flush_ticks = 0, native_data_flush_blocks = 0, native_data_timer_flushes = 0;
volatile uint32_t native_data_write_back = 0;
volatile uint64_t native_data_persist_pages = 0, native_data_persist_loaded = 0, native_data_persist_writes = 0,
                  native_data_persist_syncs = 0, native_data_persist_errors = 0, native_data_persist_first_lba = 0,
                  native_data_persist_present = 0;
}

class NativeRamBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeRamBlock)
  IOLock *gate = nullptr;
  uint8_t *memory = nullptr;
  uint64_t capacity = 0;
  bool online = false;
  RamSparseDisk sparse = {};
  // A disk above 2 GiB keeps only the nonzero pages of its seed (SeedDisk.h), in memory proportional to the content, and is served by the
  // direct (store-backed) path alone: it needs the persistent store open read-write, or its requests fail.
  SeedDisk big = {};
  bool isBig = false;
  uint8_t seedHash[32] = {};
  persist::Overlay overlay;
  uint8_t *overlayBits = nullptr, *stage = nullptr;   // stage: kStagePages pages, one write or read run
  static constexpr uint32_t kStagePages = 32;
  // Direct mode runs requests concurrently; only a request's own state is unshared:
  //  - a staging buffer from the pool (stage is its first entry), taken for the request;
  //  - a write holds its page range (writes to the same pages never overlap; a partial page is read, merged and written back) and the
  //    flush lock shared; a flush holds it exclusive, so every bit it writes names a page the sync before it made durable;
  //  - bitLock serializes Overlay::commitRun (bits, their bitmap blocks in write through, the dirty marks) and the flush timer state.
  // Lock order: page range, flush lock, bitLock; flushMutex before the flush lock. Reads take none of them.
  static constexpr uint32_t kStagePool = 8, kRanges = 16;
  uint8_t *stagePool[kStagePool] = {};
  uint32_t stageFree = 0, stageCount = 0;
  IOLock *poolLock = nullptr, *rangeLock = nullptr, *bitLock = nullptr, *flushMutex = nullptr;
  IORWLock *flushRw = nullptr;
  struct PageRange { uint64_t first, last; bool used; } ranges[kRanges] = {};
  uint32_t inflight = 0;   // direct requests running without the gate (gate held to change it)
  bool storeAttached = false;
  // Direct mode (the store is open read-write): changed pages live in the store window only, never in RAM; reads of them go to the
  // window. A failed store write stops all further writes (a page the system was told is written must be on the medium).
  bool direct = false, directFailed = false;
  // Write back (direct mode only, unless data-fua=1): page writes may stay in the device's write cache; the disk reports a
  // write cache, and its synchronize (or the idle timer, kFlushSeconds after the first write since the last flush) makes the pages
  // and then their bitmap blocks durable (PersistOverlay.h flush). A crash loses at most the writes since the last flush.
  bool writeBack = false, flushArmed = false, unflushed = false;
  uint8_t *dirtyBits = nullptr;
  uint64_t dirtyBytes = 0;
  thread_call_t flushCall = nullptr;
  static constexpr uint32_t kFlushSeconds = 5;
  struct StoreOps {
    bool cached = false;
    bool read(uint64_t b, uint32_t n, void *d) { __atomic_fetch_add(&native_data_persist_loaded, n, __ATOMIC_RELAXED); return native_ufs_store_read(b, n, d); }
    bool write(uint64_t b, uint32_t n, const void *s) { return cached ? native_ufs_store_write_cached(b, n, s) : native_ufs_store_write(b, n, s); }
    bool sync() { return native_ufs_store_sync(); }
  };
  static bool fuaRequested() { uint32_t v = 0; return PE_parse_boot_argn("data-fua", &v, sizeof v) && v == 1; }
  // Makes the written pages and their bitmap blocks durable; a failure stops all further writes, as a failed store write does. Nothing
  // written since the last flush: every page and bit is already durable, so no command is sent.
  bool flushStore(bool timer) {
    if (!writeBack || !overlay.active) return true;
    IOLockLock(flushMutex);
    IORWLockWrite(flushRw);   // waits for the writes in progress; their bits are set when they release it
    bool ok = true;
    if (__atomic_load_n(&directFailed, __ATOMIC_ACQUIRE)) ok = false;
    else if (unflushed) {
      const uint64_t t0 = mach_absolute_time();
      StoreOps store; store.cached = true;
      uint64_t blocks = 0;
      ok = overlay.flush(store, &blocks);
      __atomic_fetch_add(&native_data_flushes, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&native_data_flush_ticks, mach_absolute_time() - t0, __ATOMIC_RELAXED);
      __atomic_fetch_add(&native_data_flush_blocks, blocks, __ATOMIC_RELAXED);
      if (timer) __atomic_fetch_add(&native_data_timer_flushes, 1, __ATOMIC_RELAXED);
      native_data_persist_present = overlay.present;
      if (!ok) {
        __atomic_store_n(&directFailed, true, __ATOMIC_RELEASE);
        native_data_persist_state = 5;
        __atomic_fetch_add(&native_data_persist_errors, 1, __ATOMIC_RELAXED);
      } else {
        IOLockLock(bitLock); unflushed = false; IOLockUnlock(bitLock);
      }
    }
    IORWLockUnlock(flushRw);
    IOLockUnlock(flushMutex);
    return ok;
  }
  static void flushTimer(thread_call_param_t self, thread_call_param_t) {
    NativeRamBlock *disk = static_cast<NativeRamBlock *>(self);
    IOLockLock(disk->bitLock);
    disk->flushArmed = false;
    const bool due = disk->unflushed;
    IOLockUnlock(disk->bitLock);
    if (disk->online && due) disk->flushStore(true);
  }
  // bitLock held: start the idle timer once there is a write since the last flush and the timer is not already running.
  void armFlush() {
    if (!writeBack || !flushCall || flushArmed || !unflushed) return;
    uint64_t deadline = 0;
    clock_interval_to_deadline(kFlushSeconds, kSecondScale, &deadline);
    flushArmed = true;
    thread_call_enter_delayed(flushCall, deadline);
  }
  uint8_t *takeStage() {
    IOLockLock(poolLock);
    while (!stageFree) IOLockSleep(poolLock, &stageFree, THREAD_UNINT);
    const unsigned k = static_cast<unsigned>(__builtin_ctz(stageFree));
    stageFree &= ~(1u << k);
    IOLockUnlock(poolLock);
    return stagePool[k];
  }
  void giveStage(uint8_t *buffer) {
    IOLockLock(poolLock);
    for (unsigned k = 0; k < stageCount; ++k) if (stagePool[k] == buffer) stageFree |= 1u << k;
    IOLockWakeup(poolLock, &stageFree, true);
    IOLockUnlock(poolLock);
  }
  void takeRange(uint64_t first, uint64_t last) {
    IOLockLock(rangeLock);
    for (;;) {
      int slot = -1;
      bool busy = false;
      for (unsigned k = 0; k < kRanges; ++k) {
        if (!ranges[k].used) { if (slot < 0) slot = static_cast<int>(k); continue; }
        if (ranges[k].first <= last && first <= ranges[k].last) busy = true;
      }
      if (!busy && slot >= 0) { ranges[slot] = {first, last, true}; break; }
      IOLockSleep(rangeLock, ranges, THREAD_UNINT);
    }
    IOLockUnlock(rangeLock);
  }
  void giveRange(uint64_t first, uint64_t last) {
    IOLockLock(rangeLock);
    for (unsigned k = 0; k < kRanges; ++k)
      if (ranges[k].used && ranges[k].first == first && ranges[k].last == last) { ranges[k].used = false; break; }
    IOLockWakeup(rangeLock, ranges, false);
    IOLockUnlock(rangeLock);
  }
  struct BitCommit {
    NativeRamBlock *disk; StoreOps &store;
    bool operator()(uint64_t index, uint32_t count) {
      IOLockLock(disk->bitLock);
      const bool ok = disk->overlay.commitRun(store, index, count);
      IOLockUnlock(disk->bitLock);
      return ok;
    }
  };
  IOReturn directReadWrite(IOMemoryDescriptor *buffer, uint64_t offset, uint64_t bytes, uint64_t *actual, uint64_t *waited);
  void persistFailed() { overlay.active = false; native_data_persist_state = 5; __atomic_fetch_add(&native_data_persist_errors, 1, __ATOMIC_RELAXED); }
public:
  // Called once, before the Data volume is mounted (native_ram_block_volume_seed): open the store window, put the pages that
  // earlier boots changed back into the sparse disk, and from then on write changed pages through. The seed identity
  // (seedHash) is not affected. Any refusal or error leaves the disk volatile, exactly as without persistence.
  // data-reset=1 or 2 (the boot menu's Install and boot) starts the overlay over (the old pages are dropped, not applied); 2 also
  // asks for Setup (BootWatchdogPolicy.c). It is the only way the store is made anew: a store of another seed, a blank or a damaged one
  // is kept as it is on any other boot.
  static bool discardRequested() { uint32_t v = 0; return PE_parse_boot_argn("data-reset", &v, sizeof v) && (v == 1 || v == 2); }
  void attachStore() {
    if (!gate) return;
    IOLockLock(gate);
    if (storeAttached) { IOLockUnlock(gate); return; }
    storeAttached = true;
    unsigned mode = &native_ufs_store_mode ? native_ufs_store_mode() : 0;
    if (mode && (sparse.pages || isBig) && &native_ufs_store_open && &native_ufs_store_read && &native_ufs_store_write &&
        &native_ufs_store_sync && &native_ufs_store_gpt_check) {
      uint64_t pages = (isBig ? big.bytes : sparse.bytes) / 4096, first = 0;
      const char *why = "";
      native_data_persist_pages = pages;
      native_data_persist_gpt = native_ufs_store_gpt_check();
      if (!native_ufs_store_open(persist::windowBlocks(pages), &first, &why)) {
        native_data_persist_state = 4;
        IOLog("NativeRamBlock: persistent store refused: %s\n", why);
      } else {
        native_data_persist_first_lba = first;
        uint64_t bb = persist::bitmapBlocks(pages);
        overlayBits = static_cast<uint8_t *>(IOMallocZero(bb * persist::kBlock));
        stage = static_cast<uint8_t *>(IOMallocZero(kStagePages * persist::kBlock));
        StoreOps ops;
        persist::Open result = overlayBits && stage ? overlay.open(ops, pages, seedHash, mode == 1, overlayBits, discardRequested())
                                                          : persist::Open::Failed;
        native_data_persist_header = static_cast<uint32_t>(overlay.header);
        if (result == persist::Open::Failed) {
          native_data_persist_state = 4;
          overlay.active = false;
        } else if (result == persist::Open::Blank) {
          native_data_persist_state = 1;
        } else if (result == persist::Open::Kept) {
          native_data_persist_state = 7;
          overlay.active = false;
          IOLog("NativeRamBlock: the persistent store holds no Data of this seed (header %d); kept, Data runs from the seed: "
                "use Install and boot to start it over\n", overlay.header);
        } else if (result == persist::Open::Fresh) {
          native_data_persist_state = 3;
          direct = mode == 2;
        } else if (mode == 2) {
          // Resumed read-write: nothing is copied into RAM. The window answers for every page whose bit is set.
          direct = true;
          native_data_persist_state = 2;
        } else {
          struct Loader {
            NativeRamBlock *self;
            bool operator()(uint64_t i, const uint8_t *page) {
              if (i >= self->sparse.bytes / 4096) return false;
              if (!self->sparse.pages[i]) {
                uint8_t *fresh = static_cast<uint8_t *>(IOMallocZero(4096));
                if (!fresh) return false;
                self->sparse.pages[i] = fresh;
                self->sparse.resident += 4096;
              }
              memcpy(self->sparse.pages[i], page, 4096);
              __atomic_fetch_add(&native_data_persist_loaded, 1, __ATOMIC_RELAXED);
              return true;
            }
          } load = { this };
          if (overlay.forEachPresent(ops, load, stage, kStagePages)) {
            native_data_persist_state = mode == 1 ? 6 : 2;
            native_ram_block_resident_bytes = sparse.resident;
          } else {
            // A partly restored disk would be worse than a volatile one, but the pages already copied are consistent
            // overlay content; stop writing back and let the system decide what it can mount.
            native_data_persist_state = 4;
            overlay.active = false;
          }
        }
        // Direct and read-write: write back when the store offers cached writes and data-fua=1 does not ask for write through.
        if (direct && overlay.active && !overlay.readOnly && &native_ufs_store_write_cached && !fuaRequested()) {
          dirtyBytes = persist::bitmapBlocks(pages);
          dirtyBits = static_cast<uint8_t *>(IOMallocZero(dirtyBytes));
          flushCall = dirtyBits ? thread_call_allocate(flushTimer, this) : nullptr;
          if (dirtyBits && flushCall) {
            overlay.dirty = dirtyBits;
            overlay.dirtyBlocks = 0;
            writeBack = true;
            native_data_write_back = 1;
          } else {
            if (dirtyBits) IOFree(dirtyBits, dirtyBytes);
            dirtyBits = nullptr; dirtyBytes = 0;
            if (flushCall) { thread_call_free(flushCall); flushCall = nullptr; }
          }
        }
        // Direct mode: the staging pool (see stagePool); at least the open's own buffer.
        if (direct) {
          stagePool[0] = stage; stageCount = 1;
          while (stageCount < kStagePool) {
            uint8_t *buffer = static_cast<uint8_t *>(IOMallocZero(kStagePages * persist::kBlock));
            if (!buffer) break;
            stagePool[stageCount++] = buffer;
          }
          stageFree = (1u << stageCount) - 1;
        }
        native_data_persist_present = overlay.present;
        native_data_persist_gpt_after = native_ufs_store_gpt_check();
      }
    }
    IOLockUnlock(gate);
  }

  bool available() const { return online && (memory || sparse.pages || isBig) && capacity; }
  bool seedInfo(uint64_t *base,uint64_t *bytes,uint8_t hash[32]) {
    if(!gate || !base || !bytes || !hash)return false;
    IOLockLock(gate);bool ready=available();
    if(ready){*base=(sparse.pages||isBig)?0:reinterpret_cast<uint64_t>(memory);*bytes=capacity*512;memcpy(hash,seedHash,32);}
    IOLockUnlock(gate);return ready;
  }
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("Native"); }
  char *getProductString() override { return const_cast<char *>("Owned RAM block storage"); }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("volatile storage; no persistent trust claim"); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = capacity - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override {
    if (!online) return kIOReturnNotReady;
    // Write through: every changed page is on the medium, with its bitmap bit, before its write completed (PersistOverlay.h).
    if (!writeBack) return kIOReturnSuccess;
    // Write back: the pages written so far, then their bitmap blocks. Barrier and full requests are both served as a full flush.
    return flushStore(false) ? kIOReturnSuccess : kIOReturnIOError;
  }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
  IOReturn getWriteCacheState(bool *value) override { *value = writeBack; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors(NativeRamBlock, IOBlockStorageDevice)

bool NativeRamBlock::start(IOService *provider)
{
  if (!IOBlockStorageDevice::start(provider)) return false;
  IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
  if (!map) return false;
  OSData *packed = OSDynamicCast(OSData, map->getProperty("PackedRAMDisk"));
  OSData *range = OSDynamicCast(OSData, map->getProperty("RAMDisk"));
  if(packed && range){map->release();return false;}
  if(packed)range=packed;
  bool packedInput=packed!=nullptr;
  uint64_t physical = 0, bytes = 0;
  if (range && range->getLength() == 16) {
    memcpy(&physical, range->getBytesNoCopy(), 8);
    memcpy(&bytes, static_cast<const uint8_t *>(range->getBytesNoCopy()) + 8, 8);
  }
  map->release();
  // Boot memory belongs to the loader/kernel; never allocate or free it here.
  if (!bytes || bytes > 0x80000000 || (physical & 4095) || (!packedInput && (bytes & 4095)) ||
      physical < gPhysBase || bytes > gPhysSize || physical - gPhysBase > gPhysSize - bytes) return false;
  memory = reinterpret_cast<uint8_t *>(ml_static_ptovirt(physical));
  if (!memory || !(gate = IOLockAlloc()) || !(poolLock = IOLockAlloc()) || !(rangeLock = IOLockAlloc()) ||
      !(bitLock = IOLockAlloc()) || !(flushMutex = IOLockAlloc()) || !(flushRw = IORWLockAlloc())) return false;
  uint64_t logical=bytes;
  if(packedInput){
    if(!ramPackedLayout(memory,bytes,&logical))return false;
    struct ImportOps {uint8_t *allocate(){return static_cast<uint8_t *>(IOMallocZero(4096));}} ops;
    if(logical>0x80000000ULL){
      uint64_t nonzero=0;
      if(!seedCountNonzero(memory,bytes,&nonzero))return false;
      big.bytes=logical;big.capacity=nonzero;
      big.index=static_cast<uint64_t *>(IOMallocZero((nonzero?nonzero:1)*sizeof(uint64_t)));
      big.page=static_cast<uint8_t **>(IOMallocZero((nonzero?nonzero:1)*sizeof(uint8_t *)));
      if(!big.index||!big.page||!seedImportSparse(big,memory,bytes,ops))return false;
      isBig=true;sparse.bytes=logical;
    }else{
      sparse.bytes=logical;
      sparse.pages=static_cast<uint8_t **>(IOMallocZero((logical/4096)*sizeof(uint8_t *)));
      if(!ramSparseImport(sparse,memory,bytes,ops))return false;
    }
  }
  capacity = logical / 512;
  SHA256_CTX context;
  SHA256_Init(&context);
  if(isBig){
    seedIdentity(big,context,[](SHA256_CTX &c){SHA256_Init(&c);},[](SHA256_CTX &c,const void *p,size_t n){SHA256_Update(&c,p,n);},
                 [](SHA256_CTX &c,uint8_t *out){SHA256_Final(out,&c);},seedHash);
    memory=nullptr;
  }else{
  if(sparse.pages){
    static const uint8_t zero[4096]={};
    for(uint64_t i=0;i<logical/4096;++i)SHA256_Update(&context,sparse.pages[i]?sparse.pages[i]:zero,4096);
    memory=nullptr;
  }else SHA256_Update(&context, memory, bytes);
  SHA256_Final(seedHash, &context);
  }
  // A packed seed was copied page by page into this disk's own memory and hashed from there: nothing reads the loader's
  // copy again. With boot-payload-release=1 its whole pages go back to the VM the way XNU returns loader memory, and the
  // range leaves the registry's /chosen/memory-map, so a second start finds no seed instead of freed memory.
  uint32_t release = 0;
  if (packedInput && !memory && PE_parse_boot_argn("boot-payload-release", &release, sizeof release) && release == 1 &&
      ml_static_mfree_prepare && bytes <= INT32_MAX && ml_static_mfree_prepare(ml_static_ptovirt(physical), bytes)) {
    IODTFreeLoaderInfo("PackedRAMDisk", reinterpret_cast<void *>(physical), static_cast<int>(bytes));
    native_ram_block_seed_released = static_cast<uint32_t>(bytes >> 12);
  }
  memcpy(native_ram_block_seed_sha256,seedHash,32);
  native_ram_block_bytes = logical;
  native_ram_block_sparse=(sparse.pages||isBig)?1:0;
  native_ram_block_resident_bytes=isBig?big.resident:sparse.pages?sparse.resident:logical;
  // The medium is memory or, with the persistent store, flash: neither seeks. IOMediaBSDClient answers DKIOCISSOLIDSTATE from this
  // dictionary, found from any media above this device; without it XNU treats the disk as rotating (the swap thresholds of a hard
  // disk when the swap files live here, throttle periods, read-ahead and page-in cluster sizes, the mount's MNTK_SSD).
  OSDictionary *characteristics = OSDictionary::withCapacity(1);
  OSString *medium = OSString::withCString(kIOPropertyMediumTypeSolidStateKey);
  if (characteristics && medium && characteristics->setObject(kIOPropertyMediumTypeKey, medium)) {
    setProperty(kIOPropertyDeviceCharacteristicsKey, characteristics);
  }
  OSSafeReleaseNULL(medium);
  OSSafeReleaseNULL(characteristics);
  online = true;
  native_ram_block_ready = 1;
  registerService();
  return true;
}

IOReturn NativeRamBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= capacity ||
      count > capacity - block || buffer->getLength() < count * 512 ||
      (buffer->getDirection() != kIODirectionIn && buffer->getDirection() != kIODirectionOut)) return kIOReturnBadArgument;
  if (!gate) return kIOReturnNotReady;
  const uint64_t bytes = count * 512;
  const uint64_t entered = mach_absolute_time();
  IOLockLock(gate);
  const uint64_t gated = mach_absolute_time();
  if (!online) { IOLockUnlock(gate); return kIOReturnNotReady; }
  if (direct) {
    ++inflight;
    IOLockUnlock(gate);
    uint64_t done = 0, waited = gated - entered;
    const IOReturn result = directReadWrite(buffer, block * 512, bytes, &done, &waited);
    const uint64_t spent = mach_absolute_time() - entered;
    const bool out = buffer->getDirection() == kIODirectionOut;
    __atomic_fetch_add(out ? &native_data_io_writes : &native_data_io_reads, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(out ? &native_data_io_write_bytes : &native_data_io_read_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(out ? &native_data_io_write_ticks : &native_data_io_read_ticks, spent, __ATOMIC_RELAXED);
    __atomic_fetch_add(out ? &native_ram_block_writes : &native_ram_block_reads, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&native_data_io_gate_ticks, waited, __ATOMIC_RELAXED);
    for (uint64_t seen = __atomic_load_n(&native_data_io_max_ticks, __ATOMIC_RELAXED); spent > seen &&
         !__atomic_compare_exchange_n(&native_data_io_max_ticks, &seen, spent, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED);) {}
    if (result) __atomic_fetch_add(&native_ram_block_failures, 1, __ATOMIC_RELAXED);
    IOLockLock(gate);
    if (!--inflight && !online) IOLockWakeup(gate, &inflight, false);
    IOLockUnlock(gate);
    completion->action(completion->target, completion->parameter, result, done);
    return kIOReturnSuccess;
  }
  uint64_t actual;bool noMemory=false;
  if(isBig && !direct) {
    // Before the store is attached (APFS probes the disk for its volume UUID long before the kernel asks for the Data volume, which is what
    // attaches the store) a disk this size is read from its seed alone; it has no volatile mode, so writes wait.
    if(buffer->getDirection()==kIODirectionIn) {
      struct SeedReadOps {
        IOMemoryDescriptor *buffer;
        uint64_t copy(uint64_t at,const void *p,uint64_t n){return buffer->writeBytes(at,p,n);}
      } rops={buffer};
      static const uint8_t zero[4096]={};
      uint64_t done=0,offset=block*512;
      while(done<bytes) {
        uint64_t at=offset+done,index=at/4096,skip=at&4095,n=4096-skip;
        if(n>bytes-done)n=bytes-done;
        const uint8_t *page=big.find(index);
        uint64_t got=rops.copy(done,(page?page:zero)+skip,n);
        done+=got;if(got!=n)break;
      }
      __atomic_fetch_add(&native_ram_block_reads,1,__ATOMIC_RELAXED);
      IOReturn early=done==bytes?kIOReturnSuccess:kIOReturnIOError;
      if(early)__atomic_fetch_add(&native_ram_block_failures,1,__ATOMIC_RELAXED);
      IOLockUnlock(gate);
      completion->action(completion->target,completion->parameter,early,done);
      return kIOReturnSuccess;
    }
    IOLockUnlock(gate);return kIOReturnNotReady;
  }
  if(sparse.pages||isBig){
    struct Operations {
      IOMemoryDescriptor *buffer;
      uint8_t *allocate(){return static_cast<uint8_t *>(IOMallocZero(4096));}
      uint64_t read(uint64_t at,void *p,uint64_t n){return buffer->readBytes(at,p,n);}
      uint64_t copy(uint64_t at,const void *p,uint64_t n){return buffer->writeBytes(at,p,n);}
    } ops={buffer};
    bool write=buffer->getDirection()==kIODirectionOut;
    {
    actual=ramSparseIO(sparse,write,block*512,bytes,ops,noMemory);
    }
    if(!direct && write && actual==bytes && overlay.active && !overlay.readOnly){
      StoreOps store;
      uint64_t firstPage=block*512/4096,lastPage=(block*512+bytes-1)/4096;
      for(uint64_t i=firstPage;i<=lastPage && overlay.active;){
        uint32_t n=0;
        while(n<kStagePages && i+n<=lastPage && sparse.pages[i+n]){memcpy(stage+(uint64_t)n*persist::kBlock,sparse.pages[i+n],persist::kBlock);++n;}
        if(!n || !overlay.writeRun(store,i,n,stage)){persistFailed();break;}
        __atomic_fetch_add(&native_data_persist_writes,n,__ATOMIC_RELAXED);
        __atomic_fetch_add(&native_data_persist_syncs,1,__ATOMIC_RELAXED);   // one run = one durable commit
        native_data_persist_present=overlay.present;
        i+=n;
      }
    }
    if(write)__atomic_fetch_add(&native_ram_block_writes,1,__ATOMIC_RELAXED);
    else __atomic_fetch_add(&native_ram_block_reads,1,__ATOMIC_RELAXED);
    native_ram_block_resident_bytes=isBig?big.resident:sparse.resident;
  }else if (buffer->getDirection() == kIODirectionIn) {
    actual = buffer->writeBytes(0, memory + block * 512, bytes);
    __atomic_fetch_add(&native_ram_block_reads, 1, __ATOMIC_RELAXED);
  } else {
    actual = buffer->readBytes(0, memory + block * 512, bytes);
    __atomic_fetch_add(&native_ram_block_writes, 1, __ATOMIC_RELAXED);
  }
  IOReturn status = actual == bytes ? kIOReturnSuccess : noMemory ? kIOReturnNoMemory : kIOReturnIOError;
  if (status) __atomic_fetch_add(&native_ram_block_failures, 1, __ATOMIC_RELAXED);
  const uint64_t left = mach_absolute_time(), spent = left - entered;
  if (buffer->getDirection() == kIODirectionOut) {
    __atomic_fetch_add(&native_data_io_writes, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&native_data_io_write_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&native_data_io_write_ticks, spent, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&native_data_io_reads, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&native_data_io_read_bytes, bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&native_data_io_read_ticks, spent, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&native_data_io_gate_ticks, gated - entered, __ATOMIC_RELAXED);
  for (uint64_t seen = __atomic_load_n(&native_data_io_max_ticks, __ATOMIC_RELAXED); spent > seen &&
       !__atomic_compare_exchange_n(&native_data_io_max_ticks, &seen, spent, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED);) {}
  IOLockUnlock(gate);
  completion->action(completion->target, completion->parameter, status, actual);
  return kIOReturnSuccess;
}

// A direct-mode request, without the gate (see stagePool for what it locks). *waited gains the time spent waiting for locks.
IOReturn NativeRamBlock::directReadWrite(IOMemoryDescriptor *buffer, uint64_t offset, uint64_t bytes, uint64_t *actual,
                                         uint64_t *waited)
{
  struct BufferOps {
    IOMemoryDescriptor *buffer;
    uint64_t read(uint64_t at, void *p, uint64_t n) { return buffer->readBytes(at, p, n); }
    uint64_t copy(uint64_t at, const void *p, uint64_t n) { return buffer->writeBytes(at, p, n); }
  } bops = {buffer};
  const bool write = buffer->getDirection() == kIODirectionOut;
  const uint64_t first = offset / 4096, last = (offset + bytes - 1) / 4096;
  *actual = 0;
  if (write && __atomic_load_n(&directFailed, __ATOMIC_ACQUIRE)) return kIOReturnIOError;
  const uint64_t t0 = mach_absolute_time();
  uint8_t *buf = takeStage();
  if (write) { takeRange(first, last); IORWLockRead(flushRw); }
  *waited += mach_absolute_time() - t0;
  StoreOps store; store.cached = writeBack;
  BitCommit commit = {this, store};
  bool storeError = false;
  *actual = isBig ? directDiskIO(big, overlay, store, buf, kStagePages, write, offset, bytes, bops, &storeError, commit)
                  : directDiskIO(sparse, overlay, store, buf, kStagePages, write, offset, bytes, bops, &storeError, commit);
  if (storeError) {
    if (write) __atomic_store_n(&directFailed, true, __ATOMIC_RELEASE);
    native_data_persist_state = 5;
    __atomic_fetch_add(&native_data_persist_errors, 1, __ATOMIC_RELAXED);
  }
  if (write && *actual) {
    __atomic_fetch_add(&native_data_persist_writes, (*actual + 4095) / 4096, __ATOMIC_RELAXED);
    __atomic_fetch_add(&native_data_persist_syncs, 1, __ATOMIC_RELAXED);
    native_data_persist_present = overlay.present;
    // Write back: durable on the next synchronize, or on the idle timer.
    if (writeBack) { IOLockLock(bitLock); unflushed = true; armFlush(); IOLockUnlock(bitLock); }
  }
  if (write) { IORWLockUnlock(flushRw); giveRange(first, last); }
  giveStage(buf);
  return *actual == bytes ? kIOReturnSuccess : kIOReturnIOError;
}

void NativeRamBlock::stop(IOService *provider)
{
  if (flushCall) thread_call_cancel_wait(flushCall);
  // No new request starts; the direct requests still running finish first. Then what is still unflushed reaches the medium.
  if (gate) {
    IOLockLock(gate);
    online = false;
    while (inflight) IOLockSleep(gate, &inflight, THREAD_UNINT);
    IOLockUnlock(gate);
    if (bitLock && flushMutex && flushRw) flushStore(false);
  }
  IOBlockStorageDevice::stop(provider);
}

void NativeRamBlock::free()
{
  if (flushCall) { thread_call_cancel_wait(flushCall); thread_call_free(flushCall); flushCall = nullptr; }
  if (dirtyBits) { IOFree(dirtyBits, dirtyBytes); dirtyBits = nullptr; dirtyBytes = 0; overlay.dirty = nullptr; }
  if (overlayBits) { IOFree(overlayBits, persist::bitmapBlocks(sparse.bytes / 4096) * persist::kBlock); overlayBits = nullptr; }
  for (unsigned k = 1; k < stageCount; ++k) { IOFree(stagePool[k], kStagePages * persist::kBlock); stagePool[k] = nullptr; }
  stageCount = 0;
  if (stage) { IOFree(stage, kStagePages * persist::kBlock); stage = nullptr; }
  if (isBig) {
    for (uint64_t k = 0; k < big.count; ++k) IOFree(big.page[k], 4096);
    if (big.index) IOFree(big.index, (big.capacity ? big.capacity : 1) * sizeof(uint64_t));
    if (big.page) IOFree(big.page, (big.capacity ? big.capacity : 1) * sizeof(uint8_t *));
    big = {}; isBig = false;
  }
  if (gate) { IOLockFree(gate); gate = nullptr; }
  if (poolLock) { IOLockFree(poolLock); poolLock = nullptr; }
  if (rangeLock) { IOLockFree(rangeLock); rangeLock = nullptr; }
  if (bitLock) { IOLockFree(bitLock); bitLock = nullptr; }
  if (flushMutex) { IOLockFree(flushMutex); flushMutex = nullptr; }
  if (flushRw) { IORWLockFree(flushRw); flushRw = nullptr; }
  if(sparse.pages){
    for(uint64_t i=0;i<sparse.bytes/4096;++i)if(sparse.pages[i])IOFree(sparse.pages[i],4096);
    IOFree(sparse.pages,(sparse.bytes/4096)*sizeof(uint8_t *));sparse={};
  }
  // Dense boot allocation and packed input remain owned until reboot.
  IOBlockStorageDevice::free();
}

static int selectVolumeSeed(const char *uuid, dev_t *device, char *path, size_t size,
    uint64_t *seedBase,uint64_t *seedBytes,uint8_t seedDigest[32])
{
  if (!uuid || strlen(uuid) != 36 || !device || !path || size < 6) return 0;
  *device = NODEV;
  path[0] = 0;
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *wanted = OSString::withCString(uuid);
  if (!matching || !properties || !wanted) {
    OSSafeReleaseNULL(wanted); OSSafeReleaseNULL(properties); OSSafeReleaseNULL(matching);
    return 0;
  }
  bool ready = properties->setObject("UUID", wanted) && matching->setObject("IOPropertyMatch", properties);
  wanted->release(); properties->release();
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  matching->release();
  if (!iterator) return 0;
  bool selected = false;
  unsigned matches = 0;
  dev_t candidate = NODEV;
  char candidatePath[64] = {};
  uint64_t base=0,bytes=0;uint8_t hash[32]={};
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    if (++matches != 1) { selected = false; break; }
    OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
    OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
    OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
    if (!identity || strcmp(identity->getCStringNoCopy(), uuid) || !name || !mjr || !mnr ||
        !name->getLength() || name->getLength() >= sizeof(candidatePath) - 6 || name->getLength() >= size - 6 ||
        mjr->unsigned64BitValue() > 255 || mnr->unsigned64BitValue() > 0xffffff) continue;
    unsigned depth = 0;
    for (IOService *provider = media; provider && depth++ < 64; provider = provider->getProvider()) {
      NativeRamBlock *ram = OSDynamicCast(NativeRamBlock, provider);
      if (!ram) continue;
      ram->attachStore();
      if (!ram->seedInfo(&base,&bytes,hash)) continue;
      candidate = static_cast<dev_t>(makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
      snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name->getCStringNoCopy());
      selected = true;
      break;
    }
  }
  iterator->release();
  if (!selected || matches != 1) return 0;
  *device = candidate;
  strlcpy(path, candidatePath, size);
  if(seedBase)*seedBase=base;
  if(seedBytes)*seedBytes=bytes;
  if(seedDigest)memcpy(seedDigest,hash,32);
  return 1;
}

extern "C" int native_ram_block_volume_device(const char *uuid,dev_t *device,char *path,size_t size) {
  return selectVolumeSeed(uuid,device,path,size,nullptr,nullptr,nullptr);
}
// The GPU addon the loader placed after the trust caches (/chosen/memory-map/GpuAddon, scripts/make-gpu-addon.py), for the kernel's
// boot-time addon install (kern_exec.c). Boot memory, like the seed: never freed here. Returns 0 without one.
extern "C" int native_ram_block_gpu_addon(const uint8_t **data,uint64_t *bytes) {
  if(!data || !bytes)return 0;
  *data=nullptr;*bytes=0;
  IORegistryEntry *map=IORegistryEntry::fromPath("/chosen/memory-map",gIODTPlane);
  if(!map)return 0;
  OSData *range=OSDynamicCast(OSData,map->getProperty("GpuAddon"));
  uint64_t physical=0,size=0;
  if(range && range->getLength()==16){
    memcpy(&physical,range->getBytesNoCopy(),8);
    memcpy(&size,static_cast<const uint8_t *>(range->getBytesNoCopy())+8,8);
  }
  map->release();
  if(!size || size>(UINT64_C(64)<<20) || (physical&4095) || physical<gPhysBase || size>gPhysSize || physical-gPhysBase>gPhysSize-size)return 0;
  *data=reinterpret_cast<const uint8_t *>(ml_static_ptovirt(physical));
  if(!*data)return 0;
  *bytes=size;
  return 1;
}

extern "C" int native_ram_block_volume_seed(const char *uuid,dev_t *device,char *path,size_t size,
    uint64_t *base,uint64_t *bytes,uint8_t digest[32]) {
  if(!base || !bytes || !digest)return 0;
  *base=0;*bytes=0;memset(digest,0,32);
  return selectVolumeSeed(uuid,device,path,size,base,bytes,digest);
}
