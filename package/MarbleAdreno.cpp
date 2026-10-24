// SPDX-License-Identifier: GPL-2.0-only
// SM7475 gen7_4_0 setup follows Qualcomm KGSL adreno_gen7.c.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoPwrup.h"
#include "AdrenoPM4.h"
#include "MarbleAdrenoRegisters.h"

extern "C" const uint8_t marble_sqe_firmware[];
extern "C" const uint64_t marble_sqe_firmware_size;

namespace {
class CommandProcessor {
    IOMemoryMap *gpuMap = nullptr, *smmuMap = nullptr, *bufferMaps[5] = {};
    volatile uint32_t *registers = nullptr;
    volatile uint8_t *cpu[5] = {};
    NativeAdrenoGpuBuffer buffers[5] = {};
    NativeAdrenoRing ring;
    NativeAdrenoCpInit initializer;
    static IOMemoryMap *map(uint64_t physical, size_t bytes, IOOptionBits options) {
        auto *memory = IODeviceMemory::withRange(physical, bytes);
        auto *mapping = memory ? memory->map(kIOMapAnywhere | options) : nullptr;
        OSSafeReleaseNULL(memory);
        return mapping;
    }
    uint32_t read(uint32_t word) const { return registers[word]; }
    void write(uint32_t word, uint32_t value) {
        registers[word] = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void write64(uint32_t word, uint64_t value) {
        write(word, uint32_t(value)); write(word + 1, uint32_t(value >> 32));
    }
    static int readRegister(void *cookie, uint32_t word, uint32_t *value) {
        *value = static_cast<CommandProcessor *>(cookie)->read(word); return 0;
    }
    static int writeRegister(void *cookie, uint32_t word, uint32_t value) {
        static_cast<CommandProcessor *>(cookie)->write(word, value); return 0;
    }
    static int barrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
    static int publish(void *, const volatile uint32_t *, size_t) { return barrier(nullptr); }
    static int acquire(void *, volatile uint32_t *, size_t) { return barrier(nullptr); }
    static uint64_t now(void *) {
        uint64_t nanoseconds = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
        return nanoseconds / 1000;
    }
    static int idle(void *) { IODelay(20); return 0; }
    int context() {
        gpuMap = map(0x3d00000, 0x40000, kIOMapInhibitCache);
        smmuMap = map(0x3da0000, 0x40000, kIOMapInhibitCache);
        if (!gpuMap || !smmuMap) return -12;
        registers = reinterpret_cast<volatile uint32_t *>(gpuMap->getVirtualAddress());
        auto *smmu = reinterpret_cast<volatile uint32_t *>(smmuMap->getVirtualAddress());
        // GMU uses bank 21 and stream slot 0. GPU streams 0 and 1 belong to bank 20.
        for (unsigned slot = 1; slot <= 2; ++slot) {
            smmu[(0xc00 + slot * 4) / 4] = 2U << 16;
            smmu[(0x800 + slot * 4) / 4] = (1U << 31) | (slot - 1);
        }
        barrier(nullptr);
        NativeAdrenoGmuContextReport report = {};
        IOReturn status = NativeAdrenoGpuContextBringup(reinterpret_cast<volatile uint8_t *>(smmu),
            0x40000, 20, 21, 1, 0, 2, 1, nullptr, &report);
        IOLog("MarbleGPU: GPU context status=%x step=%u program=%u routes=%u output_bits=%u\n",
            status, report.step, report.program_status, report.routes_verified, report.output_bits);
        if (status || NativeAdrenoGpuContextBuffers(buffers)) return -5;
        for (unsigned i = 0; i < 5; ++i) {
            bufferMaps[i] = map(buffers[i].physical, buffers[i].bytes, kIOMapWriteCombineCache);
            if (!bufferMaps[i]) return -12;
            cpu[i] = reinterpret_cast<volatile uint8_t *>(bufferMaps[i]->getVirtualAddress());
        }
        return 0;
    }
    int configure() {
        if (marble_sqe_firmware_size < 8 || marble_sqe_firmware_size - 4 > buffers[0].bytes) return -22;
        auto *destination = reinterpret_cast<volatile uint32_t *>(cpu[0]);
        for (unsigned at = 4; at < marble_sqe_firmware_size; at += 4) {
            uint32_t value; memcpy(&value, marble_sqe_firmware + at, 4); destination[at / 4 - 1] = value;
        }
        barrier(nullptr);
        for (unsigned at = 4; at < marble_sqe_firmware_size; at += 4) {
            uint32_t value; memcpy(&value, marble_sqe_firmware + at, 4);
            if (destination[at / 4 - 1] != value) return -5;
        }
        write(GEN7_GBIF_HALT, 0);
        write(GEN7_RBBM_GBIF_HALT, 0);
        write(GEN7_RBBM_SECVID_TSB_CNTL, 0);
        write64(GEN7_RBBM_SECVID_TSB_TRUSTED_BASE_LO, 0);
        write(GEN7_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
        for (const auto &entry : MarbleHwcg) write(entry[0], entry[1]);
        write(GEN7_RBBM_CLOCK_CNTL, 0x8aa8aa82);
        write(GEN7_RBBM_GBIF_CLIENT_QOS_CNTL, 0x2120212);
        write(GEN7_UCHE_GBIF_GX_CONFIG, 0x10240e0);
        write(GEN7_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
        write64(GEN7_UCHE_TRAP_BASE_LO, 0x1fffffffff000ULL);
        write64(GEN7_UCHE_WRITE_THRU_BASE_LO, 0x1fffffffff000ULL);
        // gen7_4_0 does not program a UCHE GMEM range (uche_gmem_alignment is zero).
        write(GEN7_UCHE_CACHE_WAYS, 0x800000);
        write(GEN7_RBBM_PERFCTR_CNTL, 1);
        write(GEN7_RBBM_INTERFACE_HANG_INT_CNTL, 0x40cfffff);
        write(GEN7_UCHE_CLIENT_PF, 0x81);
        write(GEN7_CP_PROTECT_CNTL, 0xb);
        for (const auto &entry : MarbleProtect) write(entry[0], entry[1]);
        write(GEN7_UCHE_CMDQ_CONFIG, 0x6690e);
        write(GEN7_CP_APRIV_CNTL, 0x6f);
        write(GEN7_CP_BV_APRIV_CNTL, 0xf);
        write(GEN7_CP_CHICKEN_DBG, 1);
        write(GEN7_CP_BV_CHICKEN_DBG, 1);
        write(GEN7_RB_CMP_DBG_ECO_CNTL, read(GEN7_RB_CMP_DBG_ECO_CNTL) | (1U << 11));
        write(GEN7_RB_NC_MODE_CNTL, (1U << 11) | (1U << 4) | (3U << 1));
        write(GEN7_TPL1_NC_MODE_CNTL, 3U << 1);
        write(GEN7_SP_NC_MODE_CNTL, (2U << 4) | (3U << 1));
        for (unsigned pipe = 1; pipe <= 2; ++pipe) {
            write(GEN7_CP_APERTURE_CNTL_HOST, pipe << 12);
            write(GEN7_GRAS_NC_MODE_CNTL, 3U << 5);
        }
        write(GEN7_CP_APERTURE_CNTL_HOST, 0);
        write(GEN7_UCHE_MODE_CNTL, 3U << 21);
        write(GEN7_RBBM_NC_MODE_CNTL, 1);
        // Completion is polled until the GPU interrupt handler is attached.
        write(GEN7_RBBM_INT_0_MASK, 0);
        write64(GEN7_CP_SQE_INSTR_BASE_LO, buffers[0].iova);
        const NativeAdrenoRingIo io{this, writeRegister, readRegister, barrier};
        const NativeAdrenoRingCache cache{this, publish, acquire};
        IOReturn status = ring.attach(&io, &cache, const_cast<uint8_t *>(cpu[1]), buffers[1].bytes,
            buffers[1].iova, const_cast<uint8_t *>(cpu[2]), buffers[2].bytes, buffers[2].iova);
        if (status) return -5;
        write64(GEN7_CP_BV_RB_RPTR_ADDR_LO, buffers[2].iova + 8);
        uint32_t ifpcValues[sizeof(MarbleIfpc) / sizeof(*MarbleIfpc)];
        uint32_t preemptValues[sizeof(MarblePreempt) / sizeof(*MarblePreempt)];
        for (unsigned i = 0; i < sizeof(ifpcValues) / sizeof(*ifpcValues); ++i) ifpcValues[i] = read(MarbleIfpc[i]);
        for (unsigned i = 0; i < sizeof(preemptValues) / sizeof(*preemptValues); ++i) preemptValues[i] = read(MarblePreempt[i]);
        NativeAdrenoPwrupCounts counts{unsigned(sizeof(ifpcValues) / 4), unsigned(sizeof(preemptValues) / 4), 0};
        size_t written = 0;
        status = NativeAdrenoPwrupList::pack(counts, MarbleIfpc, ifpcValues, MarblePreempt, preemptValues,
            nullptr, const_cast<uint8_t *>(cpu[3]), buffers[3].bytes, &written);
        if (status) return -5;
        barrier(nullptr);
        write(GEN7_CP_SQE_CNTL, 1);
        const NativeAdrenoCpInitConfig config{buffers[0].iova, buffers[3].iova, 1};
        uint32_t fence = 0, seen = 0;
        status = initializer.run(&io, &ring, &config, &fence);
        const NativeAdrenoRingClock clock{nullptr, now, idle};
        auto waited = status ? NativeAdrenoRingFenceTimeout : ring.wait(fence, &clock, 200000, &seen);
        IOLog("MarbleGPU: CP init=%x fence=%u seen=%u wait=%u RBBM=%x fault=%x\n",
            status, fence, seen, waited, read(GEN7_RBBM_STATUS), read(GEN7_CP_HW_FAULT));
        if (status || waited) return -5;
        uint32_t words[4] = {};
        AdrenoPM4Type7(0x3d, 3, &words[0]);
        words[1] = uint32_t(buffers[4].iova); words[2] = uint32_t(buffers[4].iova >> 32); words[3] = 0xa7257475;
        status = ring.submit(words, 4, &fence);
        waited = status ? NativeAdrenoRingFenceTimeout : ring.wait(fence, &clock, 200000, &seen);
        uint32_t value = *reinterpret_cast<volatile uint32_t *>(cpu[4]);
        IOLog("MarbleGPU: CP write=%x fence=%u seen=%u wait=%u scratch=%x\n", status, fence, seen, waited, value);
        return status || waited || value != 0xa7257475 ? -5 : 0;
    }
public:
    int start() { int status = context(); return status ? status : configure(); }
};
CommandProcessor *commandProcessor;
}

extern "C" int native_marble_adreno_start(void) {
    commandProcessor = new CommandProcessor;
    return commandProcessor ? commandProcessor->start() : -12;
}
