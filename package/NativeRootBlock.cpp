#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
#include <sys/param.h>
#include <sys/msgbuf.h>
#include <libkern/c++/OSNumber.h>
#include <IOKit/IOBSD.h>
#include <Protocol/RootStage.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void root_block_static_init(void) __asm__("__GLOBAL__sub_I_NativeRootBlock.cpp");
volatile uint64_t native_root_block_reads;
}

class NativeRootBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeRootBlock)
  ROOT_STAGE_DISK disk = {};
  IOMemoryMap *chunks[ROOT_STAGE_EXTENTS] = {};
  bool online = false;
  bool systemRoot = false;
public:
  void configure(const ROOT_STAGE_DISK &source, unsigned index) {
    if (!source.Bytes || source.Received != source.Bytes ||
        source.Count != (source.Bytes + ROOT_STAGE_CHUNK - 1) / ROOT_STAGE_CHUNK ||
        source.Count > ROOT_STAGE_EXTENTS)
      panic("RootStage: disk %u metadata bytes=%llu received=%llu extents=%llu",
          index, source.Bytes, source.Received, source.Count);
    disk = source;
    systemRoot = index == 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < disk.Count; i++) {
      uint64_t expected = MIN(ROOT_STAGE_CHUNK, disk.Bytes - total);
      if (!disk.Extents[i].Address || disk.Extents[i].Bytes != expected)
        panic("RootStage: disk %u extent %u invalid pa=%llx bytes=%llu expected=%llu",
            index, i, disk.Extents[i].Address, disk.Extents[i].Bytes, expected);
      IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(
          disk.Extents[i].Address, expected, kIODirectionIn | kIOMemoryMapperNone, nullptr);
      if (!memory)
        panic("RootStage: disk %u extent %u descriptor failed pa=%llx bytes=%llu",
            index, i, disk.Extents[i].Address, expected);
      chunks[i] = memory->map(kIOMapAnywhere | kIOMapReadOnly);
      memory->release();
      if (!chunks[i])
        panic("RootStage: disk %u extent %u mapping failed pa=%llx bytes=%llu",
            index, i, disk.Extents[i].Address, expected);
      total += expected;
    }
  }
  bool isSystemRoot() const { return systemRoot && online; }
  bool start(IOService *provider) override {
    if (!IOBlockStorageDevice::start(provider)) return false;
    online = true;
    return true;
  }
  void stop(IOService *provider) override {
    online = false;
    IOBlockStorageDevice::stop(provider);
  }
  void free() override {
    for (auto &mapping : chunks) OSSafeReleaseNULL(mapping);
    IOBlockStorageDevice::free();
  }
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("UniXNU"); }
  char *getProductString() override { return const_cast<char *>("RootStage"); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>(""); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = disk.Bytes / 512 - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
      IOStorageAttributes *, IOStorageCompletion *completion) override {
    if (!online) return kIOReturnNotReady;
    if (buffer->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
    uint64_t capacity = disk.Bytes / 512;
    if (block > capacity || count > capacity - block || count * 512 > buffer->getLength()) return kIOReturnBadArgument;
    uint64_t offset = block * 512, bytes = count * 512, done = 0;
    while (done < bytes) {
      uint64_t position = offset + done;
      unsigned index = position / ROOT_STAGE_CHUNK;
      uint64_t within = position % ROOT_STAGE_CHUNK;
      uint64_t size = MIN(bytes - done, disk.Extents[index].Bytes - within);
      auto *source = reinterpret_cast<const uint8_t *>(chunks[index]->getVirtualAddress()) + within;
      IOByteCount copied = buffer->writeBytes(done, source, size);
      done += copied;
      if (copied != size) break;
    }
    __atomic_fetch_add(&native_root_block_reads, 1, __ATOMIC_RELAXED);
    completion->action(completion->target, completion->parameter,
        done == bytes ? kIOReturnSuccess : kIOReturnIOError, done);
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(NativeRootBlock, IOBlockStorageDevice)

extern "C" void native_root_block_start(void)
{
  static bool started;
  if (started) return;
  IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
  OSData *range = map ? OSDynamicCast(OSData, map->getProperty("RootDisks")) : nullptr;
  if (!range) { OSSafeReleaseNULL(map); return; }
  uint64_t location[2];
  if (range->getLength() != sizeof(location)) panic("RootStage: invalid memory-map property");
  memcpy(location, range->getBytesNoCopy(), sizeof(location));
  map->release();
  if (location[1] != sizeof(ROOT_STAGE_TABLE)) panic("RootStage: invalid table size");
  // These RAM ranges are CPU-backed storage, independent of a DMA mapper.
  IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(
      location[0],location[1],kIODirectionIn | kIOMemoryMapperNone,nullptr);
  IOMemoryMap *mapping = memory ? memory->map(kIOMapAnywhere | kIOMapReadOnly) : nullptr;
  OSSafeReleaseNULL(memory);
  if (!mapping) panic("RootStage: table mapping failed");
  const auto *table = reinterpret_cast<const ROOT_STAGE_TABLE *>(mapping->getVirtualAddress());
  if (table->Magic != ROOT_STAGE_MAGIC || table->Version != 2) panic("RootStage: invalid table header");
  extern void native_resident_configure(uint64_t);
  native_resident_configure(table->ReturnStub);
  kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
  if (result) panic("RootStage: IOStorageFamily load returned %x",result);
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  root_block_static_init();
  result = OSMetaClass::postModLoad(handle);
  if (result) panic("RootStage: class registration returned %x",result);
  result = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
  if (result) panic("RootStage: APFS load returned %x",result);
  IOService *parent = IOService::getPlatform();
  if (!parent) panic("RootStage: platform is unavailable");
  for (unsigned i = 0; i < table->DiskCount; i++) {
    auto *disk = new NativeRootBlock;
    if (!disk) panic("RootStage: disk %u allocation failed", i);
    if (!disk->init(nullptr)) panic("RootStage: disk %u initialization failed", i);
    disk->configure(table->Disks[i], i);
    if (!disk->attach(parent)) panic("RootStage: disk %u attachment failed", i);
    if (!disk->start(parent)) panic("RootStage: disk %u service start failed", i);
    disk->setName(i ? "SystemOS" : "OS");
    disk->registerService(kIOServiceSynchronous);
    IOLog("RootStage: disk %u published, %llu bytes\n",i,table->Disks[i].Bytes);
  }
  mapping->release();
  started = true;
}

extern "C" const char *native_root_block_report(void)
{
  static char report[2048];
  size_t used = snprintf(report, sizeof(report), "reads=%llu\n", native_root_block_reads);
  // Preserve the final kernel messages across the return to the USB loader.
  const size_t length = MIN((size_t)msgbufp->msg_size, sizeof(report) - used - 1);
  size_t start = (msgbufp->msg_bufx + msgbufp->msg_size - length) % msgbufp->msg_size;
  for (size_t i = 0; i < length; i++) {
    char c = msgbufp->msg_bufc[(start + i) % msgbufp->msg_size];
    if (c) report[used++] = c;
  }
  report[used] = '\0';
  return report;
}

extern "C" int native_root_block_authorized(dev_t device)
{
  IORegistryIterator *iterator = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
  if (!iterator) return 0;
  int authorized = 0;
  while (IORegistryEntry *entry = iterator->getNextObject()) {
    OSNumber *majorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
    OSNumber *minorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
    if (!majorNumber || !minorNumber || majorNumber->unsigned32BitValue() != (unsigned)major(device) ||
        minorNumber->unsigned32BitValue() != (unsigned)minor(device)) continue;
    for (IORegistryEntry *parent = entry; parent; parent = parent->getParentEntry(gIOServicePlane)) {
      NativeRootBlock *disk = OSDynamicCast(NativeRootBlock, parent);
      if (disk) { authorized = disk->isSystemRoot(); break; }
    }
    break;
  }
  iterator->release();
  return authorized;
}
