// SPDX-License-Identifier: MIT
// Physical display idle policy from the marble iOS 27 v62 integration.
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOKitKeys.h>
#include "MarbleDisplayEvents.h"
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static IMP originalIdleUpdate;
static IMP originalIdleReset;
static id applicationActiveObserver;

static bool matchesIdleReasonMethod(Method method, char result)
{
    if (!method || method_getNumberOfArguments(method) != 3) return false;
    char type[16] = {};
    method_getReturnType(method, type, sizeof(type));
    if (type[0] != result && !(result == 'B' && type[0] == 'c')) return false;
    method_getArgumentType(method, 2, type, sizeof(type));
    return type[0] == '@' && type[1] != '?';
}

static bool isCellularServiceDisabled(void)
{
    char args[4096] = {};
    size_t size = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0)) return false;
    static const char key[] = "sheng-disable-jobs=";
    char *state = NULL;
    for (char *arg = strtok_r(args, " ", &state); arg; arg = strtok_r(NULL, " ", &state)) {
        if (strncmp(arg, key, sizeof(key)-1)) continue;
        char *jobs = NULL;
        for (char *job = strtok_r(arg+sizeof(key)-1, ",", &jobs); job; job = strtok_r(NULL, ",", &jobs))
            if (!strcmp(job, "com.apple.CommCenter")) return true;
    }
    return false;
}

static bool isSyntheticCellularRefresh(NSString *reason)
{
    return [reason isEqualToString:@"TelephonyManager:UpdateSB"];
}

static BOOL updateIdleTimer(id self, SEL command, NSString *reason)
{
    // With CommCenter disabled there is no cellular state transition to apply.
    if (isSyntheticCellularRefresh(reason)) return NO;
    return ((BOOL (*)(id, SEL, NSString *))originalIdleUpdate)(self, command, reason);
}

static void resetIdleTimer(id self, SEL command, NSString *reason)
{
    // The same notification independently resets the active idle timer.
    if (isSyntheticCellularRefresh(reason)) return;
    ((void (*)(id, SEL, NSString *))originalIdleReset)(self, command, reason);
}

static void migrateSeedIdleFlags(void)
{
    const CFStringRef app = CFSTR("com.apple.springboard");
    Boolean valid = false;
    CFIndex version = CFPreferencesGetAppIntegerValue(CFSTR("MarbleNativeIdlePolicyVersion"), app, &valid);
    if (valid && version >= 2) return;
    const CFStringRef seedFlags[] = {CFSTR("SBDisableAutoDim"), CFSTR("SBDontDimOrLockOnAC"), CFSTR("SBDisableIdleSleep")};
    for (unsigned index = 0; index < sizeof(seedFlags) / sizeof(seedFlags[0]); ++index) {
        CFPropertyListRef value = CFPreferencesCopyValue(seedFlags[index], app,
            kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        if (value == kCFBooleanTrue) {
            CFPreferencesSetValue(seedFlags[index], NULL, app,
                kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        }
        if (value) CFRelease(value);
    }
    const int epoch = 2;
    CFNumberRef marker = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &epoch);
    if (marker) {
        CFPreferencesSetValue(CFSTR("MarbleNativeIdlePolicyVersion"), marker, app,
            kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
        CFRelease(marker);
    }
    CFPreferencesSynchronize(app, kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
}

static IONotificationPortRef sideButtonPort;
static io_object_t sideButtonInterest;

static void sideButtonMessage(void *context, io_service_t service, natural_t message, void *argument)
{
    (void)context;
    (void)service;
    (void)argument;
    if (message != kMarbleDisplayWakeMessage) return;
    Class managerClass = objc_lookUpClass("SBLockScreenManager");
    SEL shared = sel_registerName("sharedInstanceIfExists");
    SEL wake = sel_registerName("requestUserAttentionScreenWakeFromSource:reason:");
    Method method = class_getInstanceMethod(managerClass, wake);
    if (!class_getClassMethod(managerClass, shared) || !method || method_getNumberOfArguments(method) != 4) return;
    char type[16] = {};
    method_getReturnType(method, type, sizeof(type));
    if (type[0] != 'v') return;
    method_getArgumentType(method, 2, type, sizeof(type));
    if (type[0] != 'q' && type[0] != 'Q' && type[0] != 'i' && type[0] != 'I') return;
    method_getArgumentType(method, 3, type, sizeof(type));
    if (type[0] != '@') return;
    id manager = ((id (*)(id, SEL))objc_msgSend)((id)managerClass, shared);
    // SpringBoard's side button uses source 1 in the lock-screen wake path.
    // Let the lock screen prepare its normal content and transition.
    const NSInteger sideButton = 1;
    if (manager) ((void (*)(id, SEL, NSInteger, NSString *))objc_msgSend)(manager, wake, sideButton,
        @"Marble side button");
}

static void observeSideButton(void)
{
    if (sideButtonInterest) return;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceNameMatching(kMarbleSideButtonServiceName));
    if (!service) return;
    sideButtonPort = IONotificationPortCreate(kIOMainPortDefault);
    if (sideButtonPort) {
        IONotificationPortSetDispatchQueue(sideButtonPort, dispatch_get_main_queue());
        if (IOServiceAddInterestNotification(sideButtonPort, service, kIOGeneralInterest,
            sideButtonMessage, NULL, &sideButtonInterest) != KERN_SUCCESS) {
            IONotificationPortDestroy(sideButtonPort);
            sideButtonPort = NULL;
        }
    }
    IOObjectRelease(service);
}

static void installIdlePolicy(void *context)
{
    (void)context;
    static bool activated;
    if (activated) return;
    activated = true;
    if (applicationActiveObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:applicationActiveObserver];
        [applicationActiveObserver release];
        applicationActiveObserver = nil;
    }
    observeSideButton();
    migrateSeedIdleFlags();
    if (!isCellularServiceDisabled()) return;
    Class coordinator = objc_lookUpClass("SBIdleTimerGlobalCoordinator");
    if (!coordinator) return;
    Method update = class_getInstanceMethod(coordinator, sel_registerName("_updateIdleTimerForReason:"));
    if (matchesIdleReasonMethod(update, 'B')) {
        originalIdleUpdate = method_getImplementation(update);
        method_setImplementation(update, (IMP)updateIdleTimer);
    }
    Method reset = class_getInstanceMethod(coordinator, sel_registerName("resetIdleTimerForReason:"));
    if (matchesIdleReasonMethod(reset, 'v')) {
        originalIdleReset = method_getImplementation(reset);
        method_setImplementation(reset, (IMP)resetIdleTimer);
    }
}

__attribute__((constructor)) static void initializePhysicalIdlePolicy(void)
{
    if (strcmp(getprogname(), "SpringBoard")) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        // Side-button notifications are independent of the idle-timer hooks.
        // Subscribe before the first lock rather than waiting for UI activity.
        observeSideButton();
        // Install after the UI is active. Loading this dylib while dyld is
        // preparing SpringBoard must leave its initial lock and display policy intact.
        applicationActiveObserver = [[[NSNotificationCenter defaultCenter]
            addObserverForName:@"UIApplicationDidBecomeActiveNotification"
            object:nil queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification *note) {
                (void)note;
                dispatch_async_f(dispatch_get_main_queue(), NULL, installIdlePolicy);
            }] retain];
        // The renderer may load the dylib after the first active notification.
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_SEC),
            dispatch_get_main_queue(), NULL, installIdlePolicy);
    });
}
