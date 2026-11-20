// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM7475 EPSS layout from marble DT and MiCode qcom-cpufreq-hw.c.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <arm/machine_routines.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include "MarbleCpuIdlePolicy.h"

extern "C" {
extern volatile uint32_t armcpu_smp_running, armcpu_smp_requested;
extern volatile uint32_t armcpu_smp_step[8], armcpu_smp_fail[8];
extern volatile int32_t armcpu_smp_psci_rc[8];
extern volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];
extern volatile uint64_t armcpu_ipi_sent, armcpu_ipi_interrupts;
extern volatile uint32_t armcpu_gic_boot[8];
extern unsigned int vm_page_free_count;
}
static unsigned domainFrequency[3], domainState[3];
extern "C" { volatile uint32_t marble_cpu_l3[4]; }
static MarbleCpuIdlePolicy idlePolicy;
static IOMemoryMap *frequencyMap, *voteMemoryMap;
static volatile uint32_t *frequencyRegisters, *voteRegisters;
static unsigned minimumState[3], peakState[3], screenOffState[3], configuredDomains;
static unsigned l3Minimum, l3Maximum;
static unsigned voteLock, votesReady;
static bool displayOn = true, l3Low = false;
static bool l3Available = false;
static uint64_t idleSince[8], idleElapsed[8];
static uint64_t l3SampleTicks, l3LastSample, l3PreviousIdle[8];
static MarbleL3IdlePolicy l3Policy;
extern "C" {
volatile uint32_t marble_cpu_screen_budget[8];
volatile uint64_t marble_cpu_idle_power[24], marble_cpu_l3_power[8];
}
extern "C" void native_marble_system_sleep_configure(void);

static void voteForDomain(unsigned domain)
{
    if (!(configuredDomains & (1u << domain))) return;
    unsigned state = minimumState[domain];
    if (idlePolicy.active(domain)) state = displayOn ? peakState[domain] : screenOffState[domain];
    uint64_t before, after;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(before));
    voteRegisters[0x804 / 4 + domain] = state;
    auto *performance = &frequencyRegisters[(domain + 1) * 0x400 + 0x320 / 4];
    *performance = state;
    __asm__ volatile("dsb sy" ::: "memory");
    const uint32_t readback = *performance;
    __atomic_store_n(&domainState[domain], readback, __ATOMIC_RELAXED);
    marble_cpu_idle_power[17 + domain] = readback;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(after));
    if (after - before > marble_cpu_idle_power[20 + domain])
        marble_cpu_idle_power[20 + domain] = after - before;
}

static uint64_t counterTicks()
{
    uint64_t ticks;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(ticks));
    return ticks;
}

// The caller holds voteLock with local interrupts masked.
static void setL3(bool low)
{
    if (!l3Available || l3Low == low) return;
    frequencyRegisters[0x90 / 4] = low ? l3Minimum : l3Maximum;
    __asm__ volatile("dsb sy" ::: "memory");
    l3Low = low;
    ++marble_cpu_l3_power[low ? 2 : 3];
    marble_cpu_l3_power[4] = frequencyRegisters[0x90 / 4];
    marble_cpu_l3_power[5] = low;
}

// Existing CPU idle transitions carry the workload signal. The last idle
// CPU can release the vote immediately without scheduling a timer.
static void updateIdleL3(uint64_t now)
{
    if (displayOn || !l3Available) return;
    const bool allIdle = idlePolicy.allIdle();
    if (!allIdle && l3LastSample && now - l3LastSample < l3SampleTicks) return;
    unsigned busiest = 0;
    for (unsigned cpu = 0; cpu < 8; ++cpu) {
        if (!idlePolicy.owns(cpu)) continue;
        const uint64_t elapsed = idleElapsed[cpu] + (idleSince[cpu] ? now - idleSince[cpu] : 0);
        if (l3LastSample && now > l3LastSample) {
            const uint64_t span = now - l3LastSample;
            const uint64_t delta = elapsed - l3PreviousIdle[cpu];
            const unsigned busy = delta >= span ? 0 : unsigned((span - delta) * 100 / span);
            if (busy > busiest) busiest = busy;
        }
        l3PreviousIdle[cpu] = elapsed;
    }
    if (allIdle) setL3(true);
    else if (l3LastSample) setL3(l3Policy.update(false, busiest));
    l3LastSample = now;
    marble_cpu_l3_power[6] = allIdle ? 0 : busiest;
    marble_cpu_l3_power[7] = l3Policy.quietSamples();
}

extern "C" void native_marble_cpu_idle(unsigned physical, bool entering)
{
    if (!__atomic_load_n(&votesReady, __ATOMIC_ACQUIRE) || !idlePolicy.owns(physical)) return;
    const bool interrupts = ml_set_interrupts_enabled(false);
    while (__atomic_exchange_n(&voteLock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield");
    const unsigned domain = idlePolicy.domain(physical);
    const bool wasActive = (idlePolicy.active(domain) & (1u << physical)) != 0;
    const uint64_t now = counterTicks();
    if (entering && wasActive) idleSince[physical] = now;
    if (!entering && !wasActive && idleSince[physical]) {
        idleElapsed[physical] += now - idleSince[physical];
        idleSince[physical] = 0;
    }
    if (idlePolicy.change(physical, entering) && (configuredDomains & (1u << domain))) {
        voteForDomain(domain);
        ++marble_cpu_idle_power[(entering ? 11 : 14) + domain];
    }
    marble_cpu_idle_power[2 + domain] = idlePolicy.active(domain);
    if (entering == wasActive) updateIdleL3(now);
    __atomic_store_n(&voteLock, 0u, __ATOMIC_RELEASE);
    ml_set_interrupts_enabled(interrupts);
}

extern "C" void native_marble_cpu_display_power(bool on)
{
    if (!__atomic_load_n(&votesReady, __ATOMIC_ACQUIRE)) return;
    const bool interrupts = ml_set_interrupts_enabled(false);
    while (__atomic_exchange_n(&voteLock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield");
    const bool changed = displayOn != on;
    if (changed) {
        displayOn = on;
        marble_cpu_screen_budget[0] = on;
        l3LastSample = 0;
        if (on) setL3(l3Policy.update(true, 0));
        else updateIdleL3(counterTicks());
        for (unsigned domain = 0; domain < 3; ++domain) {
            voteForDomain(domain);
            marble_cpu_screen_budget[4 + domain] = marble_cpu_idle_power[17 + domain];
        }
        ++marble_cpu_screen_budget[7];
    }
    __atomic_store_n(&voteLock, 0u, __ATOMIC_RELEASE);
    ml_set_interrupts_enabled(interrupts);
}

extern "C" void native_marble_cpu_l3_sleep(bool asleep)
{
    if (!__atomic_load_n(&votesReady, __ATOMIC_ACQUIRE)) return;
    const bool interrupts = ml_set_interrupts_enabled(false);
    while (__atomic_exchange_n(&voteLock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield");
    // Background wake keeps the retention vote. The workload sampler can
    // raise it for CPU work, and display wake restores it immediately.
    if (asleep) {
        setL3(true);
        l3LastSample = 0;
    } else if (displayOn) setL3(l3Policy.update(true, 0));
    __atomic_store_n(&voteLock, 0u, __ATOMIC_RELEASE);
    ml_set_interrupts_enabled(interrupts);
}

extern "C" void native_marble_cpu_start(void) {
    if (__atomic_load_n(&votesReady, __ATOMIC_ACQUIRE)) return;
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
    auto *base = reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress());
    auto *pdmem = reinterpret_cast<volatile uint32_t *>(voteMap->getVirtualAddress());
    // The third qcom,freq-domain DT cell is 4 for each EPSS domain.
    const unsigned lutCoreCount = 4;
    for (unsigned domain = 0; domain < 3; ++domain) {
        auto *registers = base + (domain + 1) * 0x400;
        unsigned previous = registers[0x320 / 4], selected = 0, best = 0, last = 0;
        unsigned floor = UINT32_MAX, floorState = 0, offFrequency = 0, offState = 0;
        for (unsigned index = 0; index < 40; ++index) {
            uint32_t row = registers[0x100 / 4 + index];
            unsigned frequency = row >> 30 ? (row & 255) * 19200 : 300000;
            if (index && frequency == last) break;
            last = frequency;
            if (((row >> 16) & 7) != lutCoreCount) continue;
            if (frequency > best) {
                selected = index;
                best = frequency;
            }
            if (frequency < floor) {
                floor = frequency;
                floorState = index;
            }
            if (frequency <= 1209600 && frequency > offFrequency) {
                offFrequency = frequency;
                offState = index;
            }
        }
        if (!best) {
            IOLog("MarbleCPU: domain=%u has no normal operating point\n",domain);
            continue;
        }
        // Match the EPSS driver's paired PDMEM and performance-state votes.
        pdmem[0x804 / 4 + domain] = selected;
        registers[0x320 / 4] = selected;
        __asm__ volatile("dsb sy" ::: "memory");
        domainFrequency[domain] = best;
        domainState[domain] = registers[0x320 / 4];
        configuredDomains |= 1u << domain;
        minimumState[domain] = floorState;
        peakState[domain] = selected;
        screenOffState[domain] = offFrequency ? offState : floorState;
        marble_cpu_screen_budget[1 + domain] = screenOffState[domain];
        marble_cpu_screen_budget[4 + domain] = domainState[domain];
        marble_cpu_idle_power[5 + domain] = floorState;
        marble_cpu_idle_power[8 + domain] = selected;
        marble_cpu_idle_power[17 + domain] = domainState[domain];
        IOLog("MarbleCPU: domain=%u state=%u->%u frequency_khz=%u readback=%u\n",
            domain, previous, selected, best, registers[0x320 / 4]);
    }
    // marble-s-oss dcvs_epss.c: the L3 LUT starts at 0x100 and the
    // shared vote is at 0x90 in the same EPSS block as the CPU domains.
    unsigned l3Index=0,l3Frequency=0,l3Previous=0,l3Floor=UINT32_MAX;
    for (unsigned index=0;index<40;++index) {
        const uint32_t row=base[0x100/4+index];
        const unsigned frequency=row>>30 ? (row&255)*19200 : 300000;
        if (index && frequency==l3Previous) break;
        l3Previous=frequency;
        if (frequency>l3Frequency) {l3Frequency=frequency;l3Index=index;}
        if (frequency && frequency < l3Floor) { l3Floor = frequency; l3Minimum = index; }
    }
    l3Maximum = l3Index;
    l3Available = l3Frequency && l3Floor != UINT32_MAX;
    marble_cpu_l3_power[0] = l3Available ? l3Floor : 0;
    marble_cpu_l3_power[1] = l3Frequency;
    marble_cpu_l3[0]=base[0x90/4];
    base[0x90/4]=l3Index;
    __asm__ volatile("dsb sy":::"memory");
    marble_cpu_l3[1]=l3Index;marble_cpu_l3[2]=l3Frequency;marble_cpu_l3[3]=base[0x90/4];
    IOLog("MarbleCPU: L3 state=%u->%u frequency_khz=%u readback=%u\n",
        marble_cpu_l3[0],l3Index,l3Frequency,marble_cpu_l3[3]);
    const ml_topology_info_t *topology = ml_get_topology_info();
    uint32_t enabled = 0;
    for (unsigned cpu = 0; cpu < topology->num_cpus; ++cpu) {
        const unsigned physical = (topology->cpus[cpu].phys_id >> 8) & 0xff;
        if (physical < 8) enabled |= 1u << physical;
    }
    idlePolicy.configure(enabled);
    for (unsigned domain = 0; domain < 3; ++domain)
        marble_cpu_idle_power[2 + domain] = idlePolicy.active(domain);
    frequencyMap = mapping;
    voteMemoryMap = voteMap;
    frequencyRegisters = base;
    voteRegisters = pdmem;
    marble_cpu_idle_power[0] = marble_cpu_screen_budget[0] = 1;
    uint64_t frequency;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(frequency));
    l3SampleTicks = frequency * 64 / 1000;
    __atomic_store_n(&votesReady, 1u, __ATOMIC_RELEASE);
    native_marble_system_sleep_configure();
}

extern "C" unsigned native_marble_cpu_report(char *out, unsigned cap) {
    unsigned at = snprintf(out, cap, "CPU smp=%u secondaries=%u free_pages=%u kHz=%u/%u/%u states=%u/%u/%u\n",
        armcpu_smp_requested, armcpu_smp_running, vm_page_free_count,
        domainFrequency[0], domainFrequency[1], domainFrequency[2],
        __atomic_load_n(&domainState[0], __ATOMIC_RELAXED),
        __atomic_load_n(&domainState[1], __ATOMIC_RELAXED),
        __atomic_load_n(&domainState[2], __ATOMIC_RELAXED));
    for (unsigned core = 0; core < 8 && at + 100 < cap; ++core)
        at += snprintf(out + at, cap - at, "CPU%u step=%u failure=%u psci=%d timer=%u ipi=%u\n",
            core, armcpu_smp_step[core], armcpu_smp_fail[core], armcpu_smp_psci_rc[core],
            armcpu_core_timer[core], armcpu_core_sgi[core]);
    at += snprintf(out + at, cap - at, "IPI sent=%llu received=%llu GICD_TYPER=%x\n",
        armcpu_ipi_sent, armcpu_ipi_interrupts, armcpu_gic_boot[3]);
    return at;
}
