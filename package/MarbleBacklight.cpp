// SPDX-License-Identifier: MIT
// CoreBrightness registration and calibration follow UniXNU-sheng
// package/AppleARMBacklight.cpp. The marble OLED uses DCS through its display owner.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
bool native_marble_panel_control_available(void);
IOReturn native_marble_panel_set_brightness(uint32_t);
IOReturn native_legacy_panel_power(unsigned long);
void native_marble_display_backlight_state(bool);
volatile uint32_t marble_backlight_stats[8];
volatile uint32_t marble_backlight_request[4];
void native_marble_backlight_start(void);
void native_backlight_static_init(void) __asm__("__GLOBAL__sub_I_MarbleBacklight.cpp");
}
namespace {
constexpr uint32_t kMin=4,kMax=4095,kInitial=0x133;
// Nominal slider calibration, as in sheng; these describe the control scale.
constexpr uint32_t kMinNits=2,kMidNits=140,kMaxNits=500;
// 22H384 PLDisplayAgent's AppleARMBacklight interest callback.
constexpr uint32_t kBacklightOn=0xe0014100,kBacklightOff=0xe0014101;
const OSMetaClass *appleClass;
bool useAppleClass;
bool backlightOwnsPanel;
}
class MarbleBacklight : public IOService {
    OSDeclareDefaultStructors(MarbleBacklight)
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    uint64_t pendingBrightness=0;
    uint32_t level=kInitial,goal=kInitial,resumeLevel=kInitial,steps=0;
    bool offRequested=false,asleep=false,systemOn=true;
    bool powerRegistered=false;

    // Only this work loop changes panel power, the fade state and the level
    // published to CoreBrightness. PM callbacks enter through the same gate.
    IOReturn setPanelPower(bool on) {
        if (on!=asleep) return kIOReturnSuccess;
        const IOReturn status=native_legacy_panel_power(on);
        marble_backlight_stats[6]=status;
        if (status!=kIOReturnSuccess) return status;
        asleep=!on;
        __atomic_store_n(&marble_backlight_stats[7],uint32_t(on),__ATOMIC_RELEASE);
        publishLevel(on?level:0);
        native_marble_display_backlight_state(on);
        return status;
    }
    IOReturn writeLevel(uint32_t next) {
        if (level==next) return kIOReturnSuccess;
        const IOReturn status=native_marble_panel_set_brightness(next);
        marble_backlight_stats[3]=status;
        if (status==kIOReturnSuccess) {
            level=next;
            marble_backlight_stats[2]=next;
            ++marble_backlight_stats[4];
            publishLevel(next);
        }
        return status;
    }
    static uint32_t clamp(uint64_t value) {return value<kMin?kMin:value>kMax?kMax:uint32_t(value);}
    void publishLevel(uint32_t value) {
        auto *brightness=OSDictionary::withCapacity(3);
        auto *nits=OSDictionary::withCapacity(3);
        auto *parameters=OSDictionary::withCapacity(2);
        // Logical zero reserves CoreBrightness's display-blank request. Every
        // positive hardware write retains kMin, including the slider floor.
        const uint32_t levels[]={0,kMax,value};
        const uint32_t nominal=value ? kMinNits*1000+uint32_t(uint64_t(clamp(value)-kMin)*(kMaxNits-kMinNits)*1000/(kMax-kMin)) : 0;
        const uint32_t luminance[]={0,kMaxNits*1000,nominal};
        const char *keys[]={"min","max","value"};
        if (brightness && nits && parameters) {
            for (unsigned i=0;i<3;++i) {
                if (auto *number=OSNumber::withNumber(levels[i],32)) {brightness->setObject(keys[i],number);number->release();}
                if (auto *number=OSNumber::withNumber(luminance[i],32)) {nits->setObject(keys[i],number);number->release();}
            }
            parameters->setObject("brightness",brightness);parameters->setObject("BrightnessMilliNits",nits);
            setProperty("IODisplayParameters",parameters);
        }
        OSSafeReleaseNULL(brightness);OSSafeReleaseNULL(nits);OSSafeReleaseNULL(parameters);
    }
    void calibration() {
        const int32_t table[]={int32_t(kMinNits<<16),int32_t(kMidNits<<16),int32_t(kMaxNits<<16)};
        if (auto *data=OSData::withBytes(table,sizeof(table))) {setProperty("backlight-marketing-table",data);data->release();}
        if (auto *data=OSData::withBytes(&table[2],4)) {setProperty("LmaxProduct",data);data->release();}
        setProperty("LminProduct",kMinNits,32);setProperty("LmidProduct",kMidNits,32);
        // iOS 18 reads all nine entries as 16.16 CFNumbers during display
        // discovery. OLED DCS has no independently programmable current rail:
        // use the same nominal control units for its current/luminance model.
        // These values are not electrical measurements; hardware writes remain
        // bounded by the panel's DT brightness range.
        auto *values=OSDictionary::withCapacity(9);
        const char *keys[]={"lMinProduct","lMidProduct","lMaxProduct","lMinPanel","lMidPanel","lMaxPanel",
                            "current-for-max-backlight","current-for-mid-backlight","hardware-max-current-limit"};
        const uint32_t fixed[]={kMinNits<<16,kMidNits<<16,kMaxNits<<16,kMinNits<<16,kMidNits<<16,kMaxNits<<16,
                                kMaxNits<<16,kMidNits<<16,kMaxNits<<16};
        if (values) {
            for (unsigned i=0;i<9;++i) if (auto *n=OSNumber::withNumber(fixed[i],32)) {values->setObject(keys[i],n);n->release();}
            setProperty("backlight-calibration-parameters",values);values->release();
        }
    }
    static void update(OSObject *owner,IOTimerEventSource *) {
        auto *self=static_cast<MarbleBacklight *>(owner);
        const uint64_t request=__atomic_exchange_n(&self->pendingBrightness,0,__ATOMIC_ACQ_REL);
        if (request>>63) {
            self->goal=clamp(uint32_t(request));
            self->steps=uint32_t((request>>32)&0x3fffffff)/16;
            self->offRequested=(request&(1ULL<<62))!=0;
            if (!self->offRequested) self->resumeLevel=self->goal;
        }
        // A slider update received during system sleep supplies the resume
        // level. Only the PM wake may turn the panel on in that state.
        if (!self->systemOn || (self->asleep && self->offRequested)) {
            self->steps=0;
            return;
        }
        if (self->asleep && self->setPanelPower(true)!=kIOReturnSuccess) return;
        uint32_t next=self->goal;
        if (self->steps) {
            const int64_t delta=int64_t(self->goal)-self->level;
            next=clamp(int64_t(self->level)+delta/int64_t(self->steps));
            if (!--self->steps) next=self->goal;
        }
        const IOReturn status=self->writeLevel(next);
        if (!self->steps && self->offRequested) self->setPanelPower(false);
        if ((status==kIOReturnSuccess && self->steps) ||
            __atomic_load_n(&self->pendingBrightness,__ATOMIC_ACQUIRE)) self->timer->setTimeoutMS(16);
    }
    static IOReturn setSystemPower(OSObject *owner,void *state,void *,void *,void *) {
        auto *self=static_cast<MarbleBacklight *>(owner);
        const bool on=state!=nullptr;
        if (self->systemOn==on) return kIOReturnSuccess;
        self->systemOn=on;
        self->steps=0;
        // A dark wake restores device power, not the user's display state.
        // Only a positive brightness request clears a CoreBrightness blank.
        const bool panelOn = on && !self->offRequested;
        if (panelOn) self->goal=self->resumeLevel;
        const IOReturn status=self->setPanelPower(panelOn);
        if (status==kIOReturnSuccess && panelOn) return self->writeLevel(self->goal);
        return status;
    }

public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        loop=IOWorkLoop::workLoop();timer=IOTimerEventSource::timerEventSource(this,&MarbleBacklight::update);
        if (!loop || !timer || loop->addEventSource(timer)!=kIOReturnSuccess) return false;
        const OSSymbol *name=OSSymbol::withCString("AppleARMBacklight");
        appleClass=name?OSMetaClass::getMetaClassWithName(name):nullptr;OSSafeReleaseNULL(name);
        if (!appleClass) {marble_backlight_stats[0]=2;return false;}
        calibration();publishLevel(level);
        setName("AppleARMBacklight");setProperty("NativeSandboxGrantProcess","*");
        __atomic_store_n(&useAppleClass,true,__ATOMIC_RELEASE);
        setProperty("backlight-control",true);setProperty("built-in",true);
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        states[1].outputPowerCharacter=kIOPMPowerOn;
        states[1].inputPowerRequirement=kIOPMPowerOn;
        PMinit();
        if (!initialized) return false;
        provider->joinPMtree(this);
        if (registerPowerDriver(this,states,2)!=kIOReturnSuccess) {PMstop();return false;}
        powerRegistered=true;
        if (changePowerStateToPriv(1)!=kIOReturnSuccess) return false;
        marble_backlight_stats[2]=level;
        __atomic_store_n(&marble_backlight_stats[7],1u,__ATOMIC_RELEASE);
        __atomic_store_n(&marble_backlight_stats[0],3u,__ATOMIC_RELEASE);
        __atomic_store_n(&backlightOwnsPanel,true,__ATOMIC_RELEASE);
        registerService();IOLog("MarbleBacklight: DCS range=%u..%u initial=%u\n",kMin,kMax,level);
        return true;
    }
    IOReturn setPowerState(unsigned long state,IOService *) override {
        if (state>1 || !loop) return kIOPMAckImplied;
        (void)loop->runAction(setSystemPower,this,reinterpret_cast<void *>(state));
        return kIOPMAckImplied;
    }
    void free() override {
        __atomic_store_n(&backlightOwnsPanel,false,__ATOMIC_RELEASE);
        if (powerRegistered) PMstop();
        if (timer) timer->cancelTimeout();
        if (loop && timer) loop->removeEventSource(timer);
        OSSafeReleaseNULL(timer);OSSafeReleaseNULL(loop);
        IOService::free();
    }
    IOReturn setProperties(OSObject *object) override {
        auto *properties=OSDynamicCast(OSDictionary,object);
        if (!properties) return kIOReturnBadArgument;
        uint32_t target,raw,kind;bool off=false;
        if (auto *number=OSDynamicCast(OSNumber,properties->getObject("brightness-nits"))) {
            const int32_t fixed=int32_t(number->unsigned32BitValue());
            raw=uint32_t(fixed);kind=1;off=fixed==0;
            target=fixed<=int32_t(kMinNits<<16)?kMin:
                clamp(kMin+uint64_t(fixed-(kMinNits<<16))*(kMax-kMin)/((kMaxNits-kMinNits)<<16));
        } else if (auto *number=OSDynamicCast(OSNumber,properties->getObject("brightness"))) {
            const int32_t value=int32_t(number->unsigned32BitValue());target=clamp(value<0?kMin:uint32_t(value));
            raw=uint32_t(value);kind=2;off=value==0;
        } else return kIOReturnSuccess;
        uint32_t ms=0;
        if (auto *fade=OSDynamicCast(OSNumber,properties->getObject("FadePeriod"))) {
            const uint64_t value=(uint64_t(fade->unsigned32BitValue())*1000)>>16;ms=value>5000?5000:uint32_t(value);
        }
        __atomic_add_fetch(&marble_backlight_stats[1],1u,__ATOMIC_RELAXED);
        marble_backlight_stats[5]=target;
        // CoreBrightness blanks the display with zero. Positive slider values
        // keep the panel's nonzero floor; a zero finishes its fade, then sleeps.
        marble_backlight_request[0]=kind;marble_backlight_request[1]=raw;
        marble_backlight_request[2]=ms;marble_backlight_request[3]=off;
        __atomic_store_n(&pendingBrightness,(1ULL<<63)|(off?(1ULL<<62):0)|(uint64_t(ms)<<32)|target,__ATOMIC_RELEASE);
        timer->setTimeoutUS(1);return kIOReturnSuccess;
    }
};
// Preserve authenticated virtual-method entries while answering the original
// backlight class identity used by CoreBrightness's built-in-display discovery.
MarbleBacklight::MetaClass MarbleBacklight::gMetaClass;
const OSMetaClass *const MarbleBacklight::metaClass=&MarbleBacklight::gMetaClass;
const OSMetaClass *const MarbleBacklight::superClass=&IOService::gMetaClass;
MarbleBacklight::MarbleBacklight(const OSMetaClass *meta):IOService(meta) {}
MarbleBacklight::~MarbleBacklight() {}
const OSMetaClass *MarbleBacklight::getMetaClass() const {
    return __atomic_load_n(&useAppleClass,__ATOMIC_ACQUIRE)&&appleClass?appleClass:&gMetaClass;
}
OSMetaClassConstructorInit(MarbleBacklight,IOService,)
OSDefineDefaultStructors(MarbleBacklight,IOService)

extern "C" bool native_marble_backlight_owns_panel(void) {
    return __atomic_load_n(&backlightOwnsPanel,__ATOMIC_ACQUIRE);
}

extern "C" void native_marble_backlight_start(void) {
    if (marble_backlight_stats[0] || !native_marble_panel_control_available()) return;
    marble_backlight_stats[0]=1;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return;
    native_backlight_static_init();
    if (OSMetaClass::postModLoad(handle)) return;
    auto *service=OSTypeAlloc(MarbleBacklight);
    auto *provider=IOService::getPlatform();
    if (!service || !service->init() || !service->attach(provider) || !service->start(provider))
        IOLog("MarbleBacklight: publication stage=%u\n",marble_backlight_stats[0]);
}
