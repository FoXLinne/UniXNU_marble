#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise the board's brightness and PM callbacks with fake DCS endpoints."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
source = (repo / 'package/MarbleBacklight.cpp').read_text()
member_begin = source.index('    uint64_t pendingBrightness=0;')
member_end = source.index('    static uint32_t clamp(', member_begin)
callback_begin = source.index('    static void update(')
callback_end = source.index('\npublic:', callback_begin)

harness = r'''
#include <cassert>
#include <stdint.h>
#include <vector>
#include "MarbleCpuIdlePolicy.h"
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr uint32_t kMin = 4, kMax = 4095, kInitial = 0x133;
struct OSObject {};
struct IOTimerEventSource { void setTimeoutMS(unsigned) {} };
static volatile uint32_t marble_backlight_stats[8];
static std::vector<unsigned> power, brightness, levels, notifications;
static int failPower = 0;
static IOReturn native_legacy_panel_power(bool on) {
    power.push_back(on);
    return failPower;
}
static IOReturn native_marble_panel_set_brightness(uint32_t level) {
    brightness.push_back(level);
    return 0;
}
static void native_marble_display_backlight_state(bool on) {
    notifications.push_back(on);
}
struct MarbleBacklight : OSObject {
    IOTimerEventSource *timer = nullptr;
    static uint32_t clamp(uint64_t n) { return n < kMin ? kMin : n > kMax ? kMax : uint32_t(n); }
    static void publishLevel(uint32_t n) { levels.push_back(n); }
'''

cases = r'''
};

static void request(MarbleBacklight &backlight, uint32_t level, bool blank = false, unsigned fade = 0) {
    backlight.pendingBrightness = (1ULL << 63) | (blank ? 1ULL << 62 : 0) |
        uint64_t(fade) << 32 | level;
    MarbleBacklight::update(&backlight, nullptr);
}
static void systemPower(MarbleBacklight &backlight, bool on) {
    assert(!MarbleBacklight::setSystemPower(&backlight, reinterpret_cast<void *>(uintptr_t(on)),
                                           nullptr, nullptr, nullptr));
}
int main() {
    IOTimerEventSource timer;
    MarbleBacklight backlight;
    backlight.timer = &timer;
    for (unsigned level = 350; level <= 800; ++level) request(backlight, level);
    assert(power.empty() && notifications.empty() && brightness.size() == 451);
    request(backlight, 0, true, 32);
    assert(power.empty());
    MarbleBacklight::update(&backlight, nullptr);
    assert(power == std::vector<unsigned>{0} && levels.back() == 0);
    systemPower(backlight, false);
    request(backlight, 900);
    assert(power.size() == 1 && brightness.back() == kMin);
    systemPower(backlight, true);
    assert((power == std::vector<unsigned>{0, 1} && power == notifications));
    assert(brightness.back() == 900 && levels.back() == 900);
    systemPower(backlight, true);
    assert(power.size() == 2);
    failPower = -1;
    request(backlight, 0, true);
    assert(power.size() == 3 && notifications.size() == 2 && marble_backlight_stats[7] == 1);
    failPower = 0;
    request(backlight, 0, true);
    assert(power.size() == 4 && notifications.size() == 3 && marble_backlight_stats[7] == 0);

    const auto powerChanges = power.size();
    const auto brightnessChanges = brightness.size();
    systemPower(backlight, false);
    systemPower(backlight, true);
    assert(power.size() == powerChanges && brightness.size() == brightnessChanges);
    assert(marble_backlight_stats[7] == 0 && levels.back() == 0);
    request(backlight, 650);
    assert(power.size() == powerChanges + 1 && power.back() == 1);
    assert(brightness.back() == 650 && levels.back() == 650);

    for (const uint32_t ownership : {0x0du, 0xfdu, 0xffu}) {
        MarbleCpuIdlePolicy cpu;
        cpu.configure(ownership);
        assert(cpu.owns(1) == bool(ownership & 2));
        for (unsigned core = 0; core < 8; ++core) if (cpu.owns(core)) cpu.change(core, true);
        assert(cpu.allIdle());
        for (unsigned core = 0; core < 8; ++core) if (cpu.owns(core)) cpu.change(core, false);
        for (unsigned domain = 0; domain < 3; ++domain) {
            const uint32_t mask[] = {0x0f, 0x70, 0x80};
            assert(cpu.active(domain) == (ownership & mask[domain]));
        }
    }
    MarbleL3IdlePolicy l3;
    for (unsigned i = 0; i < 3; ++i) assert(!l3.update(false, 19));
    assert(!l3.update(false, 20));
    for (unsigned i = 0; i < 3; ++i) assert(!l3.update(false, 19));
    assert(l3.update(false, 19));
    assert(l3.update(false, 34));
    assert(!l3.update(false, 35));
    for (unsigned i = 0; i < 4; ++i) l3.update(false, 0);
    assert(!l3.update(true, 0));
}
'''

with tempfile.TemporaryDirectory(prefix='marble-display-power-') as directory:
    source_file = Path(directory) / 'test.cpp'
    executable = Path(directory) / 'test'
    source_file.write_text(harness + source[member_begin:member_end] +
                           source[callback_begin:callback_end] + cases)
    subprocess.run(['xcrun', 'clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(repo / 'package'),
                    str(source_file), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PASS: consecutive slider updates, fade to blank, PM sleep/wake, dark wake, DCS failure, CPU ownership and L3 thresholds')
