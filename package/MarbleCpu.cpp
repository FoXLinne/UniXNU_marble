// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM7475 EPSS layout from marble DT and MiCode qcom-cpufreq-hw.c.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include "MarbleCpuIdlePolicy.h"
#include "MarblePkg/MarbleCpuOwnership.h"
static_assert(MARBLE_USB_AFFINITY==0x100,"Update idle policy for resident CPU ownership");

extern "C" {
extern volatile uint32_t armcpu_smp_running, armcpu_smp_requested;
extern volatile uint32_t armcpu_smp_step[8], armcpu_smp_fail[8];
extern volatile int32_t armcpu_smp_psci_rc[8];
extern volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];
extern volatile uint64_t armcpu_ipi_sent, armcpu_ipi_interrupts;
extern volatile uint32_t armcpu_gic_boot[8];
extern unsigned int vm_page_free_count;
}
extern "C" void native_marble_boot_record(unsigned, uint64_t, uint64_t);
static unsigned domainFrequency[3], domainState[3];
extern "C" { volatile uint32_t marble_cpu_l3[4]; }
static IOMemoryMap *idleFrequencyMap,*idleVoteMap;
static volatile uint32_t *idleFrequencyRegisters,*idleVoteRegisters;
static uint32_t idleMinimum[3],idleMaximum[3],idleLocks[3],idleReady,idleConfigured;
static MarbleCpuIdlePolicy idlePolicy;
// ready, reserved, active[3], minimum[3], maximum[3], downs[3], ups[3],
// readback[3], maximum transition counter ticks[3], reserved.
extern "C" {volatile uint64_t marble_cpu_idle_power[24];}
extern "C" void native_marble_cpu_idle(unsigned physical,bool enter)
{
    if(physical>=8 || physical==1 || !__atomic_load_n(&idleReady,__ATOMIC_ACQUIRE))return;
    const unsigned domain=MarbleCpuIdlePolicy::domain(physical);
    if(!(idleConfigured&(1u<<domain)))return;
    while(__atomic_exchange_n(&idleLocks[domain],1u,__ATOMIC_ACQUIRE))__asm__ volatile("yield");
    const auto decision=idlePolicy.change(physical,enter);
    marble_cpu_idle_power[2+domain]=idlePolicy.active(domain);
    if(decision.vote>=0) {
        uint64_t before,after;
        __asm__ volatile("mrs %0,cntpct_el0":"=r"(before));
        const unsigned selected=decision.vote?idleMaximum[domain]:idleMinimum[domain];
        idleVoteRegisters[0x804/4+domain]=selected;
        idleFrequencyRegisters[(domain+1)*0x400+0x320/4]=selected;
        __asm__ volatile("dsb sy":::"memory");
        marble_cpu_idle_power[17+domain]=idleFrequencyRegisters[(domain+1)*0x400+0x320/4];
        ++marble_cpu_idle_power[(decision.vote?14:11)+domain];
        __asm__ volatile("mrs %0,cntpct_el0":"=r"(after));
        if(after-before>marble_cpu_idle_power[20+domain])marble_cpu_idle_power[20+domain]=after-before;
    }
    __atomic_store_n(&idleLocks[domain],0u,__ATOMIC_RELEASE);
}

extern "C" void native_marble_cpu_start(void) {
    if(__atomic_load_n(&idleReady,__ATOMIC_ACQUIRE))return;
    native_marble_boot_record(0x210,0x17d90000,0);
    auto *memory = IODeviceMemory::withRange(0x17d90000, 0x4000);
    auto *mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    auto *votes = IODeviceMemory::withRange(0x17d09000, 0x1000);
    auto *voteMap = votes ? votes->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(votes);
    if (!mapping || !voteMap) {
        IOLog("MarbleCPU: EPSS mapping failed\n");
        OSSafeReleaseNULL(mapping);
        OSSafeReleaseNULL(voteMap);
        return;
    }
    native_marble_boot_record(0x211,0x17d90000,0);
    auto *base = reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress());
    auto *pdmem = reinterpret_cast<volatile uint32_t *>(voteMap->getVirtualAddress());
    // The third qcom,freq-domain DT cell is 4 for each EPSS domain.
    const unsigned lutCoreCount = 4;
    for (unsigned domain = 0; domain < 3; ++domain) {
        auto *registers = base + (domain + 1) * 0x400;
        native_marble_boot_record(0x212,0x17d91000+domain*0x1000+0x320,domain);
        unsigned previous = registers[0x320 / 4], selected = 0, best = 0, last = 0;
        unsigned minimum=UINT32_MAX,minimumState=0;
        for (unsigned index = 0; index < 40; ++index) {
            uint32_t row = registers[0x100 / 4 + index];
            unsigned frequency = row >> 30 ? (row & 255) * 19200 : 300000;
            if (index && frequency == last) break;
            last = frequency;
            if (((row >> 16) & 7) == lutCoreCount && frequency > best) {
                selected = index;
                best = frequency;
            }
            if (((row >> 16) & 7) == lutCoreCount && frequency<minimum) {
                minimum=frequency;minimumState=index;
            }
        }
        if (!best) {
            IOLog("MarbleCPU: domain=%u has no normal operating point\n",domain);
            continue;
        }
        // Match the EPSS driver's paired PDMEM and performance-state votes.
        native_marble_boot_record(0x213,0x17d09804+4*domain,selected);
        pdmem[0x804 / 4 + domain] = selected;
        native_marble_boot_record(0x214,0x17d91320+domain*0x1000,selected);
        registers[0x320 / 4] = selected;
        __asm__ volatile("dsb sy" ::: "memory");
        domainFrequency[domain] = best;
        domainState[domain] = registers[0x320 / 4];
        idleMinimum[domain]=minimumState;idleMaximum[domain]=selected;
        idleConfigured|=1u<<domain;
        marble_cpu_idle_power[5+domain]=minimumState;
        marble_cpu_idle_power[8+domain]=selected;
        IOLog("MarbleCPU: domain=%u state=%u->%u frequency_khz=%u readback=%u\n",
            domain, previous, selected, best, registers[0x320 / 4]);
    }
    // marble-s-oss dcvs_epss.c: the L3 LUT starts at 0x100 and the
    // shared vote is at 0x90 in the same EPSS block as the CPU domains.
    native_marble_boot_record(0x215,0x17d90000,0);
    unsigned l3Index=0,l3Frequency=0,l3Previous=0;
    for (unsigned index=0;index<40;++index) {
        const uint32_t row=base[0x100/4+index];
        const unsigned frequency=row>>30 ? (row&255)*19200 : 300000;
        if (index && frequency==l3Previous) break;
        l3Previous=frequency;
        if (frequency>l3Frequency) {l3Frequency=frequency;l3Index=index;}
    }
    marble_cpu_l3[0]=base[0x90/4];
    native_marble_boot_record(0x216,0x17d90090,l3Index);
    base[0x90/4]=l3Index;
    __asm__ volatile("dsb sy":::"memory");
    marble_cpu_l3[1]=l3Index;marble_cpu_l3[2]=l3Frequency;marble_cpu_l3[3]=base[0x90/4];
    IOLog("MarbleCPU: L3 state=%u->%u frequency_khz=%u readback=%u\n",
        marble_cpu_l3[0],l3Index,l3Frequency,marble_cpu_l3[3]);
    // Retain the mapped EPSS votes for the IRQ-masked idle callback. L3 and
    // GPU keep their existing active configuration throughout this candidate.
    idleFrequencyMap=mapping;idleVoteMap=voteMap;
    idleFrequencyRegisters=base;idleVoteRegisters=pdmem;
    marble_cpu_idle_power[0]=1;
    __atomic_store_n(&idleReady,1u,__ATOMIC_RELEASE);
}

extern "C" unsigned native_marble_cpu_report(char *out, unsigned cap) {
    unsigned at = snprintf(out, cap, "CPU smp=%u secondaries=%u free_pages=%u kHz=%u/%u/%u states=%u/%u/%u\n",
        armcpu_smp_requested, armcpu_smp_running, vm_page_free_count,
        domainFrequency[0], domainFrequency[1], domainFrequency[2],
        domainState[0], domainState[1], domainState[2]);
    for (unsigned core = 0; core < 8 && at + 100 < cap; ++core)
        at += snprintf(out + at, cap - at, "CPU%u step=%u failure=%u psci=%d timer=%u ipi=%u\n",
            core, armcpu_smp_step[core], armcpu_smp_fail[core], armcpu_smp_psci_rc[core],
            armcpu_core_timer[core], armcpu_core_sgi[core]);
    at += snprintf(out + at, cap - at, "IPI sent=%llu received=%llu GICD_TYPER=%x\n",
        armcpu_ipi_sent, armcpu_ipi_interrupts, armcpu_gic_boot[3]);
    return at;
}
