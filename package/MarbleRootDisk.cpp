// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <mach/kmod.h>
#include <Protocol/RootStage.h>
#include <arm/machine_routines.h>
#include <sys/param.h>

extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_marble_root_start(void);
extern "C" int native_root_block_authorized(dev_t);
extern "C" const OSMetaClass *native_source_storage_parent(void);
extern "C" void marble_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void marble_root_disk_init(void) __asm__("__GLOBAL__sub_I_MarbleRootDisk.cpp");
extern "C" kmod_info_t g_kernel_kmod_info;
static const OSMetaClass *storageParent;

extern "C" const OSMetaClass *native_source_storage_parent(void)
{
    if (!storageParent) panic("MarbleStorage: base class registration order");
    return storageParent;
}

class MarbleRootDisk : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(MarbleRootDisk)
    ROOT_STAGE_DISK disk = {};
    unsigned diskIndex = 0;
public:
    unsigned sourceIndex() const { return diskIndex; }
    void configure(const ROOT_STAGE_DISK &source, unsigned index) {
        if (!source.Bytes || source.Bytes % 512 || source.Received != source.Bytes ||
            source.Count != (source.Bytes + ROOT_STAGE_CHUNK - 1) / ROOT_STAGE_CHUNK ||
            source.Count > ROOT_STAGE_EXTENTS) panic("MarbleStorage: incomplete root image");
        uint64_t total = 0;
        for (unsigned i = 0; i < source.Count; ++i) {
            uint64_t size = MIN(ROOT_STAGE_CHUNK, source.Bytes - total);
            if (!source.Extents[i].Address || source.Extents[i].Bytes != size)
                panic("MarbleStorage: invalid root extent %u", i);
            total += size;
        }
        disk = source;
        diskIndex = index;
    }
    IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
    IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
    char *getVendorString() override { return const_cast<char *>("UniXNU"); }
    char *getProductString() override { return const_cast<char *>("iOS Root"); }
    char *getRevisionString() override { return const_cast<char *>("1"); }
    char *getAdditionalDeviceInfoString() override { return const_cast<char *>(""); }
    IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
    IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
    IOReturn reportMaxValidBlock(UInt64 *value) override { *value = disk.Bytes / 512 - 1; return kIOReturnSuccess; }
    IOReturn reportMediaState(bool *present, bool *changed) override {
        *present = true; if (changed) *changed = false; return kIOReturnSuccess;
    }
    IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
    IOReturn reportWriteProtection(bool *value) override { *value = true; return kIOReturnSuccess; }
    IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
    IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
        IOStorageAttributes *, IOStorageCompletion *completion) override {
        if (buffer->getDirection() == kIODirectionOut) return kIOReturnNotWritable;
        uint64_t capacity = disk.Bytes / 512;
        if (block > capacity || count > capacity - block || count * 512 > buffer->getLength())
            return kIOReturnBadArgument;
        uint64_t offset = block * 512, bytes = count * 512, done = 0;
        while (done < bytes) {
            uint64_t position = offset + done, within = position % ROOT_STAGE_CHUNK;
            const ROOT_STAGE_EXTENT &extent = disk.Extents[position / ROOT_STAGE_CHUNK];
            IOByteCount available;
            addr64_t destination = buffer->getPhysicalSegment(done, &available, kIOMemoryMapperNone);
            if (!destination || !available) break;
            uint64_t size = MIN(bytes - done, MIN(extent.Bytes - within, available));
            if (copypv(extent.Address + within, destination, static_cast<unsigned int>(size),
                cppvPsrc | cppvPsnk | cppvNoRefSrc) != KERN_SUCCESS) break;
            done += size;
        }
        completion->action(completion->target, completion->parameter,
            done == bytes ? kIOReturnSuccess : kIOReturnIOError, done);
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(MarbleRootDisk, IOBlockStorageDevice)

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
            MarbleRootDisk *disk = OSDynamicCast(MarbleRootDisk, parent);
            if (disk) { authorized = disk->sourceIndex() == 0; break; }
        }
        break;
    }
    iterator->release();
    IOLog("MarbleStorage: resident root policy device %u:%u accepted %d\n", major(device), minor(device), authorized);
    return authorized;
}

extern "C" void native_marble_root_start(void)
{
    kern_return_t status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status) panic("MarbleStorage: family load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("MarbleStorage: original base size %u differs from source %lu",
            storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("MarbleStorage: class registration handle unavailable");
    marble_storage_base_init();
    marble_root_disk_init();
    status = OSMetaClass::postModLoad(handle);
    if (status) panic("MarbleStorage: class registration failed %x", status);
    IOLog("MarbleStorage: source arm64e block class registered, base size %lu\n", sizeof(IOBlockStorageDevice));
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    OSData *range = map ? OSDynamicCast(OSData, map->getProperty("RootDisks")) : nullptr;
    if (!range || range->getLength() != 16) panic("MarbleStorage: RootDisks handoff absent");
    uint64_t location[2];
    memcpy(location, range->getBytesNoCopy(), sizeof(location));
    map->release();
    if (!location[0] || location[1] != sizeof(ROOT_STAGE_TABLE)) panic("MarbleStorage: RootDisks handoff invalid");
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(
        location[0], location[1], kIODirectionIn | kIOMemoryMapperNone, nullptr);
    IOMemoryMap *mapping = memory ? memory->map(kIOMapAnywhere | kIOMapReadOnly) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!mapping) panic("MarbleStorage: RootDisks table mapping failed");
    const ROOT_STAGE_TABLE *table = reinterpret_cast<const ROOT_STAGE_TABLE *>(mapping->getVirtualAddress());
    if (table->Magic != ROOT_STAGE_MAGIC || table->Version != 2 || table->DiskCount > 2)
        panic("MarbleStorage: RootDisks header invalid");
    IOService *provider = IOService::getPlatform();
    for (unsigned i = 0; i < table->DiskCount; ++i) {
        MarbleRootDisk *disk = new MarbleRootDisk;
        if (!disk) panic("MarbleStorage: root disk allocation failed");
        disk->configure(table->Disks[i], i);
        if (!disk->init(nullptr) || !disk->attach(provider) || !disk->start(provider))
            panic("MarbleStorage: root disk %u start failed", i);
        disk->registerService(kIOServiceSynchronous);
        IOLog("MarbleStorage: root disk %u published, bytes %llu\n", i, table->Disks[i].Bytes);
    }
    mapping->release();
}
