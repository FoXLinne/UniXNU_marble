// SPDX-License-Identifier: GPL-2.0-only
// SM7475 gen7_4_0 setup follows Qualcomm KGSL adreno_gen7.c.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <mach/kmod.h>
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoPwrup.h"
#include "AdrenoPM4.h"
#include "MarbleAdrenoRegisters.h"
#include "MarbleGpu.h"

extern "C" volatile uint32_t native_adreno_pool_mib;
extern "C" int native_marble_zap_load(void);
extern "C" const uint8_t marble_sqe_firmware[];
extern "C" const uint64_t marble_sqe_firmware_size;
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *,
    const NativeAdrenoRingClock *, const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;

namespace {
class CommandProcessor {
    IOMemoryMap *gpuMap = nullptr, *smmuMap = nullptr, *bufferMaps[5] = {};
    volatile uint32_t *registers = nullptr;
    volatile uint8_t *cpu[5] = {};
    NativeAdrenoGpuBuffer buffers[5] = {};
    NativeAdrenoRing ring;
    NativeAdrenoCpInit initializer;
    NativeAdrenoSubmit submit;
    NativeAdrenoTimer timer;
    uint32_t outputBits = 0;
    // Live DT chip ID and UBWC configuration; KGSL gen7_4_0 supplies 2 MiB GMEM and no UCHE GMEM window.
    static constexpr NativeAdrenoAbiDeviceInfo deviceInfo = {
        0x07030002, 2U << 20, 0, 0x1fffffffff000ULL, 0x20000000, 0xe0000000,
        16, 6, 1, 4, NativeAdrenoDeviceClientIova, 0
    };
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
    static void contextProgress(uint32_t step) {
        native_marble_gpu_checkpoint(MarbleGpuContextAllocate + step - NativeAdrenoGpuContext::Allocate);
    }
    int context() {
        gpuMap = map(0x3d00000, 0x40000, kIOMapInhibitCache);
        smmuMap = map(0x3da0000, 0x40000, kIOMapInhibitCache);
        if (!gpuMap || !smmuMap) return -12;
        registers = reinterpret_cast<volatile uint32_t *>(gpuMap->getVirtualAddress());
        auto *smmu = reinterpret_cast<volatile uint32_t *>(smmuMap->getVirtualAddress());
        // DT masks SID bit 10: route both values for each GPU stream.
        const ArmSmmu4KStream streams[] = {{1, 0}, {2, 1}, {3, 0x400}, {4, 0x401}};
        for (const auto &stream : streams) {
            smmu[(0xc00 + stream.slot * 4) / 4] = 2U << 16;
            smmu[(0x800 + stream.slot * 4) / 4] = (1U << 31) | stream.sid;
        }
        barrier(nullptr);
        // GX was reset before this boot; retire the context retained by the previous kernel.
        const unsigned contextBase = 0x34000;
        uint32_t inheritedControl = smmu[contextBase / 4];
        smmu[contextBase / 4] = 0;
        barrier(nullptr);
        smmu[(contextBase + 0x618) / 4] = 0;
        smmu[(contextBase + 0x7f0) / 4] = 0xffffffff;
        barrier(nullptr);
        unsigned elapsed = 0;
        for (; elapsed < 100 && (smmu[(contextBase + 0x7f4) / 4] & 1); ++elapsed) IOSleep(1);
        IOLog("MarbleGPU: GPU context reset SCTLR=%x/%x TLB=%x elapsed=%u\n",
            inheritedControl, smmu[contextBase / 4], smmu[(contextBase + 0x7f4) / 4], elapsed);
        if (elapsed == 100) return -110;
        native_adreno_pool_mib = 256;
        IOLog("MarbleGPU: BO pool target=256 MiB\n");
        NativeAdrenoGmuContextReport report = {};
        IOReturn status = NativeAdrenoGpuContextBringup(reinterpret_cast<volatile uint8_t *>(smmu),
            0x40000, 20, 21, streams, 4, contextProgress, &report);
        IOLog("MarbleGPU: GPU context status=%x step=%u program=%u routes=%u output_bits=%u\n",
            status, report.step, report.program_status, report.routes_verified, report.output_bits);
        if (status || NativeAdrenoGpuContextBuffers(buffers)) return -5;
        outputBits = report.output_bits;
        for (unsigned i = 0; i < 5; ++i) {
            bufferMaps[i] = map(buffers[i].physical, buffers[i].bytes, kIOMapWriteCombineCache);
            if (!bufferMaps[i]) return -12;
            cpu[i] = reinterpret_cast<volatile uint8_t *>(bufferMaps[i]->getVirtualAddress());
        }
        return 0;
    }
    int configure() {
        native_marble_gpu_checkpoint(MarbleGpuCpFirmware);
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
        native_marble_gpu_checkpoint(MarbleGpuCpRegisters);
        native_marble_gpu_checkpoint(MarbleGpuGbif);
        write(GEN7_GBIF_HALT, 0);
        write(GEN7_RBBM_GBIF_HALT, 0);
        native_marble_gpu_checkpoint(MarbleGpuSecvid);
        write(GEN7_RBBM_SECVID_TSB_CNTL, 0);
        write64(GEN7_RBBM_SECVID_TSB_TRUSTED_BASE_LO, 0);
        write(GEN7_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
        write(GEN7_RBBM_GBIF_CLIENT_QOS_CNTL, 0x2120212);
        write(GEN7_UCHE_GBIF_GX_CONFIG, 0x10240e0);
        native_marble_gpu_checkpoint(MarbleGpuUche);
        write(GEN7_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
        write64(GEN7_UCHE_TRAP_BASE_LO, 0x1fffffffff000ULL);
        write64(GEN7_UCHE_WRITE_THRU_BASE_LO, 0x1fffffffff000ULL);
        // gen7_4_0 does not program a UCHE GMEM range (uche_gmem_alignment is zero).
        write(GEN7_UCHE_CACHE_WAYS, 0x800000);
        write(GEN7_RBBM_PERFCTR_CNTL, 1);
        write(GEN7_RBBM_INTERFACE_HANG_INT_CNTL, 0x40cfffff);
        write(GEN7_UCHE_CLIENT_PF, 0x81);
        native_marble_gpu_checkpoint(MarbleGpuProtect);
        write(GEN7_CP_PROTECT_CNTL, 0xb);
        for (const auto &entry : MarbleProtect) write(entry[0], entry[1]);
        native_marble_gpu_checkpoint(MarbleGpuNoncoherent);
        write(GEN7_UCHE_CMDQ_CONFIG, 0x6690e);
        write(GEN7_CP_APRIV_CNTL, 0x6f);
        write(GEN7_CP_BV_APRIV_CNTL, 0xf);
        write(GEN7_CP_CHICKEN_DBG, 1);
        write(GEN7_CP_BV_CHICKEN_DBG, 1);
        write(GEN7_RB_CMP_DBG_ECO_CNTL, read(GEN7_RB_CMP_DBG_ECO_CNTL) | (1U << 11));
        write(GEN7_RB_NC_MODE_CNTL, (1U << 11) | (1U << 4) | (3U << 1));
        write(GEN7_TPL1_NC_MODE_CNTL, 3U << 1);
        write(GEN7_SP_NC_MODE_CNTL, (2U << 4) | (3U << 1));
        native_marble_gpu_checkpoint(MarbleGpuAperture);
        for (unsigned pipe = 1; pipe <= 2; ++pipe) {
            write(GEN7_CP_APERTURE_CNTL_HOST, pipe << 12);
            write(GEN7_GRAS_NC_MODE_CNTL, 3U << 5);
        }
        write(GEN7_CP_APERTURE_CNTL_HOST, 0);
        write(GEN7_UCHE_MODE_CNTL, 3U << 21);
        write(GEN7_RBBM_NC_MODE_CNTL, 1);
        // Completion is polled until the GPU interrupt handler is attached.
        write(GEN7_RBBM_INT_0_MASK, 0);
        native_marble_gpu_checkpoint(MarbleGpuSqeBase);
        write64(GEN7_CP_SQE_INSTR_BASE_LO, buffers[0].iova);
        // KGSL enables HWCG after programming registers to keep their clocks available.
        native_marble_gpu_checkpoint(MarbleGpuHwcg);
        for (const auto &entry : MarbleHwcg) write(entry[0], entry[1]);
        write(GEN7_RBBM_CLOCK_CNTL, 0x8aa8aa82);
        native_marble_gpu_checkpoint(MarbleGpuCpRing);
        const NativeAdrenoRingIo io{this, writeRegister, readRegister, barrier};
        const NativeAdrenoRingCache cache{this, publish, acquire};
        IOReturn status = ring.attach(&io, &cache, const_cast<uint8_t *>(cpu[1]), buffers[1].bytes,
            buffers[1].iova, const_cast<uint8_t *>(cpu[2]), buffers[2].bytes, buffers[2].iova);
        if (status) return -5;
        native_marble_gpu_checkpoint(MarbleGpuRingReady);
        write64(GEN7_CP_BV_RB_RPTR_ADDR_LO, buffers[2].iova + 8);
        uint32_t ifpcValues[sizeof(MarbleIfpc) / sizeof(*MarbleIfpc)];
        uint32_t preemptValues[sizeof(MarblePreempt) / sizeof(*MarblePreempt)];
        native_marble_gpu_checkpoint(MarbleGpuPwrupRead);
        for (unsigned i = 0; i < sizeof(ifpcValues) / sizeof(*ifpcValues); ++i) ifpcValues[i] = read(MarbleIfpc[i]);
        for (unsigned i = 0; i < sizeof(preemptValues) / sizeof(*preemptValues); ++i) preemptValues[i] = read(MarblePreempt[i]);
        NativeAdrenoPwrupCounts counts{unsigned(sizeof(ifpcValues) / 4), unsigned(sizeof(preemptValues) / 4), 0};
        size_t written = 0;
        status = NativeAdrenoPwrupList::pack(counts, MarbleIfpc, ifpcValues, MarblePreempt, preemptValues,
            nullptr, const_cast<uint8_t *>(cpu[3]), buffers[3].bytes, &written);
        if (status) return -5;
        barrier(nullptr);
        native_marble_gpu_checkpoint(MarbleGpuCpInit);
        write(GEN7_CP_SQE_CNTL, 1);
        const NativeAdrenoCpInitConfig config{buffers[0].iova, buffers[3].iova, 1};
        uint32_t fence = 0, seen = 0;
        status = initializer.run(&io, &ring, &config, &fence);
        const NativeAdrenoRingClock clock{nullptr, now, idle};
        auto waited = status ? NativeAdrenoRingFenceTimeout : ring.wait(fence, &clock, 200000, &seen);
        IOLog("MarbleGPU: CP init=%x fence=%u seen=%u wait=%u RBBM=%x fault=%x\n",
            status, fence, seen, waited, read(GEN7_RBBM_STATUS), read(GEN7_CP_HW_FAULT));
        if (status || waited) return -5;
        native_marble_gpu_checkpoint(MarbleGpuCpScratch);
        uint32_t words[4] = {};
        AdrenoPM4Type7(0x3d, 3, &words[0]);
        words[1] = uint32_t(buffers[4].iova); words[2] = uint32_t(buffers[4].iova >> 32); words[3] = 0xa7257475;
        status = ring.submit(words, 4, &fence);
        waited = status ? NativeAdrenoRingFenceTimeout : ring.wait(fence, &clock, 200000, &seen);
        uint32_t value = *reinterpret_cast<volatile uint32_t *>(cpu[4]);
        IOLog("MarbleGPU: CP write=%x fence=%u seen=%u wait=%u scratch=%x\n", status, fence, seen, waited, value);
        if (status || waited || value != 0xa7257475) return -5;
        native_marble_gpu_checkpoint(MarbleGpuZap);
        int secure = native_marble_zap_load();
        if (secure) return secure;
        native_marble_gpu_checkpoint(MarbleGpuSecureExit);
        uint32_t leaveSecure[2] = {};
        AdrenoPM4Type7(0x66, 1, leaveSecure);
        status = ring.submit(leaveSecure, 2, &fence);
        waited = status ? NativeAdrenoRingFenceTimeout : ring.wait(fence, &clock, 200000, &seen);
        IOLog("MarbleGPU: secure mode exit status=%x fence=%u seen=%u wait=%u\n", status, fence, seen, waited);
        return status || waited ? -5 : 0;
    }
    int publishService() {
        NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
        IOReturn status = submit.attach(vm, &ring);
        const NativeAdrenoTimeIo timeIo{this, readRegister};
        if (!status) status = timer.attach(&timeIo, NativeAdrenoTimeCp);
        const NativeAdrenoRingClock clock{nullptr, now, idle};
        if (!status) status = native_adreno_publish(vm, &submit, &ring, &clock, &timer, &deviceInfo, 32, outputBits);
        if (status) return status;
        native_marble_gpu_checkpoint(MarbleGpuFamily);
        status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
        if (status) return status;
        native_marble_gpu_checkpoint(MarbleGpuMetaclass);
        void *module = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (!module) return kIOReturnNoResources;
        native_adreno_accelerator_static_init();
        status = OSMetaClass::postModLoad(module);
        if (status) return status;
        native_marble_gpu_checkpoint(MarbleGpuAccelerator);
        IOService *accelerator = native_adreno_accelerator_allocate();
        if (!accelerator || !accelerator->init()) return kIOReturnNoMemory;
        accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
        accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
        accelerator->setProperty("NativeSandboxGrantProcess", "*");
        if (!accelerator->attach(IOService::getPlatform())) return kIOReturnNotAttached;
        native_marble_gpu_checkpoint(MarbleGpuRegister);
        accelerator->registerService();
        IOLog("MarbleGPU: Adreno 725 Metal accelerator published chip=%x\n", deviceInfo.chip_id);
        return 0;
    }
public:
    int start() {
        native_marble_gpu_checkpoint(MarbleGpuContext);
        int status = context();
        IOLog("MarbleGPU: stage configure entry status=%d\n", status);
        if (!status) status = configure();
        if (!status) {
            native_marble_gpu_checkpoint(MarbleGpuPublish);
            status = publishService();
        }
        if (!status) native_marble_gpu_checkpoint(MarbleGpuReady);
        return status;
    }
};
CommandProcessor *commandProcessor;
}

extern "C" int native_marble_adreno_start(void) {
    if (!native_marble_gmu_ready()) {
        IOLog("MarbleGPU: CP requires completed GMU and GX initialization\n");
        return -6;
    }
    static bool starting;
    if (__atomic_exchange_n(&starting, true, __ATOMIC_ACQ_REL)) return -16;
    commandProcessor = new CommandProcessor;
    int status = commandProcessor ? commandProcessor->start() : -12;
    if (status) {
        native_marble_gpu_failed(status);
        char report[128];
        native_marble_gpu_report(report, sizeof(report));
        IOLog("%s CP result=%d\n", report, status);
    }
    return status;
}
