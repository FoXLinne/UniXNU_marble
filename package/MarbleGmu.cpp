// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM7475 register and firmware layout: MiCode marble-s-oss KGSL gen7.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "ArmSmmu4K.h"
#include "ArmSmmu4KMmio.h"
#include "ArmSmmu4KProgram.h"
#include "AdrenoFirmware.h"
#include "AdrenoHfi.h"

extern "C" const uint8_t marble_gmu_firmware[];
extern "C" const uint64_t marble_gmu_firmware_size;

namespace {
struct Registers {
    IOMemoryMap *mapping = nullptr;
    volatile uint32_t *words = nullptr;
    bool map(uint64_t physical, size_t bytes) {
        auto *memory = IODeviceMemory::withRange(physical, bytes);
        mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        words = mapping ? reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()) : nullptr;
        return words != nullptr;
    }
    uint32_t read(unsigned offset) const { return words[offset / 4]; }
    void write(unsigned offset, uint32_t value) {
        words[offset / 4] = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
};
struct Memory {
    IOBufferMemoryDescriptor *descriptor = nullptr;
    IOMemoryMap *mapping = nullptr;
    uint8_t *cpu = nullptr;
    uint64_t physical = 0;
    bool allocate(size_t bytes) {
        descriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, bytes, 0xffffff000ULL);
        if (!descriptor || descriptor->prepare() != kIOReturnSuccess) return false;
        mapping = descriptor->map(kIOMapAnywhere | kIOMapInhibitCache);
        if (!mapping) return false;
        cpu = reinterpret_cast<uint8_t *>(mapping->getVirtualAddress());
        addr64_t span = 0;
        physical = descriptor->getPhysicalSegment(0, &span);
        if (span < bytes) return false;
        memset(cpu, 0, bytes);
        return true;
    }
};
struct Gmu {
    Registers core, rscc, pdc, gpu, smmu;
    Memory cache, scratch, tables;
    ArmSmmu4KIo smmuIo = {};
    ArmSmmu4KMmio smmuMmio = {};
    ArmSmmu4KProgramResult programmed = {};
    AdrenoHfi hfi = {};
    unsigned bank = 0, slot = 0;
    unsigned offset(uint32_t word) const { return word * 4 - 0x68000; }
    uint32_t read(uint32_t word) const { return core.read(offset(word)); }
    void write(uint32_t word, uint32_t value) { core.write(offset(word), value); }
    static int notify(void *cookie) {
        static_cast<Gmu *>(cookie)->write(0x1f994, 1);
        return 0;
    }
    int mapMemory() {
        if (!smmu.map(0x3da0000, 0x40000)) return -12;
        uint32_t id0 = smmu.read(0x20), id1 = smmu.read(0x24), id2 = smmu.read(0x28);
        unsigned banks = id1 & 255, slots = id0 & 255;
        unsigned page = (id1 & (1U << 31)) ? 0x10000 : 0x1000;
        unsigned contextBase = page << (((id1 >> 28) & 7) + 1);
        // Reuse a route for the stopped GMU, or select an unused hardware slot.
        bool found = false;
        for (unsigned i = 0; i < slots; ++i) {
            uint32_t match = smmu.read(0x800 + i * 4);
            if ((match & 0x8000ffffU) == 0x80000005U) {
                slot = i;
                bank = smmu.read(0xc00 + i * 4) & 255;
                found = true;
                smmu.write(0xc00 + slot * 4, 2U << 16);
                smmu.write(contextBase + bank * page, 0);
                break;
            }
        }
        if (!found) {
            for (slot = 0; slot < slots; ++slot)
                if (!(smmu.read(0x800 + slot * 4) & (1U << 31))) break;
            if (slot == slots) return -28;
            for (bank = banks; bank > 0; --bank)
                if (!smmu.read(contextBase + (bank - 1) * page)) break;
            if (!bank) return -28;
            --bank;
        }
        if (!cache.allocate(0xffc000) || !scratch.allocate(0x13000) || !tables.allocate(32 * 4096)) return -12;
        static const unsigned outputBits[] = {32, 36, 40, 42, 44, 48};
        unsigned output = (id2 >> 4) & 15;
        if (output >= 6) return -22;
        ArmSmmu4KConfig config = {32, outputBits[output], tables.physical, 8192};
        const ArmSmmu4KRange ranges[] = {
            {0x4000, cache.physical, 0xffc000, 15, 0, 2},
            {0x60000000, scratch.physical, 0xc000, 3, 0, 2},
            {0x60400000, scratch.physical + 0xc000, 0x7000, 3, 0, 2}
        };
        ArmSmmu4KResult built = {};
        auto result = ArmSmmu4KBuild(&config, ranges, 3,
            reinterpret_cast<uint64_t *>(tables.cpu), 32, &built);
        IOLog("MarbleGPU: GMU tables status=%u pages=%lu root=%llx bank=%u slot=%u\n",
            result, built.table_pages, built.root_physical, bank, slot);
        if (result) return -22;
        smmuMmio = {reinterpret_cast<volatile unsigned char *>(smmu.words), 0x40000};
        if (ArmSmmu4KBindMmio(&smmuMmio, &smmuIo)) return -22;
        ArmSmmu4KContextConfig context = {&config, nullptr, &built, nullptr,
            id0, id2, bank + 1, 0, 0, 0};
        __asm__ volatile("dsb sy" ::: "memory");
        result = ArmSmmu4KProgramDisabledContext(&smmuIo, bank, &context, 10000, &programmed);
        IOLog("MarbleGPU: GMU SMMU status=%u phase=%u context=%llx SCTLR=%x TTBR=%llx\n",
            result, programmed.phase, programmed.context_offset, programmed.readback.sctlr, programmed.readback.ttbr0);
        if (result) return -5;
        // The live DT masks SID bit 10 for privileged and unprivileged GMU accesses.
        smmu.write(0xc00 + slot * 4, bank);
        smmu.write(0x800 + slot * 4, 0x84000005);
        IOLog("MarbleGPU: GMU route SMR=%x S2CR=%x\n", smmu.read(0x800 + slot * 4), smmu.read(0xc00 + slot * 4));
        return 0;
    }
    int loadFirmware() {
        const AdrenoFirmwareRegion regions[] = {{0, 0x4000, 1}, {0x4000, 0xffc000, 2}, {0x10004000, 0x4000, 3}};
        AdrenoFirmwareBlock blocks[32];
        size_t count = 0;
        auto plan = AdrenoFirmwarePlanGmu(marble_gmu_firmware, marble_gmu_firmware_size,
            regions, 3, blocks, 32, &count);
        if (plan) return -22;
        for (size_t i = 0; i < count; ++i) {
            const auto &block = blocks[i];
            for (unsigned at = 0; at < block.bytes; at += 4) {
                uint32_t value;
                memcpy(&value, marble_gmu_firmware + block.payload_offset + at, 4);
                if (block.region_identifier == 2)
                    *reinterpret_cast<volatile uint32_t *>(cache.cpu + block.address - 0x4000 + at) = value;
                else write((block.region_identifier == 1 ? 0x1b400 : 0x1c400) +
                    ((block.address - (block.region_identifier == 1 ? 0 : 0x10004000)) + at) / 4, value);
            }
        }
        __asm__ volatile("dsb sy" ::: "memory");
        for (size_t i = 0; i < count; ++i) {
            const auto &block = blocks[i];
            for (unsigned at = 0; at < block.bytes; at += 4) {
                uint32_t expected;
                memcpy(&expected, marble_gmu_firmware + block.payload_offset + at, 4);
                uint32_t observed = block.region_identifier == 2 ?
                    *reinterpret_cast<volatile uint32_t *>(cache.cpu + block.address - 0x4000 + at) :
                    read((block.region_identifier == 1 ? 0x1b400 : 0x1c400) +
                        ((block.address - (block.region_identifier == 1 ? 0 : 0x10004000)) + at) / 4);
                if (observed != expected) return -5;
            }
        }
        IOLog("MarbleGPU: GMU firmware blocks=%lu bytes=%llu version=%x readback=passed\n",
            count, marble_gmu_firmware_size, read(0x1d3f8));
        return 0;
    }
    int start() {
        if (!core.map(0x3d68000, 0x37000) || !rscc.map(0x3d50000, 0x10000) ||
            !pdc.map(0xb290000, 0x10000) || !gpu.map(0x3d00000, 0x40000)) return -12;
        if (read(0x1f800) != 1) return -16;
        int result = mapMemory();
        if (result) return result;
        const uint32_t setup[][2] = {
            {0x4, 1U << 24}, {0xb, 1}, {0xe, 0}, {0xd, 0},
            {0x10, 0}, {0xf, 0}, {0x12, 0x80000000}, {0x11, 0},
            {0x100, 0}, {0x8, 0x4520}, {0x9, 0x4510}, {0xa, 0x4514}
        };
        for (const auto &entry:setup) rscc.write(entry[0] * 4, entry[1]);
        const uint32_t sequence[] = {0xeaaae5a0, 0xe1a1ebab, 0xa2e0a581, 0xecac82e2, 0x0020edad};
        for (unsigned i = 0; i < 5; ++i) rscc.write((0x180 + i) * 4, sequence[i]);
        pdc.write(0x1148 * 4, 0);
        pdc.write(0x1140 * 4, 0x80000001);
        result = loadFirmware();
        if (result) return result;
        if (AdrenoHfiInitializeStopped(&hfi, reinterpret_cast<volatile uint32_t *>(scratch.cpu + 0x8000),
            0x4000, 0x60008000, this, notify)) return -22;
        write(0x1f8f1, 1);
        write(0x1f8f0, 1);
        write(0x1f81c, 0);
        write(0x1f801, 2);
        write(0x1f985, 0x60008000);
        write(0x1f984, 1);
        write(0x23b11, 0x80c808a0);
        write(0x1f82d, 0x4052);
        for (unsigned i = 0; i < 4; ++i) gpu.write((0x3c03 + i) * 4, 0x71620);
        gpu.write(0x3c45 * 4, 0);
        write(0x1f9ce, 0);
        write(0x1f9cf, 0x07030200);
        write(0x1f9cd, 0x60004003);
        write(0x1f40f, 1);
        write(0x1f400, 1);
        write(0x1f401, 1);
        write(0x1f800, 0);
        unsigned elapsed = 0;
        for (; elapsed < 100 && (read(0x1f81c) & 0x1ff) != 0x100; ++elapsed) IOSleep(1);
        IOLog("MarbleGPU: GMU init=%x elapsed=%u AO=%x CM3=%x FSR=%x\n", read(0x1f81c),
            elapsed, read(0x23b05), read(0x1f992), smmu.read(programmed.context_offset + 0x58));
        if (elapsed == 100) { write(0x1f800, 1); return -110; }
        write(0x1f986, 1);
        for (elapsed = 0; elapsed < 100 && !(read(0x1f980) & 1); ++elapsed) IOSleep(1);
        IOLog("MarbleGPU: HFI status=%x elapsed=%u\n", read(0x1f980), elapsed);
        if (elapsed == 100) { write(0x1f800, 1); return -110; }
        return 0;
    }
};
}
extern "C" int native_marble_gmu_load(void) {
    // Context tables and firmware buffers remain mapped while the GMU owns them.
    static Gmu *device;
    if (device) return -16;
    device = new Gmu;
    return device ? device->start() : -12;
}
