// SPDX-License-Identifier: GPL-2.0-only
// SM7475 gen7_4_0 setup follows Qualcomm KGSL adreno_gen7.c.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
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
#include "NativeGpuTrace.h"

extern "C" volatile uint32_t native_adreno_io_coherent;
extern "C" volatile uint32_t native_adreno_cp_aperture[4];
extern "C" volatile uint32_t native_adreno_wc_enabled,native_adreno_owned_coherent;
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned int);
extern "C" void ml_io_unmap(vm_offset_t, vm_size_t);
extern "C" int native_marble_zap_load(void);
extern "C" const uint8_t marble_sqe_firmware[];
extern "C" const uint64_t marble_sqe_firmware_size;
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *,
    const NativeAdrenoRingClock *, const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
extern "C" bool native_adreno_preempt_irq(void);
extern "C" int native_marble_gmu_write(uint32_t, uint32_t);
extern "C" int native_marble_gmu_read(uint32_t, uint32_t *);
extern "C" int get_preemption_level(void);
extern "C" void native_adreno_set_restart(NativeAdrenoRestart, void *);
extern "C" kmod_info_t g_kernel_kmod_info;
// Interrupt count, observed sources, last source, fence IRQs, preemption IRQs,
// waiter sleeps, timed-out sleeps and deferred preemption completions.
extern "C" { volatile uint32_t marble_gpu_irq[8]; }
// Context-switch writes, AHB-fenced retries, exhausted retries, last fence status.
extern "C" { volatile uint32_t marble_gpu_preempt_writes[4]; }
extern "C" { volatile uint32_t marble_gpu_system_cache[20]; }
extern "C" { volatile uint64_t marble_gpu_clock[64][4]; volatile uint32_t marble_gpu_clock_count; }

namespace {
class CommandProcessor {
    IOMemoryMap *gpuMap = nullptr, *smmuMap = nullptr, *bufferMaps[GpuBufferCount] = {};
    volatile uint32_t *registers = nullptr;
    volatile uint8_t *cpu[GpuBufferCount] = {};
    NativeAdrenoGpuBuffer buffers[GpuBufferCount] = {};
    NativeAdrenoRing ring;
    NativeAdrenoRing backgroundRing;
    bool irqAttached = false;
    volatile uint32_t interrupts = 0;
    thread_call_t preemptCall = nullptr;
    unsigned preemptMode = 2;
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
    void configureSystemCache() {
        // marble-s-oss llcc_qcom.c: ukee_data, four LLCC v2.1 banks.
        // adreno_gen7.c assigns the GPU slice to the six GBIF clients.
        auto *mapping=map(0x19a00000,0x42000,kIOMapInhibitCache);
        if (!mapping) {marble_gpu_system_cache[0]=0x80000001;return;}
        auto *cache=reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress());
        auto store=[&](uint32_t offset,uint32_t value) {
            cache[offset/4]=value;__asm__ volatile("dsb sy":::"memory");
        };
        constexpr uint32_t slices[2]={11,12},capacityKiB[2]={256,1024};
        constexpr uint32_t mask=(1U<<11)|(1U<<12);
        marble_gpu_system_cache[0]=1;
        for (unsigned i=0;i<2;++i) {
            const uint32_t slice=slices[i],at=1+i*3;
            marble_gpu_system_cache[at]=cache[(slice*4096+4)/4];
            marble_gpu_system_cache[at+1]=cache[(0x21000+slice*8)/4];
            marble_gpu_system_cache[at+2]=cache[(0x21004+slice*8)/4];
            store(0x21004+slice*8,((capacityKiB[i]*1024/4/64)<<16)|0x18);
            store(0x21000+slice*8,0x00ff0000);
        }
        store(0x21f20,(cache[0x21f20/4]&~mask)|(1U<<12));
        store(0x21f2c,cache[0x21f2c/4]&~mask);
        store(0x21f00,cache[0x21f00/4]&~mask);
        store(0x21f04,cache[0x21f04/4]|mask);
        for (unsigned i=0;i<2;++i) {
            const uint32_t slice=slices[i],at=7+i*3;
            store(slice*4096,3);store(slice*4096,2);
            for (unsigned turn=0;turn<100 && (cache[(slice*4096+4)/4]&2);++turn) IODelay(1);
            marble_gpu_system_cache[at]=cache[(slice*4096+4)/4];
            marble_gpu_system_cache[at+1]=cache[(0x21000+slice*8)/4];
            marble_gpu_system_cache[at+2]=cache[(0x21004+slice*8)/4];
        }
        if (!(marble_gpu_system_cache[10]&2)) {
            uint32_t clients=0;
            for (unsigned block=0;block<6;++block) clients|=12U<<(block*5);
            write(0x3c02,clients);write(0x3c01,(12U<<10)|(1U<<8));
        }
        marble_gpu_system_cache[13]=read(0x3c01);marble_gpu_system_cache[14]=read(0x3c02);
        marble_gpu_system_cache[15]=cache[0x21f20/4];marble_gpu_system_cache[16]=cache[0x21f2c/4];
        marble_gpu_system_cache[17]=cache[0x21f00/4];marble_gpu_system_cache[18]=cache[0x21f04/4];
        marble_gpu_system_cache[0]=2;
        IOLog("MarbleGPU: LLCC gpu=%x/%x/%x htw=%x/%x/%x gbif=%x/%x\n",
            marble_gpu_system_cache[10],marble_gpu_system_cache[11],marble_gpu_system_cache[12],
            marble_gpu_system_cache[7],marble_gpu_system_cache[8],marble_gpu_system_cache[9],
            marble_gpu_system_cache[13],marble_gpu_system_cache[14]);
        mapping->release();
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
    // UniXNU-sheng GpuPowerProbe.cpp preemptWrite: the keepalive vote owns
    // the power transition while AHB-fenced context-switch writes are retried.
    static int preemptWrite(void *cookie, uint32_t word, uint32_t value) {
        ++marble_gpu_preempt_writes[0];
        for (unsigned attempt = 0; attempt < 200; ++attempt) {
            if (writeRegister(cookie, word, value)) return 1;
            barrier(nullptr);
            uint32_t status = 0;
            if (native_marble_gmu_read(0x23b13, &status)) return 1;
            marble_gpu_preempt_writes[3] = status;
            if (!(status & 2)) return 0;
            ++marble_gpu_preempt_writes[1];
            IODelay(10);
        }
        ++marble_gpu_preempt_writes[2];
        return 1;
    }
    static int barrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
    static int publish(void *, const volatile uint32_t *, size_t) { return barrier(nullptr); }
    static int acquire(void *, volatile uint32_t *, size_t) { return barrier(nullptr); }
    static uint64_t now(void *) {
        uint64_t nanoseconds = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
        return nanoseconds / 1000;
    }
    static int idle(void *cookie) {
        auto *self = static_cast<CommandProcessor *>(cookie);
        if (self && self->irqAttached && ml_get_interrupts_enabled() &&
            !ml_at_interrupt_context() && get_preemption_level() == 0) {
            assert_wait_timeout(event_t(&self->interrupts), THREAD_UNINT, 1000, NSEC_PER_USEC);
            const wait_result_t result = thread_block(THREAD_CONTINUE_NULL);
            __atomic_fetch_add(&marble_gpu_irq[5], 1U, __ATOMIC_RELAXED);
            if (result == THREAD_TIMED_OUT)
                __atomic_fetch_add(&marble_gpu_irq[6], 1U, __ATOMIC_RELAXED);
        } else IODelay(20);
        return 0;
    }
    static uint32_t preemptCompletion(void *) {
        return __atomic_load_n(&marble_gpu_irq[4], __ATOMIC_ACQUIRE);
    }
    static void finishPreemption(thread_call_param_t, thread_call_param_t) {
        (void)native_adreno_preempt_irq();
        __atomic_fetch_add(&marble_gpu_irq[7], 1U, __ATOMIC_RELAXED);
    }
    static void interrupt(void *cookie) {
        auto *self = static_cast<CommandProcessor *>(cookie);
        const uint32_t status = self->read(GEN7_RBBM_INT_0_STATUS);
        if (status) self->write(GEN7_RBBM_INT_CLEAR_CMD, status);
        ++self->interrupts;
        marble_gpu_irq[0] = self->interrupts;
        marble_gpu_irq[1] |= status;
        marble_gpu_irq[2] = status;
        if (status & MarbleGpuFenceInterrupt) {
            ++marble_gpu_irq[3];
            if (!(self->interrupts&63) && native_gpu_trace_enabled()) {
                const uint32_t sequence=marble_gpu_clock_count+1;
                volatile uint64_t *sample=marble_gpu_clock[sequence&63];
                auto counter=[&](uint32_t reg) {
                    uint32_t high,low;
                    do {high=self->read(reg+1);low=self->read(reg);} while (high!=self->read(reg+1));
                    return (uint64_t(high)<<32)|low;
                };
                sample[0]=sequence;sample[1]=mach_absolute_time();
                sample[2]=counter(0x31c);sample[3]=counter(0x980);
                __atomic_store_n(&marble_gpu_clock_count,sequence,__ATOMIC_RELEASE);
            }
        }
        if (status & MarbleGpuPreemptInterrupt) {
            __atomic_fetch_add(&marble_gpu_irq[4], 1U, __ATOMIC_RELEASE);
            // The submission mutex can have a turnstile inheritor. Complete
            // the switch on a thread, as in sheng's GPU interrupt handler.
            if (self->preemptCall) thread_call_enter(self->preemptCall);
        }
        thread_wakeup(event_t(&self->interrupts));
    }
    static int gmuWrite(void *, uint32_t reg, uint32_t value) {
        return native_marble_gmu_write(reg, value);
    }
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
        NativeAdrenoGmuContextReport report = {};
        IOReturn status = NativeAdrenoGpuContextBringup(reinterpret_cast<volatile uint8_t *>(smmu),
            0x40000, 20, 21, streams, 4, contextProgress, &report);
        IOLog("MarbleGPU: GPU context status=%x step=%u program=%u routes=%u output_bits=%u\n",
            status, report.step, report.program_status, report.routes_verified, report.output_bits);
        if (status || NativeAdrenoGpuContextBuffers(buffers)) return -5;
        outputBits = report.output_bits;
        for (unsigned i = 0; i < GpuBufferCount; ++i) {
            bufferMaps[i] = map(buffers[i].physical, buffers[i].bytes, kIOMapWriteCombineCache);
            if (!bufferMaps[i]) return -12;
            cpu[i] = reinterpret_cast<volatile uint8_t *>(bufferMaps[i]->getVirtualAddress());
        }
        return 0;
    }
    static IOReturn restart(void *cookie, NativeAdrenoRing *activeRing, uint32_t completed) {
        auto *self = static_cast<CommandProcessor *>(cookie);
        if (activeRing != &self->ring) return kIOReturnBadArgument;
        self->write(GEN7_RBBM_INT_0_MASK, 0);
        self->write(GEN7_CP_SQE_CNTL, 3);
        IOReturn status = activeRing->busHalt(true, 1000000);
        if (status) return status;
        self->write(GEN7_RBBM_SW_RESET_CMD, 1);
        (void)self->read(GEN7_RBBM_SW_RESET_CMD);
        IODelay(100);
        self->write(GEN7_RBBM_SW_RESET_CMD, 0);
        (void)self->read(GEN7_RBBM_SW_RESET_CMD);
        IODelay(1);
        // SQE bootstrap runs before recover() replays the process tables.
        // Install the kernel mapping while CP is stopped, including its SQE,
        // ring and power-up list, through the existing SMMU switch operation.
        auto *context = NativeAdrenoGpuContextObject();
        auto *kernelVm = NativeAdrenoGpuContextVm();
        if (!context || !kernelVm) return kIOReturnNotReady;
        status = context->writeTtbr0(kernelVm->ttbr0());
        if (status) return status;
        status = self->configure(true, completed);
        if (!status) self->write(GEN7_RBBM_INT_0_MASK, MarbleGpuInterruptMask);
        IOLog("MarbleGPU: restart fence=%u status=%x\n", completed, status);
        return status;
    }
    int configure(bool restarting = false, uint32_t completed = 0) {
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
        write(0x507,0); // RBBM_ALWAYS_COUNT: core cycles beside the 19.2 MHz CP counter.
        write(GEN7_RBBM_INTERFACE_HANG_INT_CNTL, 0x40cfffff);
        write(GEN7_UCHE_CLIENT_PF, 0x81);
        native_marble_gpu_checkpoint(MarbleGpuProtect);
        write(GEN7_CP_PROTECT_CNTL, 0xb);
        for (const auto &entry : MarbleProtect) write(entry[0], entry[1]);
        configureSystemCache();
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
        ring.setTableUpdateWords(3);
        IOReturn status = restarting ? ring.restart(completed) :
            ring.attach(&io, &cache, const_cast<uint8_t *>(cpu[1]), buffers[1].bytes,
                        buffers[1].iova, const_cast<uint8_t *>(cpu[2]), buffers[2].bytes, buffers[2].iova);
        if (status) return -5;
        unsigned timeline=0,diag=0;
        PE_parse_boot_argn("sheng-diag",&diag,sizeof(diag));
        if(!PE_parse_boot_argn("sheng-gpu-timeline",&timeline,sizeof(timeline)))timeline=diag==1;
        ring.setTimeline(timeline!=0);
        PE_parse_boot_argn("sheng-gpu-preempt", &preemptMode, sizeof(preemptMode));
        if (preemptMode && !restarting) {
            backgroundRing.setTableUpdateWords(3);
            status = backgroundRing.attachSecondary(&io, &cache, const_cast<uint8_t *>(cpu[9]),
                buffers[9].bytes, buffers[9].iova, const_cast<uint8_t *>(cpu[10]), buffers[10].bytes, buffers[10].iova);
            if (!status) backgroundRing.setTimeline(timeline!=0);
            if (!status) status = ring.setPreempt(buffers[5].iova, buffers[7].iova, buffers[2].iova + 8);
            if (!status) status = backgroundRing.setPreempt(buffers[6].iova, buffers[8].iova, buffers[10].iova + 8);
            if (status) return status;
        }
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
        const NativeAdrenoCpInitConfig config{buffers[0].iova, buffers[3].iova, 1, true};
        NativeAdrenoCpInit initializer;
        uint32_t fence = 0, seen = 0;
        status = initializer.run(&io, &ring, &config, &fence, !restarting);
        const NativeAdrenoRingClock clock{this, now, idle};
        if (restarting) {
            if (!status) status = ring.idle(&clock, 1000000);
            uint32_t leaveSecure[2] = {};
            AdrenoPM4Type7(0x66, 1, leaveSecure);
            if (!status) status = ring.submitUnfenced(leaveSecure, 2);
            if (!status) status = ring.idle(&clock, 1000000);
            return status;
        }
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
    static void openCpAperture() {
        auto call = [](uint64_t function, uint64_t arguments, uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3) {
            struct Result {int64_t status; uint64_t value;};
            uint64_t next = function, resumeState = 0;
            for (unsigned resumes = 0, busy = 0; resumes < 10000;) {
                register uint64_t x0 __asm__("x0") = next, x1 __asm__("x1") = arguments;
                register uint64_t x2 __asm__("x2") = p0, x3 __asm__("x3") = p1, x4 __asm__("x4") = p2;
                register uint64_t x5 __asm__("x5") = p3, x6 __asm__("x6") = resumeState, x7 __asm__("x7") = 0;
                __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7) ::
                    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
                if (x0 == 1) {next = 1; resumeState = x6; ++resumes; continue;}
                if (int64_t(x0) == -12 && busy++ < 20) {next = function; resumeState = 0; IOSleep(30); continue;}
                return Result{int64_t(x0), x1};
            }
            return Result{-110, 0};
        };
        const auto available = call(0x42000601u, 1, 0x02000c1bu, 0, 0, 0);
        native_adreno_cp_aperture[0] = available.status ? 0x80000000u | uint32_t(available.status) : uint32_t(available.value);
        if (available.status || !available.value) {native_adreno_cp_aperture[1] = 0xffffffffu; return;}
        // KGSL sets the aperture to the GPU context bank selected by the board.
        const auto opened = call(0x42000c1bu, 4, 0xffff0000u | 20u, 0xffffffffu, 0xffffffffu, 0xffffffffu);
        native_adreno_cp_aperture[1] = uint32_t(opened.status);
    }
    static bool dramPage(uint64_t pa, uint32_t *out) {
        const vm_offset_t page = ml_io_map_unmappable(vm_offset_t(pa), 0x1000, 0x7 /* VM_WIMG_IO */);
        if (!page) return false;
        for (unsigned i = 0; i < 1024; ++i) out[i] = reinterpret_cast<const volatile uint32_t *>(page)[i];
        ml_io_unmap(page, 0x1000);
        return true;
    }
    static int dramWord(uint64_t physical, uint32_t *out) {
        const vm_offset_t page=ml_io_map_unmappable(vm_offset_t(physical&~0xfffULL),0x1000,0x7);
        if (!page) return -1;
        *out=reinterpret_cast<const volatile uint32_t *>(page)[(physical&0xfff)/4];
        ml_io_unmap(page,0x1000);
        return 0;
    }
    bool gpuCopyPage(uint64_t src, uint64_t dst) {
        uint32_t words[8] = {};
        if (AdrenoPM4Type7(0x75, 5, &words[0]) != AdrenoPM4Success || AdrenoPM4Type7(0x12, 0, &words[6]) != AdrenoPM4Success) return false;
        words[1] = 1024; words[2] = uint32_t(src); words[3] = uint32_t(src >> 32); words[4] = uint32_t(dst); words[5] = uint32_t(dst >> 32);
        const NativeAdrenoRingClock clock{this, now, idle};
        uint32_t fence=0;
        return !ring.submit(words,7,&fence) && !ring.wait(fence,&clock,200000,nullptr);
    }
    void coherenceProbe(NativeAdrenoVm *vm) {
        uint32_t k[8] = {};
        unsigned gCoherenceAttempts=0;
        unsigned wanted = 1;
        PE_parse_boot_argn("marble-gpu-coherent", &wanted, sizeof wanted);
        if (!wanted || !vm) {
            IOLog("MarbleGPU: coherence disabled arg=%u vm=%u\n",wanted,vm!=nullptr);
            return;
        }
        constexpr uint64_t kIova = 0x18000000;
        constexpr size_t kBytes = 4 * 4096;
        uint8_t *buf = static_cast<uint8_t *>(IOMallocAligned(kBytes, 4096));
        NativeAdrenoMemory mem;
        uint64_t handle = 0;
        IOReturn st = buf ? mem.importTask(kernel_task, uint64_t(uintptr_t(buf)), kBytes, 40, kBytes) : kIOReturnNoMemory;
        k[0] = 1;
        if (!st) {k[0] = 2; st = vm->map(&mem, 0, kIova, 3, 1, 3, &handle);}
        uint64_t phys[4] = {};
        for (size_t j = 0; !st && j < mem.spanCount(); ++j)
            for (size_t at = 0; at < mem.spans()[j].bytes; at += 4096) phys[(mem.spans()[j].offset + at) / 4096] = mem.spans()[j].physical + at;
        static uint32_t words[1024];
        bool pass = false;
        if (!st && phys[0] && phys[1] && phys[2] && phys[3]) {
            k[0] = 3;
            volatile uint32_t *S = reinterpret_cast<volatile uint32_t *>(buf), *D = S + 1024, *R = S + 2048, *T = S + 3072;
            // read direction. The proof needs S's new words still dirty in a CPU cache (DRAM still holding the old ones, k[1] >= 48 of 64
            // lines) when the GPU copies S. int-150 found every line already written back once (d0/64) while both directions were right
            // (r1024/0 w1024/1024): the precondition, not the snoop, had failed, and the whole boot ran with per-submission maintenance.
            // So the setup repeats until the lines are seen dirty, up to eight times (attempts in the row), and only then does the GPU copy.
            bool dirty = false;
            for (unsigned attempt = 0; attempt < 8 && !st && !dirty; ++attempt) {
                gCoherenceAttempts = attempt + 1;
                for (unsigned i = 0; i < 1024; ++i) {S[i] = 0x11110000u | i; D[i] = 0; R[i] = 0x33330000u | i; T[i] = 0x44440000u | i;}
                st = NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
                for (unsigned i = 0; i < 1024; ++i) S[i] = 0x22220000u | i;
                __asm__ volatile("dsb sy" ::: "memory");
                k[1] = 0;
                if (!st && dramPage(phys[0], words)) for (unsigned i = 0; i < 1024; i += 16) k[1] += words[i] == (0x11110000u | i);
                dirty = k[1] >= 48;
            }
            if (!st && dirty) {
                k[0] = 4;
                if (gpuCopyPage(kIova, kIova + 4096) && dramPage(phys[1], words)) {
                    k[0] = 5;
                    for (unsigned i = 0; i < 1024; ++i) {k[2] += words[i] == (0x22220000u | i); k[3] += words[i] == (0x11110000u | i);}
                }
            }
            // write direction: T written back and kept (clean, resident), then read so its lines are present when the GPU writes
            if (!st && k[0] == 5) st = NativeAdrenoMaintainOwnedPages(const_cast<uint32_t *>(T), 4096, NativeAdrenoClean);
            uint32_t touch = 0;
            for (unsigned i = 0; i < 1024; ++i) touch += T[i];
            __asm__ volatile("dsb sy" ::: "memory");
            if (!st && k[0] == 5 && gpuCopyPage(kIova + 8192, kIova + 12288)) {
                k[0] = 6;
                for (unsigned i = 0; i < 1024; ++i) k[4] += T[i] == (0x33330000u | i);
                if (dramPage(phys[3], words)) for (unsigned i = 0; i < 1024; ++i) k[5] += words[i] == (0x33330000u | i);
            }
            (void)touch;   /* the loads are volatile; the sum only keeps them one expression */
            pass = k[0] == 6 && k[1] >= 48 && k[2] == 1024 && k[4] == 1024 && k[5] == 1024;
        }
        k[6] = uint32_t(st);
        if (handle) (void)vm->unmap(handle);
        // Release the imported pages after their GPU copies and cache maintenance.
        if (buf) (void)NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
        (void)mem.release();
        if (buf) IOFreeAligned(buf, kBytes);
        k[7] = pass;
        native_adreno_io_coherent = pass ? 1 : 0;
        IOLog("MarbleGPU: coherence dirty=%u/64 attempts=%u read=%u/%u write=%u/%u status=%x stage=%u enabled=%u\n",
            k[1],gCoherenceAttempts,k[2],k[3],k[4],k[5],k[6],k[0],k[7]);
    }
    int publishService() {
        NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
        native_adreno_dram_read32=dramWord;
        // Cacheable system BOs use the GPU's board-qualified coherent mapping.
        native_adreno_wc_enabled=1;
        native_adreno_owned_coherent=1;
        coherenceProbe(vm);
        openCpAperture();
        IOReturn status = submit.attach(vm, &ring);
        if (!status && preemptMode) {
            NativeAdrenoSubmit::PreemptSetup setup{};
            setup.io = {this, preemptWrite, readRegister, barrier, this, gmuWrite, preemptCompletion};
            for (unsigned i = 0; i < 2; ++i) {
                setup.pages.record[i] = buffers[5 + i].iova;
                setup.pages.smmu_info[i] = buffers[7 + i].iova;
                setup.pages.record_cpu[i] = const_cast<uint8_t *>(cpu[5 + i]);
                setup.pages.smmu_cpu[i] = const_cast<uint8_t *>(cpu[7 + i]);
            }
            setup.now = now; setup.clock = this; setup.mode = preemptMode;
            status = submit.setPreemption(&ring, &backgroundRing, setup, true);
        }
        const NativeAdrenoTimeIo timeIo{this, readRegister};
        if (!status) status = timer.attach(&timeIo, NativeAdrenoTimeCp);
        const NativeAdrenoRingClock clock{this, now, idle};
        static NativeAdrenoAbiDeviceInfo publishedInfo = deviceInfo;
        const unsigned inputBits = NativeAdrenoGpuContextInputBits();
        publishedInfo.va_start = GpuClientVaStart;
        publishedInfo.va_size = inputBits > 32 ? (UINT64_C(1) << inputBits) - GpuClientVaStart : 0;
        if (native_adreno_io_coherent) publishedInfo.flags |= NativeAdrenoDeviceCachedCoherent;
        if (!status) status = native_adreno_publish(vm, &submit, &ring, &clock, &timer, &publishedInfo, inputBits, outputBits);
        if (status) return status;
        native_adreno_set_restart(restart, this);
        preemptCall = thread_call_allocate_with_options(finishPreemption, this,
            THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
        if (!preemptCall) return kIOReturnNoMemory;
        status = armcpu_spi_attach(32 + 300, false, interrupt, this, nullptr, nullptr);
        if (status) return status;
        irqAttached = true;
        write(GEN7_RBBM_INT_CLEAR_CMD, 0xffffffff);
        write(GEN7_RBBM_INT_0_MASK, MarbleGpuInterruptMask);
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
    int stop() {
        if (!registers) return 0;
        write(GEN7_RBBM_INT_0_MASK, 0);
        irqAttached = false;
        write(GEN7_CP_SQE_CNTL, 3);
        write(GEN7_RBBM_GBIF_HALT, 1);
        write(GEN7_GBIF_HALT, 3);
        for (unsigned i = 0; i < 10000; ++i) {
            if ((read(GEN7_GBIF_HALT_ACK) & 3) == 3) return 0;
            IODelay(10);
        }
        return -110;
    }
    unsigned report(char *text, unsigned capacity) const {
        if (!registers) return 0;
        const auto *fence = reinterpret_cast<const volatile uint32_t *>(cpu[2]);
        return snprintf(text,capacity,"GPU hardware status=%x irq=%x fault=%x/%x protect=%x/%x IB=%x:%x/%x shadows=%u/%u/%u\n",
            read(GEN7_RBBM_STATUS),read(GEN7_RBBM_INT_0_STATUS),read(GEN7_CP_HW_FAULT),read(GEN7_CP_BV_HW_FAULT),
            read(GEN7_CP_PROTECT_STATUS),read(GEN7_CP_BV_PROTECT_STATUS),read(GEN7_CP_IB1_BASE_HI),read(GEN7_CP_IB1_BASE_LO),read(GEN7_CP_IB1_REM_SIZE),
            fence ? fence[0] : 0,fence ? fence[2] : 0,fence ? fence[3] : 0);
    }
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

extern "C" unsigned native_marble_adreno_report(char *text, unsigned capacity) {
    return commandProcessor ? commandProcessor->report(text,capacity) : 0;
}

extern "C" int native_marble_adreno_stop(void) {
    return commandProcessor ? commandProcessor->stop() : 0;
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
