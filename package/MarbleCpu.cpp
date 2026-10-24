// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM7475 EPSS layout from marble DT and MiCode qcom-cpufreq-hw.c.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>

extern "C" {
extern volatile uint32_t armcpu_smp_running, armcpu_smp_requested;
extern volatile uint32_t armcpu_smp_step[8], armcpu_smp_fail[8];
extern volatile int32_t armcpu_smp_psci_rc[8];
extern volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];
extern unsigned int vm_page_free_count;
}
static unsigned domainFrequency[3], domainState[3];

extern "C" void native_marble_cpu_start(void) {
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
        for (unsigned index = 0; index < 40; ++index) {
            uint32_t row = registers[0x100 / 4 + index];
            unsigned frequency = row >> 30 ? (row & 255) * 19200 : 300000;
            if (index && frequency == last) break;
            last = frequency;
            if (((row >> 16) & 7) == lutCoreCount && frequency > best) {
                selected = index;
                best = frequency;
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
        IOLog("MarbleCPU: domain=%u state=%u->%u frequency_khz=%u readback=%u\n",
            domain, previous, selected, best, registers[0x320 / 4]);
    }
    OSSafeReleaseNULL(mapping);
    OSSafeReleaseNULL(voteMap);
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
    return at;
}
