// SPDX-License-Identifier: BSD-2-Clause-Patent
// marble DT, MiCode gcc-waipio.c and gpucc-waipio.c.
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
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
    int result = powerCx();
    IOLog("MarbleGPU: CX initialization result=%d\n", result);
    if (!result) {
        result = native_marble_gmu_load();
        IOLog("MarbleGPU: GMU initialization result=%d\n", result);
        unsigned nativeMetal = 0;
        PE_parse_boot_argn("marble-adreno", &nativeMetal, sizeof(nativeMetal));
        if (!result && nativeMetal) {
            result = native_marble_adreno_start();
            IOLog("MarbleGPU: native Metal initialization result=%d\n", result);
        }
    }
}
}
extern "C" void native_marble_gpu_quiesce_inherited(void) {
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
