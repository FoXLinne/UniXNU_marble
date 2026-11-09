// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#include <CoreGraphics/CGGeometry.h>
#include <assert.h>
#include <math.h>
#include "MarbleScreenGeometry.h"
#include "MarbleScreenGeometryProfile.h"

@interface UIScreen : NSObject {
@public
    NSInteger _screenType;
    CGFloat _pointsPerInch, _nativePointsPerMillimeter;
    NSUInteger densityQueries;
}
@property BOOL embedded;
@property CGRect nativeBounds;
@property CGFloat scale, nativeScale;
- (BOOL)_isEmbeddedScreen;
- (NSInteger)_screenType;
- (void)_ensureComputedMainScreenDPI;
- (CGFloat)_nativePointsPerMillimeter;
- (id)_exclusionArea;
- (CGFloat)_displayCornerRadius;
- (CGFloat)_displayCornerRadiusIgnoringZoom;
@end
@implementation UIScreen
- (id)init {
    if ((self = [super init])) {
        _screenType = -1; self.embedded = YES;
        self.nativeBounds = CGRectMake(0, 0, 1080, 2400);
        self.scale = MARBLE_UI_DISPLAY_SCALE; self.nativeScale = 1080.0 / 440.0;
    }
    return self;
}
- (BOOL)_isEmbeddedScreen { return self.embedded; }
- (NSInteger)_screenType { return _screenType; }
- (void)_ensureComputedMainScreenDPI { ++densityQueries; }
- (CGFloat)_nativePointsPerMillimeter { [self _ensureComputedMainScreenDPI]; return _nativePointsPerMillimeter; }
- (id)_exclusionArea { return [NSNull null]; }
- (CGFloat)_displayCornerRadius { return 62; }
- (CGFloat)_displayCornerRadiusIgnoringZoom { return 62; }
@end

uint32_t SBHScreenTypeForCurrentDevice[16] __attribute__((aligned(4096)));
static struct { uintptr_t once, family; } homeCache = {UINTPTR_MAX, 4};
static void prepareHomeCache(void) {
    intptr_t page = (intptr_t)SBHScreenTypeForCurrentDevice & ~(intptr_t)4095;
    intptr_t target = (intptr_t)&homeCache.family & ~(intptr_t)4095;
    uint32_t displacement = (uint32_t)((target-page)/4096) & 0x1fffff;
    SBHScreenTypeForCurrentDevice[0] = 0x90000008u | ((displacement & 3) << 29) | ((displacement >> 2) << 5);
    SBHScreenTypeForCurrentDevice[1] = 0xf9400100u | ((uint32_t)(((uintptr_t)&homeCache.family & 4095)/8) << 10);
    SBHScreenTypeForCurrentDevice[2] = 0xd65f03c0u;
}

NSInteger SBHScreenTypeForShortString(NSString *name) { return [name isEqual:@"D94"] ? 27 : 4; }
@interface SBHDefaultIconListLayoutProvider : NSObject
+ (NSInteger)currentDeviceScreenType;
@end
@implementation SBHDefaultIconListLayoutProvider
+ (NSInteger)currentDeviceScreenType { return 4; }
@end

@interface UISDisplaySingleRectShape : NSObject
@property CGRect rect;
+ (UISDisplaySingleRectShape *)sb_thisDeviceDisplayShape;
- (id)initWithRect:(CGRect)rect;
@end
@implementation UISDisplaySingleRectShape
+ (UISDisplaySingleRectShape *)sb_thisDeviceDisplayShape { return [[[self alloc] initWithRect:CGRectMake(0, 0, 125, 37)] autorelease]; }
- (id)initWithRect:(CGRect)rect { if ((self = [super init])) self.rect = rect; return self; }
@end

@interface SBUISystemApertureLayoutMetrics : NSObject
+ (CGSize)_deviceNativeScreenSizeInPoints;
- (CGSize)sensorRegionSize;
- (CGRect)interSensorRegionInWindowSpace;
- (CGFloat)_minimumScreenEdgeInset;
@end
@implementation SBUISystemApertureLayoutMetrics
+ (CGSize)_deviceNativeScreenSizeInPoints { return CGSizeMake(402, 874); }
- (CGSize)sensorRegionSize { return CGSizeMake(125, 37); }
- (CGRect)interSensorRegionInWindowSpace { return CGRectMake(210, 11, 9, 37); }
- (CGFloat)_minimumScreenEdgeInset { return 11; }
@end

@interface SBHFloatyFolderVisualConfiguration : NSObject
- (CGSize)contentBackgroundSize;
@end
@implementation SBHFloatyFolderVisualConfiguration
- (CGSize)contentBackgroundSize { return CGSizeMake(300, 330); }
@end

@interface TestWindowServerDisplay : NSObject
@property CGRect nativeBounds;
@property(nonatomic) CGFloat scale;
@property NSUInteger requests;
- (CGRect)bounds;
- (CGFloat)minimumScale;
- (CGFloat)maximumScale;
@end
@implementation TestWindowServerDisplay
- (id)init {
    if ((self = [super init])) { self.nativeBounds = CGRectMake(0, 0, 1080, 2400); _scale = 1; }
    return self;
}
- (CGRect)bounds {
    return CGRectMake(0, 0, round(self.nativeBounds.size.width * self.scale), round(self.nativeBounds.size.height * self.scale));
}
- (void)setScale:(CGFloat)scale { _scale = scale; ++_requests; }
- (CGFloat)minimumScale { return 0.5; }
- (CGFloat)maximumScale { return 2; }
@end

int main(void) {
    @autoreleasepool {
        prepareHomeCache();
        marble_screen_geometry_install();
        assert(marble_screen_geometry_state == 2);
        UIScreen *panel = [UIScreen new];
        assert([panel _screenType] == 29 && panel->_screenType == 29);
        assert([SBHDefaultIconListLayoutProvider currentDeviceScreenType] == 27 && homeCache.family == 27);
        CGFloat pitch = [panel _nativePointsPerMillimeter];
        assert(fabs(pitch - 395.0 / 25.4 / panel.nativeScale) < 0.001 && panel->densityQueries == 0);
        assert(fabs([panel _displayCornerRadius] * panel.nativeScale - 100) < 0.001);
        UIScreen *external = [UIScreen new]; external.embedded = NO;
        assert([external _screenType] == -1 && [external _displayCornerRadius] == 62);
        assert([external _exclusionArea] == [NSNull null]);
        UISDisplaySingleRectShape *shape = [panel _exclusionArea];
        SBUISystemApertureLayoutMetrics *metrics = [SBUISystemApertureLayoutMetrics new];
        assert(CGRectContainsRect(shape.rect, [metrics interSensorRegionInWindowSpace]));
        assert(fabs(CGRectGetMidX(shape.rect) - 220) < 0.001);
        assert(fabs(shape.rect.origin.y * panel.nativeScale - 18) < 0.001);
        assert(fabs([metrics sensorRegionSize].height * panel.nativeScale - 84) < 0.001);
        CGSize canvas = [SBUISystemApertureLayoutMetrics _deviceNativeScreenSizeInPoints];
        assert(canvas.width == 440 && fabs(canvas.height - 1956.0 / 2) < 0.001);
        SBHFloatyFolderVisualConfiguration *folder = [SBHFloatyFolderVisualConfiguration new];
        assert([folder contentBackgroundSize].width == [folder contentBackgroundSize].height);
        TestWindowServerDisplay *externalDisplay = [TestWindowServerDisplay new];
        externalDisplay.nativeBounds = CGRectMake(0, 0, 1920, 1080);
        marble_screen_geometry_apply_display(externalDisplay);
        assert(externalDisplay.requests == 0);
        TestWindowServerDisplay *display = [TestWindowServerDisplay new];
        marble_screen_geometry_apply_display(display);
        assert(fabs(display.scale - 22.0 / 27) < 0.0001);
        assert(display.bounds.size.width == 880 && display.bounds.size.height == 1956);
        assert(marble_display_density[7] == 2);
        marble_screen_geometry_apply_display(display);
        assert(display.requests == 1);
        CGFloat iconPoints = 60, userFontPoints = 23;
        assert(fabs(iconPoints * MARBLE_UI_DISPLAY_SCALE / display.scale - 147.27272727) < 0.001);
        assert(fabs(userFontPoints * MARBLE_UI_DISPLAY_SCALE / display.scale - 56.45454545) < 0.001);
        assert(userFontPoints == 23);
        puts("PASS global compositor density, 440-point canvas, repeated display setup, physical cutout/corners, device classification and density");
    }
}
