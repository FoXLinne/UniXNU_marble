// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOPMPowerSource backed by the device ADSP battery manager.
// Protocol callbacks queue responses onto the service work loop.
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include "QcomUcsi.h"
extern "C" {
bool native_runtime_usb_configured(void);
bool native_resident_usb_service_active(void);
void native_marble_usb_vbus(int);
#include "MarbleAdsp.h"
#include "MarbleBattmgr.h"
}

extern "C" MarbleAdspChannel *marble_adsp_channel(const char *, uint32_t, uint32_t, int, const MarbleAdspClient *, void *) __attribute__((weak_import));
extern "C" int marble_adsp_send(MarbleAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
// [0] stage (1 class up, 2 channel registered, 3 channel up, 4 published; negative: the step that failed), [1] percent, [2] mV,
// [3] mA, [4] temperature (raw), [5] flags (bit 0 present, 1 external, 2 charging, 3 full, 4 notifications enabled), [6] refresh
// cycles, [7] answers, [8] timeouts | errors << 16, [9] notifications, [10] USB type (raw), [11] full mAh, [12] design mAh,
// [13] cycles, [14] messages dropped (ring full) | ignored << 16, [15] send refusals
extern "C" {extern volatile int32_t native_battery_stats[16];}
extern "C" {volatile int32_t native_battery_raw[10], native_charger_stats[16];}

namespace {
constexpr uint32_t kRequestMs = 2000, kRefreshMs = 15000, kSlowEvery = 10, kRing = 16, kMessage = QCOM_UCSI_MAX_MESSAGE;
uint32_t configuredSdpLimit() {
    // Resident mode keeps the firmware USB owner; XNU's device configuration
    // stays zero there. The battery manager also requires USB online and SDP
    // before applying this standard 500 mA host budget.
    return native_runtime_usb_configured() || native_resident_usb_service_active() ? 500000u : 0u;
}
}

class MarbleBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(MarbleBattery)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *request = nullptr, *refresh = nullptr;
    IOTimerEventSource *ucsiTimer = nullptr;
    QcomUcsi ucsi = {};
    uint8_t ucsiOutput[QCOM_UCSI_MAX_MESSAGE];
    IOInterruptEventSource *inbox = nullptr;
    IOSimpleLock *ringLock = nullptr;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t ringHead = 0, ringCount = 0;
    volatile uint32_t channelUp = 0, channelChanged = 0;
    bool wasUp = false, published = false;
    uint32_t refreshes = 0;
    MarbleBattmgr bm;
    uint32_t sdpLimit = 0;
    MarbleAdspChannel *channel = nullptr;

    void sendUcsi(unsigned bytes) {
        if(bytes)marble_adsp_send(channel,ucsiOutput,bytes);
        if(ucsi.polling)ucsiTimer->setTimeoutMS(kUcsiPollMs);
        else ucsiTimer->cancelTimeout();
        if(ucsi.status_valid)native_marble_usb_vbus(ucsi.connected&&ucsi.usb&&ucsi.partner==kUcsiPartnerDfp);
    }
    static void ucsiAction(OSObject *owner,IOTimerEventSource *) {
        auto *self=OSDynamicCast(MarbleBattery,owner);
        if(self&&self->wasUp)self->sendUcsi(ucsi_tick(&self->ucsi,self->ucsiOutput));
    }

    static void dspState(void *context, int up) {
        MarbleBattery *self = static_cast<MarbleBattery *>(context);
        __atomic_store_n(&self->channelUp, up ? 1u : 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&self->channelChanged, 1u, __ATOMIC_RELEASE);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }
    static void dspReceive(void *context, const uint8_t *data, uint32_t length) {
        MarbleBattery *self = static_cast<MarbleBattery *>(context);
        IOSimpleLockLock(self->ringLock);
        if (self->ringCount < kRing && length <= kMessage) {
            auto &slot = self->ring[(self->ringHead + self->ringCount) % kRing];
            memcpy(slot.data, data, length);
            slot.length = length;
            self->ringCount++;
        } else {
            native_battery_stats[14] = native_battery_stats[14] + 1;
        }
        IOSimpleLockUnlock(self->ringLock);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }

    void pump() {
        bm.sdp_limit_ua = configuredSdpLimit();
        if (!bm.sdp_limit_ua && bm.limit_port == 1) bm.limit_due = 0;
        uint8_t out[MARBLE_BATTMGR_REQUEST_BYTES];
        const uint32_t n = marble_battmgr_next(&bm, out);
        if (!n) return;
        if (marble_adsp_send(channel, out, n)) native_battery_stats[15] = native_battery_stats[15] + 1;  // the timer skips the field
        request->setTimeoutMS(kRequestMs);
    }
    void number(const char *key, int64_t value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
        if (symbol && n) setPSProperty(symbol, n);
        OSSafeReleaseNULL(n); OSSafeReleaseNULL(symbol);
    }
    void flag(const char *key, bool value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }
    void recordRaw() {
        if (!ucsi.status_valid && (bm.valid & (1u << BmUsbOnline))) native_marble_usb_vbus(bm.value[BmUsbOnline]);
        const int fields[] = {BmCapacity, BmVoltage, BmCurrent, BmTemperature, BmCharge, BmFull, BmDesign};
        for (unsigned i=0; i<7; ++i) native_battery_raw[i]=(bm.valid >> fields[i]) & 1 ? bm.value[fields[i]] : -999999;
        native_battery_raw[7]=(int32_t)bm.valid;
        native_battery_raw[8]=bm.last_timeout;
        const int fieldsCharger[] = {BmUsbOnline, BmUsbType, BmUsbAdapter, BmUsbLimit, BmUsbMax, BmUsbVoltage,
            BmUsbCurrent, BmStatus, BmChargeType, BmThermalLevel, BmChargeCurrent};
        for (unsigned i=0; i<11; ++i) native_charger_stats[i]=(bm.valid >> fieldsCharger[i]) & 1 ? bm.value[fieldsCharger[i]] : -1;
        native_charger_stats[11]=(int32_t)bm.limit_sets;
        native_charger_stats[12]=(int32_t)bm.limit_tries;
        native_charger_stats[13]=bm.limit_result;
    }
    void publish() {
        MarbleBatterySnapshot s;
        marble_battmgr_snapshot(&bm, &s);
        setBatteryInstalled(s.present);
        setExternalConnected(s.external);
        setExternalChargeCapable(s.external);
        setIsCharging(s.charging);
        setAtCriticalLevel(s.critical);
        setCurrentCapacity((unsigned)s.percent);
        setMaxCapacity(100);
        setVoltage((unsigned)(s.voltage_mv > 0 ? s.voltage_mv : 0));
        setAmperage(s.current_ma);
        if (s.cycles > 0) setCycleCount((unsigned)s.cycles);
        setTimeRemaining(s.charging ? (s.time_to_full_min >= 0 ? s.time_to_full_min : 0xffff)
                                    : (s.time_to_empty_min >= 0 ? s.time_to_empty_min : 0xffff));
        flag("FullyCharged", s.fully_charged);
        number("InstantAmperage", s.current_ma);
        if (s.temperature_valid) number("Temperature", s.temperature);
        if (s.raw_mah > 0) number("AppleRawCurrentCapacity", s.raw_mah);
        if (s.full_mah > 0) {number("AppleRawMaxCapacity", s.full_mah); number("NominalChargeCapacity", s.full_mah);}
        if (s.design_mah > 0) number("DesignCapacity", s.design_mah);
        OSDictionary *adapter = OSDictionary::withCapacity(5);
        if (adapter) {
            OSNumber *family = OSNumber::withNumber(s.adapter_family, 32);
            if (family) {adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family); family->release();}
            if (s.external) {
                OSNumber *w = OSNumber::withNumber((unsigned long long)s.adapter_watts, 32), *mv = OSNumber::withNumber((unsigned long long)s.adapter_mv, 32),
                         *ma = OSNumber::withNumber((unsigned long long)s.adapter_ma, 32);
                if (w) {adapter->setObject(kIOPMPSAdapterDetailsWattsKey, w); w->release();}
                if (mv) {adapter->setObject(kIOPMPSAdapterDetailsVoltage, mv); mv->release();}
                if (ma) {adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, ma); ma->release();}
            }
            const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
            if (key) setPSProperty(key, adapter);
            OSSafeReleaseNULL(key);
            adapter->release();
        }
        native_battery_stats[1] = s.percent; native_battery_stats[2] = s.voltage_mv; native_battery_stats[3] = s.current_ma;
        native_battery_stats[4] = s.temperature;
        native_battery_stats[5] = s.present | s.external << 1 | s.charging << 2 | s.fully_charged << 3 | bm.enabled << 4;
        native_battery_stats[10] = (bm.valid >> BmUsbType) & 1 ? bm.value[BmUsbType] : -1;
        native_battery_stats[11] = s.full_mah; native_battery_stats[12] = s.design_mah; native_battery_stats[13] = s.cycles;
        recordRaw();
        if (!published && !s.charge_known) return;  // no charge reading yet: 0 % would read as an empty battery
        if (!published) {
            // The properties are in place before anyone finds the service.
            published = true;
            registerService();
            native_battery_stats[0] = 4;
        }
        updateStatus();
        IOLog("MarbleBattery percent=%d mv=%d ma=%d temp=%d external=%d charging=%d valid=%x\n",s.percent,s.voltage_mv,s.current_ma,s.temperature,s.external,s.charging,bm.valid);
    }
    void counters() {
        native_battery_stats[6] = (int32_t)bm.cycles_done;
        native_battery_stats[7] = (int32_t)bm.responses;
        native_battery_stats[8] = (int32_t)(bm.timeouts | bm.errors << 16);
        native_battery_stats[9] = (int32_t)bm.notifications;
        native_battery_stats[14] = (native_battery_stats[14] & 0xffff) | (int32_t)(bm.ignored << 16);
    }
    void handle(int result) {
        if (result == BmCycleDone) publish();
        pump();
        counters();
    }

    static void inboxAction(OSObject *owner, IOInterruptEventSource *, int) {
        MarbleBattery *self = OSDynamicCast(MarbleBattery, owner);
        if (!self) return;
        if (__atomic_exchange_n(&self->channelChanged, 0u, __ATOMIC_ACQ_REL)) {
            const bool up = __atomic_load_n(&self->channelUp, __ATOMIC_ACQUIRE) != 0;
            if (up && !self->wasUp) {
                self->sendUcsi(ucsi_start(&self->ucsi,self->ucsiOutput));
                native_battery_stats[0] = self->published ? 4 : 3;
                uint8_t enable[MARBLE_BATTMGR_ENABLE_BYTES];
                marble_adsp_send(self->channel, enable, marble_battmgr_enable_request(enable));
                marble_battmgr_refresh(&self->bm, 1);
                self->pump();
                self->refresh->setTimeoutMS(kRefreshMs);
            }
            if (!up && self->wasUp) {self->request->cancelTimeout();self->ucsiTimer->cancelTimeout();self->ucsi={};marble_battmgr_init(&self->bm); self->bm.sdp_limit_ua = self->sdpLimit;}
            self->wasUp = up;
        }
        for (;;) {
            uint8_t data[kMessage];
            uint32_t length = 0;
            IOSimpleLockLock(self->ringLock);
            if (self->ringCount) {
                auto &slot = self->ring[self->ringHead];
                length = slot.length;
                memcpy(data, slot.data, length);
                self->ringHead = (self->ringHead + 1) % kRing;
                self->ringCount--;
            }
            IOSimpleLockUnlock(self->ringLock);
            if (!length) break;
            if(length>=12&&ucsi_get32(data)==QCOM_UCSI_OWNER){
                self->sendUcsi(ucsi_receive(&self->ucsi,data,length,self->ucsiOutput));
                continue;
            }
            const int result = marble_battmgr_receive(&self->bm, data, length);
            if(!self->ucsi.status_valid&&(self->bm.valid&(1u<<BmUsbOnline)))native_marble_usb_vbus(self->bm.value[BmUsbOnline]);
            if (result == BmAnswer || result == BmCycleDone) self->request->cancelTimeout();
            self->handle(result);
        }
    }
    static void requestAction(OSObject *owner, IOTimerEventSource *) {
        MarbleBattery *self = OSDynamicCast(MarbleBattery, owner);
        if (!self || !self->wasUp) return;
        const uint32_t before = self->bm.cycles_done;
        marble_battmgr_timeout(&self->bm);
        self->handle(self->bm.cycles_done != before ? BmCycleDone : BmAnswer);
    }
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender) {
        MarbleBattery *self = OSDynamicCast(MarbleBattery, owner);
        if (!self || !self->wasUp) return;
        marble_battmgr_refresh(&self->bm, ++self->refreshes % kSlowEvery == 0);
        self->handle(BmAnswer);
        sender->setTimeoutMS(kRefreshMs);
    }

public:
    void free() override {
        if (request) request->cancelTimeout();
        if (refresh) refresh->cancelTimeout();
        if (ucsiTimer) ucsiTimer->cancelTimeout();
        if (loop) {
            if (inbox) loop->removeEventSource(inbox);
            if (request) loop->removeEventSource(request);
            if (refresh) loop->removeEventSource(refresh);
            if (ucsiTimer) loop->removeEventSource(ucsiTimer);
        }
        OSSafeReleaseNULL(inbox); OSSafeReleaseNULL(request); OSSafeReleaseNULL(refresh); OSSafeReleaseNULL(loop);
        OSSafeReleaseNULL(ucsiTimer);
        if (ringLock) {IOSimpleLockFree(ringLock); ringLock = nullptr;}
        IOPMPowerSource::free();
    }
    bool startBattery() {
        marble_battmgr_init(&bm);
        bm.sdp_limit_ua = configuredSdpLimit();
        sdpLimit = bm.sdp_limit_ua;
        loop = IOWorkLoop::workLoop();
        ringLock = IOSimpleLockAlloc();
        inbox = IOInterruptEventSource::interruptEventSource(this, &MarbleBattery::inboxAction);
        request = IOTimerEventSource::timerEventSource(this, &MarbleBattery::requestAction);
        refresh = IOTimerEventSource::timerEventSource(this, &MarbleBattery::refreshAction);
        ucsiTimer = IOTimerEventSource::timerEventSource(this, &MarbleBattery::ucsiAction);
        if (!loop || !ringLock || !inbox || !request || !refresh || !ucsiTimer || loop->addEventSource(inbox) != kIOReturnSuccess ||
            loop->addEventSource(request) != kIOReturnSuccess || loop->addEventSource(refresh) != kIOReturnSuccess ||
            loop->addEventSource(ucsiTimer) != kIOReturnSuccess)
            return false;
        native_battery_stats[0] = 1;
        const MarbleAdspClient client = {&MarbleBattery::dspState, &MarbleBattery::dspReceive};
        channel = marble_adsp_channel ? marble_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 4, 0, &client, this) : nullptr;
        if (!channel) return false;
        native_battery_stats[0] = 2;
        return true;
    }
};
OSDefineMetaClassAndStructors(MarbleBattery, IOPMPowerSource)

// Called by BatteryProbe.cpp once this file's classes are registered.
extern "C" void marble_battery_start(void) {
    MarbleBattery *battery = OSTypeAlloc(MarbleBattery);
    if (!battery || !battery->init() || !battery->attach(IOService::getPlatform())) {
        if (battery) battery->release();
        native_battery_stats[0] = -1;
        return;
    }
    if (!battery->startBattery()) {
        native_battery_stats[0] = native_battery_stats[0] > 0 ? -native_battery_stats[0] - 1 : -1;
        battery->detach(IOService::getPlatform());
        battery->release();
        return;
    }
    battery->release();  // the platform's attachment keeps it
}
