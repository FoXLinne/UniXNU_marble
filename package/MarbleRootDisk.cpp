// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <mach/kmod.h>
#include <Protocol/RootStage.h>
#include <arm/machine_routines.h>
#include <sys/param.h>
#include <libkern/zlib.h>
#include <SparseDisk.h>
#include "MarbleUfs.h"
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
extern "C" {
char marble_data_uuid[37];
RamSparseDisk *marble_data_sparse;
const uint8_t *marble_adsp_firmware;
extern const uint64_t marble_adsp_firmware_size;
uint64_t marble_data_bytes;
volatile uint64_t marble_ufs_reads, marble_ufs_writes;
}
extern "C" void marble_runtime18_start(void);
extern "C" void marble_runtime18_observe(unsigned);
extern "C" void marble_runtime18_observation_start(void);
extern "C" void native_resident_configure(uint64_t);
extern "C" void native_resident_usb_control(const ROOT_STAGE_TABLE *);
extern "C" bool native_root_block_has_data(void);
extern "C" bool native_root_block_has_data(void) { return marble_data_bytes != 0; }
extern "C" { char *native_root_block_boot_args; }
// The resident interrupt worker owns its shared table for the kernel lifetime.
static IOMemoryMap *residentControlMapping;
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
    uint8_t *dataScratch = nullptr;
    IOLock *dataGate = nullptr;
public:
    unsigned sourceIndex() const { return diskIndex; }
    void configure(const ROOT_STAGE_DISK &source, unsigned index) {
        (void)source;
        disk.Bytes = marble_ufs_root_bytes(index);
        if (!disk.Bytes) panic("MarbleStorage: UFS root window %u absent", index);
        diskIndex = index;
    }
    void configureData(uint64_t bytes) {
        if (!bytes || (bytes & 4095) || !marble_ufs_store_open(bytes / 4096))
            panic("MarbleStorage: UFS Data window invalid");
        dataScratch = static_cast<uint8_t *>(IOMalloc(128 * 1024));
        dataGate = IOLockAlloc();
        if (!dataScratch || !dataGate) panic("MarbleStorage: Data I/O allocation failed");
        disk.Bytes = bytes;
        diskIndex = 2;
        marble_data_bytes = bytes;
        const bool probe = marble_ufs_store_read(0, 1, dataScratch);
        uint32_t magic = 0;
        if (probe) memcpy(&magic, dataScratch + 32, sizeof(magic));
        IOLog("MarbleStorage: UFS Data bytes %llu header_read=%u magic=%x\n", bytes, probe, magic);
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
    IOReturn doSynchronizeCache() override {
        return diskIndex != 2 || marble_ufs_store_sync() ? kIOReturnSuccess : kIOReturnIOError;
    }
    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
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
            IOLockLock(dataGate);
            while (done < bytes) {
                uint64_t at = offset + done;
                unsigned within = static_cast<unsigned>(at & 4095);
                uint64_t length = MIN(bytes - done, uint64_t(128 * 1024 - within));
                uint32_t blocks = static_cast<uint32_t>((within + length + 4095) / 4096);
                if ((!write || within || (length & 4095)) &&
                    !marble_ufs_store_read(at / 4096, blocks, dataScratch)) break;
                if (write) {
                    if (buffer->readBytes(done, dataScratch + within, length) != length ||
                        !marble_ufs_store_write(at / 4096, blocks, dataScratch)) break;
                    ++marble_ufs_writes;
                } else {
                    if (buffer->writeBytes(done, dataScratch + within, length) != length) break;
                    ++marble_ufs_reads;
                }
                done += length;
            }
            IOLockUnlock(dataGate);
        } else {
            done = marble_ufs_root_read(diskIndex, buffer, offset, bytes);
            ++marble_ufs_reads;
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

static int waitRootBlockVolume(const char *uuid, unsigned index, dev_t *device, char *path, size_t size)
{
    marble_runtime18_observation_start();
    if (rootBlockVolume(uuid, index, device, path, size)) return 1;
    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSString *identity = OSString::withCString(uuid);
    OSString *bsd = OSString::withCString(kIOBSDNameKey);
    if (!matching || !properties || !identity || !bsd) {
        OSSafeReleaseNULL(matching); OSSafeReleaseNULL(properties);
        OSSafeReleaseNULL(identity); OSSafeReleaseNULL(bsd);
        return 0;
    }
    properties->setObject("UUID", identity);
    matching->setObject(kIOPropertyMatchKey, properties);
    matching->setObject(kIOPropertyExistsMatchKey, bsd);
    IOLog("MarbleStorage: await volume uuid=%s index=%u\n", uuid, index);
    IOService *volume = IOService::waitForMatchingService(matching, UINT64_C(10000000000));
    const int selected = volume && rootBlockVolume(uuid, index, device, path, size);
    IOLog("MarbleStorage: volume uuid=%s matched=%u selected=%d\n", uuid, volume != nullptr, selected);
    OSSafeReleaseNULL(volume); OSSafeReleaseNULL(matching); OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(identity); OSSafeReleaseNULL(bsd);
    return selected;
}

extern "C" int native_root_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
    return waitRootBlockVolume(uuid, 1, device, path, size);
}

extern "C" int native_marble_data_volume(dev_t *device, char *path, size_t size)
{
    return marble_data_uuid[0] && waitRootBlockVolume(marble_data_uuid, 2, device, path, size);
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

static uint8_t *inflatePayload(const void *input, uint64_t compressed, uint64_t encoded)
{
    uint8_t *output = static_cast<uint8_t *>(IOMalloc(encoded));
    if (!output) panic("MarbleStorage: payload decompression allocation failed");
    z_stream stream = {};
    stream.zalloc = dataInflateAllocate;
    stream.zfree = dataInflateFree;
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<void *>(input));
    stream.avail_in = static_cast<uInt>(compressed);
    stream.next_out = output;
    stream.avail_out = static_cast<uInt>(encoded);
    if (inflateInit(&stream) != Z_OK) panic("MarbleStorage: payload inflater initialization failed");
    int status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (status != Z_STREAM_END || stream.total_out != encoded || stream.avail_in)
        panic("MarbleStorage: payload decompression failed %d", status);
    return output;
}

static uint8_t *inflateHandoff(const ROOT_STAGE_DISK &kernel, uint64_t tableOffset,
                               uint64_t compressed, uint64_t encoded)
{
    uint64_t limit = kernel.Bytes - 1024 * 1024;
    if (tableOffset > limit || 272 > limit - tableOffset)
        panic("MarbleStorage: payload descriptor range invalid");
    if (!compressed || tableOffset > limit || compressed > limit - tableOffset)
        panic("MarbleStorage: firmware range invalid");
    uint64_t copyBytes = (compressed + 4095) & ~uint64_t(4095);
    uint8_t *input = static_cast<uint8_t *>(IOMalloc(copyBytes));
    if (!input) panic("MarbleStorage: firmware copy allocation failed");
    if (copypv(kernel.Extents[0].Address + tableOffset, reinterpret_cast<addr64_t>(input),
        static_cast<unsigned int>(copyBytes), cppvPsrc | cppvKmap | cppvNoRefSrc) != KERN_SUCCESS)
        panic("MarbleStorage: firmware copy failed");
    uint8_t *output = inflatePayload(input, compressed, encoded);
    IOFree(input, copyBytes);
    return output;
}

static uint64_t prepareDataHandoff(const ROOT_STAGE_DISK &kernel)
{
    constexpr uint64_t mailboxBytes = 1024 * 1024;
    if (kernel.Count != 1 || kernel.Bytes < mailboxBytes || kernel.Bytes > kernel.Extents[0].Bytes)
        panic("MarbleStorage: kernel handoff invalid");
    uint8_t header[128 + BOOT_LINE_LENGTH];
    uint64_t mailbox = kernel.Extents[0].Address + kernel.Bytes - mailboxBytes;
    if (copypv(mailbox, reinterpret_cast<addr64_t>(header), sizeof(header), cppvPsrc | cppvKmap | cppvNoRefSrc) != KERN_SUCCESS)
        panic("MarbleStorage: Data handoff copy failed");
    if (memcmp(header, "ROOTLOG1", 8)) panic("MarbleStorage: mailbox header invalid");
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
    uint32_t observationSeconds = 0;
    memcpy(&observationSeconds, header + 32, sizeof(observationSeconds));
    marble_runtime18_observe(observationSeconds);
    marble_runtime18_start();
    uint64_t firmwareOffset = ramU64(header + 104), firmwareBytes = ramU64(header + 112);
    if (firmwareBytes) {
        uint64_t firmwareRawBytes = ramU64(header + 120);
        if (firmwareRawBytes != marble_adsp_firmware_size || firmwareBytes > firmwareRawBytes)
            panic("MarbleStorage: ADSP firmware range invalid");
        marble_adsp_firmware = inflateHandoff(kernel, firmwareOffset, firmwareBytes, firmwareRawBytes);
    }
    uint64_t logical = ramU64(header + 56);
    memcpy(marble_data_uuid, header + 64, sizeof(marble_data_uuid));
    if (strnlen(marble_data_uuid, sizeof(marble_data_uuid)) != 36) panic("MarbleStorage: Data UUID invalid");
    return logical;
}

static void publishData(uint64_t logical, IOService *provider)
{
    MarbleRootDisk *data = new MarbleRootDisk;
    if (!data) panic("MarbleStorage: Data disk allocation failed");
    data->configureData(logical);
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
        location[0], location[1], kIODirectionInOut | kIOMemoryMapperNone, nullptr);
    IOMemoryMap *mapping = memory ? memory->map(kIOMapAnywhere | kIOMapCopybackCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!mapping) panic("MarbleStorage: RootDisks table mapping failed");
    const ROOT_STAGE_TABLE *table = reinterpret_cast<const ROOT_STAGE_TABLE *>(mapping->getVirtualAddress());
    if (table->Magic != ROOT_STAGE_MAGIC || table->Version != ROOT_STAGE_VERSION || table->DiskCount > 2)
        panic("MarbleStorage: RootDisks header invalid");
    uint64_t dataBytes = prepareDataHandoff(table->Disks[2]);
    residentControlMapping=mapping;
    native_resident_configure(table->ReturnStub);
    native_resident_usb_control(table);
    if (!marble_ufs_start()) panic("MarbleStorage: UFS initialization failed");
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
    publishData(dataBytes, provider);
}
