// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <pexpert/pexpert.h>
#include <Protocol/RootStage.h>
#include "MarbleRtc.h"
#include "MarbleUfs.h"

static IOMemoryMap *rtcMapping;
static volatile MARBLE_RTC *rtc;
static uint64_t monotonicSeconds, monotonicCounter, monotonicFrequency;
static bool kernelRtcOwner;
static int64_t utcOffsetSeconds;
static IOMemoryMap *spmiCoreMapping, *rtcObserverMapping;
static volatile uint32_t *rtcObserver;
static IOSimpleLock *rtcObserverLock;
static IOLock *rtcCalibrationLock;
static bool rtcCalibrationPending = true;

// PMK8350 SID 0, peripheral 0x61. The transaction format and observer-lane
// selection match MarblePkg/RootRtc.c. Native service ownership transfers
// this read-only observer lane from the resident firmware to XNU.
static bool prepareHardwareRtc()
{
    auto *memory = IOMemoryDescriptor::withAddressRange(0xc400000, 0x3000,
        kIODirectionInOut | kIOMemoryMapperNone, nullptr);
    spmiCoreMapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!spmiCoreMapping) return false;
    auto *core = reinterpret_cast<volatile uint32_t *>(spmiCoreMapping->getVirtualAddress());
    const uint32_t version = core[0];
    if (version < 0x50000000) return false;
    const bool v7 = version >= 0x70000000;
    const unsigned count = v7 ? 1024 : 512;
    unsigned selected = count;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t entry = core[((v7 ? 0x2000 : 0x900) + i * 4) / 4];
        if (entry && ((entry >> 8) & 0xfff) == 0x61) selected = i;
    }
    if (selected == count) return false;
    const uint64_t address = 0xc440000 + uint64_t(selected) * (v7 ? 0x20 : 0x80);
    memory = IOMemoryDescriptor::withAddressRange(address & ~0xfffULL, 0x1000,
        kIODirectionInOut | kIOMemoryMapperNone, nullptr);
    rtcObserverMapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    rtcObserverLock = IOSimpleLockAlloc();
    rtcCalibrationLock = IOLockAlloc();
    if (!rtcObserverMapping || !rtcObserverLock || !rtcCalibrationLock) return false;
    rtcObserver = reinterpret_cast<volatile uint32_t *>(
        rtcObserverMapping->getVirtualAddress() + (address & 0xfff));
    return true;
}

static bool readHardwareRtc(uint32_t *seconds)
{
    if (!rtcObserver || !rtcObserverLock) return false;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(rtcObserverLock);
    bool success = false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint8_t bytes[5];
        for (unsigned i = 0; i < 5; ++i) {
            const unsigned reg = 0x48 + i % 4;
            rtcObserver[0] = (1u << 27) | (reg << 4);
            __asm__ volatile("dsb sy" ::: "memory");
            unsigned wait;
            for (wait = 0; wait < 1000; ++wait) {
                const uint32_t status = rtcObserver[2];
                if (status & 1) {
                    if (status & 14) goto finished;
                    break;
                }
                IODelay(1);
            }
            if (wait == 1000) goto finished;
            bytes[i] = uint8_t(rtcObserver[6]);
        }
        if (bytes[4] >= bytes[0]) {
            *seconds = uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
                uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
            success = true;
            break;
        }
    }
finished:
    IOSimpleLockUnlockEnableInterrupt(rtcObserverLock, state);
    return success;
}

// PowerlogCore in 22H384 reads an eight-byte microsecond clock. XNU's
// calendar code reads the same value together with its mach-time latch.
// Native ownership reads the PMIC's always-on seconds. Resident firmware
// ownership retains its counter handoff, independently of calendar writes.
static int marbleMonotonicClock SYSCTL_HANDLER_ARGS
{
    if (!monotonicFrequency) return ENXIO;
    uint64_t counter, machTime;
    clock_get_uptime(&machTime);
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(counter));
    if (counter < monotonicCounter) return EIO;
    const uint64_t delta = counter - monotonicCounter;
    uint64_t sample[2] = {
        (monotonicSeconds + delta / monotonicFrequency) * USEC_PER_SEC +
            (delta % monotonicFrequency) * USEC_PER_SEC / monotonicFrequency,
        machTime
    };
    if (kernelRtcOwner) {
        uint32_t raw;
        if (!readHardwareRtc(&raw)) return EIO;
        sample[0] = uint64_t(raw) * USEC_PER_SEC;
    }
    const size_t bytes = req->oldlen == sizeof(uint64_t) ? sizeof(uint64_t) : sizeof(sample);
    return SYSCTL_OUT(req, sample, bytes);
}

static int marbleMonotonicRate SYSCTL_HANDLER_ARGS
{
    const uint64_t resolution = kernelRtcOwner ? USEC_PER_SEC : 1;
    return SYSCTL_OUT(req, &resolution, sizeof(resolution));
}

SYSCTL_OID_MANUAL(_kern, OID_AUTO, monotonicclock_usecs,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
    0, 0, marbleMonotonicClock, "Q", "PMIC RTC and physical-counter time in microseconds");
SYSCTL_OID_MANUAL(_kern, OID_AUTO, monotonicclock_rate_usecs,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
    0, 0, marbleMonotonicRate, "Q", "Monotonic clock resolution in microseconds");
extern "C" {
volatile uint64_t marble_rtc_seconds, marble_rtc_counter;
volatile uint32_t marble_rtc_error, marble_rtc_sets;
volatile uint32_t marble_rtc_persist;
uint64_t marble_rtc_observation[6];
void native_marble_rtc_start(const ROOT_STAGE_TABLE *);
int native_marble_rtc_get(clock_sec_t *, clock_usec_t *);
int native_marble_rtc_set(clock_sec_t, clock_usec_t);
void native_marble_rtc_observe(void);
void native_marble_rtc_sync(void);
}

extern "C" void native_marble_rtc_start(const ROOT_STAGE_TABLE *stage)
{
    const ROOT_STAGE_DISK &kernel=stage->Disks[2];
    if (kernel.Count!=1 || kernel.Bytes<0x100000 || kernel.Bytes>kernel.Extents[0].Bytes) return;
    uint64_t address=kernel.Extents[0].Address+kernel.Bytes-0x100000+MARBLE_RTC_OFFSET;
    auto *memory=IOMemoryDescriptor::withAddressRange(address,sizeof(MARBLE_RTC),
        kIODirectionInOut|kIOMemoryMapperNone,nullptr);
    rtcMapping=memory ? memory->map(kIOMapAnywhere|kIOMapCopybackCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!rtcMapping) return;
    rtc=reinterpret_cast<volatile MARBLE_RTC *>(rtcMapping->getVirtualAddress());
    if (rtc->magic!=MARBLE_RTC_MAGIC || !rtc->frequency) { rtc=nullptr; return; }
    unsigned nativeServices = 0;
    kernelRtcOwner = PE_parse_boot_argn("marble-native-services", &nativeServices,
        sizeof(nativeServices)) && nativeServices;
    utcOffsetSeconds = int64_t(rtc->seconds) - int64_t(rtc->reserved[1]);
    if (kernelRtcOwner && !prepareHardwareRtc()) {
        IOLog("MarbleRTC: PMIC observer initialization failed\n");
        return;
    }
    if (kernelRtcOwner) {
        uint32_t raw;
        if (!readHardwareRtc(&raw)) {
            IOLog("MarbleRTC: PMIC seconds read failed\n");
            return;
        }
        IOLog("MarbleRTC: PMIC raw=%u offset=%lld UTC=%lld\n",
            raw, utcOffsetSeconds, int64_t(raw) + utcOffsetSeconds);
    }
    if (!rtc->error) {
        monotonicSeconds = rtc->reserved[1];
        monotonicCounter = rtc->counter;
        monotonicFrequency = rtc->frequency;
        sysctl_register_oid(&sysctl__kern_monotonicclock_usecs);
        sysctl_register_oid(&sysctl__kern_monotonicclock_rate_usecs);
    }
    IOService::publishResource("IORTC",IOService::getPlatform());
    IOLog("MarbleRTC: firmware clock UTC=%llu status=%u frequency=%llu\n",rtc->seconds,rtc->error,rtc->frequency);
}

extern "C" int native_marble_rtc_get(clock_sec_t *seconds, clock_usec_t *microseconds)
{
    if (!rtc) return 0;
    if (kernelRtcOwner) {
        uint32_t raw;
        if (!readHardwareRtc(&raw)) return 0;
        *seconds = int64_t(raw) + __atomic_load_n(&utcOffsetSeconds, __ATOMIC_ACQUIRE);
        *microseconds = 0;
        marble_rtc_seconds = *seconds;
        marble_rtc_error = 0;
        return 1;
    }
    for (unsigned tries=0;tries<4;tries++) {
        __asm__ volatile("dc ivac, %0\n\tdsb sy" :: "r"(rtc) : "memory");
        unsigned sequence=rtc->sequence, request=rtc->requested;
        if ((sequence|request)&1) continue;
        __asm__ volatile("dmb ishld" ::: "memory");
        bool pending=request!=rtc->completed;
        uint64_t secs=pending ? rtc->request_seconds : rtc->seconds;
        uint64_t ns=pending ? rtc->request_nanoseconds : rtc->nanoseconds;
        uint64_t counter=pending ? rtc->request_counter : rtc->counter;
        uint64_t frequency=rtc->frequency, now;
        uint32_t error=rtc->error;
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0\n\tdmb ishld" : "=r"(now) :: "memory");
        if (sequence!=rtc->sequence || request!=rtc->requested) continue;
        if (!frequency || now<counter) return 0;
        uint64_t delta=now-counter;
        secs+=delta/frequency;
        ns+=(delta%frequency)*1000000000/frequency;
        *seconds=secs+ns/1000000000;
        *microseconds=(ns%1000000000)/1000;
        marble_rtc_seconds=*seconds; marble_rtc_counter=now; marble_rtc_error=error;
        return 1;
    }
    return 0;
}

extern "C" int native_marble_rtc_set(clock_sec_t seconds, clock_usec_t microseconds)
{
    if (!rtc || rtc->error) return 0;
    uint64_t counter;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(counter));
    uint64_t raw;
    if (kernelRtcOwner) {
        uint32_t measured;
        if (!readHardwareRtc(&measured)) return 0;
        raw = measured;
    } else {
        raw = monotonicSeconds + (counter - monotonicCounter) / monotonicFrequency;
    }
    // XNU serializes calendar writes with settime_lock. Firmware owns line 0.
    rtc->requested++;
    __asm__ volatile("dmb ishst" ::: "memory");
    rtc->request_seconds=seconds;
    rtc->request_nanoseconds=microseconds*1000;
    rtc->request_counter=counter;
    // Calibration uses a hardware sample in native mode or the immutable
    // firmware boot sample/counter pair while the resident owns the RTC.
    const int64_t offset=int64_t(seconds)-int64_t(raw);
    if (kernelRtcOwner) IOLockLock(rtcCalibrationLock);
    __atomic_store_n(&utcOffsetSeconds, offset, __ATOMIC_RELEASE);
    marble_rtc_persist=marble_ufs_rtc_store(offset) ? 1 : 2;
    rtcCalibrationPending = marble_rtc_persist != 1;
    if (kernelRtcOwner) IOLockUnlock(rtcCalibrationLock);
    IOLog("MarbleRTC: calendar UTC=%llu raw=%llu offset=%lld writes=%u persist=%u\n",
        uint64_t(seconds),raw,offset,marble_rtc_sets+1,marble_rtc_persist);
    __asm__ volatile("dmb ishst" ::: "memory");
    rtc->requested++;
    __asm__ volatile("dc cvac, %0\n\tdsb sy" :: "r"(reinterpret_cast<uintptr_t>(rtc)+64) : "memory");
    marble_rtc_sets++;
    return 1;
}

extern "C" void native_marble_rtc_observe(void)
{
    clock_sec_t seconds=0;
    clock_usec_t microseconds=0;
    clock_get_calendar_microtime(&seconds,&microseconds);
    marble_rtc_observation[0]=seconds; marble_rtc_observation[1]=microseconds;
    int status=native_marble_rtc_get(&seconds,&microseconds);
    marble_rtc_observation[2]=seconds; marble_rtc_observation[3]=microseconds;
    marble_rtc_observation[4]=status;
    marble_rtc_observation[5]=rtc ? rtc->error : ~uint64_t(0);
}

extern "C" void native_marble_rtc_sync(void)
{
    if (!rtc) return;
    __asm__ volatile("dc ivac, %0\n\tdsb sy" :: "r"(rtc) : "memory");
    clock_set_calendar_microtime(rtc->reserved[0],0);
    native_marble_rtc_observe();
}

extern "C" void native_marble_rtc_host_sync(uint64_t seconds)
{
    clock_set_calendar_microtime(seconds,0);
    native_marble_rtc_observe();
}

extern "C" void native_marble_rtc_kernel_service(void)
{
    unsigned enabled=0;
    if (!rtc || !PE_parse_boot_argn("marble-native-services",&enabled,sizeof(enabled)) || !enabled) return;
    // Retain the bootloader's calibrated UTC origin once UFS is available.
    // The same lock serializes later calendar writes with this initial save.
    if (kernelRtcOwner && rtcCalibrationLock) {
        IOLockLock(rtcCalibrationLock);
        if (rtcCalibrationPending && marble_ufs_rtc_store(utcOffsetSeconds)) {
            rtcCalibrationPending = false;
            marble_rtc_persist = 1;
        }
        IOLockUnlock(rtcCalibrationLock);
    }
    const uint32_t request=rtc->requested;
    if ((request&1) || request==rtc->completed) return;
    const uint64_t seconds=rtc->request_seconds,counter=rtc->request_counter;
    const uint32_t nanoseconds=rtc->request_nanoseconds;
    __asm__ volatile("dmb ishld":::"memory");
    if (request!=rtc->requested) return;
    ++rtc->sequence;
    __asm__ volatile("dmb ishst":::"memory");
    rtc->seconds=seconds;rtc->counter=counter;rtc->nanoseconds=nanoseconds;
    rtc->completed=request;rtc->error=0;
    __asm__ volatile("dmb ishst":::"memory");
    ++rtc->sequence;
    __asm__ volatile("dc cvac,%0\n\tdsb sy"::"r"(rtc):"memory");
}
