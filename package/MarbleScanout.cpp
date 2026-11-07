// SPDX-License-Identifier: BSD-2-Clause-Patent
// Marble keeps firmware timing/format and changes the completed frame address.
// CTL flush and SSPP offsets follow sheng's latched-display path and the
// SM7475 live device tree; no panel, PHY or display SMMU configuration is written.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "MarbleScanout.h"

extern "C" {
// setup, error, pipe mask, CTL, presented, latch timeouts, longest latch us,
// shown slot, pending slot, SMMU route, DPU version, boot address/stride.
volatile uint64_t marble_scanout_state[17];
uint32_t marble_scanout_registers[8][8];
}
namespace {
constexpr uint32_t kPipe[] = {0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000};
constexpr uint32_t kRead[] = {0x00, 0x08, 0x14, 0x24, 0x30, 0x38, 0x70, 0x170};
IOMemoryMap *mdpMap;
volatile uint32_t *mdp;
MarbleScanoutLayout layout;
MarbleScanoutOwnership ownership;
uint32_t pipeMask, flushMask, ctl, pipeOffset[8];
bool ready, failed;
IOLock *scanoutLock;

IOMemoryMap *map(uint64_t address, uint64_t bytes) {
    IODeviceMemory *memory = IODeviceMemory::withRange(address, bytes);
    IOMemoryMap *mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    return mapping;
}
uint32_t read(uint32_t offset) { return mdp[offset / 4]; }
void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }

// The nonsecure display stream includes SID bits 1 and 10 (DT mask 0x402).
// Rotation inside the reserved physical splash range needs the inherited
// bypass route, or an inherited context with stage-1 translation disabled.
bool physicalDisplayRoute() {
    IOMemoryMap *mapping = map(0x15000000, 0xd0000);
    if (!mapping) return false;
    auto *smmu = reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress());
    const unsigned groups = smmu[0x20 / 4] & 255U;
    const unsigned id1 = smmu[0x24 / 4];
    const unsigned pages = 1U << (((id1 >> 28) & 7) + 1);
    bool good = groups && groups <= 128 && !(id1 & (1U << 31));
    static constexpr uint32_t streams[] = {0x2800U, 0x2802U, 0x2c00U, 0x2c02U};
    for (uint32_t sid : streams) {
        unsigned matches = 0;
        bool bypass = !(smmu[0] & (1U << 10));
        for (unsigned i = 0; good && i < groups; ++i) {
            const uint32_t smr = smmu[(0x800 + i * 4) / 4];
            const uint32_t mask = (smr >> 16) & 0x7fff;
            if (!(smr & (1U << 31)) || ((smr ^ sid) & ~mask & 0x7fff)) continue;
            ++matches;
            const uint32_t route = smmu[(0xc00 + i * 4) / 4];
            const uint32_t type = (route >> 16) & 3;
            bypass = type == 1;
            if (type == 0) {
                const unsigned offset = (pages + (route & 255U)) * 4096;
                bypass = offset < 0xd0000 && !(smmu[offset / 4] & 1);
            }
            marble_scanout_state[9] = uint64_t(smr) << 32 | route;
        }
        good = good && matches <= 1 && bypass;
    }
    mapping->release();
    return good;
}
bool configure() {
    if (ready) return true;
    if (failed) return false;
    failed = true;
    unsigned enabled = 0;
    PE_parse_boot_argn("marble-display-flip", &enabled, sizeof(enabled));
    if (enabled != 1) return false;
    PE_Video video = {};
    marble_scanout_state[0] = 1;
    if (PE_current_console(&video) || video.v_depth != 32 || video.v_rotate ||
        !marbleScanoutLayout(video.v_baseAddr, video.v_width, video.v_height, video.v_rowBytes, &layout)) {
        marble_scanout_state[1] = 1;
        return false;
    }
    mdpMap = map(0xae00000, 0x84000);
    if (!mdpMap) { marble_scanout_state[1] = 2; return false; }
    mdp = reinterpret_cast<volatile uint32_t *>(mdpMap->getVirtualAddress());
    marble_scanout_state[10] = read(0x1000);
    marble_scanout_state[11] = video.v_baseAddr;
    marble_scanout_state[12] = video.v_rowBytes;
    marble_scanout_state[13] = layout.slotBytes;
    // Read all inherited sources before changing a display address. Only
    // linear, single-rectangle paths over the GOP frame are selected. A split
    // pipe may express its crop through SRC_XY or a source-address offset.
    for (unsigned i = 0; i < 8; ++i) {
        for (unsigned j = 0; j < 8; ++j) marble_scanout_registers[i][j] = read(kPipe[i] + kRead[j]);
        const uint64_t source = marble_scanout_registers[i][2];
        if (source < video.v_baseAddr || source >= video.v_baseAddr + video.v_rowBytes * video.v_height) continue;
        if (!marbleScanoutPipeValid(i, marble_scanout_registers[i],
                                  video.v_width, video.v_height, video.v_rowBytes)) {
            marble_scanout_state[1] = 3;
            return false;
        }
        const uint32_t size = marble_scanout_registers[i][0], xy = marble_scanout_registers[i][1];
        const uint64_t offset = source - video.v_baseAddr;
        const uint64_t end = offset + video.v_rowBytes * ((xy >> 16) + (size >> 16) - 1) +
            uint64_t((xy & 0xffff) + (size & 0xffff)) * 4;
        if (end > video.v_rowBytes * video.v_height) { marble_scanout_state[1] = 3; return false; }
        pipeOffset[i] = uint32_t(offset);
        pipeMask |= 1U << i;
        flushMask |= marbleScanoutFlushBit(i);
    }
    marble_scanout_state[2] = pipeMask;
    marble_scanout_state[16] = flushMask;
    if (!pipeMask) { marble_scanout_state[1] = 4; return false; }
    // DT marks CTL0 as primary. Firmware video timing must be active and
    // there must be no previous address update waiting for its latch.
    ctl = 0x16000;
    marble_scanout_state[3] = ctl;
    marble_scanout_state[14] = read(ctl + 0x14);
    marble_scanout_state[15] = read(ctl + 0xf4);
    if ((read(ctl + 0x14) & (1U << 17)) || (read(ctl + 0x18) & flushMask) ||
        !(read(ctl + 0xf4) & 0x6) || !physicalDisplayRoute()) {
        marble_scanout_state[1] = 5;
        return false;
    }
    scanoutLock = IOLockAlloc();
    if (!scanoutLock) { marble_scanout_state[1] = 6; return false; }
    ready = true;
    failed = false;
    marble_scanout_state[0] = 2;
    IOLog("MarbleScanout: buffers=%u span=%llu pipes=%x ctl=%x\n",
          MarbleScanoutSlots, layout.slotBytes, pipeMask, ctl);
    return true;
}
IOReturn flip(uint32_t address) {
    if (read(ctl + 0x18) & flushMask) return kIOReturnBusy;
    for (unsigned i = 0; i < 8; ++i) if (pipeMask & (1U << i)) mdp[(kPipe[i] + 0x14) / 4] = address + pipeOffset[i];
    barrier();
    mdp[(ctl + 0x18) / 4] = flushMask;
    barrier();
    const uint64_t began = mach_absolute_time();
    uint64_t ns = 0;
    do {
        if (!(read(ctl + 0x18) & flushMask)) {
            absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
            if (ns / 1000 > marble_scanout_state[6]) marble_scanout_state[6] = ns / 1000;
            return kIOReturnSuccess;
        }
        IOSleep(1);
        absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    } while (ns < 50000000);
    ++marble_scanout_state[5];
    return kIOReturnTimeout;
}
}
extern "C" IOMemoryDescriptor *native_gop_scanout_ring(uint32_t *slots, uint64_t *slotBytes) {
    if (!slots || !slotBytes || !configure()) return nullptr;
    auto *memory = IODeviceMemory::withRange(layout.address, layout.bytes);
    if (!memory) return nullptr;
    *slots = MarbleScanoutSlots;
    *slotBytes = layout.slotBytes;
    return memory;
}
extern "C" IOReturn native_gop_scanout_present(uint32_t slot) {
    if (!ready) return kIOReturnNotReady;
    IOLockLock(scanoutLock);
    if (failed || !ownership.begin(slot)) { IOLockUnlock(scanoutLock); return kIOReturnNotReady; }
    marble_scanout_state[8] = ownership.pending;
    const IOReturn status = flip(uint32_t(layout.address + slot * layout.slotBytes));
    if (!status) {
        ownership.latched(slot);
        marble_scanout_state[7] = ownership.shown;
        marble_scanout_state[8] = ownership.pending;
        ++marble_scanout_state[4];
    } else {
        failed = true;
        marble_scanout_state[1] = uint32_t(status);
    }
    IOLockUnlock(scanoutLock);
    return status;
}
extern "C" void native_gop_scanout_restore() {
    if (!ready) return;
    IOLockLock(scanoutLock);
    const IOReturn status = flip(uint32_t(MarbleSplashBase));
    if (!status) {
        ownership.restored();
        marble_scanout_state[7] = ownership.shown;
        marble_scanout_state[8] = ownership.pending;
    } else {
        failed = true;
        marble_scanout_state[1] = uint32_t(status);
    }
    IOLockUnlock(scanoutLock);
}
