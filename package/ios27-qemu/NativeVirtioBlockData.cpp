// SPDX-License-Identifier: BSD-2-Clause-Patent
// Writable VirtIO 1.2 MMIO block transport for the QEMU scratch Data disk.
//
// Separate real protocol path from the signed upstream NativeVirtioBlock
// (VirtioMmioBlock 4e1013eb8c45077b57f8d38d61b389fe899ea1c3): this class only
// attaches devices that do NOT offer VIRTIO_BLK_F_RO, issues VIRTIO_BLK_T_OUT
// writes with a device-readable data descriptor, and negotiates
// VIRTIO_BLK_F_FLUSH so doSynchronizeCache performs an actual flush request.
// It carries no verification policy and can never authenticate a volume: the
// signed-policy provider-chain checks cast to NativeVirtioBlock, which this
// class is not. A read-only device is refused so verified transports keep
// exclusive ownership of the root and cache disks.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/crypto/sha2.h>
#include <kern/clock.h>
#include <kern/task.h>

extern "C" {
volatile uint32_t native_qemu_data_ready = 0;
volatile uint32_t native_qemu_data_read_requests = 0;
volatile uint32_t native_qemu_data_write_requests = 0;
volatile uint32_t native_qemu_data_write_bytes = 0;
volatile uint32_t native_qemu_data_request_failures = 0;
volatile uint32_t native_qemu_data_flush_requests = 0;
volatile uint32_t native_qemu_data_flush_refused = 0;
volatile uint32_t native_qemu_data_readonly_refused = 0;
volatile uint64_t native_qemu_data_sectors = 0;
volatile uint64_t native_qemu_data_mmio_physical = 0;
unsigned char native_qemu_data_first_sector_sha256[32] = {};
__attribute__((noinline)) void native_qemu_data_write_checkpoint(void *device) {
  __asm__ volatile("" : : "r"(device) : "memory");
}
}

// VirtIO 1.2 MMIO, modern split queue; offsets are protocol numbers identical
// to the read-only transport. Data descriptors take the other DMA direction
// for writes: device-readable (no VRING_DESC_F_WRITE) for VIRTIO_BLK_T_OUT.
namespace {
constexpr unsigned queueSize = 8;
constexpr unsigned transferSize = 65536;
constexpr uint32_t opRead = 0, opWrite = 1, opFlush = 4;
constexpr uint32_t featureReadOnly = 1U << 5, featureFlush = 1U << 9;
struct Descriptor { uint64_t address; uint32_t length; uint16_t flags; uint16_t next; };
struct Request { uint32_t operation; uint32_t reserved; uint64_t sector; };
struct Used { uint32_t id; uint32_t length; };
static_assert(sizeof(Descriptor) == 16 && sizeof(Request) == 16 && sizeof(Used) == 8, "VirtIO wire layout");
inline void dmaBarrier() { __asm__ volatile("dmb osh" ::: "memory"); }
}

class NativeVirtioBlockData : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeVirtioBlockData)
  IOMemoryMap *mapping = nullptr;
  IOBufferMemoryDescriptor *queueMemory = nullptr;
  IOBufferMemoryDescriptor *transferMemory = nullptr;
  IOLock *gate = nullptr;
  uintptr_t registers = 0;
  uint64_t queuePhysical = 0;
  uint64_t transferPhysical = 0;
  uint64_t capacity = 0;
  uint16_t availableIndex = 0;
  uint16_t usedIndex = 0;
  bool online = false;
  bool ownsDevice = false;
  bool flushSupported = false;
  uint32_t read32(unsigned offset) const {
    return *reinterpret_cast<volatile uint32_t *>(registers + offset);
  }
  void write32(unsigned offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(registers + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
  }
  void writeAddress(unsigned offset, uint64_t value) {
    write32(offset, static_cast<uint32_t>(value));
    write32(offset + 4, static_cast<uint32_t>(value >> 32));
  }
  bool reset();
  bool configureQueue();
  IOReturn submit(uint32_t operation, uint64_t sector, uint32_t bytes);
  IOReturn readSectors(uint64_t sector, uint32_t bytes);
  IOReturn writeSectors(uint64_t sector, uint32_t bytes);
  IOReturn flush();
public:
  IOService *probe(IOService *, SInt32 *) override;
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("VirtIO"); }
  char *getProductString() override { return const_cast<char *>("VirtIO writable block"); }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("modern MMIO, coherent DMA, flush"); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = capacity - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override;
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
  IOReturn getWriteCacheState(bool *value) override { *value = flushSupported; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors(NativeVirtioBlockData, IOBlockStorageDevice)

IOService *NativeVirtioBlockData::probe(IOService *provider, SInt32 *score)
{
  OSData *compatible = OSDynamicCast(OSData, provider->getProperty("compatible"));
  if (!compatible || compatible->getLength() != 12 ||
      memcmp(compatible->getBytesNoCopy(), "virtio,mmio", 12)) return nullptr;
  return IOBlockStorageDevice::probe(provider, score);
}

bool NativeVirtioBlockData::reset()
{
  if (!registers || !ownsDevice) return true;
  online = false;
  write32(0x70, 0);
  uint64_t deadline;
  clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
  while (read32(0x70)) {
    if (mach_absolute_time() >= deadline) return false;
    IODelay(1);
  }
  return true;
}

bool NativeVirtioBlockData::configureQueue()
{
  if (!reset()) return false;
  write32(0x70, 1);
  write32(0x70, 3);
  write32(0x14, 0);
  uint32_t lowFeatures = read32(0x10);
  write32(0x14, 1);
  uint32_t highFeatures = read32(0x10);
  // Writable transport only: read-only disks belong to the verified upstream
  // class; VIRTIO_F_VERSION_1 remains mandatory. Flush is negotiated when the
  // device offers it.
  if ((lowFeatures & featureReadOnly) || !(highFeatures & 1)) {
    if (lowFeatures & featureReadOnly)
      __atomic_fetch_add(&native_qemu_data_readonly_refused, 1, __ATOMIC_RELAXED);
    return false;
  }
  flushSupported = (lowFeatures & featureFlush) != 0;
  write32(0x24, 0);
  write32(0x20, flushSupported ? featureFlush : 0);
  write32(0x24, 1);
  write32(0x20, 1);
  write32(0x70, 11);
  if (!(read32(0x70) & 8)) return false;
  write32(0x30, 0);
  if (read32(0x44) || read32(0x34) < queueSize) return false;
  queueMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
    static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), 4096, 4096);
  transferMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
    static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous), transferSize, 4096);
  if (!queueMemory || !transferMemory) return false;
  IOByteCount queueLength = 0, transferLength = 0;
  queuePhysical = queueMemory->getPhysicalSegment(0, &queueLength);
  transferPhysical = transferMemory->getPhysicalSegment(0, &transferLength);
  if (!queuePhysical || !transferPhysical || queueLength < 4096 || transferLength < transferSize) return false;
  bzero(queueMemory->getBytesNoCopy(), 4096);
  // Suppress used-buffer interrupts: this bounded transport polls.
  *reinterpret_cast<volatile uint16_t *>(static_cast<unsigned char *>(queueMemory->getBytesNoCopy()) + 128) = 1;
  availableIndex = usedIndex = 0;
  dmaBarrier();
  write32(0x38, queueSize);
  writeAddress(0x80, queuePhysical);
  writeAddress(0x90, queuePhysical + 128);
  writeAddress(0xa0, queuePhysical + 256);
  write32(0x44, 1);
  // Read 64-bit capacity through stable, aligned 32-bit configuration accesses.
  bool stable = false;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    uint32_t generation = read32(0xfc);
    uint64_t low = read32(0x100), high = read32(0x104);
    if (generation == read32(0xfc)) {
      capacity = low | (high << 32);
      stable = true;
      break;
    }
  }
  if (!stable || !capacity || capacity > UINT64_MAX / 512) return false;
  write32(0x70, 15);
  online = (read32(0x70) & 15) == 15;
  return online;
}

// One virtqueue round trip. `bytes == 0` issues a header+status request
// (flush); otherwise the transfer buffer participates as device-writable
// (read) or device-readable (write) data.
IOReturn NativeVirtioBlockData::submit(uint32_t operation, uint64_t sector, uint32_t bytes)
{
  if (!online) return kIOReturnNotReady;
  auto *queue = static_cast<unsigned char *>(queueMemory->getBytesNoCopy());
  auto *descriptors = reinterpret_cast<Descriptor *>(queue);
  auto *request = reinterpret_cast<Request *>(queue + 512);
  volatile uint8_t *status = queue + 528;
  *request = {operation, 0, sector};
  *status = 0xff;
  uint32_t expected = 1;
  if (operation == opRead || operation == opWrite) {
    if (!bytes || bytes > transferSize) return kIOReturnBadArgument;
    descriptors[0] = {queuePhysical + 512, sizeof(Request), 1, 1};
    descriptors[1] = {transferPhysical, bytes,
                      static_cast<uint16_t>(operation == opRead ? 3 : 1), 2};
    descriptors[2] = {queuePhysical + 528, 1, 2, 0};
    expected = operation == opRead ? bytes + 1 : 1;
  } else {
    descriptors[0] = {queuePhysical + 512, sizeof(Request), 1, 1};
    descriptors[1] = {queuePhysical + 528, 1, 2, 0};
  }
  auto *available = reinterpret_cast<volatile uint16_t *>(queue + 128);
  available[2 + availableIndex % queueSize] = 0;
  dmaBarrier();
  ++availableIndex;
  available[1] = availableIndex;
  dmaBarrier();
  write32(0x50, 0);
  auto *used = reinterpret_cast<volatile uint16_t *>(queue + 256);
  uint64_t deadline;
  clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
  while (used[1] == usedIndex) {
    if (mach_absolute_time() >= deadline) {
      reset();
      __atomic_fetch_add(&native_qemu_data_request_failures, 1, __ATOMIC_RELAXED);
      return kIOReturnTimeout;
    }
    dmaBarrier();
    IODelay(1);
  }
  dmaBarrier();
  auto *element = reinterpret_cast<volatile Used *>(queue + 260) + usedIndex % queueSize;
  bool valid = used[1] == static_cast<uint16_t>(usedIndex + 1) && element->id == 0 &&
               element->length >= expected && *status == 0;
  ++usedIndex;
  uint32_t interrupts = read32(0x60) & 3;
  if (interrupts) write32(0x64, interrupts);
  if (!valid) {
    reset();
    __atomic_fetch_add(&native_qemu_data_request_failures, 1, __ATOMIC_RELAXED);
    return kIOReturnIOError;
  }
  return kIOReturnSuccess;
}

IOReturn NativeVirtioBlockData::readSectors(uint64_t sector, uint32_t bytes)
{
  if (!bytes || bytes > transferSize || (bytes & 511) ||
      sector >= capacity || bytes / 512 > capacity - sector) return kIOReturnBadArgument;
  IOReturn status = submit(opRead, sector, bytes);
  if (status == kIOReturnSuccess)
    __atomic_fetch_add(&native_qemu_data_read_requests, 1, __ATOMIC_RELAXED);
  return status;
}

IOReturn NativeVirtioBlockData::writeSectors(uint64_t sector, uint32_t bytes)
{
  if (!bytes || bytes > transferSize || (bytes & 511) ||
      sector >= capacity || bytes / 512 > capacity - sector) return kIOReturnBadArgument;
  IOReturn status = submit(opWrite, sector, bytes);
  if (status == kIOReturnSuccess) {
    __atomic_fetch_add(&native_qemu_data_write_requests, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&native_qemu_data_write_bytes, bytes, __ATOMIC_RELAXED);
    native_qemu_data_write_checkpoint(this);
  }
  return status;
}

IOReturn NativeVirtioBlockData::flush()
{
  if (!online) return kIOReturnNotReady;
  if (!flushSupported) {
    __atomic_fetch_add(&native_qemu_data_flush_refused, 1, __ATOMIC_RELAXED);
    return kIOReturnUnsupported;
  }
  IOReturn status = submit(opFlush, 0, 0);
  if (status == kIOReturnSuccess)
    __atomic_fetch_add(&native_qemu_data_flush_requests, 1, __ATOMIC_RELAXED);
  return status;
}

IOReturn NativeVirtioBlockData::doSynchronizeCache()
{
  IOLockLock(gate);
  IOReturn status = flush();
  IOLockUnlock(gate);
  return status;
}

bool NativeVirtioBlockData::start(IOService *provider)
{
  if (!IOBlockStorageDevice::start(provider) || !provider->getProperty("dma-coherent") ||
      provider->getResources() != kIOReturnSuccess) return false;
  mapping = provider->mapDeviceMemoryWithIndex(0, kIOMapAnywhere | kIOMapInhibitCache);
  if (!mapping || mapping->getLength() < 0x108) return false;
  registers = static_cast<uintptr_t>(mapping->getVirtualAddress());
  if (read32(0x00) != 0x74726976 || read32(0x04) != 2 || read32(0x08) != 2) return false;
  ownsDevice = true;
  gate = IOLockAlloc();
  if (!gate || !configureQueue()) return false;
  if (readSectors(0, 512)) return false;
  SHA256_CTX hash;
  SHA256_Init(&hash);
  SHA256_Update(&hash, transferMemory->getBytesNoCopy(), 512);
  SHA256_Final(native_qemu_data_first_sector_sha256, &hash);
  native_qemu_data_sectors = capacity;
  native_qemu_data_mmio_physical = mapping->getPhysicalAddress();
  native_qemu_data_ready = 1;
  registerService();
  return true;
}

IOReturn NativeVirtioBlockData::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                               IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= capacity ||
      count > capacity - block || count > UINT64_MAX / 512 || buffer->getLength() < count * 512) return kIOReturnBadArgument;
  IODirection direction = buffer->getDirection();
  if (direction != kIODirectionIn && direction != kIODirectionOut) return kIOReturnBadArgument;
  uint64_t actual = 0;
  IOReturn result = kIOReturnNotReady;
  IOLockLock(gate);
  while (online && actual < count * 512) {
    uint32_t bytes = static_cast<uint32_t>(count * 512 - actual > transferSize ? transferSize : count * 512 - actual);
    if (direction == kIODirectionOut) {
      if (buffer->readBytes(actual, transferMemory->getBytesNoCopy(), bytes) != bytes) {
        result = kIOReturnIOError;
        break;
      }
      result = writeSectors(block + actual / 512, bytes);
    } else {
      result = readSectors(block + actual / 512, bytes);
      if (!result && buffer->writeBytes(actual, transferMemory->getBytesNoCopy(), bytes) != bytes)
        result = kIOReturnIOError;
    }
    if (result) break;
    actual += bytes;
  }
  IOLockUnlock(gate);
  completion->action(completion->target, completion->parameter, result, actual);
  return kIOReturnSuccess;
}

void NativeVirtioBlockData::stop(IOService *provider)
{
  if (!reset()) panic("NativeVirtioBlockData: device did not acknowledge DMA reset");
  IOBlockStorageDevice::stop(provider);
}

void NativeVirtioBlockData::free()
{
  if (registers && !reset()) panic("NativeVirtioBlockData: refusing to free active DMA buffers");
  if (queueMemory) { queueMemory->release(); queueMemory = nullptr; }
  if (transferMemory) { transferMemory->release(); transferMemory = nullptr; }
  if (mapping) { mapping->release(); mapping = nullptr; }
  if (gate) { IOLockFree(gate); gate = nullptr; }
  IOBlockStorageDevice::free();
}
