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
#include "AdrenoHfiVotes.h"

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
    bool allocate(size_t bytes, bool uncached = true) {
        descriptor = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous | (uncached ? kIOMapInhibitCache : 0), bytes, 0xffffff000ULL);
        if (!descriptor || descriptor->prepare() != kIOReturnSuccess) return false;
        mapping = descriptor->map(kIOMapAnywhere | (uncached ? kIOMapInhibitCache : kIOMapCopybackCache));
        if (!mapping) return false;
        cpu = reinterpret_cast<uint8_t *>(mapping->getVirtualAddress());
        addr64_t span = 0;
        physical = descriptor->getPhysicalSegment(0, &span);
        if (span < bytes) return false;
        if (uncached) {
            auto *words = reinterpret_cast<volatile uint64_t *>(cpu);
            for (size_t i = 0; i < bytes / 8; ++i) words[i] = 0;
        } else memset(cpu, 0, bytes);
        return true;
    }
};
struct Gmu {
    Registers core, rscc, pdc, gpu, smmu, database;
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
    struct Resource { const uint8_t *data = nullptr; unsigned bytes = 0; uint32_t address = 0; };
    Resource resource(const char *name) const {
        auto *db = reinterpret_cast<const uint8_t *>(database.mapping->getVirtualAddress());
        auto u16 = [](const uint8_t *p) { return unsigned(p[0]) | unsigned(p[1]) << 8; };
        auto u32 = [](const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
        if (u32(db + 4) != 0x0c0330db) return {};
        for (unsigned group = 0; group < 8; ++group) {
            const uint8_t *header = db + 8 + group * 16;
            unsigned entries = 144 + u16(header + 2), aux = 144 + u16(header + 4), count = u16(header + 6);
            if (entries + count * 24 > 0x20000) return {};
            for (unsigned index = 0; index < count; ++index) {
                const uint8_t *entry = db + entries + index * 24;
                if (strncmp(reinterpret_cast<const char *>(entry), name, 8)) continue;
                unsigned bytes = u16(entry + 20), offset = aux + u16(entry + 22);
                if (offset + bytes > 0x20000) return {};
                return {db + offset, bytes, u32(entry + 16)};
            }
        }
        return {};
    }
    int exchange(unsigned command, const uint32_t *payload, size_t words) {
        uint32_t header = 0, error = 0, reply[16] = {};
        size_t replyWords = 0;
        auto status = AdrenoHfiSend(&hfi, command, payload, words, &header);
        if (status) return -5;
        unsigned elapsed = 0;
        for (; elapsed < 100; ++elapsed) {
            status = AdrenoHfiPoll(&hfi, reply, 16, &replyWords, &error);
            if (status != AdrenoHfiEmpty) break;
            IOSleep(1);
        }
        IOLog("MarbleGPU: HFI command=%u header=%x status=%u error=%x reply_words=%lu elapsed=%u skipped=%x/%x/%x\n",
            command, header, status, error, replyWords, elapsed, hfi.skipped_header, hfi.skipped_ret, hfi.skipped_code);
        return status ? -5 : 0;
    }
    int performance() {
        if (!database.map(0x80860000, 0x20000)) return -12;
        Resource gfx = resource("gfx.lvl"), mx = resource("mx.lvl"), cx = resource("cx.lvl");
        IOLog("MarbleGPU: ARC gfx=%x/%u mx=%x/%u cx=%x/%u\n",
            gfx.address, gfx.bytes, mx.address, mx.bytes, cx.address, cx.bytes);
        const uint32_t gpuHz[] = {0, 220000000, 285000000, 324000000, 364000000, 439000000, 515000000, 580000000};
        const uint32_t gpuLevels[] = {0, 0x30, 0x38, 0x40, 0x50, 0x60, 0x80, 0x90};
        const uint32_t dependentCx[] = {0, 0x40, 0x40, 0x40, 0x80, 0x80, 0x80, 0xc0};
        const uint32_t gmuHz[] = {0, 200000000, 500000000}, gmuLevels[] = {0, 0x40, 0x80};
        uint32_t gpuVotes[8], gmuVotes[3];
        if (AdrenoHfiArcVotes(gpuLevels, 8, gfx.data, gfx.bytes, mx.data, mx.bytes, gpuVotes, 8) ||
            AdrenoHfiArcVotes(gmuLevels, 3, cx.data, cx.bytes, mx.data, mx.bytes, gmuVotes, 3)) return -22;
        uint32_t payload[58] = {8, 3};
        for (unsigned i = 0; i < 8; ++i) {
            payload[2 + i * 3] = gpuVotes[i];
            unsigned index = 0;
            for (; index < cx.bytes / 2; ++index) {
                unsigned level = unsigned(cx.data[index * 2]) | unsigned(cx.data[index * 2 + 1]) << 8;
                if (level >= dependentCx[i]) break;
            }
            if (index == cx.bytes / 2) return -22;
            payload[3 + i * 3] = index;
            payload[4 + i * 3] = gpuHz[i] / 1000;
        }
        for (unsigned i = 0; i < 3; ++i) {
            payload[50 + i * 2] = gmuVotes[i];
            payload[51 + i * 2] = gmuHz[i] / 1000;
        }
        int result = exchange(4, payload, 58);
        if (result) return result;
        return bandwidth();
    }
    int bandwidth() {
        Resource resources[] = {resource("SH0"), resource("MC0"), resource("ACV"), resource("CN0")};
        AdrenoHfiBcm buses[4] = {};
        const unsigned widths[] = {16, 4, 0, 4};
        for (unsigned i = 0; i < 4; ++i) {
            buses[i].address = resources[i].address;
            buses[i].buswidth = widths[i];
            buses[i].fixed = i == 2;
            if (!buses[i].address || AdrenoHfiBcmAux(&buses[i], resources[i].data, resources[i].bytes)) return -22;
            IOLog("MarbleGPU: BCM=%u address=%x unit=%u width=%u vcd=%u\n", i,
                buses[i].address, buses[i].unit, buses[i].width, buses[i].vcd);
        }
        const uint32_t rates[] = {0, 0xbebc2, 0x1ae1b6, 0x209a8e, 0x28973c, 0x2dc6c0,
            0x5caf6a, 0x65ce03, 0x7cb163, 0xa3140c, 0xbe7f17, 0xc7acf1};
        uint32_t payload[159] = {12, 1, 3, 1, 0};
        auto vote = [](const AdrenoHfiBcm &bus, uint32_t rate, bool commit) {
            uint64_t peak = bus.fixed ? (rate ? 8 : 0) :
                uint64_t(rate) * bus.width / bus.buswidth * 1000 / bus.unit;
            if (rate && !peak) peak = 1;
            if (peak > 0x3fff) peak = 0x3fff;
            return (commit ? 1U << 30 : 0) | (rate ? 1U << 29 : 0) | uint32_t(peak);
        };
        payload[5] = buses[3].address;
        payload[11] = vote(buses[3], 0, true);
        payload[17] = vote(buses[3], 100, true);
        for (unsigned j = 0; j < 3; ++j) {
            payload[23 + j] = buses[j].address;
            bool commit = j == 2 || buses[j].vcd != buses[j + 1].vcd;
            if (commit) payload[4] |= 1U << j;
            for (unsigned i = 0; i < 12; ++i) payload[31 + i * 8 + j] = vote(buses[j], rates[i], commit);
        }
        int result = exchange(3, payload, 159);
        if (result) return result;
        const uint32_t handle = 0;
        result = exchange(14, &handle, 1);
        if (result) return result;
        return powerGpu();
    }
    int powerGpu() {
        // KGSL gen7: blocking DCVS vote, OOB GPU request, then HFI START.
        const uint32_t vote[] = {1, 2, 2}; // 285 MHz and DDR table level 2.
        int result = exchange(30, vote, 3);
        if (result) return result;
        write(0x1f994, 1U << 30);
        unsigned elapsed = 0;
        for (; elapsed < 100 && !(read(0x1f992) & (1U << 31)); ++elapsed) IOSleep(1);
        uint32_t response = read(0x1f992), power = read(0x1f8d0), state = read(0x1f8ec);
        IOLog("MarbleGPU: GX OOB response=%x power=%x state=%x elapsed=%u\n",
            response, power, state, elapsed);
        write(0x1f991, 1U << 31);
        if (elapsed == 100 || (power & 3)) {
            write(0x1f994, 1U << 31);
            return -110;
        }
        result = exchange(10, nullptr, 0);
        if (!result) IOLog("MarbleGPU: GX RBBM status=%x secure=%x\n",
            gpu.read(0x210 * 4), gpu.read(0xf400 * 4));
        // Hold the GPU OOB vote while the command processor is being brought up.
        return result;
    }
    int mapMemory() {
        if (!smmu.map(0x3da0000, 0x40000)) return -12;
        uint32_t id0 = smmu.read(0x20), id1 = smmu.read(0x24), id2 = smmu.read(0x28);
        unsigned banks = id1 & 255, slots = id0 & 255;
        unsigned page = (id1 & (1U << 31)) ? 0x10000 : 0x1000;
        unsigned contextBase = page << (((id1 >> 28) & 7) + 1);
        IOLog("MarbleGPU: SMMU banks=%u slots=%u context_base=%x\n", banks, slots, contextBase);
        // Reuse a route for the stopped GMU, or select an unused hardware slot.
        bool found = false;
        for (unsigned i = 0; i < slots; ++i) {
            uint32_t match = smmu.read(0x800 + i * 4);
            if (match & (1U << 31)) IOLog("MarbleGPU: inherited route=%u SMR=%x S2CR=%x\n",
                i, match, smmu.read(0xc00 + i * 4));
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
            if (slot == slots) { IOLog("MarbleGPU: SMMU stream slots occupied\n"); return -28; }
            for (bank = banks; bank > 0; --bank) {
                if (smmu.read(contextBase + (bank - 1) * page) & 1) continue;
                bool routed = false;
                for (unsigned i = 0; i < slots; ++i) {
                    uint32_t match = smmu.read(0x800 + i * 4), route = smmu.read(0xc00 + i * 4);
                    if ((match & (1U << 31)) && !(route & (3U << 16)) && (route & 255) == bank - 1) routed = true;
                }
                if (!routed) break;
            }
            if (!bank) { IOLog("MarbleGPU: SMMU context banks occupied\n"); return -28; }
            --bank;
        }
        if (!cache.allocate(0xffc000) || !scratch.allocate(0x13000) || !tables.allocate(32 * 4096, false)) return -12;
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
        uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        size_t line = 4U << ((ctr >> 16) & 15);
        for (uintptr_t at = reinterpret_cast<uintptr_t>(tables.cpu);
             at < reinterpret_cast<uintptr_t>(tables.cpu) + built.table_pages * 4096; at += line)
            __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
        __asm__ volatile("dsb sy" ::: "memory");
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
        write(0x1f800, 1);
        IODelay(100);
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
        return performance();
    }
};
Gmu *device;
}
extern "C" void native_marble_gmu_stop(void) {
    if (device && device->core.words) {
        device->write(0x1f800, 1);
        IODelay(100);
    }
}
extern "C" int native_marble_gmu_load(void) {
    // Context tables and firmware buffers remain mapped while the GMU owns them.
    if (device) return -16;
    device = new Gmu;
    return device ? device->start() : -12;
}
