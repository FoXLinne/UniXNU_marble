// SPDX-License-Identifier: MIT
// CoreBrightness registration and calibration follow UniXNU-sheng
// package/AppleARMBacklight.cpp. The marble OLED uses DCS through its display owner.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
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
volatile uint32_t marble_backlight_stats[8];
volatile uint32_t marble_backlight_request[4];
void native_marble_backlight_start(void);
void native_backlight_static_init(void) __asm__("__GLOBAL__sub_I_MarbleBacklight.cpp");
}
namespace {
constexpr uint32_t kMin=4,kMax=4095,kInitial=0x133;
// Nominal slider calibration, as in sheng; these describe the control scale.
constexpr uint32_t kMinNits=2,kMidNits=140,kMaxNits=500;
const OSMetaClass *appleClass;
bool useAppleClass;
uint64_t pendingBrightness;
}
class MarbleBacklight : public IOService {
    OSDeclareDefaultStructors(MarbleBacklight)
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *timer=nullptr;
    uint32_t level=kInitial,goal=kInitial,steps=0;
    bool offRequested=false,asleep=false;
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
        const uint64_t request=__atomic_exchange_n(&pendingBrightness,0,__ATOMIC_ACQ_REL);
        if (request>>63) {
            self->goal=clamp(uint32_t(request));
            self->steps=uint32_t((request>>32)&0x3fffffff)/16;
            self->offRequested=(request&(1ULL<<62))!=0;
            if (!self->offRequested) {
                const IOReturn power=native_legacy_panel_power(1);
                marble_backlight_stats[6]=power;
                if (power!=kIOReturnSuccess) {marble_backlight_stats[3]=power;return;}
                self->asleep=false;marble_backlight_stats[7]=1;
            }
        }
        uint32_t next=self->goal;
        if (self->steps) {
            const int64_t delta=int64_t(self->goal)-self->level;
            next=clamp(int64_t(self->level)+delta/int64_t(self->steps));
            if (!--self->steps) next=self->goal;
        }
        const IOReturn result=native_marble_panel_set_brightness(next);
        marble_backlight_stats[3]=result;marble_backlight_stats[2]=next;
        if (result==kIOReturnSuccess) {self->level=next;++marble_backlight_stats[4];self->publishLevel(self->asleep?0:next);}
        if (result==kIOReturnSuccess && self->offRequested && !self->steps && !self->asleep) {
            const IOReturn power=native_legacy_panel_power(0);
            marble_backlight_stats[6]=power;
            if (power==kIOReturnSuccess) {self->asleep=true;marble_backlight_stats[7]=0;self->publishLevel(0);}
        }
        if (self->steps || __atomic_load_n(&pendingBrightness,__ATOMIC_ACQUIRE)) self->timer->setTimeoutMS(16);
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
        marble_backlight_stats[0]=3;marble_backlight_stats[2]=level;marble_backlight_stats[7]=1;
        registerService();IOLog("MarbleBacklight: DCS range=%u..%u initial=%u\n",kMin,kMax,level);
        return true;
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
        ++marble_backlight_stats[1];marble_backlight_stats[5]=target;
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
