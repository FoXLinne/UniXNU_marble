// SPDX-License-Identifier: BSD-2-Clause-Patent
// AudioServerPlugIn ABI from Apple's CoreAudio headers. The Actuator UID is
// consumed by 22H384 VirtualAudio; PCM reaches the board through a versioned
// character-device protocol. Speaker and microphone routes belong to their
// respective board drivers. Diagnostic companion endpoints exercise the N841
// VirtualAudio initialization dependency while board audio is brought up.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <os/log.h>
#include "MarbleHapticsAbi.h"
#include "audio/MarbleSpeakerAbi.h"

enum { kPlugin = kAudioObjectPlugInObject, kActuator = 2, kStream = 3, kSpeaker = 4, kSpeakerOut = 5, kSpeakerIn = 6, kHawking = 7, kHawkingIn = 8, kSpeakerVolume = 9, kCodec = 10, kCodecIn = 11, kMicSource = 12, kPeriod = 16384 };
static AudioServerPlugInHostRef host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint references = 1;
static _Atomic unsigned users;
static uint64_t anchor, clockSeed = 1;
static double ticksPerFrame;
static int endpoint = -1;
static int speakerEndpoint = -1;
static _Atomic float speakerVolume = 1.0f;
static _Atomic Boolean streamInactive[13];
// Live diagnostics: initialized, open errno, StartIO calls, writes, frames,
// last write errno, nonzero samples, StopIO calls. Read through firmware memory.
// Speaker: [15] StartIO, [16] StopIO, [17] users, [18] last StartIO errno,
// [19] last write time (mach >> 24), [20] IsActive sets, [21] last object << 16 | value,
// [22] zero time stamps, [23] last period, [24] WillDo, [25] Begin/End, [26] DoIO calls,
// [27] last DoIO, [28] last zero time stamp, [29] last StartIO, [30] last StopIO (mach >> 24), [31] last Begin/End operation.
static _Atomic uint32_t diagnostics[32];
static uint32_t propertyTrace[256][4];
static unsigned propertyTraceCount;
#ifdef MARBLE_HAPTIC_SERVER_DIAGNOSTICS
#include "MarbleHapticServerDiagnostics.inc"
#endif
#if defined(MARBLE_HAPTIC_SERVER_DIAGNOSTICS) || defined(MARBLE_AUDIO_ROUTE_SNAPSHOTS)
#include "MarbleAudioRouteSnapshots.inc"
#endif
#ifdef MARBLE_AUDIO_ROUTE_TRACE
static void routingWatch(void);
#endif

static unsigned companionUsers[13];
static uint64_t companionAnchor[13], companionSeed[13];
static unsigned deviceFor(unsigned object) {
    if (object == kActuator || object == kStream) return kActuator;
    if ((object >= kSpeaker && object <= kSpeakerIn) || object == kSpeakerVolume) return kSpeaker;
    if (object == kHawking || object == kHawkingIn) return kHawking;
    if (object == kCodec || object == kCodecIn || object == kMicSource) return kCodec;
    return 0;
}
static CFStringRef deviceUID(unsigned device) {
#ifdef MARBLE_SPEAKER_THUNDERBOLT
    if (device == kSpeaker) return CFSTR("MarbleSpeakerDevice");
#endif
#ifdef MARBLE_VIRTUAL_ACTUATOR
    if (device == kActuator) return CFSTR("VirtualAudioDevice_Actuator");
#endif
    return device == kActuator ? CFSTR("Actuator") : device == kSpeaker ? CFSTR("Speaker") : device == kCodec ? CFSTR("Codec") : CFSTR("Hawking");
}
static unsigned outputFor(unsigned device) { return device == kActuator ? kStream : device == kSpeaker ? kSpeakerOut : 0; }
static unsigned inputFor(unsigned device) { return device == kSpeaker ? kSpeakerIn : device == kHawking ? kHawkingIn : device == kCodec ? kCodecIn : 0; }
static unsigned channelsFor(unsigned object) { return deviceFor(object) == kSpeaker ? 2 : 1; }
static AudioStreamBasicDescription formatFor(unsigned object) {
    unsigned channels = channelsFor(object);
    Boolean actuator = deviceFor(object) == kActuator;
    unsigned bytes = actuator ? 4 : 2;
    return (AudioStreamBasicDescription){ .mSampleRate = MARBLE_HAPTICS_RATE,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = (actuator ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = bytes * channels, .mFramesPerPacket = 1, .mBytesPerFrame = bytes * channels,
        .mChannelsPerFrame = channels, .mBitsPerChannel = bytes * 8 };
}
static int openEndpoint(void) {
    if (endpoint < 0) endpoint = open(MARBLE_HAPTICS_DEVICE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    int result = endpoint < 0 ? errno : 0;
    atomic_store(&diagnostics[1], (uint32_t)result);
    return result;
}
static int speakerCommand(unsigned flags) {
    MarbleHapticsPacket packet = {.magic = MARBLE_SPEAKER_MAGIC, .version = 1, .flags = flags, .rate = MARBLE_SPEAKER_RATE};
    return write(speakerEndpoint, &packet, sizeof(packet)) == sizeof(packet) ? 0 : errno ? errno : EIO;
}
static int command(unsigned flags) {
    MarbleHapticsPacket packet = { .magic = MARBLE_HAPTICS_MAGIC, .version = MARBLE_HAPTICS_VERSION,
        .flags = flags, .rate = MARBLE_HAPTICS_RATE };
    return write(endpoint, &packet, sizeof(packet)) == sizeof(packet) ? 0 : errno ? errno : EIO;
}
static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out) {
    if (!out) return E_POINTER;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    Boolean matches = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(requested);
    *out = matches ? self : NULL;
    if (matches) atomic_fetch_add(&references, 1);
    return matches ? S_OK : E_NOINTERFACE;
}
static ULONG AddRef(void *self) { (void)self; return atomic_fetch_add(&references, 1) + 1; }
static ULONG Release(void *self) {
    (void)self;
    unsigned value = atomic_load(&references);
    while (value > 1 && !atomic_compare_exchange_weak(&references, &value, value - 1)) {}
    return value > 1 ? value - 1 : value;
}
static OSStatus Initialize(AudioServerPlugInDriverRef self, AudioServerPlugInHostRef newHost) {
    (void)self;
#ifdef MARBLE_HAPTIC_SERVER_DIAGNOSTICS
    serverWatch();
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC),
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), NULL, routeInspect);
#endif
#ifdef MARBLE_AUDIO_ROUTE_TRACE
    routingWatch();
#endif
    // Keep the image header and diagnostics resident for firmware USB capture.
    Dl_info image;
    if (dladdr((const void *)Initialize, &image) && image.dli_fbase) mlock(image.dli_fbase, 4096);
    mlock((const void *)diagnostics, sizeof(diagnostics));
    mach_timebase_info_data_t timebase;
    if (mach_timebase_info(&timebase)) return kAudioHardwareUnspecifiedError;
    pthread_mutex_lock(&lock);
    host = newHost;
    ticksPerFrame = 1e9 * timebase.denom / timebase.numer / MARBLE_HAPTICS_RATE;
    int result = openEndpoint();
    atomic_fetch_add(&diagnostics[0], 1);
    pthread_mutex_unlock(&lock);
#if defined(MARBLE_HAPTIC_SERVER_DIAGNOSTICS) || defined(MARBLE_AUDIO_ROUTE_SNAPSHOTS)
    audioRouteWatch();
#endif
    os_log_error(OS_LOG_DEFAULT, "MarbleHaptics: HAL initialized, endpoint result %d", result);
    return 0; // Device discovery precedes StartIO; StartIO retries the endpoint.
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
    const AudioServerPlugInClientInfo *c, AudioObjectID *o) {
    (void)d; (void)desc; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError;
}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) {
    (void)d; (void)o; return kAudioHardwareUnsupportedOperationError;
}
static OSStatus DeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {
    (void)d; (void)c; return deviceFor(o) && o == deviceFor(o) ? 0 : kAudioHardwareBadObjectError;
}
static OSStatus ConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 action, void *info) {
    (void)d; (void)action; (void)info; return deviceFor(o) && o == deviceFor(o) ? 0 : kAudioHardwareBadObjectError;
}

typedef struct {
    UInt32 size;
    Boolean found, string;
    union { UInt32 words[4]; Float64 number; CFStringRef text; AudioChannelLayout layout;
        Float32 scalar; AudioValueRange range; AudioStreamBasicDescription format; AudioStreamRangedDescription ranged; } data;
} Value;
static Value property(AudioObjectID object, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q) {
    Value v = {0};
    if (a) {
        unsigned i;
        for (i = 0; i < propertyTraceCount; ++i)
            if (propertyTrace[i][0] == object && propertyTrace[i][1] == a->mSelector && propertyTrace[i][2] == a->mScope) break;
        if (i == propertyTraceCount && i < 256) {
            propertyTrace[i][0] = object; propertyTrace[i][1] = a->mSelector;
            propertyTrace[i][2] = a->mScope; propertyTraceCount = i + 1;
        }
        if (i < 256) ++propertyTrace[i][3];
    }
    unsigned device = deviceFor(object);
    Boolean isDevice = device && object == device;
    if (!a || (object != kPlugin && !device)) return v;
#define WORD(x) do { v.found = true; v.size = sizeof(UInt32); v.data.words[0] = (x); } while (0)
#define TEXT(x) do { v.found = true; v.string = true; v.size = sizeof(CFStringRef); v.data.text = CFSTR(x); } while (0)
#define EMPTY() do { v.found = true; v.size = 0; } while (0)
    if (a->mSelector == kAudioObjectPropertyCustomPropertyInfoList ||
        (a->mSelector == 'taps' && device && !isDevice)) { EMPTY(); return v; }
    if (object == kMicSource) {
        v.found = true;
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: WORD(kAudioSelectorControlClassID); break;
            case kAudioObjectPropertyClass: WORD(kAudioDataSourceControlClassID); break;
            case kAudioObjectPropertyOwner: WORD(kCodec); break;
            case kAudioObjectPropertyOwnedObjects: EMPTY(); break;
            case kAudioControlPropertyScope: WORD(kAudioObjectPropertyScopeInput); break;
            case kAudioControlPropertyElement: WORD(0); break;
            case kAudioSelectorControlPropertyCurrentItem:
            case kAudioSelectorControlPropertyAvailableItems: WORD('imic'); break;
            case kAudioSelectorControlPropertyItemName: TEXT("Clock reference input"); break;
            case kAudioSelectorControlPropertyItemKind: WORD(0); break;
            default: v.found = false; break;
        }
        return v;
    }
    if (object == kSpeakerVolume) {
        v.found = true;
        switch (a->mSelector) {
            case kAudioObjectPropertyBaseClass: WORD(kAudioLevelControlClassID); break;
            case kAudioObjectPropertyClass: WORD(kAudioVolumeControlClassID); break;
            case kAudioObjectPropertyOwner: WORD(kSpeaker); break;
            case kAudioObjectPropertyOwnedObjects: EMPTY(); break;
            case kAudioControlPropertyScope: WORD(kAudioObjectPropertyScopeOutput); break;
            case kAudioControlPropertyElement: WORD(0); break;
            case kAudioLevelControlPropertyScalarValue: v.size = 4; v.data.scalar = speakerVolume; break;
            case kAudioLevelControlPropertyDecibelValue: v.size = 4; v.data.scalar = speakerVolume > 0 ? 20 * log10f(speakerVolume) : -48; break;
            case kAudioLevelControlPropertyDecibelRange: v.size = sizeof(AudioValueRange); v.data.range = (AudioValueRange){-48, 0}; break;
            default: v.found = false; break;
        }
        return v;
    }
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: WORD(kAudioObjectClassID); return v;
        case kAudioObjectPropertyClass:
            WORD(object == kPlugin ? kAudioPlugInClassID : isDevice ? kAudioDeviceClassID : kAudioStreamClassID); return v;
        case kAudioObjectPropertyOwner: WORD(object == kPlugin ? kAudioObjectUnknown : isDevice ? kPlugin : device); return v;
        case kAudioObjectPropertyManufacturer: TEXT("Marble"); return v;
        case kAudioObjectPropertyName: v.found = v.string = true; v.size = sizeof(CFStringRef); v.data.text = device == kSpeaker ? CFSTR("Speaker") : deviceUID(device); return v;
    }
    if (object == kPlugin) {
        switch (a->mSelector) {
            case kAudioObjectPropertyOwnedObjects:
            case kAudioPlugInPropertyDeviceList: WORD(kActuator); v.data.words[1] = kSpeaker; v.data.words[2] = kHawking; v.data.words[3] = kCodec; v.size = 16; break;
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyClockDeviceList: EMPTY(); break;
            case kAudioPlugInPropertyTranslateUIDToDevice:
                WORD(kAudioObjectUnknown);
                if (qs == sizeof(CFStringRef) && q && *(CFStringRef const *)q) {
                    unsigned devices[] = {kActuator, kSpeaker, kHawking, kCodec};
                    for (unsigned i = 0; i < 4; ++i) if (CFEqual(*(CFStringRef const *)q, deviceUID(devices[i]))) v.data.words[0] = devices[i];
                }
                break;
            case kAudioPlugInPropertyTranslateUIDToBox:
            case kAudioPlugInPropertyTranslateUIDToClockDevice: WORD(kAudioObjectUnknown); break;
            case kAudioPlugInPropertyResourceBundle: TEXT(""); break;
        }
    } else if (isDevice) {
        switch (a->mSelector) {
            case kAudioDevicePropertyDeviceUID:
                v.found = v.string = true; v.size = sizeof(CFStringRef); v.data.text = deviceUID(device); break;
            case kAudioDevicePropertyModelUID:
                TEXT("MarbleAudioModel"); break;
            case kAudioDevicePropertyTransportType:
#ifdef MARBLE_SPEAKER_THUNDERBOLT
                if (device == kSpeaker) { WORD(kAudioDeviceTransportTypeThunderbolt); break; }
#endif
#ifdef MARBLE_VIRTUAL_ACTUATOR
                WORD(device == kActuator ? kAudioDeviceTransportTypeVirtual : kAudioDeviceTransportTypeBuiltIn); break;
#else
                WORD(kAudioDeviceTransportTypeBuiltIn); break;
#endif
            case 'accs':
            case 'auto': WORD(0); break;
            case kAudioDevicePropertyConfigurationApplication: TEXT("com.apple.Preferences"); break;
            case kAudioObjectPropertyModelName:
                v.found = v.string = true; v.size = sizeof(CFStringRef); v.data.text = device == kActuator ? CFSTR("Actuator") : CFSTR("Speaker"); break;
            case kAudioObjectPropertyIdentify: WORD(0); break;
            case kAudioObjectPropertyElementName: TEXT(""); break;
            case kAudioObjectPropertySerialNumber: TEXT("marble"); break;
            case kAudioObjectPropertyFirmwareVersion: TEXT("1.0"); break;
            case kAudioDevicePropertyRelatedDevices: WORD(device); break;
            case kAudioObjectPropertyOwnedObjects:
            case kAudioDevicePropertyStreams:
                EMPTY();
                if (a->mScope != kAudioObjectPropertyScopeInput && outputFor(device)) { v.data.words[v.size / 4] = outputFor(device); v.size += 4; }
                if (a->mScope != kAudioObjectPropertyScopeOutput && inputFor(device)) { v.data.words[v.size / 4] = inputFor(device); v.size += 4; }
                if (a->mSelector == kAudioObjectPropertyOwnedObjects && device == kSpeaker && a->mScope != kAudioObjectPropertyScopeInput) {
                    v.data.words[v.size / 4] = kSpeakerVolume; v.size += 4;
                }
                if (a->mSelector == kAudioObjectPropertyOwnedObjects && device == kCodec && a->mScope != kAudioObjectPropertyScopeOutput) {
                    v.data.words[v.size / 4] = kMicSource; v.size += 4;
                }
                break;
            case kAudioObjectPropertyControlList:
                if (device == kSpeaker) WORD(kSpeakerVolume); else if (device == kCodec) WORD(kMicSource); else EMPTY(); break;
            case kAudioDevicePropertyDeviceIsAlive: WORD(1); break;
            case kAudioDevicePropertyDeviceIsRunning: WORD(device == kActuator ? users != 0 : companionUsers[device] != 0); break;
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                WORD(device != kActuator &&
                    (a->mScope == kAudioObjectPropertyScopeInput ? inputFor(device) != 0 : outputFor(device) != 0)); break;
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyIsHidden: WORD(0); break;
            case kAudioDevicePropertySafetyOffset: WORD(512); break;
            case kAudioDevicePropertyClockIsStable: WORD(1); break;
            case kAudioDevicePropertyClockAlgorithm: WORD(kAudioDeviceClockAlgorithmRaw); break;
            case kAudioDevicePropertyZeroTimeStampPeriod: WORD(kPeriod); break;
            case kAudioDevicePropertyNominalSampleRate:
                v.found = true; v.size = sizeof(Float64); v.data.number = MARBLE_HAPTICS_RATE; break;
            case kAudioDevicePropertyAvailableNominalSampleRates:
                v.found = true; v.size = sizeof(AudioValueRange);
                v.data.range = (AudioValueRange){ MARBLE_HAPTICS_RATE, MARBLE_HAPTICS_RATE }; break;
            case kAudioDevicePropertyPreferredChannelsForStereo:
                v.found = true; v.size = 8; v.data.words[0] = 1; v.data.words[1] = device == kSpeaker ? 2 : 1; break;
            case kAudioDevicePropertyPreferredChannelLayout:
                v.found = true; v.size = sizeof(AudioChannelLayout);
                v.data.layout.mChannelLayoutTag = device == kSpeaker ? kAudioChannelLayoutTag_Stereo : kAudioChannelLayoutTag_UseChannelDescriptions;
                if (device != kSpeaker) {
                    v.data.layout.mNumberChannelDescriptions = 1;
                    v.data.layout.mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Mono;
                }
                break;
        }
    } else {
        switch (a->mSelector) {
            case kAudioObjectPropertyOwnedObjects: EMPTY(); break;
            case kAudioStreamPropertyIsActive: WORD(!streamInactive[object]); break;
            case kAudioStreamPropertyDirection: WORD(object == inputFor(device)); break;
            case kAudioStreamPropertyTerminalType: WORD(object == inputFor(device) ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker); break;
            case kAudioStreamPropertyStartingChannel: WORD(1); break;
            case kAudioStreamPropertyLatency: WORD(0); break;
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                v.found = true; v.size = sizeof(AudioStreamBasicDescription); v.data.format = formatFor(object); break;
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:
                v.found = true; v.size = sizeof(AudioStreamRangedDescription);
                v.data.ranged.mFormat = formatFor(object);
                v.data.ranged.mSampleRateRange = (AudioValueRange){ MARBLE_HAPTICS_RATE, MARBLE_HAPTICS_RATE }; break;
        }
    }
#undef WORD
#undef TEXT
#undef EMPTY
    return v;
}
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)d; (void)pid; return property(o, a, 0, NULL).found;
}
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)d; (void)pid;
    if (!a || !out) return kAudioHardwareIllegalOperationError;
    if (!property(o, a, 0, NULL).found) return kAudioHardwareUnknownPropertyError;
    if (o == kSpeakerVolume) {
        *out = a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue;
        return 0;
    }
    *out = (o == deviceFor(o) && a->mSelector == kAudioDevicePropertyNominalSampleRate) ||
        (deviceFor(o) && o != deviceFor(o) && (a->mSelector == kAudioStreamPropertyIsActive ||
         a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat));
    return 0;
}
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *size) {
    (void)d; (void)pid;
    if (!size) return kAudioHardwareIllegalOperationError;
    Value v = property(o, a, qs, q);
    if (!v.found) return kAudioHardwareUnknownPropertyError;
    *size = v.size; return 0;
}
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 capacity, UInt32 *size, void *data) {
    (void)d; (void)pid;
    if (!size) return kAudioHardwareIllegalOperationError;
    Value v = property(o, a, qs, q);
    if (!v.found) return kAudioHardwareUnknownPropertyError;
    if (capacity < v.size || (v.size && !data)) return kAudioHardwareBadPropertySizeError;
    *size = v.size;
    if (v.size) { memcpy(data, &v.data, v.size); if (v.string) CFRetain(v.data.text); }
    return 0;
}
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid,
    const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 size, const void *data) {
    (void)qs; (void)q;
    Boolean settable = false;
    OSStatus result = IsPropertySettable(d, o, pid, a, &settable);
    if (result) return result;
    if (!settable || !data) return kAudioHardwareIllegalOperationError;
    if (o == kSpeakerVolume) {
        if (size != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        float value = *(const Float32 *)data;
        if (!isfinite(value)) return kAudioHardwareIllegalOperationError;
        speakerVolume = a->mSelector == kAudioLevelControlPropertyDecibelValue ? powf(10, fminf(0, fmaxf(-48, value)) / 20) : fminf(1, fmaxf(0, value));
        if (host && host->PropertiesChanged) {
            AudioObjectPropertyAddress addresses[] = {{kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0},
                {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0}};
            host->PropertiesChanged(host, kSpeakerVolume, 2, addresses);
        }
        return 0;
    }
    if (o == deviceFor(o)) return size == sizeof(Float64) && *(const Float64 *)data == MARBLE_HAPTICS_RATE ? 0 : kAudioDeviceUnsupportedFormatError;
    if (a->mSelector == kAudioStreamPropertyIsActive) {
        if (size != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        const Boolean active = *(const UInt32 *)data != 0;
        Boolean changed = atomic_exchange(&streamInactive[o], !active) != !active;
        atomic_fetch_add(&diagnostics[20], 1); atomic_store(&diagnostics[21], o << 16 | active);
        // AudioServerPlugIn SetPropertyData completes through this notification.
        // The host may read the property from its callback.
        if (changed && host && host->PropertiesChanged) {
            const AudioObjectPropertyAddress changedAddress = {
                kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal, 0};
            host->PropertiesChanged(host, o, 1, &changedAddress);
        }
        return 0;
    }
    if (size != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
    const AudioStreamBasicDescription *f = data, wanted = formatFor(o);
    return f->mSampleRate == wanted.mSampleRate && f->mFormatID == wanted.mFormatID &&
        f->mFormatFlags == wanted.mFormatFlags && f->mBytesPerPacket == wanted.mBytesPerPacket &&
        f->mFramesPerPacket == 1 && f->mBytesPerFrame == wanted.mBytesPerFrame && f->mChannelsPerFrame == wanted.mChannelsPerFrame &&
        f->mBitsPerChannel == wanted.mBitsPerChannel ? 0 : kAudioDeviceUnsupportedFormatError;
}
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kActuator) {
        if (o != kSpeaker && o != kHawking && o != kCodec) return kAudioHardwareBadObjectError;
        pthread_mutex_lock(&lock);
        int result = 0;
        if (o == kSpeaker && !companionUsers[o]) {
            if (speakerEndpoint < 0) speakerEndpoint = open(MARBLE_SPEAKER_DEVICE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            result = speakerEndpoint < 0 ? errno : speakerCommand(MARBLE_HAPTICS_START);
            atomic_store(&diagnostics[8], (uint32_t)result);
        }
        if (!result && !companionUsers[o]++) { companionAnchor[o] = mach_absolute_time(); ++companionSeed[o]; }
        if (o == kSpeaker) {
            atomic_fetch_add(&diagnostics[15], 1); atomic_store(&diagnostics[17], companionUsers[o]);
            atomic_store(&diagnostics[18], (uint32_t)result);
            atomic_store(&diagnostics[29], (uint32_t)(mach_absolute_time() >> 24));
        }
        pthread_mutex_unlock(&lock);
        return result ? kAudioHardwareNotRunningError : 0;
    }
    pthread_mutex_lock(&lock);
    atomic_fetch_add(&diagnostics[2], 1);
    int result = openEndpoint();
    if (!result && !users) {
        result = command(MARBLE_HAPTICS_START);
        if (!result) { anchor = mach_absolute_time(); ++clockSeed; }
    }
    if (!result) ++users;
    pthread_mutex_unlock(&lock);
    if (result) os_log_error(OS_LOG_DEFAULT, "MarbleHaptics: StartIO error %d", result);
    return result ? kAudioHardwareNotRunningError : 0;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kActuator) {
        if (o != kSpeaker && o != kHawking && o != kCodec) return kAudioHardwareBadObjectError;
        pthread_mutex_lock(&lock);
        if (companionUsers[o] && !--companionUsers[o] && o == kSpeaker && speakerEndpoint >= 0) speakerCommand(MARBLE_HAPTICS_STOP);
        if (o == kSpeaker) {
            atomic_fetch_add(&diagnostics[16], 1); atomic_store(&diagnostics[17], companionUsers[o]);
            atomic_store(&diagnostics[30], (uint32_t)(mach_absolute_time() >> 24));
        }
        pthread_mutex_unlock(&lock);
        return 0;
    }
    pthread_mutex_lock(&lock);
    atomic_fetch_add(&diagnostics[7], 1);
    if (users && !--users && endpoint >= 0) command(MARBLE_HAPTICS_STOP);
    pthread_mutex_unlock(&lock);
    return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client,
    Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
    (void)d; (void)client;
    if (o != deviceFor(o) || !deviceFor(o) || !sampleTime || !hostTime || !seed) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&lock);
    uint64_t now = mach_absolute_time();
    uint64_t origin = o == kActuator ? anchor : companionAnchor[o];
    double periodTicks = ticksPerFrame * kPeriod;
    uint64_t periods = origin && now > origin && periodTicks > 0 ? (uint64_t)((now - origin) / periodTicks) : 0;
    *sampleTime = (Float64)periods * kPeriod;
    *hostTime = origin + (uint64_t)(periods * periodTicks);
    *seed = o == kActuator ? clockSeed : companionSeed[o];
    pthread_mutex_unlock(&lock);
    if (o == kSpeaker) {
        atomic_fetch_add(&diagnostics[22], 1); atomic_store(&diagnostics[23], (uint32_t)periods);
        atomic_store(&diagnostics[28], (uint32_t)(now >> 24));
    }
    return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client,
    UInt32 operation, Boolean *willDo, Boolean *inPlace) {
    (void)d; (void)client;
    if (!deviceFor(o) || o != deviceFor(o) || !willDo || !inPlace) return kAudioHardwareBadObjectError;
    if (o == kSpeaker) atomic_fetch_add(&diagnostics[24], 1);
    *willDo = operation == kAudioServerPlugInIOOperationWriteMix || (inputFor(o) && operation == kAudioServerPlugInIOOperationReadInput); *inPlace = true; return 0;
}
static OSStatus BeginEndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client,
    UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle) {
    (void)d; (void)client; (void)frames; (void)cycle;
    if (o == kSpeaker) { atomic_fetch_add(&diagnostics[25], 1); atomic_store(&diagnostics[31], operation); }
    return 0;
}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream,
    UInt32 client, UInt32 operation, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *buffer, void *secondary) {
    (void)d; (void)client; (void)secondary;
    if (o != kActuator) {
        if (!deviceFor(o) || o != deviceFor(o)) return kAudioHardwareBadObjectError;
        if (o == kSpeaker) {
            atomic_fetch_add(&diagnostics[26], 1);
            atomic_store(&diagnostics[27], (uint32_t)(mach_absolute_time() >> 24));
        }
        if (operation == kAudioServerPlugInIOOperationReadInput && buffer && stream == inputFor(o)) memset(buffer, 0, frames * channelsFor(stream) * sizeof(int16_t));
        if (o == kSpeaker && stream == kSpeakerOut && operation == kAudioServerPlugInIOOperationWriteMix && buffer) {
            struct { MarbleHapticsPacket header; int32_t samples[512 * 4]; } packet;
            memset(&packet.header, 0, sizeof(packet.header));
            packet.header.magic = MARBLE_SPEAKER_MAGIC; packet.header.version = 1; packet.header.rate = MARBLE_SPEAKER_RATE;
            const int16_t *samples = buffer;
            float gain = speakerVolume;
            union { float f; uint32_t u; } gainBits = {.f = gain};
            atomic_store(&diagnostics[12], gainBits.u);
            pthread_mutex_lock(&lock);
            int error = 0;
            for (unsigned offset = 0; offset < frames && !error; offset += 512) {
                unsigned n = frames - offset < 512 ? frames - offset : 512;
                packet.header.frames = n * 2;
                uint32_t inputPeak = 0, outputPeak = 0;
                for (unsigned frame = 0; frame < n; ++frame) {
                    int32_t left = (int32_t)(samples[(offset + frame) * 2] * gain) * 65536;
                    int32_t right = (int32_t)(samples[(offset + frame) * 2 + 1] * gain) * 65536;
                    packet.samples[frame * 4] = packet.samples[frame * 4 + 2] = left;
                    packet.samples[frame * 4 + 1] = packet.samples[frame * 4 + 3] = right;
                    uint32_t in = (uint32_t)abs(samples[(offset + frame) * 2]), out = (uint32_t)abs(left >> 16);
                    if (in > inputPeak) inputPeak = in;
                    if (out > outputPeak) outputPeak = out;
                }
                if (inputPeak > atomic_load(&diagnostics[13])) atomic_store(&diagnostics[13], inputPeak);
                if (outputPeak > atomic_load(&diagnostics[14])) atomic_store(&diagnostics[14], outputPeak);
                size_t bytes = sizeof(packet.header) + packet.header.frames * 8;
                if (speakerEndpoint < 0 || write(speakerEndpoint, &packet, bytes) != (ssize_t)bytes) error = errno ? errno : EIO;
                atomic_fetch_add(&diagnostics[9], 1); atomic_fetch_add(&diagnostics[10], n);
                atomic_store(&diagnostics[19], (uint32_t)(mach_absolute_time() >> 24));
            }
            atomic_store(&diagnostics[11], (uint32_t)error);
            pthread_mutex_unlock(&lock);
            return error ? kAudioHardwareUnspecifiedError : 0;
        }
        return 0;
    }
    if (stream != kStream) return kAudioHardwareBadObjectError;
    if (operation != kAudioServerPlugInIOOperationWriteMix) return 0;
    if (!buffer || !cycle || frames > 65536) return kAudioHardwareIllegalOperationError;
    struct { MarbleHapticsPacket header; int16_t samples[512]; } packet;
    memset(&packet.header, 0, sizeof(packet.header));
    packet.header.magic = MARBLE_HAPTICS_MAGIC; packet.header.version = MARBLE_HAPTICS_VERSION;
    packet.header.rate = MARBLE_HAPTICS_RATE;
    pthread_mutex_lock(&lock);
    if (!users || endpoint < 0 || streamInactive[kStream]) { pthread_mutex_unlock(&lock); return 0; }
    const float *input = buffer;
    uint64_t at = cycle->mOutputTime.mFlags & kAudioTimeStampHostTimeValid ? cycle->mOutputTime.mHostTime :
        anchor + (uint64_t)(fmax(0, cycle->mOutputTime.mSampleTime) * ticksPerFrame);
    int result = 0;
    for (UInt32 offset = 0; offset < frames; offset += 512) {
        UInt32 count = frames - offset < 512 ? frames - offset : 512;
        packet.header.frames = count;
        packet.header.host_time = at + (uint64_t)(offset * ticksPerFrame);
        for (UInt32 i = 0; i < count; ++i) {
            float sample = input[offset + i];
            packet.samples[i] = !isfinite(sample) ? 0 : sample >= 1 ? INT16_MAX : sample <= -1 ? INT16_MIN : (int16_t)(sample * 32767);
        }
        atomic_fetch_add(&diagnostics[3], 1);
        atomic_fetch_add(&diagnostics[4], count);
        for (UInt32 i = 0; i < count; ++i) if (packet.samples[i]) atomic_fetch_add(&diagnostics[6], 1);
        size_t bytes = sizeof(packet.header) + count * sizeof(int16_t);
        if (write(endpoint, &packet, bytes) != (ssize_t)bytes) { result = errno ? errno : EIO; atomic_store(&diagnostics[5], (uint32_t)result); break; }
    }
    pthread_mutex_unlock(&lock);
    return result ? kAudioHardwareUnspecifiedError : 0;
}
static AudioServerPlugInDriverInterface driver = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, DeviceClient, DeviceClient,
    ConfigurationChange, ConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation,
    BeginEndIOOperation, DoIOOperation, BeginEndIOOperation
};
static AudioServerPlugInDriverInterface *driverPointer = &driver;
__attribute__((visibility("default"))) void *MarbleHaptics_Create(CFAllocatorRef allocator, CFUUIDRef type);
void *MarbleHaptics_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    (void)allocator;
    return type && CFEqual(type, kAudioServerPlugInTypeUUID) ? &driverPointer : NULL;
}
#ifdef MARBLE_AUDIO_ROUTE_TRACE
#include "MarbleAudioRoutingTrace.inc"
#endif
