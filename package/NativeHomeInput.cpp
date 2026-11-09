// SPDX-License-Identifier: BSD-2-Clause-Patent
#if __has_feature(ptrauth_calls)
#include <IOKit/hid/IOHIDDevice.h>
#include <ptrauth.h>
static_assert(sizeof(IOHIDDevice) == 0xd0, "22H384 IOHIDDevice extent");
static constexpr IOHIDReportType kInputReportType = kIOHIDReportTypeInput;
#else
#include "OriginalHidDeviceABI.h"
static constexpr unsigned kInputReportType = 0;
#endif
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSArray.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/clock.h>
#include "MarbleButtonReports.h"
#include "MarbleInputLatency.h"
extern "C" void native_hid_input_observed(unsigned,int);
extern "C" void native_hid_live_checkpoint(IOService *);
extern "C" bool native_hid_input_requested(void);
extern "C" void native_buttons_set_sink(void (*)(void *, uint32_t), void *);
extern "C" { volatile uint32_t marble_buttons_hid_stats[8]; }

// The iOS 18 board personality matches Consumer Control. Publish that usage
// as the application collection so backboardd classifies the built-in switches
// by their actual function. Report 2 carries Menu, Power and the volume keys.
static const uint8_t reportDescriptor[] = {
#ifdef MARBLE_IOS18_GPU
    0x05,0x0c,0x09,0x01,0xa1,0x01,
#else
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,0x75,0x08,0x95,0x06,0x81,0x00,
#endif
    0x85,0x02,0x05,0x0c,0x09,0x40,0x09,0x30,0x09,0xe9,0x09,0xea,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x04,0x81,0x02,0x75,0x04,0x95,0x01,0x81,0x03,0xc0
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
#if defined(NATIVE_HID_SOURCE_API) && NATIVE_HID_SOURCE_API && !__has_feature(ptrauth_calls)
// Source-only bring-up without pointer authentication emits ordinary digitizer reports: a copied vtable of another family's ABI is never used.
extern "C" {volatile uint32_t native_touch_hid_multi[3];}
extern "C" {volatile uint32_t native_touch_edge_stats[4];} // installed stays zero
static void install(IORegistryEntry *) {}
#else
enum {kSlots=256,kDispatch=198,kGetType=18,kGetInteger=23,kSetInteger=24,kGetFixed=25,kContacts=256};
enum : uint32_t {kDigitizer=11,kX=0xB0000,kY=0xB0001,kIndex=0xB0005,kMask=0xB0007,kTouch=0xB0009,kMaskTouch=2,kFromEdgeTip=0x800};
// 48 panel pixels of 3048 x 2032 (the sweet reference uses 48 px), in the driver's normalised IOFixed coordinates.
static int32_t kEdgeX=48*65536/1080,kEdgeY=48*65536/2400;
#if __has_feature(ptrauth_calls)
static void (*originalDispatch)(IOService *,OSObject *,IOOptionBits);
#else
static uintptr_t originalDispatch;
#endif
static bool fromEdge[kContacts];
extern "C" {volatile uint32_t native_touch_edge_stats[4];}   // [0] installed, [1] contacts begun, [2] of them at an edge, [3] skipped events
// [0] reports with two or more fingers down, [1] the most fingers down in one report, [2] tip-up entries sent for vanished fingers
extern "C" {volatile uint32_t native_touch_hid_multi[3];}
#if __has_feature(ptrauth_calls)
// arm64e (iPadOS 27): an object's vtable pointer is signed with the DA key (address of the object, discriminator 0xcda1 of OSMetaClassBase),
// each slot with the IA key (address of the slot, the discriminator of the virtual function). IOHIDFamily-2238.120.5 keeps the slot numbers
// above; the discriminators are those of 24A446's vtables.
enum : uint16_t {kDivDispatch=0x1134,kDivGetType=0x8758,kDivGetInteger=0x02c5,kDivSetInteger=0x130e,kDivGetFixed=0xf637,kVptrDiscriminator=0xcda1};
static void **vtableOf(void *object) {
    return reinterpret_cast<void **>(ptrauth_auth_data(*reinterpret_cast<void **>(object),ptrauth_key_process_independent_data,
        ptrauth_blend_discriminator(object,kVptrDiscriminator)));
}
template <typename R,typename... A> static R call(void *object,unsigned slot,uint16_t diversity,A... args) {
    void **entry=vtableOf(object)+slot;
    void *function=ptrauth_auth_and_resign(*entry,ptrauth_key_function_pointer,ptrauth_blend_discriminator(entry,diversity),
        ptrauth_key_function_pointer,0);
    return reinterpret_cast<R (*)(void *,A...)>(function)(object,args...);
}
#else
template <typename R,typename... A> static R call(void *object,unsigned slot,uint16_t,A... args) {
    return reinterpret_cast<R (*)(void *,A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object,args...);
}
enum : uint16_t {kDivDispatch=0,kDivGetType=0,kDivGetInteger=0,kDivSetInteger=0,kDivGetFixed=0};
#endif
// One finger event; true when it now carries the bit.
static bool mark(void *child) {
    if(call<uint32_t,IOOptionBits>(child,kGetType,kDivGetType,0)!=kDigitizer)return false;   // the scan-time vendor child
    const uint32_t index=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kDivGetInteger,kIndex,0);
    if(index>=kContacts) {native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;return false;}
    const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kDivGetInteger,kMask,0);
    const bool touch=call<int32_t,uint32_t,IOOptionBits>(child,kGetInteger,kDivGetInteger,kTouch,0)!=0;
    if((mask&kMaskTouch) && touch) {   // the contact begins: its position now decides
        const int32_t x=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kDivGetFixed,kX,0),y=call<int32_t,uint32_t,IOOptionBits>(child,kGetFixed,kDivGetFixed,kY,0);
        fromEdge[index]=x<kEdgeX || x>65536-kEdgeX || y<kEdgeY || y>65536-kEdgeY;
        native_touch_edge_stats[1]=native_touch_edge_stats[1]+1;
        if(fromEdge[index])native_touch_edge_stats[2]=native_touch_edge_stats[2]+1;
    }
    const bool edged=fromEdge[index];
    if(edged)call<void,uint32_t,int32_t,IOOptionBits>(child,kSetInteger,kDivSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
    if(!touch && (mask&kMaskTouch))fromEdge[index]=false;   // the lift keeps the bit; the next contact with this index starts fresh
    return edged;
}
static void dispatch(IOService *self,OSObject *event,IOOptionBits options) {
    if(event && call<uint32_t,IOOptionBits>(event,kGetType,kDivGetType,0)==kDigitizer) {
        OSArray *children=*reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event)+24);
        if(children && OSDynamicCast(OSArray,children) && children->getCount()) {
            // A collection (the driver's touch-screen dispatch): its mask is the union of its fingers', so it takes the bit as well.
            bool any=false;
            for(unsigned i=0;i<children->getCount();++i) {
                OSObject *child=children->getObject(i);
                if(child && child->metaCast("IOHIDEvent"))any|=mark(child);
                else native_touch_edge_stats[3]=native_touch_edge_stats[3]+1;
            }
            if(any) {
                const uint32_t mask=(uint32_t)call<int32_t,uint32_t,IOOptionBits>(event,kGetInteger,kDivGetInteger,kMask,0);
                call<void,uint32_t,int32_t,IOOptionBits>(event,kSetInteger,kDivSetInteger,kMask,(int32_t)(mask|kFromEdgeTip),0);
            }
        } else if(!children) mark(event);   // one transducer dispatched on its own
    }
#if __has_feature(ptrauth_calls)
    originalDispatch(self,event,options);
#else
    reinterpret_cast<void (*)(IOService *,OSObject *,IOOptionBits)>(originalDispatch)(self,event,options);
#endif
}
// This device's own event driver gets the wrapped copy; any other driver keeps the original table.
#if __has_feature(ptrauth_calls)
// The copy's slots are signed for their new addresses, each with its own discriminator. Those are Apple's: the diversity of each slot's
// authenticated fixup in 24A446's kernelcache (IOHIDEventDriver's vtable, 220 slots, IOHIDFamily-2238.120.5; ipsw kernel cpp --methods).
// They are not searched for: a kernel pointer's PAC has about 24 bits, so among 65536 discriminators a slot often has a second one whose
// signature for the old address is the same, and the first match found was then wrong for the copy (slot 214, 0xcca1: a PAC failure in
// IOHIDFamily and dead touch, untagged-31ec7a92ba). Every slot is checked against the original table before the copy is used; a table that
// is not this one (another OS build) is left alone. [0] the outcome (1 another class, 2 a slot that did not check out, 3 installed), [1]
// the number of slots copied (0 = not installed), [2] the first slot that did not check out (kSlots: none).
extern "C" {volatile uint32_t native_touch_edge_install[3];}
static const uint16_t kSlotDiversity[]={
    0x3771,0x56b7,0xbaa2,0x3607,0x2e4a,0x3a87,0x89a9,0xfffc,0xfc74,0x5635,0xbe60,0x32e5,
    0x9209,0x4a6a,0xedc5,0x5c68,0x6a10,0x7a2a,0xaf75,0x137e,0x0655,0x43aa,0x12e9,0x4578,
    0x4275,0xff53,0x1814,0x122e,0x13f6,0x1d35,0xacb1,0x7eb0,0x1262,0x82eb,0x164e,0x37a5,
    0xb659,0x6c51,0xa20f,0xb3b6,0x6bcb,0x5a20,0x5062,0x00d7,0x7c85,0x8a26,0x3539,0x688b,
    0x1e60,0x1955,0x0689,0xc256,0xa383,0xf021,0x1f0a,0xb4bb,0x8ffc,0xb5b9,0x8764,0x5d96,
    0x80d9,0x0c9c,0x5d0a,0xcbcc,0x617d,0x848a,0x2312,0x3540,0xc257,0x3025,0x9fc2,0x5038,
    0xc666,0x6cc3,0x550c,0xa19a,0xa51b,0x4577,0x573c,0x1a4e,0x6c3d,0xb049,0xc4b2,0xc90d,
    0x7d59,0x4897,0x3c68,0xb085,0x4529,0x639f,0xccfb,0x55eb,0xe933,0xaec3,0x5ec5,0x5219,
    0xc6b2,0x8a43,0x4a20,0xd9f2,0x981a,0xa27f,0xc4f9,0x6b87,0x60a1,0x7e78,0x36aa,0x86ef,
    0x9be9,0x7318,0x93b7,0x638e,0x61a6,0x9175,0x136b,0xdb58,0x4a31,0x0988,0x5393,0xabe0,
    0x0ad9,0x6c99,0xd52d,0xe213,0x308f,0xd78d,0x3a1d,0xa390,0x240b,0x1b89,0x8d3c,0x2652,
    0x7f14,0x0759,0x63c4,0x800f,0x9cc2,0x02ac,0x785f,0xcc6b,0x82cd,0x808e,0x37ce,0xa4c7,
    0xe8de,0xa343,0x4bc0,0xf8a6,0xac7f,0x7974,0xea1b,0x4b35,0x9eb4,0x595a,0x5b2b,0x699e,
    0x2b52,0xf40e,0x0ddb,0x0f88,0x8700,0x36c3,0x058e,0xf16e,0x3a71,0xda1e,0x10b6,0x8654,
    0x96c3,0x4cb5,0x19b8,0x6518,0x5717,0xd2bf,0x09fa,0xad91,0xd335,0x4902,0xae57,0xaad9,
    0x5299,0xe604,0x8660,0x0189,0xc3de,0x41f6,0x5c1e,0xb54e,0xef25,0x4bdd,0xefa8,0x9835,
    0xae71,0x67cf,0xa85e,0x7b4f,0x4abb,0xc0d0,0x1134,0x7eca,0xf128,0xc748,0xa49a,0xde1d,
    0x90ba,0xea29,0xce88,0xc62d,0x0078,0x0dce,0x454b,0xc58b,0xe23b,0x2684,0xcca1,0xfe11,
    0x69fc,0xaaca,0x6518,0x7547,
};
static constexpr unsigned kDriverSlots=sizeof(kSlotDiversity)/sizeof(kSlotDiversity[0]);
static_assert(kDriverSlots>kDispatch && kDriverSlots<=kSlots,"the dispatch slot lies in the table");
static void install(IORegistryEntry *driver) {
    if(originalDispatch || !driver)return;
    if(kSlotDiversity[kDispatch]!=kDivDispatch)return;
    // The table fits these classes only, each checked in 24A446's kernelcache to have exactly these 220 slots with these discriminators:
    // IOHIDEventDriver, and AppleUserHIDEventService, its subclass that drives this device (the user-space HID driver's service). Any
    // other subclass may have slots past the table, which a copy of this size would lose.
    const OSMetaClass *meta=driver->getMetaClass();
    const char *name=meta?meta->getClassName():nullptr;
    if(!name || (strcmp(name,"IOHIDEventDriver") && strcmp(name,"AppleUserHIDEventService"))) {native_touch_edge_install[0]=1;return;}
    void **table=vtableOf(driver);
    void **copy=static_cast<void **>(IOMalloc(kDriverSlots*sizeof(void *)));
    if(!copy)return;
    unsigned copied=0;
    native_touch_edge_install[2]=kSlots;
    for(unsigned i=0;i<kDriverSlots;++i) {
        void *entry=table[i];
        void *function=entry?ptrauth_strip(entry,ptrauth_key_function_pointer):nullptr;
        if(!entry || ptrauth_sign_unauthenticated(function,ptrauth_key_function_pointer,
               ptrauth_blend_discriminator(&table[i],kSlotDiversity[i]))!=entry) {
            native_touch_edge_install[2]=i;copied=0;break;
        }
        if(i==kDispatch) {
            originalDispatch=reinterpret_cast<void (*)(IOService *,OSObject *,IOOptionBits)>(
                ptrauth_sign_unauthenticated(function,ptrauth_key_function_pointer,0));
            function=ptrauth_strip(reinterpret_cast<void *>(&dispatch),ptrauth_key_function_pointer);
        }
        copy[i]=ptrauth_sign_unauthenticated(function,ptrauth_key_function_pointer,ptrauth_blend_discriminator(&copy[i],kSlotDiversity[i]));
        copied=i+1;
    }
    native_touch_edge_install[1]=copied;
    if(copied!=kDriverSlots || !originalDispatch) {
        native_touch_edge_install[0]=2;originalDispatch=nullptr;IOFree(copy,kDriverSlots*sizeof(void *));return;
    }
    native_touch_edge_install[0]=3;
    __asm__ volatile("dmb ish":::"memory");
    *reinterpret_cast<void **>(driver)=ptrauth_sign_unauthenticated(copy,ptrauth_key_process_independent_data,
        ptrauth_blend_discriminator(driver,kVptrDiscriminator));
    native_touch_edge_stats[0]=1;
}
#else
static void install(IORegistryEntry *driver) {
    if(originalDispatch || !driver)return;
    uintptr_t *table=*reinterpret_cast<uintptr_t **>(driver);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kSlots*sizeof(uintptr_t)));
    if(!copy)return;
    memcpy(copy,table,kSlots*sizeof(uintptr_t));
    originalDispatch=copy[kDispatch];copy[kDispatch]=(uintptr_t)&dispatch;
    __asm__ volatile("dmb ish":::"memory");
    *reinterpret_cast<uintptr_t **>(driver)=copy;
    native_touch_edge_stats[0]=1;
}
#endif
#endif
}  // namespace edge
class NativeHomeInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHomeInput)
    IOWorkLoop *inputLoop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOBufferMemoryDescriptor *reportBuffer = nullptr;
    IOLock *queueLock = nullptr;
    MarbleButtonReports reports;
    bool active = false, matched = false;
    static void buttonsChanged(void *context, uint32_t mask) {
        auto *self = static_cast<NativeHomeInput *>(context);
        auto *report = static_cast<MarbleButtonReport *>(IOMalloc(sizeof(MarbleButtonReport)));
        if (!report) { marble_buttons_hid_stats[2] = uint32_t(kIOReturnNoMemory); return; }
        report->timestamp = mach_absolute_time(); report->mask = mask;
        IOLockLock(self->queueLock);
        if (!self->active || !self->reports.append(report)) {
            IOLockUnlock(self->queueLock); IOFree(report, sizeof(*report)); return;
        }
        marble_buttons_hid_stats[6] = self->reports.count;
        if (self->reports.count > marble_buttons_hid_stats[7]) marble_buttons_hid_stats[7] = self->reports.count;
        // Scheduling a timer enqueues work; the sampling thread never waits for handleReport.
        if (self->timer) self->timer->setTimeoutUS(1);
        IOLockUnlock(self->queueLock);
    }
    IOReturn report(uint32_t mask, uint64_t timestamp) {
        const uint8_t bytes[] = {2, uint8_t(mask)};
        reportBuffer->writeBytes(0, bytes, sizeof(bytes));
#if __has_feature(ptrauth_calls)
        IOReturn result = handleReportWithTime(timestamp, reportBuffer, kInputReportType, 0);
#else
        (void)timestamp;
        IOReturn result = handleReport(reportBuffer, kInputReportType, 0);
#endif
        marble_buttons_hid_stats[1] = marble_buttons_hid_stats[1] + 1;
        marble_buttons_hid_stats[2] = uint32_t(result);
        if (!result) marble_buttons_hid_stats[3] = mask;
        else marble_buttons_hid_stats[4] = marble_buttons_hid_stats[4] + 1;
        native_hid_input_observed(4, result);
        return result;
    }
    IOReturn flush() {
        if (!matched) {
            marble_buttons_hid_stats[5] = marble_buttons_hid_stats[5] + 1;
            IORegistryEntry *driver = findEventDriver(this, 4);
            matched = driver != nullptr;
            if (driver) {
                driver->setProperty("Built-In", kOSBooleanTrue);
                if (auto *interface = driver->getParentEntry(gIOServicePlane)) interface->setProperty("Built-In", kOSBooleanTrue);
                marble_buttons_hid_stats[0] = 1;
                native_hid_input_observed(3, 1);
            }
        }
        if (!matched) return kIOReturnNotReady;
        for (;;) {
            IOLockLock(queueLock);
            MarbleButtonReport *next = reports.head;
            IOLockUnlock(queueLock);
            if (!next) return kIOReturnSuccess;
            IOReturn result = report(next->mask, next->timestamp);
            if (result) return result;
            IOLockLock(queueLock);
            reports.completed(); marble_buttons_hid_stats[6] = reports.count;
            IOLockUnlock(queueLock);
            IOFree(next, sizeof(*next));
        }
    }
    static void tick(OSObject *owner, IOTimerEventSource *source) {
        auto *self = OSDynamicCast(NativeHomeInput, owner);
        if (!self) return;
        IOLockLock(self->queueLock);const bool active=self->active;IOLockUnlock(self->queueLock);
        if (!active) return;
        native_hid_live_checkpoint(self);
        const IOReturn result = self->flush();
        IOLockLock(self->queueLock);
        if (self->active) {
            if (result) source->setTimeoutMS(25);
            else if (self->reports.head) source->setTimeoutUS(1);
        }
        IOLockUnlock(self->queueLock);
    }
    static IOReturn releaseReports(OSObject *owner, void *, void *, void *, void *) {
        auto *self = static_cast<NativeHomeInput *>(owner);
        self->flush();
        return self->report(0, mach_absolute_time());
    }
    void discardReports() {
        while (auto *next = reports.completed()) IOFree(next, sizeof(*next));
    }

public:
    IOWorkLoop *getWorkLoop() const override { return inputLoop; }
    OSString *newTransportString() const override { return OSString::withCString("SPMI"); }
    OSString *newProductString() const override { return OSString::withCString("MarbleButtons"); }
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        *out = IOBufferMemoryDescriptor::withBytes(reportDescriptor,sizeof(reportDescriptor),kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        inputLoop = IOWorkLoop::workLoop();
        queueLock = IOLockAlloc();
        const uint8_t released[] = {2, 0};
        reportBuffer = IOBufferMemoryDescriptor::withBytes(released, sizeof(released), kIODirectionNone);
        if (!inputLoop || !queueLock || !reportBuffer || !setProperty("Built-In", true) ||
            !setProperty("HIDDefaultBehavior",true) || !IOHIDDevice::start(provider)) return false;
        timer = IOTimerEventSource::timerEventSource(this,tick);
        if (!timer || inputLoop->addEventSource(timer) != kIOReturnSuccess) {
            OSSafeReleaseNULL(timer); IOHIDDevice::stop(provider); return false;
        }
        active = true;
        native_buttons_set_sink(buttonsChanged, this);
        native_hid_input_observed(2,0); return true;
    }
    void stop(IOService *provider) override {
        native_buttons_set_sink(nullptr, nullptr);
        if (queueLock) { IOLockLock(queueLock); active = false; IOLockUnlock(queueLock); }
        if (timer) timer->cancelTimeout();
        if (inputLoop && reportBuffer && queueLock) inputLoop->runAction(releaseReports, this);
        if (timer) { inputLoop->removeEventSource(timer); OSSafeReleaseNULL(timer); }
        IOHIDDevice::stop(provider);
    }
    void free() override {
        discardReports();
        if (queueLock) { IOLockFree(queueLock); queueLock = nullptr; }
        OSSafeReleaseNULL(reportBuffer); OSSafeReleaseNULL(inputLoop); IOHIDDevice::free();
    }
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
        uint8_t report[62] = {3}, seen[10][6] = {}, next[10][6];
        memcpy(next, self->previous, sizeof(next));
        // Unused transducers must not share the state of contact 0.
        for (unsigned slot = 0; slot < 10; ++slot) report[2 + 6 * slot] = 240 + slot;
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
        bool pending = false;
        for (unsigned id = 0; id < 10; ++id) {
            if (seen[id][0]) {
                uint8_t *finger = report + 1 + 6 * slots++;
                finger[0] = 3; finger[1] = id;
                memcpy(finger + 2, seen[id] + 2, 4);
                next[id][0] = 3;
                memcpy(next[id] + 2, seen[id] + 2, 4);
                pending = true;
            } else if (next[id][0] == 3) {
                uint8_t *finger = report + 1 + 6 * slots;
                finger[1] = id;
                memcpy(finger + 2, next[id] + 2, 4);
                finger[0] = 2;
                next[id][0] = 0;
                ++slots; pending = true;
            }
        }
        if (!pending) return kIOReturnSuccess;
        report[61] = slots;
        auto *buffer = IOBufferMemoryDescriptor::withBytes(report, sizeof(report), kIODirectionNone);
        if(count)native_marble_input_hid_begin();
        IOReturn result = buffer ? self->handleReport(buffer, kInputReportType, 0) : kIOReturnNoMemory;
        if(count)native_marble_input_hid_end(uint32_t(result));
        if(!result)memcpy(self->previous,next,sizeof(next));
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
        IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
        if(chosen) {
            const char *keys[]={"canvas-width","canvas-height"};
            int32_t *edges[]={&edge::kEdgeX,&edge::kEdgeY};
            for(unsigned i=0;i<2;++i) {
                OSData *value=OSDynamicCast(OSData,chosen->getProperty(keys[i]));
                uint32_t pixels=0;
                if(value && value->getLength()==4)memcpy(&pixels,value->getBytesNoCopy(),4);
                if(pixels>=48 && pixels<=16384)*edges[i]=48*65536/pixels;
            }
            chosen->release();
        }
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
    touchInput->publish(count, points);
}
extern "C" void native_hid_request_tap(unsigned x, unsigned y) {
    if (touchInput && x < 1080 && y < 2400) touchInput->tap(x, y);
}
extern "C" void native_hid_request_contact(unsigned x, unsigned y, bool active) {
    if (touchInput && x < 1080 && y < 2400) touchInput->contact(x, y, active);
}
