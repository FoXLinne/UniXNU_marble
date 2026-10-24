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
class NativeHomeInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHomeInput)
    IOWorkLoop *inputLoop = nullptr;
    IOTimerEventSource *timer = nullptr;
    bool down = false;
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

#define TOUCH_FINGER \
    0x09,0x22,0xa1,0x02,0x09,0x42,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02, \
    0x09,0x47,0x81,0x02,0x75,0x06,0x95,0x01,0x81,0x03, \
    0x09,0x51,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02, \
    0x05,0x01,0x09,0x30,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x01,0x81,0x02, \
    0x09,0x31,0x81,0x02,0x05,0x0d,0xc0,
static const uint8_t touchDescriptor[] = {
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x03,
    TOUCH_FINGER TOUCH_FINGER TOUCH_FINGER TOUCH_FINGER TOUCH_FINGER
    TOUCH_FINGER TOUCH_FINGER TOUCH_FINGER TOUCH_FINGER TOUCH_FINGER
    0x09,0x54,0x75,0x08,0x95,0x01,0x15,0x00,0x25,0x0a,0x81,0x02,0xc0
};
#undef TOUCH_FINGER

class NativeMarbleTouchInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeMarbleTouchInput)
    IOWorkLoop *inputLoop = nullptr;
    IOTimerEventSource *tapTimer = nullptr;
    bool eventDriverMatched = false;
    uint8_t previous[10][6] = {};
    static void releaseTap(OSObject *owner, IOTimerEventSource *) {
        static_cast<NativeMarbleTouchInput *>(owner)->publish(0, nullptr);
    }
    static IOReturn send(OSObject *owner, void *countValue, void *pointsValue, void *, void *) {
        auto *self = static_cast<NativeMarbleTouchInput *>(owner);
        unsigned count = reinterpret_cast<uintptr_t>(countValue);
        auto *points = static_cast<const uint8_t *>(pointsValue);
        uint8_t report[62] = {3}, current[10][6] = {};
        unsigned active = 0;
        bool pending = false;
        for (unsigned i = 0; i < count; ++i) {
            const uint8_t *p = points + 8 * i;
            unsigned id = p[0] >> 4;
            if (id >= 10) continue;
            unsigned x = p[2] | unsigned(p[3]) << 8, y = p[4] | unsigned(p[5]) << 8;
            x = (x < 1080 ? x : 1079) * 32767 / 1079;
            y = (y < 2400 ? y : 2399) * 32767 / 2399;
            uint8_t *finger = current[id];
            finger[0] = 3; finger[1] = id;
            finger[2] = x; finger[3] = x >> 8; finger[4] = y; finger[5] = y >> 8;
        }
        for (unsigned id = 0; id < 10; ++id) {
            uint8_t *finger = report + 1 + 6 * id;
            if (current[id][0]) {
                memcpy(finger, current[id], 6);
                active++;
                pending = true;
            }
            else if (self->previous[id][0]) {
                memcpy(finger, self->previous[id], 6);
                finger[0] = 2;
                pending = true;
            }
            // Every parallel collection keeps its own contact identifier, including idle slots.
            finger[1] = id;
        }
        memcpy(self->previous, current, sizeof(current));
        if (!pending) return kIOReturnSuccess;
        report[61] = active;
        auto *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
        IOReturn result = buffer ? self->handleReport(buffer, 0, 0) : kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6, result);
        return result;
    }
public:
    IOWorkLoop *getWorkLoop() const override { return inputLoop; }
    OSString *newTransportString() const override { return OSString::withCString("SPI"); }
    OSString *newProductString() const override { return OSString::withCString("MarbleTouch"); }
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        *out = IOBufferMemoryDescriptor::withBytes(touchDescriptor, sizeof(touchDescriptor), kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        inputLoop = IOWorkLoop::workLoop();
        setProperty("Built-In", true);
        setProperty("DisplayIntegrated", true);
        setProperty("DigitizerCollectionDispatch", true);
        setProperty("HIDDefaultBehavior", true);
        if (!inputLoop || !IOHIDDevice::start(provider)) return false;
        tapTimer = IOTimerEventSource::timerEventSource(this, releaseTap);
        return tapTimer && inputLoop->addEventSource(tapTimer) == kIOReturnSuccess;
    }
    void publish(unsigned count, const uint8_t *points) {
        if (!eventDriverMatched) eventDriverMatched = hasEventDriver(this, 4);
        native_hid_input_observed(7, eventDriverMatched);
        inputLoop->runAction(send, this, reinterpret_cast<void *>(uintptr_t(count)), const_cast<uint8_t *>(points));
    }
    void tap(unsigned x, unsigned y) {
        uint8_t point[8] = {0, 0, uint8_t(x), uint8_t(x >> 8), uint8_t(y), uint8_t(y >> 8), 1, 0};
        IOLog("MarbleTouch: synthetic tap x=%u y=%u\n", x, y);
        publish(1, point);
        tapTimer->setTimeoutMS(60);
    }
    void stop(IOService *provider) override {
        if (tapTimer) { tapTimer->cancelTimeout(); inputLoop->removeEventSource(tapTimer); OSSafeReleaseNULL(tapTimer); }
        IOHIDDevice::stop(provider);
    }
    void free() override { OSSafeReleaseNULL(inputLoop); IOHIDDevice::free(); }
};
OSDefineMetaClassAndStructors(NativeMarbleTouchInput, IOHIDDevice)
static NativeMarbleTouchInput *touchInput;
extern "C" IOService *native_hid_touch_allocate(void) {
    touchInput = new NativeMarbleTouchInput;
    return touchInput;
}
extern "C" void native_touch_publish(unsigned count, const uint8_t *points) {
    if (touchInput) touchInput->publish(count, points);
}
extern "C" void native_hid_request_tap(unsigned x, unsigned y) {
    if (touchInput && x < 1080 && y < 2400) touchInput->tap(x, y);
}
