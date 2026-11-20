// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MARBLE_DISPLAY_FEATURE_TAP_WAKE = 1u << 0,
    MARBLE_DISPLAY_FEATURE_AUTO_BRIGHTNESS = 1u << 1,
    MARBLE_DISPLAY_FEATURE_ACCESSIBILITY_FILTER = 1u << 2,
    MARBLE_DISPLAY_FEATURE_NIGHT_SHIFT = 1u << 3,
    MARBLE_DISPLAY_FEATURE_TRUE_TONE = 1u << 4,
};

enum {
    MARBLE_DISPLAY_SOURCE_TOUCH = 1u << 0,
    MARBLE_DISPLAY_SOURCE_BACKLIGHT = 1u << 1,
    MARBLE_DISPLAY_SOURCE_AMBIENT_SENSOR = 1u << 2,
    MARBLE_DISPLAY_SOURCE_COMPOSITOR = 1u << 3,
    MARBLE_DISPLAY_SOURCE_PANEL_CALIBRATION = 1u << 4,
};

enum {
    MARBLE_DISPLAY_SENSOR_UNAVAILABLE = 0,
    MARBLE_DISPLAY_SENSOR_VALID = 1,
    MARBLE_DISPLAY_SENSOR_STALE = 2,
};

enum {
    MARBLE_DISPLAY_COLOR_PIPELINE_NONE = 0,
    MARBLE_DISPLAY_COLOR_PIPELINE_COMPOSITOR = 1,
};

enum {
    MARBLE_DISPLAY_WAKE_NONE = 0,
    MARBLE_DISPLAY_WAKE_ARM_HOLD = 1,
    MARBLE_DISPLAY_WAKE_DISPLAY = 2,
    MARBLE_DISPLAY_WAKE_FORWARD = 3,
};

typedef struct {
    uint32_t features;
    uint32_t sources;
    uint32_t sensor_state;
    uint32_t color_pipeline;
} MarbleDisplayFeatureState;

typedef struct {
    uint32_t lux;
    uint64_t timestamp_ms;
    uint32_t valid;
} MarbleAmbientLightSample;

static inline uint32_t marble_display_wake_action(
    uint32_t display_blank,
    uint32_t power_pressed,
    uint32_t power_released,
    uint32_t other_button,
    uint32_t hold_active,
    uint32_t hold_expired) {
    if (display_blank && power_pressed && !other_button) return MARBLE_DISPLAY_WAKE_ARM_HOLD;
    if (hold_active && (hold_expired || other_button)) return MARBLE_DISPLAY_WAKE_FORWARD;
    if (hold_active && power_released && !hold_expired && !other_button)
        return MARBLE_DISPLAY_WAKE_DISPLAY;
    return MARBLE_DISPLAY_WAKE_NONE;
}

typedef struct {
    uint64_t began;
    uint32_t x, y;
    uint8_t tracking, valid;
} MarbleBlankTap;

enum {
    MARBLE_BLANK_TAP_FORWARD = 0,
    MARBLE_BLANK_TAP_CONSUME = 1,
    MARBLE_BLANK_TAP_WAKE = 2,
};

static inline uint32_t marble_blank_tap_update(MarbleBlankTap *tap, uint32_t blank,
    uint32_t prior_contact, uint32_t count, uint32_t x, uint32_t y,
    uint64_t now, uint64_t max_duration, uint32_t cancelled) {
    if (!tap->tracking) {
        if (!blank || prior_contact || !count || cancelled) return MARBLE_BLANK_TAP_FORWARD;
        tap->tracking = 1;
        tap->valid = count == 1;
        tap->x = x;
        tap->y = y;
        tap->began = now;
        return MARBLE_BLANK_TAP_CONSUME;
    }

    const uint32_t dx = x > tap->x ? x - tap->x : tap->x - x;
    const uint32_t dy = y > tap->y ? y - tap->y : tap->y - y;
    if (!blank || cancelled || count > 1 || now - tap->began > max_duration ||
        (count && (dx > 240 || dy > 240))) tap->valid = 0;
    if (count && !cancelled) return MARBLE_BLANK_TAP_CONSUME;

    const uint32_t action = tap->valid ? MARBLE_BLANK_TAP_WAKE : MARBLE_BLANK_TAP_CONSUME;
    tap->tracking = tap->valid = 0;
    return action;
}

static inline MarbleDisplayFeatureState marble_display_feature_state(
    uint32_t touch_input,
    uint32_t backlight,
    uint32_t ambient_sensor,
    uint32_t compositor,
    uint32_t panel_calibration,
    uint32_t enabled_features) {
    MarbleDisplayFeatureState state = {0, 0, MARBLE_DISPLAY_SENSOR_UNAVAILABLE,
                                       MARBLE_DISPLAY_COLOR_PIPELINE_NONE};
    if (touch_input) {
        state.features |= MARBLE_DISPLAY_FEATURE_TAP_WAKE;
        state.sources |= MARBLE_DISPLAY_SOURCE_TOUCH;
    }
    if (backlight) state.sources |= MARBLE_DISPLAY_SOURCE_BACKLIGHT;
    if (ambient_sensor) {
        state.sensor_state = MARBLE_DISPLAY_SENSOR_VALID;
        state.sources |= MARBLE_DISPLAY_SOURCE_AMBIENT_SENSOR;
    }
    if (compositor) {
        state.color_pipeline = MARBLE_DISPLAY_COLOR_PIPELINE_COMPOSITOR;
        state.features |= MARBLE_DISPLAY_FEATURE_ACCESSIBILITY_FILTER |
                          MARBLE_DISPLAY_FEATURE_NIGHT_SHIFT;
        state.sources |= MARBLE_DISPLAY_SOURCE_COMPOSITOR;
    }
    if (ambient_sensor && backlight)
        state.features |= MARBLE_DISPLAY_FEATURE_AUTO_BRIGHTNESS;
    if (ambient_sensor && compositor && panel_calibration)
        state.features |= MARBLE_DISPLAY_FEATURE_TRUE_TONE;
    if (panel_calibration) state.sources |= MARBLE_DISPLAY_SOURCE_PANEL_CALIBRATION;
    state.features &= enabled_features;
    return state;
}

static inline uint32_t marble_display_sample_state(
    MarbleAmbientLightSample sample, uint64_t now_ms, uint64_t max_age_ms) {
    if (!sample.valid) return MARBLE_DISPLAY_SENSOR_UNAVAILABLE;
    if (now_ms < sample.timestamp_ms || now_ms - sample.timestamp_ms > max_age_ms)
        return MARBLE_DISPLAY_SENSOR_STALE;
    return MARBLE_DISPLAY_SENSOR_VALID;
}

static inline uint32_t marble_display_auto_brightness_level(
    uint32_t lux, uint32_t minimum, uint32_t maximum) {
    static const uint32_t lux_points[] = {0, 1, 5, 20, 80, 300, 1000, 3000, 10000};
    const uint32_t point_count = sizeof(lux_points) / sizeof(lux_points[0]);
    if (maximum <= minimum) return minimum;
    if (lux >= lux_points[point_count - 1]) return maximum;
    uint32_t index = 0;
    while (index + 1 < point_count && lux >= lux_points[index + 1]) ++index;
    const uint32_t low_lux = lux_points[index];
    const uint32_t high_lux = lux_points[index + 1];
    const uint32_t span = high_lux - low_lux;
    const uint32_t position = lux - low_lux;
    const uint64_t level_span = (uint64_t)(maximum - minimum) * index;
    const uint64_t base = level_span / (point_count - 1);
    const uint64_t segment = ((uint64_t)(maximum - minimum) * position) /
                             ((uint64_t)(point_count - 1) * span);
    return minimum + (uint32_t)(base + segment);
}

#ifdef __cplusplus
}
#endif
