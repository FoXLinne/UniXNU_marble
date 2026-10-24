// SPDX-License-Identifier: GPL-2.0-only
// Qualcomm MDT/PAS sequence and marble's gpu_microcode_region from the vendor device tree.
#include <IOKit/IOLib.h>
#include <sys/param.h>
#include <IOKit/IODeviceMemory.h>
#include "NativeAdrenoMemory.h"

extern "C" const uint8_t marble_zap_firmware[];
extern "C" const uint64_t marble_zap_firmware_size;
extern "C" int native_marble_zap_load(void);

namespace {
constexpr uint64_t zapPhysical = 0x8b91a000, zapBytes = 0x2000;
NativeAdrenoMemory metadata;
uint16_t read16(const uint8_t *p) { uint16_t value; memcpy(&value, p, sizeof(value)); return value; }
uint32_t read32(const uint8_t *p) { uint32_t value; memcpy(&value, p, sizeof(value)); return value; }
int scm(unsigned command, uint64_t arguments, uint64_t p1, uint64_t p2) {
    const uint64_t function = 0x42000200 | command;
    uint64_t next = function, resumeState = 0;
    unsigned busy = 0;
    for (unsigned resumes = 0; resumes < 10000;) {
        register uint64_t x0 __asm__("x0") = next, x1 __asm__("x1") = arguments;
        register uint64_t x2 __asm__("x2") = 13, x3 __asm__("x3") = p1, x4 __asm__("x4") = p2;
        register uint64_t x5 __asm__("x5") = 0, x6 __asm__("x6") = resumeState, x7 __asm__("x7") = 0;
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7) ::
            "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) { next = 1; resumeState = x6; ++resumes; continue; }
        if (int64_t(x0) == -12 && busy++ < 20) { next = function; resumeState = 0; IOSleep(30); continue; }
        IOLog("MarbleGPU: ZAP PAS command=%u status=%lld result=%lld resumes=%u\n", command, int64_t(x0), int64_t(x1), resumes);
        return int(x0 ? x0 : x1);
    }
    return -110;
}
}
extern "C" int native_marble_zap_load(void) {
    const uint8_t *firmware = marble_zap_firmware;
    const uint64_t size = marble_zap_firmware_size;
    if (size < 52 || memcmp(firmware, "\177ELF\1", 5)) return -22;
    unsigned count = read16(firmware + 44);
    uint64_t headers = read32(firmware + 28);
    if (read16(firmware + 42) != 32 || count < 2 || headers + count * 32ULL > size) return -22;
    auto field = [&](unsigned index, unsigned column) { return read32(firmware + headers + index * 32 + column * 4); };
    if (field(0, 0) == 1) return -22;
    unsigned hash = 0;
    uint64_t low = UINT64_MAX, high = 0;
    bool relocatable = false;
    for (unsigned i = 1; i < count; ++i) {
        if (((field(i, 6) >> 24) & 7) == 2) { hash = i; continue; }
        if (field(i, 0) != 1 || !field(i, 5)) continue;
        low = MIN(low, uint64_t(field(i, 3)));
        high = MAX(high, (uint64_t(field(i, 3)) + field(i, 5) + 4095) & ~4095ULL);
        relocatable |= field(i, 6) & (1U << 27);
    }
    uint64_t headerBytes = field(0, 4), hashBytes = field(hash, 4), hashOffset = field(hash, 1);
    if (!hash || low >= high || high - low > zapBytes || headerBytes > size || hashOffset + hashBytes > size) return -22;
    size_t bytes = (headerBytes + hashBytes + 4095) & ~4095ULL;
    IOReturn status = metadata.allocate(bytes, 40, true, bytes);
    if (status || metadata.spanCount() != 1) return -12;
    auto *destination = static_cast<uint8_t *>(metadata.mapping());
    memcpy(destination, firmware, headerBytes);
    memcpy(destination + headerBytes, firmware + hashOffset, hashBytes);
    if (metadata.publishForDevice(0, bytes, true)) return -5;
    int result = scm(1, 2 | (2U << 6), metadata.spans()[0].physical, 0);
    if (result) return result;
    if (relocatable && (result = scm(2, 3, zapPhysical, high - low))) return result;
    auto *memory = IODeviceMemory::withRange(zapPhysical, zapBytes);
    auto *mapping = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!mapping) return -12;
    auto *region = reinterpret_cast<volatile uint8_t *>(mapping->getVirtualAddress());
    const uint64_t base = relocatable ? low : zapPhysical;
    for (unsigned i = 1; i < count; ++i) {
        if (field(i, 0) != 1 || ((field(i, 6) >> 24) & 7) == 2 || !field(i, 5)) continue;
        uint64_t offset = uint64_t(field(i, 3)) - base, fileBytes = field(i, 4), memoryBytes = field(i, 5), source = field(i, 1);
        if (field(i, 3) < base || offset + memoryBytes > zapBytes || fileBytes > memoryBytes || source + fileBytes > size) {
            mapping->release(); return -22;
        }
        for (uint64_t at = 0; at < memoryBytes; ++at) region[offset + at] = at < fileBytes ? firmware[source + at] : 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    mapping->release();
    return scm(5, 1, 0, 0);
}
