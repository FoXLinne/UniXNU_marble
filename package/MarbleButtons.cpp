// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board wiring: marble live DTS. SPMI/PDC sequencing follows UniXNU-sheng
// ButtonProbe.cpp (460179565a018c741b93e0b1efa07c55d4de497a).
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

extern "C" IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
extern "C" {
volatile uint32_t marble_buttons_stats[64];
void native_marble_buttons_start(void);
void native_marble_buttons_stop(void);
void native_marble_buttons_restore(void);
void native_buttons_set_sink(void (*)(void *, uint32_t), void *);
}

namespace {
constexpr uint32_t intid = 513, pin = 1;
constexpr uint8_t rt = 0x10, type = 0x11, high = 0x12, low = 0x13;
constexpr uint8_t latch = 0x14, enable = 0x15, disable = 0x16;
struct Window {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
    bool open(uint64_t address, uint64_t bytes) {
        auto *memory = IODeviceMemory::withRange(address, bytes);
        map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        va = map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
        return va != nullptr;
    }
};
struct Peripheral {
    unsigned apid = 0, bits = 0;
    Window writer;
    uint8_t saved[4] = {};
    uint32_t access = 0;
    bool captured = false, modified = false;
};
Window core, config, observer, pending, pdc, gic;
Peripheral pon, gpio;
uint32_t apids, observerStride, channelStride, mapOffset, ownerOffset;
uint32_t savedPdc, savedPdcBank, savedGroup, savedEnable, savedConfig;
uint8_t savedPriority;
uint64_t savedRoute, frequency;
bool configured, attached, gicSaved, pdcSaved;
volatile uint32_t events, stopping, running, restored;
IOLock *sinkLock;
void (*sink)(void *, uint32_t);
OSObject *sinkContext;

void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
void fail(unsigned stage, int error) {
    marble_buttons_stats[0] = uint32_t(-int(stage));
    marble_buttons_stats[14] = marble_buttons_stats[14] + 1;
    marble_buttons_stats[15] = uint32_t(error);
    IOLog("MarbleButtons: stage=%d error=%x\n", -int(stage), error);
}
uint64_t milliseconds() {
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value / frequency * 1000 + value % frequency * 1000 / frequency;
}
int transfer(volatile uint32_t *channel, uint32_t command) {
    barrier(); channel[0] = command; barrier();
    for (unsigned us = 0; us < 1000; ++us) {
        uint32_t status = channel[2];
        if (status & 1) return (status & 14) ? int(status) : 0;
        IODelay(1);
    }
    return kIOReturnTimeout;
}
int read(Peripheral &peripheral, uint8_t reg, uint8_t &value) {
    auto *channel = observer.va + observerStride * peripheral.apid / 4;
    int result = transfer(channel, (1u << 27) | (uint32_t(reg) << 4));
    if (!result) value = uint8_t(channel[6]);
    return result;
}
int write(Peripheral &peripheral, uint8_t reg, uint8_t value) {
    peripheral.writer.va[4] = value;
    return transfer(peripheral.writer.va, uint32_t(reg) << 4);
}
void maskInterrupt(bool masked) {
    if (!attached || !gic.va) return;
    gic.va[((masked ? 0x180 : 0x100) + 4 * (intid / 32)) / 4] = 1u << (intid % 32);
    barrier();
}
bool find(Peripheral &peripheral, unsigned ppid, unsigned bits) {
    int selected = -1;
    for (unsigned i = 0; i < apids; ++i) {
        uint32_t mapping = core.va[(mapOffset + 4 * i) / 4];
        if (mapping && ((mapping >> 8) & 0xfff) == ppid && (mapping & (1u << 24))) selected = int(i);
    }
    if (selected < 0) return false;
    unsigned owner = config.va[(ownerOffset + 4 * unsigned(selected)) / 4] & 7;
    unsigned index = ppid == 0x13 ? 4 : 5;
    marble_buttons_stats[index] = unsigned(selected);
    marble_buttons_stats[index + 2] = owner;
    if (owner) return false;
    peripheral.apid = unsigned(selected); peripheral.bits = bits;
    return peripheral.writer.open(0xc500000ULL + channelStride * peripheral.apid, 0x1000);
}
int capture(Peripheral &peripheral) {
    const uint8_t regs[] = {type, high, low, enable};
    for (unsigned i = 0; i < 4; ++i) {
        int result = read(peripheral, regs[i], peripheral.saved[i]);
        if (result) return result;
    }
    peripheral.access = peripheral.writer.va[0x100 / 4];
    peripheral.captured = true;
    return 0;
}
int arm(Peripheral &peripheral) {
    peripheral.modified = true;
    int result = write(peripheral, disable, peripheral.bits);
    for (unsigned i = 0; !result && i < 3; ++i)
        result = write(peripheral, uint8_t(type + i), peripheral.saved[i] | peripheral.bits);
    if (!result) result = write(peripheral, latch, peripheral.bits);
    if (!result) result = write(peripheral, enable, peripheral.bits);
    if (!result) { peripheral.writer.va[0x100 / 4] = peripheral.access | 1; barrier(); }
    return result;
}
void restorePeripheral(Peripheral &peripheral) {
    if (!peripheral.captured || !peripheral.modified) return;
    int result = write(peripheral, disable, peripheral.bits);
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t value = 0;
        int status = read(peripheral, uint8_t(type + i), value);
        if (!status) status = write(peripheral, uint8_t(type + i),
            (value & ~peripheral.bits) | (peripheral.saved[i] & peripheral.bits));
        if (status) result = status;
    }
    if (write(peripheral, latch, peripheral.bits)) result = kIOReturnIOError;
    if (write(peripheral, enable, peripheral.saved[3] & peripheral.bits)) result = kIOReturnIOError;
    uint8_t enabled = 0;
    if (read(peripheral, enable, enabled)) result = kIOReturnIOError;
    // Preserve access for another interrupt bit enabled during this session.
    peripheral.writer.va[0x100 / 4] = peripheral.access | ((enabled & ~peripheral.bits) ? 1u : 0u);
    barrier();
    if (result) marble_buttons_stats[30] = uint32_t(result);
}
void restore() {
    if (__atomic_exchange_n(&restored, 1, __ATOMIC_ACQ_REL)) return;
    maskInterrupt(true);
    restorePeripheral(pon); restorePeripheral(gpio);
    if (pdcSaved) {
        pdc.va[(0x110 + 4 * pin) / 4] = savedPdc;
        if (marble_buttons_stats[2] < 0x30200) {
            auto &bank = pdc.va[(0x10 + 4 * (pin / 32)) / 4];
            bank = (bank & ~(1u << pin)) | (savedPdcBank & (1u << pin));
        }
    }
    if (gicSaved && attached) {
        const unsigned word = intid / 32, bit = 1u << (intid % 32);
        const unsigned cfg = (0xc00 + 4 * (intid / 16)) / 4, shift = (intid % 16) * 2;
        gic.va[cfg] = (gic.va[cfg] & ~(3u << shift)) | (savedConfig & (3u << shift));
        gic.va[(0x80 + 4 * word) / 4] = (gic.va[(0x80 + 4 * word) / 4] & ~bit) | (savedGroup & bit);
        reinterpret_cast<volatile uint8_t *>(gic.va)[0x400 + intid] = savedPriority;
        reinterpret_cast<volatile uint64_t *>(gic.va)[(0x6000 + 8 * intid) / 8] = savedRoute;
        if (savedEnable & bit) maskInterrupt(false);
    }
    barrier(); marble_buttons_stats[25] = 1;
}
void notify(uint32_t mask) {
    IOLockLock(sinkLock);
    auto callback = sink;
    OSObject *context = sinkContext;
    if (context) context->retain();
    IOLockUnlock(sinkLock);
    if (callback && context) { callback(context, mask); marble_buttons_stats[13] = marble_buttons_stats[13] + 1; }
    OSSafeReleaseNULL(context);
}
void interrupt(void *) {
    maskInterrupt(true);
    marble_buttons_stats[11] = marble_buttons_stats[11] + 1;
    __atomic_fetch_add(&events, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&events);
}
bool foreignPending() {
    bool foreign = false;
    for (unsigned word = 0; word * 32 < apids; ++word) {
        uint32_t value = pending.va[word];
        if (pon.apid / 32 == word) value &= ~(1u << (pon.apid % 32));
        if (gpio.apid / 32 == word) value &= ~(1u << (gpio.apid % 32));
        marble_buttons_stats[32 + word] = value;
        if (value) { foreign = true; marble_buttons_stats[23] = word * 32 + __builtin_ctz(value); }
    }
    Peripheral *peripherals[] = {&pon, &gpio};
    for (Peripheral *peripheral : peripherals) {
        uint32_t value = peripheral->writer.va[0x104 / 4] & ~peripheral->bits;
        if (value) { foreign = true; marble_buttons_stats[23] = peripheral->apid; }
    }
    if (foreign) marble_buttons_stats[22] = marble_buttons_stats[22] + 1;
    return foreign;
}
int sample(uint32_t &mask) {
    // Acknowledge before sampling. An edge after the level read retains its
    // latch and wakes the worker when the GIC line is enabled.
    Peripheral *peripherals[] = {&pon, &gpio};
    for (Peripheral *peripheral : peripherals) {
        int result = write(*peripheral, latch, peripheral->bits);
        if (result) return result;
        peripheral->writer.va[0x108 / 4] = peripheral->bits;
    }
    barrier();
    uint8_t p = 0, g = 0;
    int result = read(pon, rt, p);
    if (!result) result = read(gpio, rt, g);
    if (result) return result;
    marble_buttons_stats[8] = p; marble_buttons_stats[9] = g;
    marble_buttons_stats[12] = marble_buttons_stats[12] + 1;
    mask = ((p & 0x80) ? 2u : 0u) | ((p & 0x40) ? 8u : 0u) | ((g & 1) ? 0u : 4u);
    return 0;
}
bool setup() {
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (!frequency || !core.open(0xc400000, 0x3000) || !config.open(0xc42d000, 0x4000) ||
        !observer.open(0xc440000, 0x80000) || !pending.open(0xc4c0000, 0x1000) ||
        !pdc.open(0xb220000, 0x2000) || !gic.open(0x17100000, 0x10000)) { fail(1, kIOReturnNoMemory); return false; }
    uint32_t version = core.va[0]; marble_buttons_stats[1] = version;
    if (version >= 0x70000000 && version < 0x80000000) {
        apids = core.va[1] & 0x7ff; mapOffset = 0x2000; ownerOffset = 0; observerStride = 0x20; channelStride = 0x1000;
    } else if (version >= 0x50000000 && version < 0x70000000) {
        apids = 512; mapOffset = 0x900; ownerOffset = 0x700; observerStride = 0x80; channelStride = 0x10000;
    } else { fail(2, version); return false; }
    marble_buttons_stats[3] = apids;
    if (!apids || apids > 1024 || !find(pon, 0x13, 0xc0) || !find(gpio, 0x18d, 1)) { fail(3, kIOReturnNotPermitted); return false; }
    int result = capture(pon);
    if (!result) result = capture(gpio);
    if (result) { fail(4, result); return false; }
    marble_buttons_stats[28] = pon.saved[3]; marble_buttons_stats[29] = gpio.saved[3];
    for (unsigned attempt = 0;; ++attempt) {
        // armcpu_spi_attach enables the line: keep the PDC input quiescent until attachment.
        savedPdc = pdc.va[(0x110 + 4 * pin) / 4];
        savedPdcBank = pdc.va[(0x10 + 4 * (pin / 32)) / 4];
        marble_buttons_stats[2] = pdc.va[0x1000 / 4];
        pdcSaved = true;
        if (marble_buttons_stats[2] >= 0x30200) pdc.va[(0x110 + 4 * pin) / 4] = savedPdc & ~8u;
        else pdc.va[(0x10 + 4 * (pin / 32)) / 4] = savedPdcBank & ~(1u << pin);
        barrier();
        savedGroup = gic.va[(0x80 + 4 * (intid / 32)) / 4];
        savedEnable = gic.va[(0x100 + 4 * (intid / 32)) / 4];
        savedConfig = gic.va[(0xc00 + 4 * (intid / 16)) / 4];
        savedPriority = reinterpret_cast<volatile uint8_t *>(gic.va)[0x400 + intid];
        savedRoute = reinterpret_cast<volatile uint64_t *>(gic.va)[(0x6000 + 8 * intid) / 8];
        gicSaved = true;
        uint32_t readback = 0;
        result = armcpu_spi_attach(intid, false, interrupt, nullptr, &readback, nullptr);
        marble_buttons_stats[26] = readback;
        if (result != kIOReturnNotReady || attempt == 240) break;
        pdc.va[(0x110 + 4 * pin) / 4] = savedPdc;
        if (marble_buttons_stats[2] < 0x30200) pdc.va[(0x10 + 4 * (pin / 32)) / 4] = savedPdcBank;
        IOSleep(250);
    }
    if (result) { fail(5, result); return false; }
    attached = true; maskInterrupt(true);
    result = arm(pon);
    if (!result) result = arm(gpio);
    if (result) { fail(6, result); return false; }
    pdc.va[(0x110 + 4 * pin) / 4] = (savedPdc & ~0xfu) | 4u | (marble_buttons_stats[2] >= 0x30200 ? 8u : 0u);
    if (marble_buttons_stats[2] < 0x30200) pdc.va[(0x10 + 4 * (pin / 32)) / 4] |= 1u << pin;
    barrier(); configured = true; marble_buttons_stats[0] = 3;
    IOLog("MarbleButtons: SPMI=%x PDC=%x APID=%u/%u owner=%u/%u enabled=%x/%x\n",
        version, marble_buttons_stats[2], pon.apid, gpio.apid, marble_buttons_stats[6], marble_buttons_stats[7], pon.saved[3], gpio.saved[3]);
    return true;
}
void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "marble-buttons");
    if (!setup()) { restore(); __atomic_store_n(&running, 0, __ATOMIC_RELEASE); return; }
    uint32_t candidate = 0, stable = 0;
    uint64_t changed[3] = {};
    bool initial = true;
    const uint32_t bits[] = {2, 4, 8};
    while (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) {
        uint32_t seen = __atomic_load_n(&events, __ATOMIC_ACQUIRE), raw = 0;
        int result = sample(raw);
        if (result) { fail(7, result); break; }
        uint64_t now = milliseconds();
        uint32_t prior = stable;
        unsigned waitMS = 0;
        for (unsigned key = 0; key < 3; ++key) {
            if (initial || ((candidate ^ raw) & bits[key])) {
                changed[key] = now;
                candidate = (candidate & ~bits[key]) | (raw & bits[key]);
            }
            if ((stable ^ candidate) & bits[key]) {
                unsigned debounce = key == 1 ? 32 : 2;
                uint64_t elapsed = now - changed[key];
                if (elapsed >= debounce) stable = (stable & ~bits[key]) | (candidate & bits[key]);
                else if (!waitMS || debounce - elapsed < waitMS) waitMS = unsigned(debounce - elapsed);
            }
        }
        if (initial || stable != prior) {
            for (unsigned key = 0; key < 3; ++key) if ((stable ^ prior) & bits[key]) {
                unsigned slot = ((stable & bits[key]) ? 16 : 19) + key;
                marble_buttons_stats[slot] = marble_buttons_stats[slot] + 1;
            }
            marble_buttons_stats[10] = stable;
            notify(stable);
            IOLog("MarbleButtons: mask=%x raw=%x/%x\n", stable, marble_buttons_stats[8], marble_buttons_stats[9]);
        }
        initial = false;
        // A shared interrupt retains its owner's pending bits. Kernel routing stays masked on a foreign source.
        if (foreignPending()) { fail(9, kIOReturnExclusiveAccess); break; }
        maskInterrupt(false);
        if (waitMS) {
            uint64_t deadline;
            clock_interval_to_deadline(waitMS, kMillisecondScale, &deadline);
            assert_wait_deadline((event_t)&events, THREAD_UNINT, deadline);
        } else assert_wait((event_t)&events, THREAD_UNINT);
        if (__atomic_load_n(&events, __ATOMIC_ACQUIRE) != seen || __atomic_load_n(&stopping, __ATOMIC_ACQUIRE))
            clear_wait(current_thread(), THREAD_AWAKENED);
        else thread_block(THREAD_CONTINUE_NULL);
        maskInterrupt(true);
    }
    restore();
    marble_buttons_stats[31] = 1;
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&running);
}
}

extern "C" void native_buttons_set_sink(void (*callback)(void *, uint32_t), void *context) {
    if (!sinkLock) sinkLock = IOLockAlloc();
    if (!sinkLock) { fail(10, kIOReturnNoMemory); return; }
    auto *next = static_cast<OSObject *>(context);
    if (next) next->retain();
    IOLockLock(sinkLock);
    OSObject *previous = sinkContext;
    sinkContext = next; sink = callback;
    IOLockUnlock(sinkLock);
    OSSafeReleaseNULL(previous);
}
extern "C" void native_marble_buttons_start(void) {
    unsigned enabled = 1;
    PE_parse_boot_argn("marble-buttons", &enabled, sizeof(enabled));
    if (!enabled || running || !sinkLock) return;
    running = 1;
    thread_t thread = nullptr;
    kern_return_t result = kernel_thread_start(worker, nullptr, &thread);
    if (result) { running = 0; fail(11, result); }
    else thread_deallocate(thread);
}
extern "C" void native_marble_buttons_stop(void) {
    if (!running) return;
    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
    maskInterrupt(true);
    thread_wakeup((event_t)&events);
    for (unsigned tries = 0; tries < 20 && __atomic_load_n(&running, __ATOMIC_ACQUIRE); ++tries) IOSleep(10);
}
extern "C" void native_marble_buttons_restore(void) {
    if (!configured && !pdcSaved) return;
    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
    restore();
}
