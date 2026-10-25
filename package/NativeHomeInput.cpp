// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalHidDeviceABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSArray.h>
#include <IOKit/IOLib.h>
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

static IORegistryEntry *findEventDriver(IORegistryEntry *parent, unsigned depth) {
    if (!depth) return nullptr;
    OSIterator *iterator = parent->getChildIterator(gIOServicePlane);
    if (!iterator) return nullptr;
    IORegistryEntry *found = nullptr;
    while (OSObject *object = iterator->getNextObject()) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry,object);
        if (!entry) continue;
        found = entry->metaCast("IOHIDEventDriver") ? entry : findEventDriver(entry, depth - 1);
        if (found) break;
    }
    iterator->release(); return found;
}

// Screen-edge contacts: SpringBoard's home and switcher recognizers read the edge type of a touch, which the original
// IOHIDEventDriver never sets. Wrap this device's event driver dispatch in a copied vtable and mark finger events whose
// contact began within 48 px of an edge with kIOHIDDigitizerEventFromEdgeTip (0x800), as the stock multitouch stack does.
namespace edge {
enum {kSlots=256,kDispatch=198,kGetType=18,kGetInteger=23,kSetInteger=24,kGetFixed=25,kContacts=256};
enum : uint32_t {kDigitizer=11,kX=0xB0000,kY=0xB0001,kIndex=0xB0005,kMask=0xB0007,kTouch=0xB0009,kMaskTouch=2,kFromEdgeTip=0x800};
static const int32_t kEdgeX=48*65536/1080,kEdgeY=48*65536/2400;
static uintptr_t originalDispatch;
static bool fromEdge[kContacts];
template <typename R,typename... A> static R call(void *object,unsigned slot,A... args) {
    return reinterpret_cast<R (*)(void *,A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object,args...);
}
static bool mark(void *child) {
    if(call<uint32_t,IOOptionBits>(child,kGetType,0)!=kDigitizer)return false;
    const uint32_t index=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kIndex,0);
    if(index>=kContacts)return false;
    const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kMask,0);
    const bool touch=call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kTouch,0)!=0;
    if((mask&kMaskTouch) && touch) {
        const int32_t x=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kX,0),y=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kY,0);
        fromEdge[index]=x<kEdgeX || x>65536-kEdgeX || y<kEdgeY || y>65536-kEdgeY;
    }
    const bool edged=fromEdge[index];
    if(edged)call<void,uint32_t,int32_t,IOOptionBits>(child,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
    if(!touch && (mask&kMaskTouch))fromEdge[index]=false;
    return edged;
}
static void dispatch(IOService *self,OSObject *event,IOOptionBits options) {
    if(event && call<uint32_t,IOOptionBits>(event,kGetType,0)==kDigitizer) {
        OSArray *children=*reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event)+24);
        if(children && OSDynamicCast(OSArray,children) && children->getCount()) {
            bool any=false;
            for(unsigned i=0;i<children->getCount();++i) {
                OSObject *child=children->getObject(i);
                if(child && child->metaCast("IOHIDEvent"))any|=mark(child);
            }
            if(any) {
                const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(event,kGetInteger,kMask,0);
                call<void,uint32_t,int32_t,IOOptionBits>(event,kSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
            }
        } else if(!children) mark(event);
    }
    reinterpret_cast<void (*)(IOService *,OSObject *,IOOptionBits)>(originalDispatch)(self,event,options);
}
bool install(IORegistryEntry *driver) {
    if(originalDispatch || !driver)return false;
    uintptr_t *table=*reinterpret_cast<uintptr_t **>(driver);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kSlots*sizeof(uintptr_t)));
    if(!copy)return false;
    memcpy(copy,table,kSlots*sizeof(uintptr_t));
    originalDispatch=copy[kDispatch];copy[kDispatch]=(uintptr_t)&dispatch;
    __asm__ volatile("dmb ish":::"memory");
    *reinterpret_cast<uintptr_t **>(driver)=copy;
    IOLog("MarbleTouchHID: edge wrapper installed\n");
    return true;
}
}  // namespace edge
class NativeHomeInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHomeInput)
    IOWorkLoop *inputLoop = nullptr;
    IOTimerEventSource *timer = nullptr;
    bool down = false;
    static void tick(OSObject *owner, IOTimerEventSource *source) {
        NativeHomeInput *self = OSDynamicCast(NativeHomeInput,owner);
        native_hid_live_checkpoint(self);
        bool matched = findEventDriver(self,4) != nullptr;
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
    uint8_t missed[10] = {};
    // A finger absent from up to kHoldFrames frames stays down, then lifts (sheng touch core holds NVT_HOLD_FRAMES=3): one empty
    // frame otherwise ends a drag, and a short tap is reported for fewer frames than the system accepts.
    enum { kHoldFrames = 0, kHoldFlushMs = 16 };
    static void releaseTap(OSObject *owner, IOTimerEventSource *) {
        static_cast<NativeMarbleTouchInput *>(owner)->publish(0, nullptr);
    }
    static IOReturn send(OSObject *owner, void *countValue, void *pointsValue, void *, void *) {
        auto *self = static_cast<NativeMarbleTouchInput *>(owner);
        unsigned count = reinterpret_cast<uintptr_t>(countValue);
        auto *points = static_cast<const uint8_t *>(pointsValue);
        uint8_t report[62] = {3}, seen[10][6] = {};
        for (unsigned i = 0; i < count; ++i) {
            const uint8_t *p = points + 8 * i;
            unsigned id = p[0] >> 4;
            if (id >= 10) continue;
            unsigned x = p[2] | unsigned(p[3]) << 8, y = p[4] | unsigned(p[5]) << 8;
            // The controller reports tenths of a pixel (config maximum 10800 x 24000); the panel is 1080 x 2400.
            x = (x < 10800 ? x : 10799) * 32767 / 10799;
            y = (y < 24000 ? y : 23999) * 32767 / 23999;
            seen[id][0] = 1;
            seen[id][2] = x; seen[id][3] = x >> 8; seen[id][4] = y; seen[id][5] = y >> 8;
        }
        unsigned slots = 0;
        bool pending = false, holding = false, vanished = false;
        for (unsigned id = 0; id < 10; ++id) {
            if (seen[id][0]) {
                self->missed[id] = 0;
                uint8_t *finger = report + 1 + 6 * slots++;
                finger[0] = 3; finger[1] = id;
                memcpy(finger + 2, seen[id] + 2, 4);
                self->previous[id][0] = 3;
                memcpy(self->previous[id] + 2, seen[id] + 2, 4);
                pending = true;
            } else if (self->previous[id][0] == 3) {
                vanished = true;
                uint8_t *finger = report + 1 + 6 * slots;
                finger[1] = id;
                memcpy(finger + 2, self->previous[id] + 2, 4);
                if (++self->missed[id] > kHoldFrames) {
                    finger[0] = 2;
                    self->previous[id][0] = 0;
                    self->missed[id] = 0;
                } else {
                    finger[0] = 3;
                    holding = true;
                }
                ++slots; pending = true;
            }
        }
        if (!pending) return kIOReturnSuccess;
        report[61] = slots;
        IOLog("MarbleTouchHID: slots=%u tip=%u id=%u x=%u y=%u\n", slots, report[1], report[2],
            report[3] | unsigned(report[4]) << 8, report[5] | unsigned(report[6]) << 8);
        auto *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
        IOReturn result = buffer ? self->handleReport(buffer, 0, 0) : kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6, result);
        if (vanished && holding && self->tapTimer) self->tapTimer->setTimeoutMS(kHoldFlushMs);
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
        if (!eventDriverMatched) {
            if (auto *driver = findEventDriver(this, 4)) {
                // backboardd classifies the event service and its interface, rather than the HID device.
                IORegistryEntry *entries[] = {driver, driver->getParentEntry(gIOServicePlane)};
                for (auto *entry : entries) {
                    if (!entry) continue;
                    entry->setProperty("Built-In", kOSBooleanTrue);
                    entry->setProperty("DisplayIntegrated", kOSBooleanTrue);
                    entry->setProperty("PrimaryUsagePage", 0x0dULL, 32);
                    entry->setProperty("PrimaryUsage", 4ULL, 32);
                }
                eventDriverMatched = true;
                edge::install(driver);
            }
        }
        native_hid_input_observed(7, eventDriverMatched);
        inputLoop->runAction(send, this, reinterpret_cast<void *>(uintptr_t(count)), const_cast<uint8_t *>(points));
    }
    void tap(unsigned x, unsigned y) {
        IOLog("MarbleTouch: synthetic tap x=%u y=%u\n", x, y);
        contact(x, y, true);
        tapTimer->setTimeoutMS(60);
    }
    void contact(unsigned x, unsigned y, bool active) {
        tapTimer->cancelTimeout();
        x *= 10; y *= 10;   // the same tenth-of-a-pixel units as the controller frames
        uint8_t point[8] = {0, 0, uint8_t(x), uint8_t(x >> 8), uint8_t(y), uint8_t(y >> 8), 1, 0};
        publish(active ? 1 : 0, point);
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
    if (!touchInput) return;
    // Drive the same path the synthetic tap uses: one pixel coordinate and an explicit release, so the controller frame
    // timing cannot swallow the down or the up edge of a short tap.
    if (count) {
        unsigned x = points[2] | unsigned(points[3]) << 8, y = points[4] | unsigned(points[5]) << 8;
        touchInput->contact(x / 10, y / 10, true);
    } else {
        touchInput->contact(0, 0, false);
    }
}
extern "C" void native_hid_request_tap(unsigned x, unsigned y) {
    if (touchInput && x < 1080 && y < 2400) touchInput->tap(x, y);
}
extern "C" void native_hid_request_contact(unsigned x, unsigned y, bool active) {
    if (touchInput && x < 1080 && y < 2400) touchInput->contact(x, y, active);
}
