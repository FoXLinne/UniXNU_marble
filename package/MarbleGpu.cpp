// SPDX-License-Identifier: BSD-2-Clause-Patent
// marble DT, MiCode gcc-waipio.c and gpucc-waipio.c.
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include "MarbleGpu.h"

extern "C" void native_root_block_capture_log(void);
extern "C" int native_marble_gmu_load(void);
extern "C" int native_marble_adreno_start(void);

namespace {
struct Registers {
    IOMemoryMap *mapping = nullptr;
    volatile uint32_t *words = nullptr;
    bool map(uint64_t address, unsigned bytes) {
        auto *memory = IODeviceMemory::withRange(address, bytes);
        mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        words = mapping ? reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()) : nullptr;
        return words != nullptr;
    }
    ~Registers() { OSSafeReleaseNULL(mapping); }
    uint32_t read(unsigned offset) const { return words[offset / 4]; }
    void write(unsigned offset, uint32_t value) {
        words[offset / 4] = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void set(unsigned offset, uint32_t bits) { write(offset, read(offset) | bits); }
    bool wait(unsigned offset, uint32_t mask, uint32_t value) const {
        for (unsigned i = 0; i < 500; ++i) {
            if ((read(offset) & mask) == value) return true;
            IODelay(1);
        }
        IOLog("MarbleGPU: register=%x observed=%x mask=%x expected=%x\n",
            offset, read(offset), mask, value);
        return false;
    }
    bool clock(unsigned offset, unsigned divider) {
        uint32_t config = read(offset + 4);
        // GPLL0 / 2 is 300 MHz. HID encodes twice the divisor minus one.
        write(offset + 4, (config & ~0x71fu) | (6u << 8) | divider);
        set(offset, 3);
        return wait(offset, 1, 0);
    }
    bool branch(unsigned offset) {
        set(offset, 1);
        return wait(offset, 1u << 31, 0);
    }
};
uint64_t checkpoint;
bool probeOnly() {
    unsigned enabled = 0;
    return PE_parse_boot_argn("marble-gpu-probe", &enabled, sizeof(enabled)) && enabled == 1;
}
void drawCheckpoint(unsigned stage, int status) {
    static IOMemoryMap *mapping;
    static PE_Video video;
    if (!mapping) {
        if (PE_current_console(&video) || !video.v_baseAddr || video.v_depth != 32 ||
            video.v_width < 112 || video.v_height < 64) return;
        auto *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
        mapping = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
        OSSafeReleaseNULL(memory);
        if (!mapping) return;
    }
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}
    };
    auto *pixels = reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress());
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 112; ++x) {
            unsigned digit = x / 56, column = (x % 56) / 8, row = y / 8;
            unsigned number = digit ? stage % 10 : stage / 10 % 10;
            bool ink = column < 5 && row < 7 && (digits[number][row] & (1U << (4 - column)));
            pixels[y * (video.v_rowBytes / 4) + x] = ink ? 0xffffffff : status ? 0xff800000 : 0xff002060;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
int probe() {
    native_marble_gpu_checkpoint(MarbleGpuProbe);
    Registers gcc, cc, gmu;
    if (!gcc.map(0x100000, 0x82000) || !cc.map(0x3d90000, 0xa000)) return -12;
    uint32_t cx = cc.read(0x953c), gmuClock = cc.read(0x913c);
    IOLog("MarbleGPU: probe GCC=%x/%x/%x/%x/%x CX=%x/%x clocks=%x/%x/%x/%x\n",
        gcc.read(0x62018), gcc.read(0x62000), gcc.read(0x81004), gcc.read(0x81010), gcc.read(0x81018),
        cc.read(0x9108), cx, gmuClock, cc.read(0x93e8), cc.read(0x9148), cc.read(0x7000));
    if (!(cx & (1U << 31)) || !(gmuClock & 1) || (gmuClock & (1U << 31))) return -6;
    if (!gmu.map(0x3d68000, 0x37000)) return -12;
    IOLog("MarbleGPU: probe GMU reset=%x init=%x HFI=%x GX=%x state=%x AO=%x\n",
        gmu.read(0x16000), gmu.read(0x16070), gmu.read(0x16600),
        gmu.read(0x16340), gmu.read(0x163b0), gmu.read(0x26c14));
    return 0;
}
int powerCx() {
    Registers gcc, cc, gmu, smmu;
    if (!gcc.map(0x100000, 0x82000)) return -12;
    gcc.set(0x62018, 1);
    gcc.set(0x62000, (1u << 15) | (1u << 16));
    gcc.set(0x81004, 1);
    gcc.set(0x81010, 1);
    gcc.set(0x81018, 1);
    IODelay(3);
    IOLog("MarbleGPU: GCC cfg=%x memnoc=%x dvm=%x\n",
        gcc.read(0x81004), gcc.read(0x81010), gcc.read(0x81018));
    if (!cc.map(0x3d90000, 0xa000)) return -12;
    uint32_t power = cc.read(0x9108);
    // The DT supplies CLK_DIS_WAIT=8 and a separate CX power-status register.
    cc.write(0x9108, (power & ~0xf007u) | (8u << 12));
    IODelay(1);
    if (!cc.wait(0x953c, 1u << 31, 1u << 31)) return -110;
    IODelay(1);
    // GMU 200 MHz, hub 150 MHz; both use the inherited GPLL0 source.
    if (!cc.clock(0x9318, 2) || !cc.clock(0x93ec, 3)) return -110;
    cc.set(0x911c, 1);
    IODelay(3);
    if (!cc.branch(0x913c) || !cc.branch(0x93e8) ||
        !cc.branch(0x9148) || !cc.branch(0x7000)) return -110;
    IOLog("MarbleGPU: CX=%x status=%x GMU=%x hub=%x SMMU=%x\n",
        cc.read(0x9108), cc.read(0x953c), cc.read(0x913c), cc.read(0x9148), cc.read(0x7000));
    if (!gmu.map(0x3d68000, 0x37000) || !smmu.map(0x3da0000, 0x1000)) return -12;
    IOLog("MarbleGPU: GMU reset=%x firmware=%x SMMU=%x/%x/%x control=%x\n",
        gmu.read(0x16000), gmu.read(0x16070), smmu.read(0x20),
        smmu.read(0x24), smmu.read(0x28), smmu.read(0));
    return 0;
}
void worker(void *, wait_result_t) {
    if (probeOnly()) {
        int result = probe();
        native_marble_gpu_checkpoint(MarbleGpuProbe, result);
        thread_terminate(current_thread());
        return;
    }
    native_marble_gpu_checkpoint(MarbleGpuCx);
    int result = powerCx();
    IOLog("MarbleGPU: CX initialization result=%d\n", result);
    if (!result) {
        native_marble_gpu_checkpoint(MarbleGpuGmu);
        result = native_marble_gmu_load();
        IOLog("MarbleGPU: GMU initialization result=%d\n", result);
        unsigned nativeMetal = 0;
        PE_parse_boot_argn("marble-adreno", &nativeMetal, sizeof(nativeMetal));
        if (!result && nativeMetal) {
            result = native_marble_adreno_start();
            IOLog("MarbleGPU: native Metal initialization result=%d\n", result);
        }
    }
    if (result) native_marble_gpu_failed(result);
    thread_terminate(current_thread());
}
}
extern "C" void native_marble_gpu_checkpoint(unsigned stage, int status) {
    __atomic_store_n(&checkpoint, uint64_t(stage) << 32 | uint32_t(status), __ATOMIC_RELEASE);
    IOLog("MarbleGPU: checkpoint=%u status=%d\n", stage, status);
    unsigned diagnostics = 0;
    if (PE_parse_boot_argn("marble-gpu-diagnostics", &diagnostics, sizeof(diagnostics)) && diagnostics == 1) {
        drawCheckpoint(stage, status);
        native_root_block_capture_log();
        unsigned pauseMs = 0;
        if (stage >= MarbleGpuContextAllocate &&
            PE_parse_boot_argn("marble-gpu-pause-ms", &pauseMs, sizeof(pauseMs)) && pauseMs)
            IOSleep(pauseMs);
    }
}
extern "C" void native_marble_gpu_failed(int status) {
    uint64_t value = __atomic_load_n(&checkpoint, __ATOMIC_ACQUIRE);
    native_marble_gpu_checkpoint(unsigned(value >> 32), status);
}
extern "C" void native_marble_gpu_report(char *text, unsigned capacity) {
    uint64_t value = __atomic_load_n(&checkpoint, __ATOMIC_ACQUIRE);
    snprintf(text, capacity, "MarbleGPU checkpoint=%u status=%d ready=%u",
        unsigned(value >> 32), int32_t(value), native_marble_gmu_ready());
}
extern "C" void native_marble_gpu_quiesce_inherited(void) {
    if (probeOnly()) return;
    Registers cc, core;
    if (!cc.map(0x3d90000, 0xa000)) return;
    if (!(cc.read(0x953c) & (1U << 31)) || !(cc.read(0x913c) & 1)) return;
    if (!core.map(0x3d68000, 0x37000)) return;
    core.write(0x16000, 1);
    IODelay(100);
    IOLog("MarbleGPU: inherited GMU held in reset\n");
}
extern "C" void native_marble_gpu_start(void) {
    static bool started;
    if (__atomic_exchange_n(&started, true, __ATOMIC_RELAXED)) return;
    thread_t thread;
    kern_return_t result = kernel_thread_start(worker, nullptr, &thread);
    if (!result) thread_deallocate(thread);
    else IOLog("MarbleGPU: thread start=%d\n", result);
}
