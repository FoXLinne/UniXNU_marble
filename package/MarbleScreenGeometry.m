// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#include <CoreGraphics/CGGeometry.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <string.h>
#include "MarbleScreenGeometry.h"
#include "MarbleScreenGeometryProfile.h"

uint32_t marble_screen_geometry_state;

#if MARBLE_UI_SCREEN_CATEGORY >= 0
static IMP originalScreenType, originalEnsureDensity;
static IMP originalZoomMode;
static Ivar screenTypeCache, pointsPerInchCache, pointsPerMillimeterCache, screenConfigurationCache;

// Settings owns the selected canvas and relaunches the display services.
// The board supplies the mini's standard and zoomed modes to that same path.
static id zoomMode(id self, SEL selector, NSUInteger option, NSString *name, CGSize size)
{
    if (option == 0)
        size = CGSizeMake(MARBLE_UI_STANDARD_WIDTH, MARBLE_UI_STANDARD_HEIGHT);
    else if (option == 1)
        size = CGSizeMake(MARBLE_UI_ZOOMED_WIDTH, MARBLE_UI_ZOOMED_HEIGHT);
    return ((id (*)(id, SEL, NSUInteger, id, CGSize))originalZoomMode)(
        self, selector, option, name, size);
}

static BOOL panelScreen(id screen)
{
    SEL embedded = sel_registerName("_isEmbeddedScreen");
    if (![screen respondsToSelector:embedded] ||
        !((BOOL (*)(id, SEL))objc_msgSend)(screen, embedded)) return NO;
    CGRect bounds = ((CGRect (*)(id, SEL))objc_msgSend)(screen, sel_registerName("nativeBounds"));
    id configuration = screenConfigurationCache ? object_getIvar(screen, screenConfigurationCache) : nil;
    if ([configuration respondsToSelector:sel_registerName("pixelSize")])
        bounds.size = ((CGSize (*)(id, SEL))objc_msgSend)(configuration, sel_registerName("pixelSize"));
    double shortSide = fmin(bounds.size.width, bounds.size.height);
    double longSide = fmax(bounds.size.width, bounds.size.height);
    return fabs(shortSide - MARBLE_UI_NATIVE_WIDTH) < 0.5 &&
           fabs(longSide - MARBLE_UI_NATIVE_HEIGHT) < 0.5;
}

static NSInteger screenType(id self, SEL selector)
{
    NSInteger type = ((NSInteger (*)(id, SEL))originalScreenType)(self, selector);
    if (!panelScreen(self)) return type;
    CGRect bounds = ((CGRect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
    CGFloat width = fmin(bounds.size.width, bounds.size.height);
    type = width < 350 ? MARBLE_UI_ZOOMED_SCREEN_CATEGORY : MARBLE_UI_SCREEN_CATEGORY;
    NSInteger *cached = (NSInteger *)((uint8_t *)(void *)self + ivar_getOffset(screenTypeCache));
    __atomic_store_n(cached, type, __ATOMIC_RELAXED);
    return type;
}

static void ensureDensity(id self, SEL selector)
{
    if (!panelScreen(self)) {
        ((void (*)(id, SEL))originalEnsureDensity)(self, selector);
        return;
    }
    CGFloat scale = ((CGFloat (*)(id, SEL))objc_msgSend)(self, sel_registerName("scale"));
    CGFloat nativeScale = ((CGFloat (*)(id, SEL))objc_msgSend)(self, sel_registerName("nativeScale"));
    if (!(scale > 0)) scale = MARBLE_UI_DISPLAY_SCALE;
    if (!(nativeScale > 0)) nativeScale = MARBLE_UI_DISPLAY_SCALE;
    // UIKeyboardDockView uses this physical measurement for button placement
    // and touch regions. The panel dimensions supply it before Gestalt's cache.
    CGFloat inch = MARBLE_UI_PIXELS_PER_INCH / scale;
    CGFloat millimeter = MARBLE_UI_PIXELS_PER_INCH / (25.4 * nativeScale);
    memcpy((uint8_t *)(void *)self + ivar_getOffset(pointsPerInchCache), &inch, sizeof(inch));
    memcpy((uint8_t *)(void *)self + ivar_getOffset(pointsPerMillimeterCache), &millimeter, sizeof(millimeter));
}


// Device-level factories also run during UIScreen singleton creation.
// Board geometry is available before that singleton exists.
static IMP originalDisplayShape, originalEdgeInfo;
static IMP originalSensorSize, originalApertureInset, originalApertureCanvas;
static IMP originalExclusionArea, originalPeripheryInsets, originalInterSensor;
static IMP originalCornerRadius, originalCornerIgnoringZoom;
static unsigned integrations;
typedef struct { CGFloat top, left, bottom, right; } MarbleEdgeInsets;

// Core Animation owns the active canvas before UIKit creates its main screen.
// Querying it here also lets the display-service relaunch apply either mode.
static CGSize panelCanvas(void)
{
    id cls = (id)objc_lookUpClass("CADisplay");
    id display = ((id (*)(id, SEL))objc_msgSend)(cls, sel_registerName("mainDisplay"));
    id mode = ((id (*)(id, SEL))objc_msgSend)(display, sel_registerName("currentMode"));
    if (mode) {
        NSUInteger width = ((NSUInteger (*)(id, SEL))objc_msgSend)(mode, sel_registerName("width"));
        NSUInteger height = ((NSUInteger (*)(id, SEL))objc_msgSend)(mode, sel_registerName("height"));
        NSUInteger scale = ((NSUInteger (*)(id, SEL))objc_msgSend)(display, sel_registerName("pointScale"));
        if (scale && width && height)
            return CGSizeMake((CGFloat)MIN(width, height) / scale,
                              (CGFloat)MAX(width, height) / scale);
    }
    return CGSizeMake(MARBLE_UI_STANDARD_WIDTH / MARBLE_UI_ARTWORK_SCALE,
                      MARBLE_UI_STANDARD_HEIGHT / MARBLE_UI_ARTWORK_SCALE);
}

static CGFloat sensorTop(void)
{
    return MARBLE_UI_SENSOR_TOP_PIXELS * panelCanvas().height / MARBLE_UI_NATIVE_HEIGHT;
}

static CGSize sensorSize(void)
{
    CGSize canvas = panelCanvas();
    return CGSizeMake(MARBLE_UI_SENSOR_WIDTH_PIXELS * canvas.width / MARBLE_UI_NATIVE_WIDTH,
                      MARBLE_UI_SENSOR_HEIGHT_PIXELS * canvas.height / MARBLE_UI_NATIVE_HEIGHT);
}

static id nativeDisplayShape(void)
{
    CGSize canvas = panelCanvas(), sensor = sensorSize();
    CGRect rect = CGRectMake((canvas.width - sensor.width) / 2,
                             sensorTop(), sensor.width, sensor.height);
    id cls = (id)objc_lookUpClass("UISDisplaySingleRectShape");
    id shape = ((id (*)(id, SEL))objc_msgSend)(cls, sel_registerName("alloc"));
    return [((id (*)(id, SEL, CGRect))objc_msgSend)(shape, sel_registerName("initWithRect:"), rect) autorelease];
}

static id displayShape(id self, SEL selector)
{
    (void)self; (void)selector;
    return nativeDisplayShape();
}

static id exclusionArea(id self, SEL selector)
{
    return panelScreen(self) ? nativeDisplayShape() :
        ((id (*)(id, SEL))originalExclusionArea)(self, selector);
}

static MarbleEdgeInsets peripheryInsets(id self, SEL selector)
{
    if (panelScreen(self))
        return (MarbleEdgeInsets){sensorTop() + sensorSize().height, 0, 0, 0};
    return ((MarbleEdgeInsets (*)(id, SEL))originalPeripheryInsets)(self, selector);
}

static CGRect interSensor(id self, SEL selector)
{
    (void)self; (void)selector;
    CGSize canvas = panelCanvas(), sensor = sensorSize();
    CGFloat interval = sensor.height * 9.0 / (110.0 / 3.0);
    return CGRectMake((canvas.width + sensor.width) / 2 - sensor.height - interval,
                      sensorTop(), interval, sensor.height);
}

static id insetWrapper(CGFloat top, CGFloat left, CGFloat bottom, CGFloat right)
{
    id cls = (id)objc_lookUpClass("UISApplicationSupportDisplayEdgeInsetsWrapper");
    id wrapper = ((id (*)(id, SEL))objc_msgSend)(cls, sel_registerName("alloc"));
    return [((id (*)(id, SEL, CGFloat, CGFloat, CGFloat, CGFloat))objc_msgSend)(wrapper,
        sel_registerName("initWithTop:left:bottom:right:"), top, left, bottom, right) autorelease];
}

static id edgeInfo(id self, SEL selector)
{
    id native = ((id (*)(id, SEL))originalEdgeInfo)(self, selector);
    id info = [[native copy] autorelease];
    ((void (*)(id, SEL, id))objc_msgSend)(info, sel_registerName("setPeripheryInsets:"),
        insetWrapper(sensorTop() + sensorSize().height, 0, 0, 0));
    return info;
}

static CGSize apertureSensor(id self, SEL selector)
{
    (void)self; (void)selector;
    return sensorSize();
}

static CGFloat apertureInset(id self, SEL selector)
{
    (void)self; (void)selector;
    return sensorTop();
}

static CGSize apertureCanvas(id self, SEL selector)
{
    (void)self; (void)selector;
    return panelCanvas();
}

static CGFloat cornerRadius(id self, SEL selector)
{
    if (panelScreen(self)) return MARBLE_UI_CORNER_RADIUS_PIXELS * panelCanvas().width / MARBLE_UI_NATIVE_WIDTH;
    IMP original = sel_isEqual(selector, sel_registerName("_displayCornerRadius")) ? originalCornerRadius : originalCornerIgnoringZoom;
    return ((CGFloat (*)(id, SEL))original)(self, selector);
}

static void replaceMethod(const char *name, const char *selector, BOOL classMethod,
                          IMP replacement, IMP *original)
{
    Class cls = objc_lookUpClass(name);
    Method method = classMethod ? class_getClassMethod(cls, sel_registerName(selector)) :
                                 class_getInstanceMethod(cls, sel_registerName(selector));
    if (!method || method_getImplementation(method) == replacement) return;
    *original = method_getImplementation(method);
    method_setImplementation(method, replacement);
}

static void installLayoutConsumers(void)
{
    if (__atomic_exchange_n(&integrations, 1, __ATOMIC_ACQUIRE)) return;
    replaceMethod("DBSDisplayZoomMode", "initWithDisplayZoomOption:localizedName:size:",
                  NO, (IMP)zoomMode, &originalZoomMode);
    replaceMethod("UIScreen", "_displayCornerRadius", NO, (IMP)cornerRadius, &originalCornerRadius);
    replaceMethod("UIScreen", "_displayCornerRadiusIgnoringZoom", NO, (IMP)cornerRadius, &originalCornerIgnoringZoom);
    replaceMethod("UIScreen", "_exclusionArea", NO, (IMP)exclusionArea, &originalExclusionArea);
    replaceMethod("UIScreen", "_displayPeripheryInsets", NO, (IMP)peripheryInsets, &originalPeripheryInsets);
    replaceMethod("UISDisplaySingleRectShape", "sb_thisDeviceDisplayShape", YES,
                  (IMP)displayShape, &originalDisplayShape);
    replaceMethod("UISApplicationSupportDisplayEdgeInfo", "sb_thisDeviceDisplayEdgeInfo", YES,
                  (IMP)edgeInfo, &originalEdgeInfo);
    replaceMethod("SBUISystemApertureLayoutMetrics", "interSensorRegionInWindowSpace", NO,
                  (IMP)interSensor, &originalInterSensor);
    replaceMethod("SBUISystemApertureLayoutMetrics", "sensorRegionSize", NO,
                  (IMP)apertureSensor, &originalSensorSize);
    replaceMethod("SBUISystemApertureLayoutMetrics", "_minimumScreenEdgeInset", NO,
                  (IMP)apertureInset, &originalApertureInset);
    replaceMethod("SBUISystemApertureLayoutMetrics", "_deviceNativeScreenSizeInPoints", YES,
                  (IMP)apertureCanvas, &originalApertureCanvas);
    __atomic_store_n(&integrations, 0, __ATOMIC_RELEASE);
}

static BOOL scalarMethod(Method method, char type)
{
    if (!method || method_getNumberOfArguments(method) != 2) return NO;
    char *actual = method_copyReturnType(method);
    BOOL matches = actual && actual[0] == type;
    free(actual);
    return matches;
}
#endif

void marble_screen_geometry_install(void)
{
#if MARBLE_UI_SCREEN_CATEGORY >= 0
    installLayoutConsumers();
    Class screen = objc_lookUpClass("UIScreen");
    if (!screen) return;
    Method type = class_getInstanceMethod(screen, sel_registerName("_screenType"));
    Method density = class_getInstanceMethod(screen, sel_registerName("_ensureComputedMainScreenDPI"));
    Ivar cache = class_getInstanceVariable(screen, "_screenType");
    Ivar inch = class_getInstanceVariable(screen, "_pointsPerInch");
    Ivar millimeter = class_getInstanceVariable(screen, "_nativePointsPerMillimeter");
    if (!scalarMethod(type, 'q') || !scalarMethod(density, 'v') || !cache || !inch || !millimeter ||
        strcmp(ivar_getTypeEncoding(cache), @encode(NSInteger)) ||
        strcmp(ivar_getTypeEncoding(inch), @encode(CGFloat)) ||
        strcmp(ivar_getTypeEncoding(millimeter), @encode(CGFloat))) return;
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&marble_screen_geometry_state, &expected, 1, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    screenTypeCache = cache;
    screenConfigurationCache = class_getInstanceVariable(screen, "__displayConfiguration");
    pointsPerInchCache = inch;
    pointsPerMillimeterCache = millimeter;
    originalScreenType = method_getImplementation(type);
    originalEnsureDensity = method_getImplementation(density);
    method_setImplementation(type, (IMP)screenType);
    method_setImplementation(density, (IMP)ensureDensity);
    __atomic_store_n(&marble_screen_geometry_state, 2, __ATOMIC_RELEASE);
#endif
}

