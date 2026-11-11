// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the shipped HAL implementation against a recording device endpoint.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static int fakeOpen(const char *, int, ...);
static ssize_t fakeWrite(int, const void *, size_t);
#define open fakeOpen
#define write fakeWrite
#define MARBLE_AUDIO_ROUTE_SNAPSHOTS 1
#include "../../hal/MarbleHapticsPlugIn.c"
#undef open
#undef write
static unsigned writes, failWrite;
static unsigned activityNotifications;
static AudioObjectID activityObject;
static UInt32 activityValue;
static unsigned fakeRouteSuspended;
static OSStatus fakeRouteGet(AudioObjectID object, const AudioObjectPropertyAddress *a,
    UInt32 qs, const void *q, UInt32 *size, void *data) {
    if (object == 1 && a->mSelector == 'uidd') {
        assert(qs == sizeof(CFStringRef));
        CFStringRef uid = *(CFStringRef const *)q;
        *(UInt32 *)data = CFEqual(uid, CFSTR("Speaker")) ? 40 :
            CFEqual(uid, CFSTR("Actuator")) ? 42 : CFEqual(uid, CFSTR("VirtualAudioDevice_Default")) ? 60 : 0;
    } else if (object == 1 && a->mSelector == 'bidp') *(UInt32 *)data = 65;
    else if (object == 1 && a->mSelector == 'dOut') *(UInt32 *)data = 60;
    else if (a->mSelector == 'livn') *(UInt32 *)data = 1;
    else if (a->mSelector == 'goin') *(UInt32 *)data = !fakeRouteSuspended;
    else if (a->mSelector == 'oink') *(UInt32 *)data = getpid();
    else if (a->mSelector == 'stm#') *(UInt32 *)data = object + 100;
    else if (a->mSelector == 'sact') *(UInt32 *)data = !fakeRouteSuspended;
    else return kAudioHardwareUnknownPropertyError;
    *size = sizeof(UInt32);
    return 0;
}
static void routeSnapshotTest(void) {
    audioRouteGet = fakeRouteGet;
    audioRouteSample();
    assert(atomic_load(&audioRouteSnapshotCount) == 1);
    assert(atomic_load(&audioRouteSnapshots[0][0]) == 2);
    assert(atomic_load(&audioRouteSnapshots[0][33]) == 40);
    assert(atomic_load(&audioRouteSnapshots[0][37]) == 1);
    assert(atomic_load(&audioRouteSnapshots[0][43]) == 1);
    assert(atomic_load(&audioRouteSnapshots[0][113]) == 0); // virtual actuator absent
    assert(atomic_load(&audioRouteSnapshots[0][18]) == (UInt32)kAudioHardwareUnknownPropertyError);
    fakeRouteSuspended = 1;
    audioRouteSample();
    assert(atomic_load(&audioRouteSnapshots[1][37]) == 0);
    assert(atomic_load(&audioRouteSnapshots[1][43]) == 0);
    assert(!(atomic_load(&audioRouteQuery[0]) & 1));
    for (unsigned i = 2; i < 65; ++i) audioRouteSample();
    assert(atomic_load(&audioRouteSnapshotCount) == 65);
    assert(atomic_load(&audioRouteSnapshots[0][0]) == 130);
}
static OSStatus propertiesChanged(AudioServerPlugInHostRef h, AudioObjectID object,
    UInt32 count, const AudioObjectPropertyAddress *addresses) {
    (void)h;
    if (addresses[0].mSelector != kAudioStreamPropertyIsActive) return 0;
    assert(count == 1 && addresses[0].mScope == kAudioObjectPropertyScopeGlobal);
    assert(!pthread_mutex_trylock(&lock)); pthread_mutex_unlock(&lock);
    UInt32 size = sizeof(activityValue);
    assert(!GetPropertyData(NULL, object, 0, addresses, 0, NULL, size, &size, &activityValue));
    activityObject = object; ++activityNotifications;
    return 0;
}
static struct { MarbleHapticsPacket header; int16_t samples[4096]; } last;
static size_t lastSize;
static int fakeOpen(const char *path, int flags, ...) {
    assert(flags & O_NONBLOCK);
    if (!strcmp(path, MARBLE_HAPTICS_DEVICE)) return 10;
    assert(!strcmp(path, MARBLE_SPEAKER_DEVICE)); return 11;
}
static ssize_t fakeWrite(int fd, const void *data, size_t bytes) {
    assert((fd == 10 || fd == 11) && bytes <= sizeof(last));
    if (failWrite) { errno = EWOULDBLOCK; return -1; }
    memset(&last, 0, sizeof(last)); memcpy(&last, data, bytes); lastSize = bytes; ++writes; return (ssize_t)bytes;
}
int main(void) {
    assert(sizeof(MarbleHapticsPacket) == 32);
    assert(!MarbleHaptics_Create(NULL, IUnknownUUID));
    AudioServerPlugInDriverRef api = MarbleHaptics_Create(NULL, kAudioServerPlugInTypeUUID);
    AudioServerPlugInHostInterface testHost = {.PropertiesChanged = propertiesChanged};
    assert(api && (*api)->Initialize(api, &testHost) == 0);
    AudioObjectPropertyAddress address = {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, 0};
    UInt32 object = 0, size = 0;
    UInt32 deviceList[4];
    assert((*api)->GetPropertyData(api, kPlugin, 0, &address, 0, NULL, sizeof(deviceList), &size, deviceList) == 0 && size == 16 && deviceList[0] == kActuator);
    address = (AudioObjectPropertyAddress){kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, 0};
    assert((*api)->GetPropertyDataSize(api, kActuator, 0, &address, 0, NULL, &size) == 0 && !size);
    address.mScope = kAudioObjectPropertyScopeOutput;
    assert((*api)->GetPropertyData(api, kActuator, 0, &address, 0, NULL, sizeof(object), &size, &object) == 0 && object == kStream);
    address = (AudioObjectPropertyAddress){kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0};
    AudioStreamBasicDescription f;
    assert((*api)->GetPropertyData(api, kStream, 0, &address, 0, NULL, sizeof(f), &size, &f) == 0);
    assert(f.mSampleRate == 48000 && f.mChannelsPerFrame == 1 && f.mBytesPerFrame == 4);
    assert((*api)->SetPropertyData(api, kStream, 0, &address, 0, NULL, sizeof(f), &f) == 0);
    f.mChannelsPerFrame = 2;
    assert((*api)->SetPropertyData(api, kStream, 0, &address, 0, NULL, sizeof(f), &f) == kAudioDeviceUnsupportedFormatError);
    assert((*api)->StartIO(api, kActuator, 1) == 0 && writes == 1 && last.header.flags == MARBLE_HAPTICS_START);
    assert((*api)->StartIO(api, kActuator, 2) == 0 && writes == 1);
    float samples[] = {-1, -.5f, 0, .5f, 1, INFINITY, NAN};
    AudioServerPlugInIOCycleInfo cycle = {0};
    cycle.mOutputTime.mFlags = kAudioTimeStampHostTimeValid;
    cycle.mOutputTime.mHostTime = mach_absolute_time() + (uint64_t)(512 * ticksPerFrame);
    assert((*api)->DoIOOperation(api, kActuator, kStream, 1, kAudioServerPlugInIOOperationWriteMix, 7, &cycle, samples, NULL) == 0);
    assert(lastSize == 32 + 14 && last.header.frames == 7 && last.header.host_time == cycle.mOutputTime.mHostTime);
    assert(last.header.magic == MARBLE_HAPTICS_MAGIC && last.header.rate == 48000 && last.header.flags == 0);
    assert(last.samples[0] == -32768 && last.samples[1] == -16383 && !last.samples[2] && last.samples[3] == 16383);
    assert(last.samples[4] == 32767 && !last.samples[5] && !last.samples[6]);
    failWrite = 1;
    assert((*api)->DoIOOperation(api, kActuator, kStream, 1, kAudioServerPlugInIOOperationWriteMix, 7, &cycle, samples, NULL) != 0);
    failWrite = 0;
    Float64 sampleTime; UInt64 hostTime, seed;
    assert((*api)->GetZeroTimeStamp(api, kActuator, 1, &sampleTime, &hostTime, &seed) == 0);
    assert(sampleTime >= 0 && hostTime <= mach_absolute_time());
    unsigned beforeStop = writes;
    assert((*api)->StopIO(api, kActuator, 1) == 0 && writes == beforeStop);
    assert((*api)->StopIO(api, kActuator, 2) == 0 && last.header.flags == MARBLE_HAPTICS_STOP && lastSize == 32);
    assert((*api)->StartIO(api, kActuator, 1) == 0 && clockSeed > seed);
    assert((*api)->StopIO(api, kActuator, 1) == 0);
    assert((*api)->StartIO(api, kSpeaker, 3) == 0 && last.header.magic == MARBLE_SPEAKER_MAGIC && last.header.flags == MARBLE_HAPTICS_START);
    int16_t stereo[] = {4096, -8192, 16384, -16384};
    assert((*api)->DoIOOperation(api, kSpeaker, kSpeakerOut, 3, kAudioServerPlugInIOOperationWriteMix, 2, &cycle, stereo, NULL) == 0);
    assert(last.header.frames == 4 && lastSize == 64);
    int32_t pcm32[8]; memcpy(pcm32, last.samples, sizeof(pcm32));
    assert(pcm32[0] == 4096 * 65536 && pcm32[1] == -8192 * 65536);
    assert(pcm32[2] == pcm32[0] && pcm32[3] == pcm32[1]);
    address = (AudioObjectPropertyAddress){kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0};
    float half = .5f;
    assert((*api)->SetPropertyData(api, kSpeakerVolume, 0, &address, 0, NULL, sizeof(half), &half) == 0);
    assert((*api)->DoIOOperation(api, kSpeaker, kSpeakerOut, 3, kAudioServerPlugInIOOperationWriteMix, 2, &cycle, stereo, NULL) == 0);
    memcpy(pcm32, last.samples, sizeof(pcm32)); assert(pcm32[0] == 2048 * 65536 && pcm32[1] == -4096 * 65536);
    assert((*api)->StopIO(api, kSpeaker, 3) == 0 && last.header.flags == MARBLE_HAPTICS_STOP);
    Float64 speakerTime = 0; UInt64 speakerHost = 0, firstSeed = 0, secondSeed = 0;
    assert((*api)->GetZeroTimeStamp(api, kSpeaker, 3, &speakerTime, &speakerHost, &firstSeed) == 0);
    assert((*api)->StartIO(api, kSpeaker, 3) == 0 && last.header.flags == MARBLE_HAPTICS_START);
    assert((*api)->GetZeroTimeStamp(api, kSpeaker, 3, &speakerTime, &speakerHost, &secondSeed) == 0);
    assert(secondSeed != firstSeed && speakerTime == 0);
    assert((*api)->StopIO(api, kSpeaker, 3) == 0 && last.header.flags == MARBLE_HAPTICS_STOP);
    AudioObjectPropertyAddress active = {kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal, 0};
    UInt32 off = 0, value = 0, valueSize = sizeof(value);
    assert((*api)->SetPropertyData(api, kSpeakerIn, 0, &active, 0, NULL, sizeof(off), &off) == 0);
    assert(activityNotifications == 1 && activityObject == kSpeakerIn && activityValue == 0);
    assert((*api)->GetPropertyData(api, kSpeakerOut, 0, &active, 0, NULL, sizeof(value), &valueSize, &value) == 0 && value == 1);
    assert((*api)->GetPropertyData(api, kSpeakerIn, 0, &active, 0, NULL, sizeof(value), &valueSize, &value) == 0 && value == 0);
    assert((*api)->SetPropertyData(api, kSpeakerIn, 0, &active, 0, NULL, sizeof(off), &off) == 0 && activityNotifications == 1);
    UInt32 on = 1;
    assert((*api)->SetPropertyData(api, kSpeakerIn, 0, &active, 0, NULL, sizeof(on), &on) == 0);
    assert(activityNotifications == 2 && activityObject == kSpeakerIn && activityValue == 1);
    routeSnapshotTest();
    puts("PASS: PCM transport, multiple-client lifecycle, restart timeline, per-stream activity notifications, reentrant host reads, route state transitions and snapshot ring wrap");
}
