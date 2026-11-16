// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOBSD.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <sys/param.h>

extern "C" { volatile uint32_t native_qemu_data_volume_ready; }

extern "C" int native_qemu_scratch_volume(dev_t *device, char *path, size_t capacity)
{
    if (!device || !path || capacity < 7) return 0;
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *data = chosen ? OSDynamicCast(OSData, chosen->getProperty("qemu-data-volume-uuid")) : nullptr;
    char uuid[37] = {};
    if (data && data->getLength() == sizeof(uuid)) memcpy(uuid, data->getBytesNoCopy(), sizeof(uuid));
    OSSafeReleaseNULL(chosen);
    if (strnlen(uuid, sizeof(uuid)) != 36) return 0;

    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSString *identity = OSString::withCString(uuid);
    if (!matching || !properties || !identity) {
        OSSafeReleaseNULL(matching); OSSafeReleaseNULL(properties); OSSafeReleaseNULL(identity);
        return 0;
    }
    properties->setObject("UUID", identity);
    matching->setObject(kIOPropertyMatchKey, properties);
    IOService *volume = IOService::waitForMatchingService(matching, 10000000000ULL);
    OSSafeReleaseNULL(matching); OSSafeReleaseNULL(properties); OSSafeReleaseNULL(identity);
    if (!volume) return 0;
    OSBoolean *writable = OSDynamicCast(OSBoolean, volume->getProperty("Writable"));
    OSString *name = OSDynamicCast(OSString, volume->getProperty(kIOBSDNameKey));
    OSNumber *majorNumber = OSDynamicCast(OSNumber, volume->getProperty(kIOBSDMajorKey));
    OSNumber *minorNumber = OSDynamicCast(OSNumber, volume->getProperty(kIOBSDMinorKey));
    bool scratch = false;
    for (IORegistryEntry *entry = volume; entry; entry = entry->getParentEntry(gIOServicePlane)) {
        if (strcmp(entry->getMetaClass()->getClassName(), "NativeVirtioBlockData") == 0) {
            scratch = true;
            break;
        }
    }
    bool accepted = scratch && writable && writable->isTrue() && name && majorNumber && minorNumber &&
                    name->getLength() + 6 <= capacity;
    if (accepted) {
        *device = static_cast<dev_t>(makedev(majorNumber->unsigned32BitValue(), minorNumber->unsigned32BitValue()));
        snprintf(path, capacity, "/dev/%s", name->getCStringNoCopy());
        native_qemu_data_volume_ready = 1;
    }
    volume->release();
    return accepted;
}
