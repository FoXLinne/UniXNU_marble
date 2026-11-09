"""Display geometry shared by the payload and the phone UI adapter."""
import math
from pathlib import Path
import tomllib

DEVICE = Path(__file__).resolve().parents[1]


def display_geometry():
    display = tomllib.loads((DEVICE / 'profiles/marble-display.toml').read_text())['display']
    display['pixels_per_inch'] = round(25.4 * math.hypot(display['width_pixels'], display['height_pixels']) /
                                      math.hypot(display['width_mm'], display['height_mm']))
    return display



def display_layout():
    display = display_geometry()
    layout = tomllib.loads((DEVICE / 'profiles/marble-display.toml').read_text())['layout']
    native_scale = display['width_pixels'] / layout['logical_width']
    return dict(layout, point_scale=native_scale, artwork_scale=display['scale'], logical_zoom=1.0,
                logical_height=layout['standard_height_pixels'] / display['scale'],
                compositor_size=[layout['standard_width_pixels'], layout['standard_height_pixels']],
                native_points_scale=native_scale)


def write_ui_header(path, target):
    display = display_geometry()
    identity = tomllib.loads((DEVICE / 'profiles/ios-18.7.11-22H384.toml').read_text())['ui_identity']
    category = identity['screen_category'] if target == 'ios18' else -1
    layout = tomllib.loads((DEVICE / 'profiles/marble-display.toml').read_text())['layout']
    native_scale = display['width_pixels'] / layout['logical_width']
    path.write_text(f'''// Generated from the device display and OS identity profiles.
#define MARBLE_UI_SCREEN_CATEGORY {category}
#define MARBLE_UI_ZOOMED_SCREEN_CATEGORY {layout['zoomed_screen_category']}
#define MARBLE_UI_STANDARD_WIDTH {layout['standard_width_pixels']}.0
#define MARBLE_UI_STANDARD_HEIGHT {layout['standard_height_pixels']}.0
#define MARBLE_UI_ZOOMED_WIDTH {layout['zoomed_width_pixels']}.0
#define MARBLE_UI_ZOOMED_HEIGHT {layout['zoomed_height_pixels']}.0
#define MARBLE_UI_CORNER_RADIUS_PIXELS {layout['corner_radius_pixels']}
#define MARBLE_UI_SENSOR_WIDTH_PIXELS {layout['sensor_width_pixels']}
#define MARBLE_UI_SENSOR_HEIGHT_PIXELS {layout['sensor_height_pixels']}
#define MARBLE_UI_SENSOR_TOP_PIXELS {layout['sensor_top_pixels']}
#define MARBLE_UI_HOME_LAYOUT "{layout['home_screen']}"
#define MARBLE_UI_LOGICAL_WIDTH {layout['logical_width']}
#define MARBLE_UI_LOGICAL_SCALE 1.0
#define MARBLE_UI_CORNER_RADIUS {layout['corner_radius_pixels'] / native_scale:.17g}
#define MARBLE_UI_SENSOR_WIDTH {layout['sensor_width_pixels'] / native_scale:.17g}
#define MARBLE_UI_SENSOR_HEIGHT {layout['sensor_height_pixels'] / native_scale:.17g}
#define MARBLE_UI_SENSOR_TOP {layout['sensor_top_pixels'] / native_scale:.17g}
#define MARBLE_UI_NATIVE_WIDTH {display['width_pixels']}.0
#define MARBLE_UI_NATIVE_HEIGHT {display['height_pixels']}.0
#define MARBLE_UI_DISPLAY_SCALE {native_scale:.17g}
#define MARBLE_UI_ARTWORK_SCALE {display['scale']}.0
#define MARBLE_UI_PIXELS_PER_INCH {display['pixels_per_inch']}.0
''')
