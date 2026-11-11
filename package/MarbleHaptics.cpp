// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board wiring: marble live DTS pm8350b haptics (SID 3, CFG 0xf000, PATTERN
// 0xf100, HBOOST 0xf200) and PMK8350 hap_cfg_sdam (0x9d00).
// Register layout, boost/VREG sequencing and LRA period derivation follow
// MiCode/Xiaomi_Kernel_OpenSource marble-s-oss
// drivers/input/misc/qcom-hv-haptics.c (GPL-2.0-only), snapshot at
// outputs/UniXNU_marble/haptics_research_2026-11-06/sources.
// SPMI transaction shape follows UniXNU-sheng ButtonProbe.cpp through
// package/MarbleButtons.cpp.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "MarbleMicrosecondDelay.h"

extern "C" {
volatile uint32_t marble_haptics_stats[64];
volatile uint32_t marble_haptics_stage;
volatile uint32_t marble_haptics_play_count;
volatile uint32_t marble_haptics_map_dump[32];
void native_marble_haptics_start(void);
void native_marble_haptics_stop(void);
void native_marble_haptics_restore(void);
void native_marble_haptics_runtime(void);
void native_marble_haptics_runtime_stop(void);
int native_marble_haptics_drive(unsigned);
int native_marble_haptics_quiet(void);
}

namespace {
// Peripheral identity deduced from the PMIC register decode: a peripheral
// owns a 256 byte window, so the window base selects the PPID.
constexpr unsigned ppidCfg = 0x3f0, ppidPtn = 0x3f1, ppidBoost = 0x3f2, ppidSdam = 0x09d;

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
    unsigned apid = 0;
    unsigned mapping = 0;
    unsigned owner = 0;
    Window writer;
    bool found = false;
};

Window core, config, observer;
Peripheral cfg, ptn, boost, sdam;
uint32_t apids, observerStride, channelStride, mapOffset, ownerOffset;
uint32_t spmiVersion;
uint64_t frequency;
volatile uint32_t running, stopping, restored;
uint32_t lastError;
unsigned mode;
bool driveActive;

// HAP520 board parameters.
constexpr uint32_t vmaxMV = 1450, vmaxStepMV = 50;
constexpr uint32_t tlraUS = 5882, tlraStepUS = 5;
constexpr uint8_t brakeMode = 1, brakeSineGain = 0;
constexpr uint8_t directAmplitude = 0xff, playMilliseconds = 100;

void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
void note(unsigned slot, uint32_t value) { marble_haptics_stats[slot] = value; }
void fail(unsigned stage, int error) {
    marble_haptics_stage = stage;
    lastError = uint32_t(error);
    marble_haptics_stats[8] = uint32_t(-int(stage));
    marble_haptics_stats[47] = uint32_t(error);
    IOLog("MarbleHaptics: stage=%d error=%x\n", -int(stage), error);
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
        marbleMicrosecondDelay(1);
    }
    return kIOReturnTimeout;
}
volatile uint32_t *readerFor(const Peripheral &peripheral) {
    return observer.va + observerStride * peripheral.apid / 4;
}
int readByte(const Peripheral &peripheral, uint8_t reg, uint8_t &value) {
    if (!peripheral.found) return kIOReturnNotAttached;
    auto *channel = readerFor(peripheral);
    int result = transfer(channel, (1u << 27) | (uint32_t(reg) << 4));
    if (!result) value = uint8_t(channel[6]);
    return result;
}
int writeByte(const Peripheral &peripheral, uint8_t reg, uint8_t value) {
    if (!peripheral.found || !peripheral.writer.va) return kIOReturnNotPermitted;
    peripheral.writer.va[4] = value;
    return transfer(peripheral.writer.va, uint32_t(reg) << 4);
}
int readPair(const Peripheral &peripheral, uint8_t reg, uint8_t *values) {
    int result = readByte(peripheral, reg, values[0]);
    if (!result) result = readByte(peripheral, uint8_t(reg + 1), values[1]);
    return result;
}
bool locate(Peripheral &peripheral, unsigned ppid) {
    int selected = -1;
    for (unsigned i = 0; i < apids; ++i) {
        uint32_t mapping = core.va[(mapOffset + 4 * i) / 4];
        if ((mapping & (1u << 24)) && ((mapping >> 8) & 0xfff) == ppid) selected = int(i);
    }
    if (selected < 0) return false;
    const unsigned index = unsigned(selected);
    peripheral.mapping = core.va[(mapOffset + 4 * index) / 4];
    peripheral.owner = config.va[(ownerOffset + 4 * index) / 4] & 7;
    peripheral.apid = index;
    // Only AP-owned channels have an accessible write window. Other owners
    // remain observable, but must never be addressed through an AP writer.
    peripheral.found = peripheral.owner != 0 ||
        peripheral.writer.open(0xc500000ULL + channelStride * index, 0x1000);
    return peripheral.found;
}
void report(const Peripheral &peripheral, const char *name, unsigned slot) {
    if (!peripheral.found) {
        IOLog("MarbleHaptics: %s missing\n", name);
        return;
    }
    note(slot, peripheral.apid);
    note(slot + 4, peripheral.mapping);
    note(slot + 8, peripheral.owner);
    IOLog("MarbleHaptics: %s apid=%u mapping=%x owner=%u\n",
        name, peripheral.apid, peripheral.mapping, peripheral.owner);
}
bool setup() {
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (!frequency || !core.open(0xc400000, 0x3000) || !config.open(0xc42d000, 0x4000) ||
        !observer.open(0xc440000, 0x80000)) { fail(1, kIOReturnNoMemory); return false; }
    spmiVersion = core.va[0];
    note(1, spmiVersion);
    if (spmiVersion >= 0x70000000 && spmiVersion < 0x80000000) {
        apids = core.va[1] & 0x7ff; mapOffset = 0x2000; ownerOffset = 0; observerStride = 0x20; channelStride = 0x1000;
    } else if (spmiVersion >= 0x50000000 && spmiVersion < 0x70000000) {
        apids = 512; mapOffset = 0x900; ownerOffset = 0x700; observerStride = 0x80; channelStride = 0x10000;
    } else { fail(2, spmiVersion); return false; }
    note(2, apids);
    if (!apids || apids > 1024) { fail(3, kIOReturnNotPermitted); return false; }
    // Log the first mapping entries so the PPID/SID layout is visible in the
    // resident log without a second boot.
    IOLog("MarbleHaptics: SPMI=%x apids=%u\n", spmiVersion, apids);
    for (unsigned i = 0; i < apids && i < 32; ++i)
        marble_haptics_map_dump[i] = core.va[(mapOffset + 4 * i) / 4];
    for (unsigned i = 0; i < apids && i < 6; ++i)
        IOLog("MarbleHaptics: map[%u]=%x\n", i, core.va[(mapOffset + 4 * i) / 4]);
    unsigned mask = 0;
    if (locate(cfg, ppidCfg)) mask |= 1;
    if (locate(ptn, ppidPtn)) mask |= 2;
    if (locate(boost, ppidBoost)) mask |= 4;
    if (locate(sdam, ppidSdam)) mask |= 8;
    note(3, mask);
    report(cfg, "cfg", 16);
    report(ptn, "ptn", 17);
    report(boost, "boost", 18);
    report(sdam, "sdam", 19);
    if ((mask & 11) != 11) { fail(4, kIOReturnNotAttached); return false; }
    if (!cfg.writer.va || !ptn.writer.va || !sdam.writer.va) {
        fail(4, kIOReturnNotPermitted); return false;
    }
    uint8_t value = 0, pair[2] = {};
    if (!readByte(cfg, 0x00, value)) note(32, value);
    uint8_t hwConfig = 0;
    if (!readByte(cfg, 0x0d, hwConfig)) note(33, hwConfig);
    if (!readByte(ptn, 0x01, value)) note(34, value);
    if (!readByte(boost, 0x00, value)) note(35, value);
    if (!readByte(boost, 0x01, value)) note(36, value);
    if (!readByte(cfg, 0x0c, value)) note(37, value);
    if (!readByte(cfg, 0x46, value)) note(38, value);
    if (!readByte(cfg, 0x48, value)) note(39, value);
    if (!readPair(cfg, 0x5c, pair)) note(40, (uint32_t(pair[0]) << 8) | pair[1]);
    if (!readPair(sdam, 0x46, pair)) note(41, (uint32_t(pair[0]) << 8) | pair[1]);
    if (!readByte(sdam, 0x42, value)) note(42, value);
    if (!readByte(boost, 0x46, value)) note(43, value);
    if (!readByte(boost, 0x0b, value)) note(44, value);
    IOLog("MarbleHaptics: cfg=%x hw=%x ptn=%x boost=%x/%x fault=%x en=%x vmax=%x tlra=%x sdam=%x/%x vreg=%x st4=%x\n",
        marble_haptics_stats[32], marble_haptics_stats[33], marble_haptics_stats[34],
        marble_haptics_stats[35], marble_haptics_stats[36], marble_haptics_stats[37],
        marble_haptics_stats[38], marble_haptics_stats[39], marble_haptics_stats[40],
        marble_haptics_stats[41], marble_haptics_stats[42], marble_haptics_stats[43],
        marble_haptics_stats[44]);
    return true;
}
int boostVreg(bool on) {
    if (!sdam.found) return kIOReturnNotAttached;
    int result = writeByte(sdam, 0x42, on ? 1 : 2);
    if (result) return result;
    return writeByte(sdam, 0xe5, 1);
}
bool waitBoostReady(uint32_t *status) {
    constexpr unsigned tries = 100;
    for (unsigned i = 0; i < tries; ++i) {
        uint8_t vreg = 0, st4 = 0, follow = 0;
        if (readByte(boost, 0x46, vreg) || readByte(boost, 0x0b, st4) ||
            readByte(boost, 0x41, follow)) return false;
        *status = (uint32_t(vreg) << 8) | st4;
        if (((vreg & 0x80) && !(follow & 0x80)) ||
            (!(vreg & 0x80) && !(st4 & 1))) return true;
        IOSleep(1);
    }
    return false;
}
int configure() {
    int result = writeByte(cfg, 0x46, 0x80);
    if (result) return result;
    result = writeByte(cfg, 0x48, uint8_t(vmaxMV / vmaxStepMV));
    if (result) return result;
    const uint16_t tlra = uint16_t(tlraUS / tlraStepUS);
    result = writeByte(cfg, 0x5c, uint8_t((tlra >> 8) & 0x0f));
    if (!result) result = writeByte(cfg, 0x5d, uint8_t(tlra & 0xff));
    if (result) return result;
    result = writeByte(cfg, 0x50, uint8_t((brakeMode << 6) | (brakeSineGain << 2)));
    if (result) return result;
    // DIRECT_PLAY runs with auto resonance disabled and the waveform selected
    // as sine (root cells select wf-sel = sine).
    result = writeByte(cfg, 0x49, 0x00);
    if (!result) result = writeByte(cfg, 0x63, 0x00);
    if (!result) result = writeByte(ptn, 0x26, directAmplitude);
    return result;
}
int playOnce() {
    int result = configure();
    if (result) return result;
    result = writeByte(cfg, 0x66, 0x07);
    if (result) return result;
    uint32_t boostStatus = 0;
    result = boostVreg(true);
    if (result) return result;
    if (boost.found) {
        note(45, boostStatus);
        if (!waitBoostReady(&boostStatus)) {
            note(46, 1);
            boostVreg(false);
            return kIOReturnNotReady;
        }
        note(45, boostStatus);
    }
    // qcom-hv-haptics pattern_src: FIFO=0, DIRECT_PLAY=1.
    result = writeByte(cfg, 0x4c, 0x81);
    if (result) return result;
    note(48, milliseconds());
    IOSleep(playMilliseconds);
    note(49, milliseconds());
    // Stop: clear PLAY_EN and keep DIRECT_PLAY as the selected pattern source.
    result = writeByte(cfg, 0x4c, 0x01);
    uint8_t value = 0;
    if (!readByte(cfg, 0x4c, value)) note(50, value);
    if (!readByte(cfg, 0x0c, value)) note(51, value);
    if (!readByte(boost, 0x46, value)) note(52, value);
    if (!readByte(cfg, 0x46, value)) note(53, value);
    if (!readByte(ptn, 0x08, value)) note(54, value);
    if (!readByte(ptn, 0x26, value)) note(55, value);
    boostVreg(false);
    if (!readByte(sdam, 0x42, value)) note(56, value);
    marble_haptics_play_count = marble_haptics_play_count + 1;
    note(9, marble_haptics_play_count);
    return result;
}
void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "marble-haptics");
    marble_haptics_stage = 1;
    if (!setup()) { __atomic_store_n(&running, 0, __ATOMIC_RELEASE); return; }
    if (mode >= 2) {
        marble_haptics_stage = 4;
        native_marble_haptics_runtime();
        __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
        thread_wakeup((event_t)&running);
        return;
    }
    marble_haptics_stage = 2;
    if (playOnce()) fail(5, lastError ? int(lastError) : kIOReturnIOError);
    else marble_haptics_stage = 3;
    IOLog("MarbleHaptics: stage=%u fault=%x play=%x vreg=%x en=%x fifo=%x amp=%x sdam=%x duration=%u ms\n",
        marble_haptics_stage, marble_haptics_stats[51], marble_haptics_stats[50],
        marble_haptics_stats[52], marble_haptics_stats[53], marble_haptics_stats[54],
        marble_haptics_stats[55], marble_haptics_stats[56],
        uint32_t(marble_haptics_stats[49] - marble_haptics_stats[48]));
    __atomic_store_n(&running, 0, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&running);
}
}

extern "C" void native_marble_haptics_start(void) {
    unsigned enabled = 1;
    PE_parse_boot_argn("marble-haptics", &enabled, sizeof(enabled));
    if (!enabled || running) return;
    mode = enabled;
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&restored, 0, __ATOMIC_RELEASE);
    running = 1;
    thread_t thread = nullptr;
    kern_return_t result = kernel_thread_start(worker, nullptr, &thread);
    if (result) { running = 0; fail(6, result); }
    else thread_deallocate(thread);
}
extern "C" void native_marble_haptics_stop(void) {
    if (!running) return;
    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
    native_marble_haptics_runtime_stop();
    for (unsigned tries = 0; tries < 20 && __atomic_load_n(&running, __ATOMIC_ACQUIRE); ++tries) IOSleep(10);
}
extern "C" int native_marble_haptics_drive(unsigned amplitude) {
    if (!amplitude || amplitude > 255 || stopping) return kIOReturnBadArgument;
    int result = 0;
    if (!driveActive) {
        result = configure();
        if (!result) result = writeByte(cfg, 0x66, 0x07);
        if (!result) result = boostVreg(true);
        uint32_t status = 0;
        if (!result && !waitBoostReady(&status)) result = kIOReturnNotReady;
        note(45, status);
    }
    if (!result) result = writeByte(ptn, 0x26, uint8_t(amplitude));
    if (!result && !driveActive) result = writeByte(cfg, 0x4c, 0x81);
    if (!result) driveActive = true;
    else {
        writeByte(cfg, 0x4c, 0x01);
        boostVreg(false);
        driveActive = false;
        note(47, uint32_t(result));
    }
    return result;
}
extern "C" int native_marble_haptics_quiet(void) {
    int result = writeByte(cfg, 0x4c, 0x01);
    int power = boostVreg(false);
    driveActive = false;
    return result ? result : power;
}
extern "C" void native_marble_haptics_restore(void) {
    if (!cfg.found && !ptn.found) return;
    if (__atomic_exchange_n(&restored, 1, __ATOMIC_ACQ_REL)) return;
    writeByte(cfg, 0x4c, 0x00);
    writeByte(cfg, 0x46, 0x00);
    boostVreg(false);
    note(57, 1);
}
