// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// Android's cont_splash_region and the marble UEFI memory map reserve this
// entire range. Buffer zero remains the firmware's console and fallback.
static constexpr uint64_t MarbleSplashBase = 0xb8000000ULL;
static constexpr uint64_t MarbleSplashBytes = 0x02b00000ULL;
static constexpr uint32_t MarbleScanoutSlots = 3;

struct MarbleScanoutLayout {
    uint64_t slotBytes = 0;
    uint64_t address = 0;
    uint64_t bytes = 0;
};
inline bool marbleScanoutLayout(uint64_t physical, uint64_t width, uint64_t height,
                                uint64_t stride, MarbleScanoutLayout *out) {
    if (!out || physical != MarbleSplashBase || !width || !height ||
        width > 16384 || height > 16384 || stride < width * 4 ||
        stride > 65535 || height > MarbleSplashBytes / stride) return false;
    const uint64_t span = (stride * height + 4095) & ~UINT64_C(4095);
    if (!span || span > MarbleSplashBytes / (MarbleScanoutSlots + 1)) return false;
    *out = {span, physical + span, span * MarbleScanoutSlots};
    return true;
}

// A slot remains unavailable from GPU handoff until the display acknowledges
// its latch. An unsuccessful latch retains both the old and pending slots.
struct MarbleScanoutOwnership {
    uint32_t shown = MarbleScanoutSlots;
    uint32_t pending = MarbleScanoutSlots;
    bool begin(uint32_t slot) {
        if (slot >= MarbleScanoutSlots || slot == shown || pending < MarbleScanoutSlots) return false;
        pending = slot;
        return true;
    }
    bool latched(uint32_t slot) {
        if (pending != slot || slot >= MarbleScanoutSlots) return false;
        shown = slot;
        pending = MarbleScanoutSlots;
        return true;
    }
    void restored() { shown = pending = MarbleScanoutSlots; }
};

inline bool marbleScanoutPipeValid(unsigned pipe, const uint32_t *registers,
                                   uint64_t width, uint64_t height, uint64_t stride) {
    if (!registers || pipe >= 8 || (registers[3] & 0xffff) != stride ||
        (registers[5] & 1) || registers[7]) return false;
    const uint32_t size = registers[0], xy = registers[1];
    return (size & 0xffff) && (size >> 16) &&
        (xy & 0xffff) + (size & 0xffff) <= width &&
        (xy >> 16) + (size >> 16) <= height;
}

inline uint32_t marbleScanoutFlushBit(unsigned pipe) {
    // Qualcomm Gen7 SSPP-to-CTL flush mapping, including VIG3 and DMA pipes.
    constexpr unsigned bits[] = {0,1,2,18,11,12,24,25};
    return pipe < 8 ? 1U << bits[pipe] : 0;
}
