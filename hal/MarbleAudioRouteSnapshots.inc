// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diagnostic builds retain five minutes of HAL route state in process memory.
// Queries run on a separate serial queue, outside the driver's IO mutex.
#include <dispatch/dispatch.h>
enum { kAudioRouteRows = 64, kAudioRouteWords = 128, kAudioRouteDeviceWords = 16 };
typedef OSStatus (*AudioRouteGet)(AudioObjectID, const AudioObjectPropertyAddress *,
    UInt32, const void *, UInt32 *, void *);
static AudioRouteGet audioRouteGet;
static dispatch_queue_t audioRouteQueue;
static _Atomic uint32_t audioRouteSnapshotCount;
static _Atomic uint32_t audioRouteSnapshots[kAudioRouteRows][kAudioRouteWords];
// An odd sequence means a HAL query is in progress; retain its address if it waits.
static _Atomic uint32_t audioRouteQuery[6];
static OSStatus audioRouteRead(AudioObjectID object, UInt32 selector, UInt32 scope,
    UInt32 qualifierSize, const void *qualifier, UInt32 *size, void *value) {
    const AudioObjectPropertyAddress a = {selector, scope, 0};
    if (!object) return kAudioHardwareBadObjectError;
    uint32_t sequence = atomic_load(&audioRouteQuery[0]);
    atomic_store(&audioRouteQuery[1], object); atomic_store(&audioRouteQuery[2], selector);
    atomic_store(&audioRouteQuery[3], scope);
    atomic_store(&audioRouteQuery[4], (uint32_t)(mach_absolute_time() >> 24));
    atomic_store(&audioRouteQuery[0], sequence + 1);
    OSStatus status = audioRouteGet(object, &a, qualifierSize, qualifier, size, value);
    atomic_store(&audioRouteQuery[5], (uint32_t)status);
    atomic_store(&audioRouteQuery[0], sequence + 2);
    return status;
}
static OSStatus audioRouteWord(AudioObjectID object, UInt32 selector, UInt32 scope, UInt32 *value) {
    UInt32 size = sizeof(*value);
    *value = UINT32_MAX;
    OSStatus status = audioRouteRead(object, selector, scope, 0, NULL, &size, value);
    return status ? status : size == sizeof(*value) ? 0 : kAudioHardwareBadPropertySizeError;
}
static OSStatus audioRouteUID(CFStringRef uid, AudioObjectID *object) {
    UInt32 size = sizeof(*object);
    *object = 0;
    return audioRouteRead(1, 'uidd', kAudioObjectPropertyScopeGlobal, sizeof(uid), &uid, &size, object);
}
static void audioRouteStreams(AudioObjectID object, UInt32 scope, uint32_t *row) {
    AudioObjectID streams[8] = {0}; UInt32 size = sizeof(streams);
    row[0] = audioRouteRead(object, 'stm#', scope, 0, NULL, &size, streams);
    if (row[0]) return;
    row[1] = size / sizeof(streams[0]);
    row[2] = 0; row[3] = 0;
    for (unsigned i = 0; i < row[1] && i < 8; ++i) {
        UInt32 active = 0;
        OSStatus status = audioRouteWord(streams[i], 'sact', kAudioObjectPropertyScopeGlobal, &active);
        if (status && !row[2]) row[2] = status;
        if (!status && active) row[3] |= 1u << i;
    }
}
static void audioRouteDevice(CFStringRef uid, uint32_t *row) {
    AudioObjectID object = 0;
    row[0] = audioRouteUID(uid, &object); row[1] = object;
    if (row[0] || !object) return;
    row[2] = audioRouteWord(object, 'livn', kAudioObjectPropertyScopeGlobal, &row[3]);
    row[4] = audioRouteWord(object, 'goin', kAudioObjectPropertyScopeGlobal, &row[5]);
    row[6] = audioRouteWord(object, 'oink', kAudioObjectPropertyScopeGlobal, &row[7]);
    audioRouteStreams(object, kAudioObjectPropertyScopeOutput, row + 8);
    audioRouteStreams(object, kAudioObjectPropertyScopeInput, row + 12);
}
static void audioRoutePorts(AudioObjectID plugin, UInt32 selector, uint32_t *row) {
    CFArrayRef ports = NULL; UInt32 size = sizeof(ports);
    row[0] = audioRouteRead(plugin, selector, kAudioObjectPropertyScopeOutput, 0, NULL, &size, &ports);
    if (!row[0] && ports) {
        if (CFGetTypeID(ports) == CFArrayGetTypeID()) row[1] = (uint32_t)CFArrayGetCount(ports);
        else row[0] = kAudioHardwareBadPropertySizeError;
    }
    if (ports) CFRelease(ports);
}
static void audioRouteActivePorts(AudioObjectID plugin, uint32_t *row) {
    CFArrayRef routes = NULL; UInt32 size = sizeof(routes);
    row[18] = audioRouteRead(plugin, 'aprc', kAudioObjectPropertyScopeOutput, 0, NULL, &size, &routes);
    if (!row[18] && routes && CFGetTypeID(routes) == CFArrayGetTypeID()) {
        row[19] = 0; row[22] = 0; row[23] = 0;
        for (CFIndex i = 0; i < CFArrayGetCount(routes); ++i) {
            CFDictionaryRef route = CFArrayGetValueAtIndex(routes, i);
            if (!route || CFGetTypeID(route) != CFDictionaryGetTypeID()) continue;
            CFStringRef uid = CFDictionaryGetValue(route, CFSTR("device UID"));
            CFArrayRef ports = CFDictionaryGetValue(route, CFSTR("output ports"));
            if (!uid || CFGetTypeID(uid) != CFStringGetTypeID() || !CFEqual(uid, CFSTR("VirtualAudioDevice_Default")) ||
                !ports || CFGetTypeID(ports) != CFArrayGetTypeID()) continue;
            row[19] = (uint32_t)CFArrayGetCount(ports);
            for (CFIndex j = 0; j < CFArrayGetCount(ports) && j < 2; ++j) {
                CFNumberRef number = CFArrayGetValueAtIndex(ports, j); SInt32 port = 0;
                if (number && CFGetTypeID(number) == CFNumberGetTypeID() && CFNumberGetValue(number, kCFNumberSInt32Type, &port))
                    audioRouteWord((AudioObjectID)port, 'ptyp', kAudioObjectPropertyScopeGlobal, &row[22 + j]);
            }
            break;
        }
    } else if (!row[18]) row[18] = kAudioHardwareBadPropertySizeError;
    if (routes) CFRelease(routes);
}
static void audioRouteSample(void) {
    if (!audioRouteGet) return;
    uint32_t row[kAudioRouteWords];
    for (unsigned i = 0; i < kAudioRouteWords; ++i) row[i] = UINT32_MAX;
    uint64_t now = mach_absolute_time();
    row[1] = (uint32_t)now; row[2] = (uint32_t)(now >> 32); row[3] = getpid();
    row[4] = atomic_load(&diagnostics[15]); row[5] = atomic_load(&diagnostics[16]);
    row[6] = atomic_load(&diagnostics[17]); row[7] = atomic_load(&diagnostics[18]);
    row[8] = audioRouteWord(1, 'dOut', kAudioObjectPropertyScopeGlobal, &row[9]);
    CFStringRef bundle = CFSTR("com.apple.audio.CoreAudio.VirtualAudio");
    AudioObjectID plugin = 0; UInt32 size = sizeof(plugin);
    row[10] = audioRouteRead(1, 'bidp', kAudioObjectPropertyScopeGlobal, sizeof(bundle), &bundle, &size, &plugin);
    row[11] = plugin;
    uint64_t listener[2] = {0}; size = sizeof(listener);
    row[12] = audioRouteRead(plugin, 'srzl', kAudioObjectPropertyScopeGlobal, 0, NULL, &size, listener);
    row[13] = listener[0] || listener[1];
    row[14] = audioRouteWord(plugin, 'rctx', kAudioObjectPropertyScopeGlobal, &row[15]);
    row[16] = audioRouteWord(plugin, 'inpm', kAudioObjectPropertyScopeGlobal, &row[17]);
    audioRouteActivePorts(plugin, row);
    audioRoutePorts(plugin, 'prts', row + 20);
    CFStringRef uids[] = {CFSTR("Speaker"), CFSTR("Actuator"), CFSTR("Hawking"), CFSTR("Codec"),
        CFSTR("VirtualAudioDevice_Default"), CFSTR("VirtualAudioDevice_Actuator")};
    for (unsigned i = 0; i < 6; ++i) audioRouteDevice(uids[i], row + 32 + i * kAudioRouteDeviceWords);
    uint32_t n = atomic_load(&audioRouteSnapshotCount);
    _Atomic uint32_t *destination = audioRouteSnapshots[n % kAudioRouteRows];
    atomic_store(&destination[0], n * 2 + 1);
    for (unsigned i = 1; i < kAudioRouteWords; ++i) atomic_store_explicit(&destination[i], row[i], memory_order_relaxed);
    atomic_store(&destination[0], n * 2 + 2);
    atomic_store(&audioRouteSnapshotCount, n + 1);
}
static void audioRouteTick(void *context) {
    (void)context;
    audioRouteSample();
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), audioRouteQueue, NULL, audioRouteTick);
}
static void audioRouteWatch(void) {
    static atomic_bool started;
    if (atomic_exchange(&started, true)) return;
    audioRouteGet = dlsym(RTLD_DEFAULT, "AudioObjectGetPropertyData");
    if (!audioRouteGet) return;
    audioRouteQueue = dispatch_queue_create("org.unixnu.marble.audio.route-observer", DISPATCH_QUEUE_SERIAL);
    if (!audioRouteQueue) return;
    mlock((const void *)audioRouteSnapshots, sizeof(audioRouteSnapshots));
    mlock((const void *)&audioRouteSnapshotCount, sizeof(audioRouteSnapshotCount));
    mlock((const void *)audioRouteQuery, sizeof(audioRouteQuery));
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), audioRouteQueue, NULL, audioRouteTick);
}
