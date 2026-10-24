#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s HUB_LOCATION_ID PORT\n", argv[0]);
    return 2;
  }
  unsigned target = (unsigned)strtoul(argv[1], NULL, 0);
  unsigned port = (unsigned)strtoul(argv[2], NULL, 0);
  if (!port || port > 255)
    return 2;
  io_iterator_t iterator = 0;
  IOServiceGetMatchingServices(kIOMainPortDefault,
                               IOServiceMatching("IOUSBHostDevice"), &iterator);
  io_service_t service = 0, entry;
  while ((entry = IOIteratorNext(iterator))) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        entry, CFSTR("locationID"), kCFAllocatorDefault, 0);
    int location = 0;
    if (value && CFGetTypeID(value) == CFNumberGetTypeID())
      CFNumberGetValue(value, kCFNumberIntType, &location);
    if (value)
      CFRelease(value);
    if ((unsigned)location == target) {
      service = entry;
      break;
    }
    IOObjectRelease(entry);
  }
  IOObjectRelease(iterator);
  if (!service)
    return 1;
  IOCFPlugInInterface **plugin = NULL;
  SInt32 score = 0;
  IOReturn r = IOCreatePlugInInterfaceForService(
      service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plugin,
      &score);
  IOObjectRelease(service);
  printf("client=%08x\n", r);
  if (r)
    return 1;
  IOUSBDeviceInterface500 **device = NULL;
  HRESULT q = (*plugin)->QueryInterface(
      plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),
      (LPVOID *)&device);
  if (q) {
    IODestroyPlugInInterface(plugin);
    return 1;
  }
  uint32_t data = 0;
  IOUSBDevRequestTO request = {0};
  request.bmRequestType = 0xa3;
  request.bRequest = 0;
  request.wIndex = port;
  request.wLength = 4;
  request.pData = &data;
  request.noDataTimeout = 3000;
  request.completionTimeout = 3000;
  r = (*device)->DeviceRequestTO(device, &request);
  printf("port=%u request=%08x data=%08x bytes=%u\n", port, r, data,
         request.wLenDone);
  if (!r && (data & 1)) {
    unsigned char descriptor[9] = {0};
    request.bmRequestType = 0xa0;
    request.bRequest = 6;
    request.wValue = 0x2900;
    request.wIndex = 0;
    request.wLength = 9;
    request.pData = descriptor;
    r = (*device)->DeviceRequestTO(device, &request);
    unsigned characteristics = descriptor[3] | descriptor[4] << 8;
    printf("hub descriptor=%08x characteristics=%04x\n", r, characteristics);
    if (!r && request.wLenDone >= 7 && (characteristics & 3) <= 1) {
      request.bmRequestType = 0x23;
      request.bRequest = 1;
      request.wValue = 8;
      request.wIndex = port;
      request.wLength = 0;
      request.pData = NULL;
      r = (*device)->DeviceRequestTO(device, &request);
      printf("port=%u power_off=%08x\n", port, r);
      if (!r) {
        usleep(1000000);
        request.bRequest = 3;
        r = (*device)->DeviceRequestTO(device, &request);
        printf("port=%u power_on=%08x\n", port, r);
      }
    }
  }
  (*device)->Release(device);
  IODestroyPlugInInterface(plugin);
  return r ? 1 : 0;
}
