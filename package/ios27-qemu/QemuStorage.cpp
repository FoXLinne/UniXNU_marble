// SPDX-License-Identifier: BSD-2-Clause-Patent
// marble iOS 27.2 (24B119) QEMU virtual-storage module.
//
// Links beside libMarbleStorage.a: the UniXNUBlockStorageDevice source base
// and native_source_storage_parent() come from that archive, and
// package/MarbleVirtualStorage.h defines this module's contract (the weak
// stubs in MarbleVirtualStorage.cpp are replaced by these definitions).
//
//   native_qemu_storage_metaclass_init() - manually registers the late source
//       NativeVirtioBlock / NativeVirtioBlockData metaclasses; called by the
//       board agent inside its OSMetaClass::preModLoad()/postModLoad() window
//       after the storage base metaclass is registered.
//   native_qemu_storage_start() - enumerates real IOPlatformDevice children
//       of IOService::getPlatform() whose compatible is "virtio,mmio",
//       selects the class by the device's actual VIRTIO_BLK_F_RO feature,
//       then attaches/probes/starts the driver; registerService() inside each
//       start() lets the collection's IOBlockStorageDriver publish IOMedia.
//   native_qemu_volume_device() / native_qemu_root_authorized() /
//       native_qemu_cache_authorized() - signed-policy-backed lookups built on
//       the pinned upstream VerifiedRoot exports, never unconditional.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/types.h>
#include <sys/param.h>

extern "C" {
// The VM archive binds two actual VirtIO transports. A provider on a third
// storage transport carries no policy in this module.
int native_ufs_block_authenticates(IOService *, const char *, uint8_t *) { return -1; }
// Exported by pinned upstream VirtioMmioBlock NativeVirtioBlock.cpp
// (4e1013eb8c45077b57f8d38d61b389fe899ea1c3), linked into this archive.
int native_verified_block_root_authorized(dev_t);
int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]);
int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size);
}

// Late static-init functions: the build strips __DATA,__mod_init_func and
// globalizes each translation unit's init symbol so registration ordering is
// controlled by the board agent's mod-load window.
extern "C" void qemu_virtio_block_class_init(void) __asm__("__GLOBAL__sub_I_NativeVirtioBlock.cpp");
extern "C" void qemu_virtio_data_class_init(void) __asm__("__GLOBAL__sub_I_NativeVirtioBlockData.cpp");

extern "C" {
volatile uint32_t native_qemu_metaclass_ready = 0;
volatile uint32_t native_qemu_platform_children = 0;
volatile uint32_t native_qemu_mmio_devices = 0;
volatile uint32_t native_qemu_block_devices = 0;
volatile uint32_t native_qemu_verified_started = 0;
volatile uint32_t native_qemu_writable_started = 0;
volatile uint32_t native_qemu_start_failures = 0;
volatile uint32_t native_qemu_storage_started = 0;
volatile uint32_t native_qemu_root_authorized_count = 0;
volatile uint32_t native_qemu_root_refused = 0;
volatile uint32_t native_qemu_cache_authorized_count = 0;
volatile uint32_t native_qemu_cache_refused = 0;
volatile uint32_t native_qemu_volume_lookups = 0;
volatile uint32_t native_qemu_volume_authorized = 0;
uint8_t native_qemu_root_manifest_sha384[48] = {};
uint8_t native_qemu_cache_manifest_sha384[48] = {};
}

// Signed-policy volume identities: the manifest generator binds each raw
// image to exactly these UUIDs, so authorization proves the disk's signed
// manifest, the media's registry UUID, and the requested role all agree.
static const char kQemuRootVolumeUUID[] = "E0577111-0369-4BCF-A4E5-59B612AA9E31";
static const char kQemuCacheVolumeUUID[] = "46885FBD-E14C-4630-805A-8AC4BFD8D326";

extern "C" void native_qemu_storage_metaclass_init(void)
{
  qemu_virtio_block_class_init();
  qemu_virtio_data_class_init();
  native_qemu_metaclass_ready = 1;
}

// -1: not a block device or unreadable registers; 0: writable; 1: read-only.
static int qemuVirtioBlockReadOnly(IOService *provider)
{
  if (provider->getResources() != kIOReturnSuccess) return -1;
  IOMemoryMap *mapping = provider->mapDeviceMemoryWithIndex(
      0, kIOMapAnywhere | kIOMapInhibitCache);
  if (!mapping) return -1;
  if (mapping->getLength() < 0x108) { mapping->release(); return -1; }
  volatile uint32_t *registers = reinterpret_cast<volatile uint32_t *>(
      static_cast<uintptr_t>(mapping->getVirtualAddress()));
  bool block = registers[0x00 / 4] == 0x74726976 && registers[0x04 / 4] == 2 &&
               registers[0x08 / 4] == 2;
  registers[0x14 / 4] = 0;
  uint32_t lowFeatures = registers[0x10 / 4];
  mapping->release();
  if (!block) return -1;
  return (lowFeatures & (1U << 5)) ? 1 : 0;
}

static bool qemuVirtioStartDisk(IOService *provider, const char *className)
{
  IOService *disk = OSDynamicCast(
      IOService, OSMetaClass::allocClassWithName(className));
  if (!disk) return false;
  SInt32 score = 0;
  bool attached = disk->init() && disk->attach(provider);
  bool started = attached && disk->probe(provider, &score) &&
                 disk->start(provider);
  if (!started) {
    if (attached) disk->detach(provider);
    disk->release();
  }
  return started;
}

extern "C" void native_qemu_storage_start(void)
{
  if (!native_qemu_metaclass_ready || native_qemu_storage_started) {
    ++native_qemu_start_failures;
    return;
  }
  IOService *platform = IOService::getPlatform();
  if (!platform) {
    ++native_qemu_start_failures;
    return;
  }
  OSIterator *iterator = platform->getChildIterator(gIOServicePlane);
  if (!iterator) {
    ++native_qemu_start_failures;
    return;
  }
  while (OSObject *object = iterator->getNextObject()) {
    ++native_qemu_platform_children;
    IOService *provider = OSDynamicCast(IOService, object);
    if (!provider) continue;
    OSData *compatible = OSDynamicCast(OSData, provider->getProperty("compatible"));
    if (!compatible || compatible->getLength() != 12 ||
        memcmp(compatible->getBytesNoCopy(), "virtio,mmio", 12)) continue;
    ++native_qemu_mmio_devices;
    int readOnly = qemuVirtioBlockReadOnly(provider);
    if (readOnly < 0) continue;
    ++native_qemu_block_devices;
    // The disk's real RO feature selects the class: a read-only device only
    // ever binds the signed upstream driver, a writable device only the
    // unverified scratch class. There is no writable fallback for a failed
    // verified attach.
    if (readOnly) {
      if (qemuVirtioStartDisk(provider, "NativeVirtioBlock"))
        ++native_qemu_verified_started;
      else
        ++native_qemu_start_failures;
    } else {
      if (qemuVirtioStartDisk(provider, "NativeVirtioBlockData"))
        ++native_qemu_writable_started;
      else
        ++native_qemu_start_failures;
    }
  }
  iterator->release();
  native_qemu_storage_started = 1;
}

extern "C" int native_qemu_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
  ++native_qemu_volume_lookups;
  int selected = native_verified_block_volume_device(uuid, device, path, size);
  if (selected) ++native_qemu_volume_authorized;
  return selected;
}

extern "C" int native_qemu_root_authorized(dev_t device)
{
  uint8_t digest[48];
  int authorized = native_verified_block_manifest_sha384(device, kQemuRootVolumeUUID, digest);
  if (authorized) {
    ++native_qemu_root_authorized_count;
    memcpy(native_qemu_root_manifest_sha384, digest, sizeof(digest));
  } else {
    ++native_qemu_root_refused;
  }
  return authorized;
}

extern "C" int native_qemu_cache_authorized(dev_t device)
{
  uint8_t digest[48];
  int authorized = native_verified_block_manifest_sha384(device, kQemuCacheVolumeUUID, digest);
  if (authorized) {
    ++native_qemu_cache_authorized_count;
    memcpy(native_qemu_cache_manifest_sha384, digest, sizeof(digest));
  } else {
    ++native_qemu_cache_refused;
  }
  return authorized;
}
