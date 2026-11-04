// SPDX-License-Identifier: MIT
// Phone UI integration for the marble display and battery services.
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static IMP originalBatteryUpdate;
static IMP originalResourceValue;
static IMP originalDisplayComposition;
static IMP originalAddDisplay;
static bool queued, installed;
static bool signature(Method method, char result, unsigned count, const char *arguments)
{
    if (!method || method_getNumberOfArguments(method) != count + 2) return false;
    char *type = method_copyReturnType(method);
    bool valid = type && type[strspn(type, "rnNoORV")] == result;
    free(type);
    for (unsigned i = 0; valid && i < count; ++i) {
        type = method_copyArgumentType(method, i + 2);
        valid = type && type[strspn(type, "rnNoORV")] == arguments[i];
        free(type);
    }
    return valid;
}

static BOOL resourceValue(id self, SEL selector, id *value, NSString *key, NSError **error)
{
    BOOL result = ((BOOL (*)(id, SEL, id *, NSString *, NSError **))originalResourceValue)(self, selector, value, key, error);
    if (result && value && !*value && [key isEqualToString:NSURLFileProtectionKey]) {
        NSNumber *encrypted = nil;
        BOOL known = ((BOOL (*)(id, SEL, id *, NSString *, NSError **))originalResourceValue)(self,
            selector, (id *)&encrypted, NSURLVolumeIsEncryptedKey, NULL);
        if (known && [encrypted isKindOfClass:[NSNumber class]] && !encrypted.boolValue)
            *value = NSURLFileProtectionNone;
    }
    return result;
}

static void installFileProtection(void)
{
    // An unencrypted volume has no file protection even when its resource value is absent.
    Method method = class_getInstanceMethod([NSURL class], sel_registerName("getResourceValue:forKey:error:"));
    if (signature(method, 'B', 3, "^@^"))
        originalResourceValue = method_setImplementation(method, (IMP)resourceValue);
}

static void displayComposition(id self, SEL selector, BOOL enabled)
{
    (void)enabled;
    ((void (*)(id, SEL, BOOL))originalDisplayComposition)(self, selector, NO);
}

static void addDisplay(id self, SEL selector, id display)
{
    ((void (*)(id, SEL, BOOL))objc_msgSend)(display, sel_registerName("setAllowsDisplayCompositing:"), NO);
    ((void (*)(id, SEL, id))originalAddDisplay)(self, selector, display);
}

static void configureDisplays(void *context)
{
    (void)context;
    @autoreleasepool {
        id server = ((id (*)(id, SEL))objc_msgSend)((id)objc_lookUpClass("CAWindowServer"),
            sel_registerName("serverIfRunning"));
        NSArray *displays = ((id (*)(id, SEL))objc_msgSend)(server, sel_registerName("displays"));
        for (id display in displays)
            ((void (*)(id, SEL, BOOL))objc_msgSend)(display,
                sel_registerName("setAllowsDisplayCompositing:"), NO);
    }
}

static void installComposition(void)
{
    // The virtual framebuffer consumes one surface composed by the render server.
    Class display = objc_lookUpClass("CAWindowServerDisplay");
    Class server = objc_lookUpClass("CAWindowServer");
    Method composition = class_getInstanceMethod(display, sel_registerName("setAllowsDisplayCompositing:"));
    Method add = class_getInstanceMethod(server, sel_registerName("addDisplay:"));
    if (!signature(composition, 'v', 1, "B") || !signature(add, 'v', 1, "@") ||
        !signature(class_getClassMethod(server, sel_registerName("serverIfRunning")), '@', 0, "") ||
        !signature(class_getInstanceMethod(server, sel_registerName("displays")), '@', 0, "")) return;
    originalDisplayComposition = method_setImplementation(composition, (IMP)displayComposition);
    originalAddDisplay = method_setImplementation(add, (IMP)addDisplay);
    dispatch_async_f(dispatch_get_main_queue(), NULL, configureDisplays);
}

static id batteryUpdate(id self, SEL selector, id update, id item)
{
    id result = ((id (*)(id, SEL, id, id))originalBatteryUpdate)(self, selector, update, item);
    id battery = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("batteryView"));
    ((void (*)(id, SEL, BOOL))objc_msgSend)(battery, sel_registerName("setShowsPercentage:"), YES);
    return result;
}

static unsigned photoInstallState;
static IMP originalPhotoTiling, originalPhotoUpdate;

static void photoTiling(id self, SEL selector, BOOL enabled)
{
    (void)enabled;
    ((void (*)(id, SEL, BOOL))originalPhotoTiling)(self, selector, NO);
}

static char photoRefreshKey;

static id photoObject(id object, const char *selector)
{
    return ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName(selector));
}

static BOOL photoActive(id controller)
{
    return ((BOOL (*)(id, SEL))objc_msgSend)(controller, sel_registerName("isActive")) &&
        photoObject(photoObject(controller, "view"), "window") != nil;
}

static void photoUpdate(id self, SEL selector)
{
    if (((BOOL (*)(id, SEL))objc_msgSend)(self, sel_registerName("canUseFullSizeTiledLayer"))) {
        ((void (*)(id, SEL, BOOL))originalPhotoTiling)(self, sel_registerName("setCanUseFullSizeTiledLayer:"), NO);
        ((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("_invalidateImage"));
    }
    ((void (*)(id, SEL))originalPhotoUpdate)(self, selector);
    id asset = photoObject(self, "asset");
    if (!asset || !photoActive(self)) {
        objc_setAssociatedObject(self, &photoRefreshKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return;
    }
    id requester = photoObject(self, "_imageRequester");
    if (photoObject(requester, "image") || photoObject(requester, "error")) return;
    NSArray *previous = objc_getAssociatedObject(self, &photoRefreshKey);
    if (previous.firstObject == asset) return;
    NSArray *ticket = @[asset];
    objc_setAssociatedObject(self, &photoRefreshKey, ticket, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (objc_getAssociatedObject(self, &photoRefreshKey) != ticket ||
            photoObject(self, "asset") != asset || !photoActive(self)) return;
        id pending = photoObject(self, "_imageRequester");
        if (photoObject(pending, "image") || photoObject(pending, "error")) return;
        ((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("_invalidateImage"));
        photoUpdate(self, selector);
    });
}

static void installPhotoRendering(void)
{
    if (strcmp(getprogname(), "MobileSlideShow")) return;
    unsigned expected = 0;
    if (!__atomic_compare_exchange_n(&photoInstallState, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    // PhotoKit supplies decoded images for the single-surface display backend.
    Class controller = objc_lookUpClass("PUImageTileViewController");
    Method method = class_getInstanceMethod(controller, sel_registerName("setCanUseFullSizeTiledLayer:"));
    Method update = class_getInstanceMethod(controller, sel_registerName("_updateIfNeeded"));
    if (!signature(method, 'v', 1, "B") || !signature(update, 'v', 0, "") ||
        !signature(class_getInstanceMethod(controller, sel_registerName("canUseFullSizeTiledLayer")), 'B', 0, "") ||
        !signature(class_getInstanceMethod(controller, sel_registerName("_invalidateImage")), 'v', 0, "") ||
        !signature(class_getInstanceMethod(controller, sel_registerName("isActive")), 'B', 0, "") ||
        !signature(class_getInstanceMethod(controller, sel_registerName("asset")), '@', 0, "") ||
        !signature(class_getInstanceMethod(controller, sel_registerName("_imageRequester")), '@', 0, "") ||
        !signature(class_getInstanceMethod(controller, sel_registerName("view")), '@', 0, "")) {
        __atomic_store_n(&photoInstallState, 0u, __ATOMIC_RELEASE);
        return;
    }
    originalPhotoTiling = method_setImplementation(method, (IMP)photoTiling);
    originalPhotoUpdate = method_setImplementation(update, (IMP)photoUpdate);
    __atomic_store_n(&photoInstallState, 2u, __ATOMIC_RELEASE);
}

static void install(void *context)
{
    (void)context;
    installPhotoRendering();
    __atomic_store_n(&queued, false, __ATOMIC_RELEASE);
    if (installed) return;
    Class item = objc_lookUpClass("_UIStatusBarBatteryItem"), view = objc_lookUpClass("_UIBatteryView");
    Method update = class_getInstanceMethod(item, sel_registerName("applyUpdate:toDisplayItem:"));
    if (!signature(update, '@', 2, "@@") ||
        !signature(class_getInstanceMethod(item, sel_registerName("batteryView")), '@', 0, "") ||
        !signature(class_getInstanceMethod(view, sel_registerName("setShowsPercentage:")), 'v', 1, "B")) return;
    originalBatteryUpdate = method_setImplementation(update, (IMP)batteryUpdate);
    installed = true;
}

static void imageAdded(const struct mach_header *header, intptr_t slide)
{
    (void)header; (void)slide;
    installPhotoRendering();
    bool expected = false;
    if (__atomic_compare_exchange_n(&queued, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        dispatch_async_f(dispatch_get_main_queue(), NULL, install);
}

__attribute__((constructor)) static void initialize(void)
{
    installComposition();
    installPhotoRendering();
    if (!strcmp(getprogname(), "PosterBoard")) installFileProtection();
    char product[64] = {};
    size_t bytes = sizeof(product);
    int result = sysctlbyname("hw.product", product, &bytes, NULL, 0);
    if (result || strcmp(product, "iPhone10,3")) return;
    _dyld_register_func_for_add_image(imageAdded);
}
