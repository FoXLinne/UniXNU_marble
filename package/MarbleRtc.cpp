// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <Protocol/RootStage.h>
#include "MarbleRtc.h"
#include "MarbleUfs.h"

static IOMemoryMap *rtcMapping;
static volatile MARBLE_RTC *rtc;
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
    IOService::publishResource("IORTC",IOService::getPlatform());
    IOLog("MarbleRTC: firmware clock UTC=%llu status=%u frequency=%llu\n",rtc->seconds,rtc->error,rtc->frequency);
}

extern "C" int native_marble_rtc_get(clock_sec_t *seconds, clock_usec_t *microseconds)
{
    if (!rtc) return 0;
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
    // XNU serializes calendar writes with settime_lock. Firmware owns line 0.
    rtc->requested++;
    __asm__ volatile("dmb ishst" ::: "memory");
    rtc->request_seconds=seconds;
    rtc->request_nanoseconds=microseconds*1000;
    uint64_t counter;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(counter));
    rtc->request_counter=counter;
    uint64_t raw=rtc->reserved[1]+(counter-rtc->counter)/rtc->frequency;
    marble_rtc_persist=marble_ufs_rtc_store(int64_t(seconds)-int64_t(raw)) ? 1 : 2;
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
