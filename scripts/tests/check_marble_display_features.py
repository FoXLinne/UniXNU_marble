#!/usr/bin/env python3
"""Check Marble display feature sources and event-driven ambient-light policy."""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
harness = r"""
#include <cassert>
#include <cstdint>
#include "MarbleDisplayFeatures.h"

int main() {
    MarbleDisplayFeatureState marble = marble_display_feature_state(1, 1, 0, 1, 0, 0);
    assert(marble.features == 0);
    assert(marble.sources == (MARBLE_DISPLAY_SOURCE_TOUCH |
                              MARBLE_DISPLAY_SOURCE_BACKLIGHT |
                              MARBLE_DISPLAY_SOURCE_COMPOSITOR));

    MarbleDisplayFeatureState all = marble_display_feature_state(1, 1, 1, 1, 1, UINT32_MAX);
    assert((all.features & (MARBLE_DISPLAY_FEATURE_TAP_WAKE |
                           MARBLE_DISPLAY_FEATURE_AUTO_BRIGHTNESS |
                           MARBLE_DISPLAY_FEATURE_ACCESSIBILITY_FILTER |
                           MARBLE_DISPLAY_FEATURE_NIGHT_SHIFT |
                           MARBLE_DISPLAY_FEATURE_TRUE_TONE)) ==
           (MARBLE_DISPLAY_FEATURE_TAP_WAKE |
            MARBLE_DISPLAY_FEATURE_AUTO_BRIGHTNESS |
            MARBLE_DISPLAY_FEATURE_ACCESSIBILITY_FILTER |
            MARBLE_DISPLAY_FEATURE_NIGHT_SHIFT |
            MARBLE_DISPLAY_FEATURE_TRUE_TONE));

    assert(marble_display_wake_action(1, 1, 0, 0, 0, 0) == MARBLE_DISPLAY_WAKE_ARM_HOLD);
    assert(marble_display_wake_action(1, 0, 1, 0, 1, 0) == MARBLE_DISPLAY_WAKE_DISPLAY);
    assert(marble_display_wake_action(1, 0, 1, 0, 1, 1) == MARBLE_DISPLAY_WAKE_FORWARD);
    assert(marble_display_wake_action(1, 0, 1, 1, 1, 0) == MARBLE_DISPLAY_WAKE_FORWARD);
    assert(marble_display_wake_action(1, 0, 1, 1, 1, 1) == MARBLE_DISPLAY_WAKE_FORWARD);

    MarbleBlankTap tap = {};
    assert(marble_blank_tap_update(&tap, 0, 0, 1, 990, 1100, 100, 350, 0) == MARBLE_BLANK_TAP_FORWARD);
    assert(marble_blank_tap_update(&tap, 1, 1, 1, 990, 1100, 100, 350, 0) == MARBLE_BLANK_TAP_FORWARD);
    assert(marble_blank_tap_update(&tap, 1, 0, 1, 990, 1100, 100, 350, 0) == MARBLE_BLANK_TAP_CONSUME);
    assert(marble_blank_tap_update(&tap, 1, 0, 0, 0, 0, 175, 350, 0) == MARBLE_BLANK_TAP_WAKE);
    assert(marble_blank_tap_update(&tap, 1, 0, 1, 990, 1100, 100, 350, 0) == MARBLE_BLANK_TAP_CONSUME);
    assert(marble_blank_tap_update(&tap, 1, 0, 1, 1231, 1100, 110, 350, 0) == MARBLE_BLANK_TAP_CONSUME);
    assert(marble_blank_tap_update(&tap, 1, 0, 0, 0, 0, 175, 350, 0) == MARBLE_BLANK_TAP_CONSUME);
    assert(marble_blank_tap_update(&tap, 1, 0, 1, 990, 1100, 100, 350, 0) == MARBLE_BLANK_TAP_CONSUME);
    assert(marble_blank_tap_update(&tap, 1, 0, 0, 0, 0, 451, 350, 0) == MARBLE_BLANK_TAP_CONSUME);
    assert(marble_blank_tap_update(&tap, 1, 0, 2, 990, 1100, 100, 350, 0) == MARBLE_BLANK_TAP_CONSUME);
    assert(marble_blank_tap_update(&tap, 1, 0, 0, 0, 0, 175, 350, 0) == MARBLE_BLANK_TAP_CONSUME);
    assert(marble_blank_tap_update(&tap, 1, 0, 1, 990, 1100, 100, 350, 0) == MARBLE_BLANK_TAP_CONSUME);
    assert(marble_blank_tap_update(&tap, 1, 0, 0, 0, 0, 175, 350, 1) == MARBLE_BLANK_TAP_CONSUME);

    MarbleAmbientLightSample sample = {80, 1000, 1};
    assert(marble_display_sample_state(sample, 1100, 200) == MARBLE_DISPLAY_SENSOR_VALID);
    assert(marble_display_sample_state(sample, 1201, 200) == MARBLE_DISPLAY_SENSOR_STALE);
    sample.valid = 0;
    assert(marble_display_sample_state(sample, 1100, 200) == MARBLE_DISPLAY_SENSOR_UNAVAILABLE);

    const uint32_t dark = marble_display_auto_brightness_level(0, 4, 4095);
    const uint32_t room = marble_display_auto_brightness_level(300, 4, 4095);
    const uint32_t sun = marble_display_auto_brightness_level(10000, 4, 4095);
    assert(dark == 4 && room > dark && sun == 4095);
    assert(marble_display_auto_brightness_level(100000, 4, 4095) == 4095);
    assert(marble_display_auto_brightness_level(300, 90, 40) == 90);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='marble-display-features-') as directory:
    directory = Path(directory)
    source = directory / 'test.cpp'
    binary = directory / 'test'
    source.write_text(harness)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(repo / 'package'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: display capability gating, power-key dispatch and ambient-light policy')
