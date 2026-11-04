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
#include <libkern/c++/OSString.h>
#include <mach/kmod.h>
#include <Protocol/RootStage.h>
#include <arm/machine_routines.h>
#include <sys/param.h>
#include <libkern/zlib.h>
#include <SparseDisk.h>
#include <pexpert/pexpert.h>
#include <pexpert/boot.h>

extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_marble_root_start(void);
extern "C" int native_root_block_authorized(dev_t);
extern "C" int native_root_block_cache_authorized(dev_t);
extern "C" int native_root_block_volume_device(const char *, dev_t *, char *, size_t);
extern "C" int native_marble_data_volume(dev_t *, char *, size_t);
extern "C" const OSMetaClass *native_source_storage_parent(void);
extern "C" void marble_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void marble_root_disk_init(void) __asm__("__GLOBAL__sub_I_MarbleRootDisk.cpp");
extern "C" kmod_info_t g_kernel_kmod_info;
static const OSMetaClass *storageParent;
static char dataSeedUUID[37];
extern "C" { char *native_root_block_boot_args; }
static char residentBootArgs[BOOT_LINE_LENGTH * 2];

extern "C" const OSMetaClass *native_source_storage_parent(void)
{
    if (!storageParent) panic("MarbleStorage: base class registration order");
    return storageParent;
}

class MarbleRootDisk : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(MarbleRootDisk)
    ROOT_STAGE_DISK disk = {};
    unsigned diskIndex = 0;
    RamSparseDisk sparse = {};
    IOLock *dataGate = nullptr;
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
    void configureData(const uint8_t *packed, size_t bytes) {
        uint64_t logical = 0;
        if (!ramPackedLayout(packed, bytes, &logical) || logical > 0x100000000ULL)
            panic("MarbleStorage: packed Data layout invalid");
        sparse.bytes = logical;
        sparse.pages = static_cast<uint8_t **>(IOMallocZero(logical / 4096 * sizeof(uint8_t *)));
        struct ImportOps { uint8_t *allocate() { return static_cast<uint8_t *>(IOMallocZero(4096)); } } ops;
        if (!sparse.pages || !ramSparseImport(sparse, packed, bytes, ops) || !(dataGate = IOLockAlloc()))
            panic("MarbleStorage: sparse Data import failed");
        disk.Bytes = logical;
        diskIndex = 2;
        IOLog("MarbleStorage: Data logical bytes %llu, resident bytes %llu\n", logical, sparse.resident);
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
    IOReturn reportWriteProtection(bool *value) override { *value = diskIndex != 2; return kIOReturnSuccess; }
    IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
    IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
        IOStorageAttributes *, IOStorageCompletion *completion) override {
        bool write = buffer->getDirection() == kIODirectionOut;
        if (write && diskIndex != 2) return kIOReturnNotWritable;
        uint64_t capacity = disk.Bytes / 512;
        if (block > capacity || count > capacity - block || count * 512 > buffer->getLength())
            return kIOReturnBadArgument;
        uint64_t offset = block * 512, bytes = count * 512, done = 0;
        if (diskIndex == 2) {
            struct Operations {
                IOMemoryDescriptor *buffer;
                uint8_t *allocate() { return static_cast<uint8_t *>(IOMallocZero(4096)); }
                uint64_t read(uint64_t at, void *target, uint64_t length) { return buffer->readBytes(at, target, length); }
                uint64_t copy(uint64_t at, const void *source, uint64_t length) { return buffer->writeBytes(at, source, length); }
            } ops = {buffer};
            bool noMemory = false;
            IOLockLock(dataGate);
            done = ramSparseIO(sparse, write, offset, bytes, ops, noMemory);
            IOLockUnlock(dataGate);
            completion->action(completion->target, completion->parameter,
                done == bytes ? kIOReturnSuccess : noMemory ? kIOReturnNoMemory : kIOReturnIOError, done);
            return kIOReturnSuccess;
        }
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

static int rootBlockIndex(dev_t device)
{
    IORegistryIterator *iterator = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
    if (!iterator) return -1;
    int index = -1;
    while (IORegistryEntry *entry = iterator->getNextObject()) {
        OSNumber *majorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
        OSNumber *minorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
        if (!majorNumber || !minorNumber || majorNumber->unsigned32BitValue() != (unsigned)major(device) ||
            minorNumber->unsigned32BitValue() != (unsigned)minor(device)) continue;
        for (IORegistryEntry *parent = entry; parent; parent = parent->getParentEntry(gIOServicePlane)) {
            MarbleRootDisk *disk = OSDynamicCast(MarbleRootDisk, parent);
            if (disk) { index = static_cast<int>(disk->sourceIndex()); break; }
        }
        break;
    }
    iterator->release();
    return index;
}

extern "C" int native_root_block_authorized(dev_t device)
{
    return rootBlockIndex(device) == 0;
}

extern "C" int native_root_block_cache_authorized(dev_t device)
{
    return rootBlockIndex(device) == 1;
}

static int rootBlockVolume(const char *uuid, unsigned index, dev_t *device, char *path, size_t size)
{
    IORegistryIterator *iterator = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
    if (!iterator) return 0;
    int selected = 0;
    while (IORegistryEntry *entry = iterator->getNextObject()) {
        OSString *identity = OSDynamicCast(OSString, entry->getProperty("UUID"));
        if (!identity || strcmp(identity->getCStringNoCopy(), uuid)) continue;
        OSString *name = OSDynamicCast(OSString, entry->getProperty(kIOBSDNameKey));
        OSNumber *majorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
        OSNumber *minorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
        if (!name || !majorNumber || !minorNumber || name->getLength() + 6 > size) continue;
        dev_t candidate = static_cast<dev_t>(makedev(majorNumber->unsigned32BitValue(), minorNumber->unsigned32BitValue()));
        if (rootBlockIndex(candidate) != static_cast<int>(index)) continue;
        *device = candidate;
        snprintf(path, size, "/dev/%s", name->getCStringNoCopy());
        selected = 1;
        break;
    }
    iterator->release();
    return selected;
}

extern "C" int native_root_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
    return rootBlockVolume(uuid, 1, device, path, size);
}

extern "C" int native_marble_data_volume(dev_t *device, char *path, size_t size)
{
    return dataSeedUUID[0] && rootBlockVolume(dataSeedUUID, 2, device, path, size);
}

static voidpf dataInflateAllocate(voidpf, uInt items, uInt size)
{
    size_t bytes = static_cast<size_t>(items) * size + 16;
    uint8_t *block = static_cast<uint8_t *>(IOMallocZero(bytes));
    if (!block) return nullptr;
    memcpy(block, &bytes, sizeof(bytes));
    return block + 16;
}

static void dataInflateFree(voidpf, voidpf address)
{
    uint8_t *block = static_cast<uint8_t *>(address) - 16;
    size_t bytes;
    memcpy(&bytes, block, sizeof(bytes));
    IOFree(block, bytes);
}

static void publishData(const ROOT_STAGE_DISK &kernel, IOService *provider)
{
    constexpr uint64_t mailboxBytes = 1024 * 1024;
    if (kernel.Count != 1 || kernel.Bytes < mailboxBytes || kernel.Bytes > kernel.Extents[0].Bytes) return;
    uint8_t header[128 + BOOT_LINE_LENGTH];
    uint64_t mailbox = kernel.Extents[0].Address + kernel.Bytes - mailboxBytes;
    if (copypv(mailbox, reinterpret_cast<addr64_t>(header), sizeof(header), cppvPsrc | cppvNoRefSrc) != KERN_SUCCESS)
        panic("MarbleStorage: Data handoff copy failed");
    if (memcmp(header, "ROOTLOG1", 8)) return;
    const char *extra = reinterpret_cast<const char *>(header + 128);
    size_t extraLength = strnlen(extra, BOOT_LINE_LENGTH);
    if (extraLength) {
        if (extraLength >= BOOT_LINE_LENGTH) panic("MarbleStorage: boot arguments exceed limit");
        strlcpy(residentBootArgs, PE_boot_args(), sizeof(residentBootArgs));
        if (strlcat(residentBootArgs, " ", sizeof(residentBootArgs)) >= sizeof(residentBootArgs) ||
            strlcat(residentBootArgs, extra, sizeof(residentBootArgs)) >= sizeof(residentBootArgs))
            panic("MarbleStorage: boot arguments exceed limit");
        native_root_block_boot_args = residentBootArgs;
        IOLog("MarbleStorage: extra boot arguments %s\n", extra);
    }
    uint64_t offset = ramU64(header + 40), compressed = ramU64(header + 48), encoded = ramU64(header + 56);
    if (!compressed) return;
    if (!encoded || encoded > 0x10000000 || compressed > 0x10000000 ||
        offset > kernel.Bytes - mailboxBytes || compressed > kernel.Bytes - mailboxBytes - offset)
        panic("MarbleStorage: Data seed range invalid");
    memcpy(dataSeedUUID, header + 64, sizeof(dataSeedUUID));
    if (strnlen(dataSeedUUID, sizeof(dataSeedUUID)) != 36) panic("MarbleStorage: Data UUID invalid");
    void *input = IOMalloc(compressed), *output = IOMalloc(encoded);
    if (!input || !output) panic("MarbleStorage: Data decompression allocation failed");
    if (copypv(kernel.Extents[0].Address + offset, reinterpret_cast<addr64_t>(input),
        static_cast<unsigned>(compressed), cppvPsrc | cppvNoRefSrc) != KERN_SUCCESS)
        panic("MarbleStorage: Data seed copy failed");
    z_stream stream = {};
    stream.zalloc = dataInflateAllocate;
    stream.zfree = dataInflateFree;
    stream.next_in = static_cast<Bytef *>(input);
    stream.avail_in = static_cast<uInt>(compressed);
    stream.next_out = static_cast<Bytef *>(output);
    stream.avail_out = static_cast<uInt>(encoded);
    if (inflateInit(&stream) != Z_OK) panic("MarbleStorage: Data inflater initialization failed");
    int status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    IOFree(input, compressed);
    if (status != Z_STREAM_END || stream.total_out != encoded || stream.avail_in)
        panic("MarbleStorage: Data seed decompression failed %d", status);
    MarbleRootDisk *data = new MarbleRootDisk;
    if (!data) panic("MarbleStorage: Data disk allocation failed");
    data->configureData(static_cast<const uint8_t *>(output), encoded);
    IOFree(output, encoded);
    if (!data->init(nullptr) || !data->attach(provider) || !data->start(provider))
        panic("MarbleStorage: Data disk start failed");
    data->setName("Data");
    data->registerService(kIOServiceSynchronous);
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
    publishData(table->Disks[2], provider);
    mapping->release();
}
