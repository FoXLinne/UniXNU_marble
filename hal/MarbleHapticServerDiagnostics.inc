// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diagnostic builds: observe Objective-C entry/return values through the
// runtime's method API. Original implementations own their arguments/results.
#include <objc/runtime.h>
#include <objc/message.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
static _Atomic uint32_t serverDiagnostics[16];
static volatile uint32_t routeDiagnostics[32];
static char serverError[512];
static IMP serverOriginalInit, serverOriginalSetup, serverOriginalIncrement;
static void routeInspect(void *context) {
    (void)context;
    bool (*getBool)(CFStringRef) = dlsym(RTLD_DEFAULT, "MGGetBoolAnswer");
    uint32_t (*getProduct)(void) = dlsym(RTLD_DEFAULT, "MGGetProductType");
    if (getBool) {
        routeDiagnostics[0] = getBool(CFSTR("cBy4BcYs5YWtFHbBpt4C6A"));
        routeDiagnostics[1] = getBool(CFSTR("gqDnklGQnpv5ilgh5uHckw"));
        routeDiagnostics[2] = getBool(CFSTR("IsUIBuild"));
    }
    if (getProduct) routeDiagnostics[3] = getProduct();
    OSStatus (*get)(AudioObjectID, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *, void *) = dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyData");
    if (!get) return;
    CFStringRef uid = CFSTR("VirtualAudioDevice_Actuator");
    AudioObjectID device = 0; UInt32 size = sizeof(device);
    AudioObjectPropertyAddress a = {'uidd', kAudioObjectPropertyScopeGlobal, 0};
    routeDiagnostics[4] = get(1, &a, sizeof(uid), &uid, &size, &device);
    routeDiagnostics[5] = device;
    AudioValueTranslation translation = {&uid, sizeof(uid), &device, sizeof(device)};
    a.mSelector = 'duid'; size = sizeof(translation);
    routeDiagnostics[6] = get(1, &a, 0, NULL, &size, &translation);
    routeDiagnostics[7] = device;
    uid = CFSTR("com.apple.audio.CoreAudio.VirtualAudio");
    AudioObjectID plugin = 0; size = sizeof(plugin); a.mSelector = 'bidp';
    routeDiagnostics[8] = get(1, &a, sizeof(uid), &uid, &size, &plugin);
    routeDiagnostics[9] = plugin;
    if (plugin) {
        AudioObjectID devices[16] = {0}; size = sizeof(devices); a.mSelector = 'dev#';
        routeDiagnostics[10] = get(plugin, &a, 0, NULL, &size, devices);
        routeDiagnostics[11] = size;
        for (unsigned i = 0; i < size / 4 && i < 16; ++i) routeDiagnostics[12 + i] = devices[i];
    }
}
static void captureServerError(id error) {
    if (!error) return;
    long code = ((long (*)(id, SEL))objc_msgSend)(error, sel_registerName("code"));
    atomic_store(&serverDiagnostics[8], (uint32_t)code);
    id description = ((id (*)(id, SEL))objc_msgSend)(error, sel_registerName("description"));
    const char *text = ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String"));
    if (text) strlcpy(serverError, text, sizeof(serverError));
}
static id serverInit(id object, SEL selector) {
    atomic_fetch_add(&serverDiagnostics[1], 1);
    id result = ((id (*)(id, SEL))serverOriginalInit)(object, selector);
    atomic_fetch_add(&serverDiagnostics[2], 1);
    atomic_store(&serverDiagnostics[3], result != NULL);
    return result;
}
static BOOL serverSetup(id object, SEL selector) {
    atomic_fetch_add(&serverDiagnostics[4], 1);
    BOOL result = ((BOOL (*)(id, SEL))serverOriginalSetup)(object, selector);
    atomic_fetch_add(&serverDiagnostics[5], 1);
    atomic_store(&serverDiagnostics[6], result);
    Ivar ivar = class_getInstanceVariable(object_getClass(object), "_savedError");
    if (ivar) captureServerError(object_getIvar(object, ivar));
    return result;
}
static BOOL serverIncrement(id object, SEL selector, id *error) {
    atomic_fetch_add(&serverDiagnostics[9], 1);
    BOOL result = ((BOOL (*)(id, SEL, id *))serverOriginalIncrement)(object, selector, error);
    atomic_fetch_add(&serverDiagnostics[10], 1);
    atomic_store(&serverDiagnostics[11], result);
    if (error) captureServerError(*error);
    return result;
}
__attribute__((constructor)) static void serverWatch(void) {
    Class cls = objc_getClass("AVHapticServer");
    if (!cls) return;
    Method init = class_getInstanceMethod(cls, sel_registerName("init"));
    Method setup = class_getInstanceMethod(cls, sel_registerName("setupSSSClient"));
    Method increment = class_getInstanceMethod(cls, sel_registerName("incrementInit:"));
    if (!init || !setup || !increment || atomic_exchange(&serverDiagnostics[0], 1)) return;
    serverOriginalInit = method_setImplementation(init, (IMP)serverInit);
    serverOriginalSetup = method_setImplementation(setup, (IMP)serverSetup);
    serverOriginalIncrement = method_setImplementation(increment, (IMP)serverIncrement);
}
