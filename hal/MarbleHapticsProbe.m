// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diagnostic candidate only: exercise system vibration and CoreHaptics, expose
// results in process memory for firmware USB observation, then exit.
#import <Foundation/Foundation.h>
#import <CoreHaptics/CoreHaptics.h>
#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/AudioHardware.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>
#include <fcntl.h>
#include "MarbleHapticsAbi.h"
#include "audio/MarbleSpeakerAbi.h"
__attribute__((used, visibility("default"))) volatile uint32_t marble_haptics_probe_results[64];
__attribute__((used, visibility("default"))) char marble_haptics_probe_devices[16][96];
__attribute__((used, visibility("default"))) char marble_haptics_probe_version[4096];
static AudioStreamBasicDescription toneFormat;
static uint64_t toneFrame;
static OSStatus toneIO(AudioDeviceID device, const AudioTimeStamp *now, const AudioBufferList *input,
    const AudioTimeStamp *inputTime, AudioBufferList *output, const AudioTimeStamp *outputTime, void *context) {
    (void)device; (void)now; (void)input; (void)inputTime; (void)outputTime; (void)context;
    for (unsigned buffer = 0; buffer < output->mNumberBuffers; ++buffer) {
        AudioBuffer *b = &output->mBuffers[buffer];
        if (!b->mData || !toneFormat.mBytesPerFrame) continue;
        unsigned frames = b->mDataByteSize / toneFormat.mBytesPerFrame;
        for (unsigned frame = 0; frame < frames; ++frame) {
            double value = sin((toneFrame + frame) * (750.0 * 6.283185307179586 / toneFormat.mSampleRate)) * .015625;
            for (unsigned channel = 0; channel < b->mNumberChannels; ++channel) {
                unsigned index = frame * b->mNumberChannels + channel;
                if (toneFormat.mFormatFlags & kAudioFormatFlagIsFloat) ((float *)b->mData)[index] = value;
                else if (toneFormat.mBitsPerChannel == 16) ((int16_t *)b->mData)[index] = (int16_t)(value * 32767);
                else ((int32_t *)b->mData)[index] = (int32_t)(value * 2147483647);
            }
        }
        toneFrame += frames;
        ++marble_haptics_probe_results[46];
    }
    return 0;
}
static void speakerTest(void) {
    CFStringRef uid = CFSTR("Speaker");
    AudioDeviceID device = 0; UInt32 size = sizeof(device);
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, 0};
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &device);
    if (!device) {
        uid = CFSTR("MarbleSpeakerDevice"); size = sizeof(device);
        status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &device);
    }
    marble_haptics_probe_results[40] = (uint32_t)status; marble_haptics_probe_results[41] = device;
    if (status || !device) return;
    AudioStreamID stream = 0; size = sizeof(stream);
    a = (AudioObjectPropertyAddress){kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, 0};
    status = AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &stream);
    marble_haptics_probe_results[42] = (uint32_t)status;
    if (status || !stream) return;
    a = (AudioObjectPropertyAddress){kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0};
    size = sizeof(toneFormat);
    status = AudioObjectGetPropertyData(stream, &a, 0, NULL, &size, &toneFormat);
    if (status) return;
    AudioDeviceIOProcID proc = NULL;
    status = AudioDeviceCreateIOProcID(device, toneIO, NULL, &proc);
    marble_haptics_probe_results[43] = (uint32_t)status;
    if (status) return;
    status = AudioDeviceStart(device, proc); marble_haptics_probe_results[44] = (uint32_t)status;
    if (!status) { usleep(500000); marble_haptics_probe_results[45] = (uint32_t)AudioDeviceStop(device, proc); }
    AudioDeviceDestroyIOProcID(device, proc);
}
static void speakerTransportTest(void) {
    int fd = open(MARBLE_SPEAKER_DEVICE, O_WRONLY | O_NONBLOCK);
    marble_haptics_probe_results[48] = fd < 0 ? errno : 0;
    if (fd < 0) return;
    struct { MarbleHapticsPacket header; int32_t samples[960 * 2]; } packet;
    memset(&packet, 0, sizeof(packet));
    packet.header = (MarbleHapticsPacket){ .magic = MARBLE_SPEAKER_MAGIC, .version = 1,
        .flags = MARBLE_HAPTICS_START, .rate = MARBLE_SPEAKER_RATE };
    ssize_t bytes = write(fd, &packet, sizeof(packet.header));
    marble_haptics_probe_results[49] = bytes == sizeof(packet.header) ? 0 : errno;
    if (bytes == sizeof(packet.header)) {
        packet.header.flags = 0; packet.header.frames = 960;
        for (unsigned chunk = 0; chunk < 50; ++chunk) {
            for (unsigned frame = 0; frame < 960; ++frame) {
                int32_t sample = (int32_t)(sin((chunk * 960 + frame) * (750.0 * 6.283185307179586 / 96000)) * (4096.0 * 65536));
                packet.samples[frame * 2] = packet.samples[frame * 2 + 1] = sample;
            }
            if (write(fd, &packet, sizeof(packet)) != sizeof(packet)) { marble_haptics_probe_results[50] = errno; break; }
            ++marble_haptics_probe_results[51];
        }
        sleep(5);
        packet.header.frames = 0; packet.header.flags = MARBLE_HAPTICS_STOP;
        write(fd, &packet, sizeof(packet.header));
    }
    close(fd);
}
static void speakerFullVolume(void) {
    CFStringRef uid = CFSTR("Speaker");
    AudioDeviceID device = 0; UInt32 size = sizeof(device);
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, 0};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &device);
    if (!device) {
        uid = CFSTR("MarbleSpeakerDevice"); size = sizeof(device);
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &device);
    }
    if (!device) { marble_haptics_probe_results[58] = 'nodv'; return; }
    Float32 volume = 1;
    a = (AudioObjectPropertyAddress){kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, 0};
    marble_haptics_probe_results[58] = (uint32_t)AudioObjectSetPropertyData(device, &a, 0, NULL, sizeof(volume), &volume);
    size = sizeof(volume); volume = -1;
    AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &volume);
    marble_haptics_probe_results[59] = (uint32_t)(volume * 1000);
}
static void systemSpeakerTest(void) {
    const uint32_t frames = 144000, bytes = frames * 4;
    NSMutableData *wave = [NSMutableData dataWithLength:44 + bytes];
    uint8_t *header = wave.mutableBytes;
    memcpy(header, "RIFF", 4); memcpy(header + 8, "WAVEfmt ", 8);
    uint32_t chunkSize = bytes + 36, formatSize = 16, rate = 48000, byteRate = 192000;
    uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    memcpy(header + 4, &chunkSize, 4); memcpy(header + 16, &formatSize, 4);
    memcpy(header + 20, &pcm, 2); memcpy(header + 22, &channels, 2);
    memcpy(header + 24, &rate, 4); memcpy(header + 28, &byteRate, 4);
    memcpy(header + 32, &align, 2); memcpy(header + 34, &bits, 2);
    memcpy(header + 36, "data", 4); memcpy(header + 40, &bytes, 4);
    int16_t *samples = (int16_t *)(header + 44);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        int16_t sample = (int16_t)(sin(frame * (750.0 * 6.283185307179586 / rate)) * 16384);
        samples[frame * 2] = samples[frame * 2 + 1] = sample;
    }
    NSError *error = nil;
    AVAudioSession *session = [AVAudioSession sharedInstance];
    BOOL category = [session setCategory:AVAudioSessionCategoryPlayback error:&error];
    marble_haptics_probe_results[52] = category;
    marble_haptics_probe_results[53] = (uint32_t)error.code;
    error = nil;
    marble_haptics_probe_results[54] = [session setActive:YES error:&error];
    marble_haptics_probe_results[55] = (uint32_t)error.code;
    error = nil;
    AVAudioPlayer *player = [[AVAudioPlayer alloc] initWithData:wave error:&error];
    marble_haptics_probe_results[56] = (uint32_t)error.code;
    speakerFullVolume();
    marble_haptics_probe_results[57] = [player play];
    [NSThread sleepForTimeInterval:5];
    [player stop];
    [session setActive:NO error:nil];
}
static void actuatorRouteTest(void) {
    CFStringRef uid = CFSTR("VirtualAudioDevice_Actuator");
    AudioObjectID device = 0; UInt32 size = sizeof(device);
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, 0};
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &device);
    marble_haptics_probe_results[12] = (uint32_t)status;
    marble_haptics_probe_results[13] = device;
    AudioValueTranslation translation = {&uid, sizeof(uid), &device, sizeof(device)};
    a.mSelector = 'duid'; size = sizeof(translation);
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &translation);
    marble_haptics_probe_results[14] = (uint32_t)status;
    marble_haptics_probe_results[15] = device;
    if (!device) return;
    CFArrayRef ports = NULL;
    a.mSelector = 'dacp'; size = sizeof(ports);
    status = AudioObjectGetPropertyData(device, &a, 0, NULL, &size, &ports);
    marble_haptics_probe_results[16] = (uint32_t)status;
    if (!status && ports && CFGetTypeID(ports) == CFArrayGetTypeID()) {
        CFIndex count = CFArrayGetCount(ports);
        marble_haptics_probe_results[17] = (uint32_t)count;
        for (CFIndex index = 0; index < count && index < 2; ++index) {
            CFNumberRef number = CFArrayGetValueAtIndex(ports, index);
            AudioObjectID port = 0; UInt32 kind = 0; size = sizeof(kind);
            if (CFGetTypeID(number) != CFNumberGetTypeID() || !CFNumberGetValue(number, kCFNumberSInt32Type, &port)) continue;
            a.mSelector = 'ptyp';
            if (!AudioObjectGetPropertyData(port, &a, 0, NULL, &size, &kind)) marble_haptics_probe_results[18 + index] = kind;
        }
    }
    if (ports) CFRelease(ports);
}
int main(void) {
    @autoreleasepool {
        marble_haptics_probe_results[0] = 1;
        sleep(30);
        void *gestalt = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
        CFTypeRef (*copyAnswer)(CFStringRef) = gestalt ? dlsym(gestalt, "MGCopyAnswer") : NULL;
        if (copyAnswer) {
            CFTypeRef release = copyAnswer(CFSTR("ReleaseType"));
            if (release) {
                CFStringRef description = CFCopyDescription(release);
                CFStringGetCString(description, marble_haptics_probe_version, 256, kCFStringEncodingUTF8);
                CFRelease(description); CFRelease(release);
            }
        }
        CFDictionaryRef (*copyVersion)(void) = dlsym(RTLD_DEFAULT, "_CFCopySystemVersionDictionary");
        if (copyVersion) {
            CFDictionaryRef version = copyVersion();
            if (version) {
                CFStringRef description = CFCopyDescription(version);
                CFStringGetCString(description, marble_haptics_probe_version + 256, 1792, kCFStringEncodingUTF8);
                CFRelease(description); CFRelease(version);
            }
        }
        bool (*getBool)(CFStringRef) = gestalt ? dlsym(gestalt, "MGGetBoolAnswer") : NULL;
        if (getBool) {
            marble_haptics_probe_results[30] = getBool(CFSTR("DeviceSupportsHaptics"));
            marble_haptics_probe_results[31] = getBool(CFSTR("DeviceSupportsClosedLoopHaptics"));
            marble_haptics_probe_results[32] = getBool(CFSTR("VibratorCapability"));
            marble_haptics_probe_results[33] = getBool(CFSTR("IsUIBuild"));
        }
        id<CHHapticDeviceCapability> capabilities = [CHHapticEngine capabilitiesForHardware];
        marble_haptics_probe_results[1] = capabilities.supportsHaptics;
        AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0};
        AudioObjectID devices[64]; UInt32 size = sizeof(devices);
        OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, devices);
        marble_haptics_probe_results[20] = (uint32_t)status;
        unsigned count = !status ? size / sizeof(AudioObjectID) : 0;
        marble_haptics_probe_results[21] = count;
        for (unsigned i = 0; i < count && i < 16; ++i) {
            CFStringRef uid = NULL;
            a = (AudioObjectPropertyAddress){kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0};
            size = sizeof(uid);
            if (!AudioObjectGetPropertyData(devices[i], &a, 0, NULL, &size, &uid) && uid) {
                CFStringGetCString(uid, marble_haptics_probe_devices[i], sizeof(marble_haptics_probe_devices[i]), kCFStringEncodingUTF8);
                CFRelease(uid);
            }
        }
        marble_haptics_probe_results[0] = 2;
        actuatorRouteTest();
        speakerTest();
        systemSpeakerTest();
        speakerTransportTest();
        AudioServicesPlaySystemSound(kSystemSoundID_Vibrate);
        ++marble_haptics_probe_results[8];
        [NSThread sleepForTimeInterval:2];
        NSError *error = nil;
        CHHapticEngine *engine = nil;
        for (unsigned attempt = 0; attempt < 4 && !engine; ++attempt) {
            error = nil;
            engine = [[CHHapticEngine alloc] initAndReturnError:&error];
            marble_haptics_probe_results[60] = attempt + 1;
            marble_haptics_probe_results[61] = (uint32_t)error.code;
            if (!engine && attempt < 3) [NSThread sleepForTimeInterval:10];
        }
        marble_haptics_probe_results[2] = (uint32_t)error.code;
        if (error) snprintf(marble_haptics_probe_version + 2048, 2048, "%s", error.description.UTF8String);
        marble_haptics_probe_results[3] = engine != nil;
        if (engine) {
            engine.playsHapticsOnly = YES;
            BOOL started = [engine startAndReturnError:&error];
            marble_haptics_probe_results[4] = started;
            marble_haptics_probe_results[5] = (uint32_t)error.code;
            if (started) {
                NSArray *parameters = @[[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:0.8],
                    [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5]];
                CHHapticEvent *tap = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient parameters:parameters relativeTime:0];
                CHHapticEvent *hold = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:parameters relativeTime:0.5 duration:0.15];
                CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[tap, hold] parameters:@[] error:&error];
                marble_haptics_probe_results[6] = (uint32_t)error.code;
                id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&error];
                marble_haptics_probe_results[7] = player != nil;
                marble_haptics_probe_results[9] = (uint32_t)error.code;
                BOOL played = [player startAtTime:CHHapticTimeImmediate error:&error];
                marble_haptics_probe_results[10] = played;
                marble_haptics_probe_results[11] = (uint32_t)error.code;
                [NSThread sleepForTimeInterval:3];
                [engine stopWithCompletionHandler:nil];
            }
        }
        marble_haptics_probe_results[0] = 3;
        sleep(300);
    }
    return 0;
}
