// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalHidDeviceABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSString.h>
extern "C" void native_hid_input_observed(unsigned,int);
extern "C" void native_hid_live_checkpoint(IOService *);
extern "C" bool native_hid_input_requested(void);

// Keyboard collection selects the original generic event driver. Report 2 is Consumer Menu.
static const uint8_t reportDescriptor[] = {
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,0x75,0x08,0x95,0x06,0x81,0x00,
    0x85,0x02,0x05,0x0c,0x09,0x40,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x01,0x81,0x02,0x75,0x07,0x95,0x01,0x81,0x03,0xc0
};

class NativeHomeInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHomeInput)
    IOWorkLoop *inputLoop = nullptr;
    IOTimerEventSource *timer = nullptr;
    bool down = false;
    static bool hasEventDriver(IORegistryEntry *parent, unsigned depth) {
        if (!depth) return false;
        OSIterator *iterator = parent->getChildIterator(gIOServicePlane);
        if (!iterator) return false;
        bool found = false;
        while (OSObject *object = iterator->getNextObject()) {
            IORegistryEntry *entry = OSDynamicCast(IORegistryEntry,object);
            if (entry && (entry->metaCast("IOHIDEventDriver") || hasEventDriver(entry,depth-1))) { found=true; break; }
        }
        iterator->release(); return found;
    }
    static void tick(OSObject *owner, IOTimerEventSource *source) {
        NativeHomeInput *self = OSDynamicCast(NativeHomeInput,owner);
        native_hid_live_checkpoint(self);
        bool matched = hasEventDriver(self,4);
        native_hid_input_observed(3,matched);
        if (!self->down && (!matched || !native_hid_input_requested())) { source->setTimeoutMS(1000); return; }
        uint8_t report[] = {2,static_cast<uint8_t>(self->down ? 0 : 1)};
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result = buffer ? self->handleReport(buffer,0,0) : kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(self->down ? 5 : 4,result);
        self->down = !self->down;
        source->setTimeoutMS(self->down ? 100 : 1000);
    }
public:
    IOWorkLoop *getWorkLoop() const override { return inputLoop; }
    OSString *newTransportString() const override { return OSString::withCString("Virtual"); }
    OSString *newProductString() const override { return OSString::withCString("VirtualHomeInput"); }
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        *out = IOBufferMemoryDescriptor::withBytes(reportDescriptor,sizeof(reportDescriptor),kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        inputLoop = IOWorkLoop::workLoop();
        if (!inputLoop || !setProperty("HIDDefaultBehavior",true) || !IOHIDDevice::start(provider)) return false;
        timer = IOTimerEventSource::timerEventSource(this,tick);
        if (!timer || inputLoop->addEventSource(timer) != kIOReturnSuccess) {
            OSSafeReleaseNULL(timer); IOHIDDevice::stop(provider); return false;
        }
        native_hid_input_observed(2,0); timer->setTimeoutMS(1000); return true;
    }
    void stop(IOService *provider) override {
        if (timer) { timer->cancelTimeout(); inputLoop->removeEventSource(timer); OSSafeReleaseNULL(timer); }
        IOHIDDevice::stop(provider);
    }
    void free() override { OSSafeReleaseNULL(inputLoop); IOHIDDevice::free(); }
};
OSDefineMetaClassAndStructors(NativeHomeInput,IOHIDDevice)
extern "C" IOService *native_hid_input_allocate(void) { return new NativeHomeInput; }
