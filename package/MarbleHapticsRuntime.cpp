// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actuator PCM envelope to the board's 170 Hz DIRECT_PLAY carrier. The board
// voltage cap stays 1450 mV. Each segment carries two milliseconds of envelope;
// the worker owns every SPMI write and stops on silence, close and underflow.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include "MarbleHapticsAbi.h"

extern "C" {
extern volatile uint32_t marble_haptics_play_count;
int native_marble_haptics_drive(unsigned amplitude);
int native_marble_haptics_quiet(void);
void native_marble_haptics_runtime(void);
void native_marble_haptics_runtime_stop(void);
// ready, opens, writes, frames, loud frames, queued segments, consumed segments,
// hardware starts, stops, rejected packets, last error, peak PCM, queue high
// water, active, clients, STOPs, late segments, generation, STARTs, amplitude.
volatile uint32_t marble_haptics_runtime_stats[32];
}

namespace {
constexpr unsigned kSegments = 512, kFramesPerSegment = 96;
struct Segment { uint64_t at, end; uint32_t amplitude, generation; };
Segment queue[kSegments];
IOLock *lock;
unsigned head, count, generation;
bool online, stopping, opened;
uint64_t ticksPerSecond;
int majorNumber = -1;
void *deviceNode;

uint64_t frameTicks(unsigned frames) { return ticksPerSecond * frames / MARBLE_HAPTICS_RATE; }
void error(int value) { marble_haptics_runtime_stats[10] = uint32_t(value); }
void resetQueue() {
    head = count = 0;
    ++generation;
    marble_haptics_runtime_stats[17] = generation;
    IOLockWakeup(lock, &count, false);
}
int deviceOpen(dev_t, int, int, struct proc *) {
    if (!lock) return ENXIO;
    IOLockLock(lock);
    int result = !online || stopping ? ENXIO : opened ? EBUSY : 0;
    if (!result) {
        opened = true;
        resetQueue();
        ++marble_haptics_runtime_stats[1];
        marble_haptics_runtime_stats[14] = 1;
    }
    IOLockUnlock(lock);
    return result;
}
int deviceClose(dev_t, int, int, struct proc *) {
    IOLockLock(lock);
    opened = false;
    marble_haptics_runtime_stats[14] = 0;
    resetQueue();
    IOLockUnlock(lock);
    return 0;
}
int deviceWrite(dev_t, struct uio *uio, int) {
    const auto bytes = uio_resid(uio);
    if (bytes < int(sizeof(MarbleHapticsPacket)) ||
        bytes > int(sizeof(MarbleHapticsPacket) + MARBLE_HAPTICS_MAX_FRAMES * 2)) return EINVAL;
    auto *buffer = static_cast<uint8_t *>(IOMalloc(size_t(bytes)));
    if (!buffer) return ENOMEM;
    int result = uiomove(reinterpret_cast<char *>(buffer), int(bytes), uio);
    const auto *packet = reinterpret_cast<const MarbleHapticsPacket *>(buffer);
    if (!result && (packet->magic != MARBLE_HAPTICS_MAGIC || packet->version != MARBLE_HAPTICS_VERSION ||
        packet->rate != MARBLE_HAPTICS_RATE || packet->reserved ||
        packet->frames > MARBLE_HAPTICS_MAX_FRAMES ||
        bytes != int(sizeof(*packet) + packet->frames * 2) ||
        packet->flags & ~(MARBLE_HAPTICS_START | MARBLE_HAPTICS_STOP) ||
        (packet->flags && packet->frames))) result = EINVAL;
    if (!result) {
        IOLockLock(lock);
        if (!online || stopping || !opened) result = ENXIO;
        else if (packet->flags) {
            resetQueue();
            if (packet->flags & MARBLE_HAPTICS_STOP) ++marble_haptics_runtime_stats[15];
            if (packet->flags & MARBLE_HAPTICS_START) ++marble_haptics_runtime_stats[18];
        } else {
            const uint64_t now = mach_absolute_time();
            const uint64_t at = packet->host_time ? packet->host_time : now;
            const unsigned needed = (packet->frames + kFramesPerSegment - 1) / kFramesPerSegment;
            if (at > now + ticksPerSecond || at + ticksPerSecond / 10 < now) result = ERANGE;
            else if (needed > kSegments - count) result = EWOULDBLOCK;
            else {
                const auto *samples = reinterpret_cast<const int16_t *>(packet + 1);
                for (unsigned offset = 0; offset < packet->frames; offset += kFramesPerSegment) {
                    const unsigned frames = packet->frames - offset < kFramesPerSegment ?
                        packet->frames - offset : kFramesPerSegment;
                    unsigned peak = 0;
                    for (unsigned i = 0; i < frames; ++i) {
                        int sample = samples[offset + i];
                        unsigned magnitude = unsigned(sample < 0 ? -sample : sample);
                        if (magnitude > peak) peak = magnitude;
                        if (sample) ++marble_haptics_runtime_stats[4];
                    }
                    if (peak > marble_haptics_runtime_stats[11]) marble_haptics_runtime_stats[11] = peak;
                    Segment &segment = queue[(head + count) % kSegments];
                    segment = {at + frameTicks(offset), at + frameTicks(offset + frames),
                        peak < 128 ? 0u : (peak * 255u + 16384u) / 32768u, generation};
                    ++count;
                }
                ++marble_haptics_runtime_stats[2];
                marble_haptics_runtime_stats[3] += packet->frames;
                marble_haptics_runtime_stats[5] += needed;
                if (count > marble_haptics_runtime_stats[12]) marble_haptics_runtime_stats[12] = count;
                IOLockWakeup(lock, &count, false);
            }
        }
        IOLockUnlock(lock);
    }
    if (result) { ++marble_haptics_runtime_stats[9]; error(result); }
    IOFree(buffer, size_t(bytes));
    return result;
}
struct cdevsw deviceSwitch = {
    .d_open = deviceOpen, .d_close = deviceClose, .d_read = eno_rdwrt,
    .d_write = deviceWrite, .d_ioctl = eno_ioctl, .d_stop = eno_stop,
    .d_reset = eno_reset, .d_ttys = nullptr, .d_select = eno_select,
    .d_mmap = eno_mmap, .d_strategy = eno_strat,
    .d_reserved_1 = eno_getc, .d_reserved_2 = eno_putc, .d_type = 0
};
}

extern "C" void native_marble_haptics_runtime(void) {
    lock = IOLockAlloc();
    if (!lock) { error(ENOMEM); return; }
    mach_timebase_info_data_t timebase;
    clock_timebase_info(&timebase);
    ticksPerSecond = 1000000000ULL * timebase.denom / timebase.numer;
    majorNumber = cdevsw_add(-1, &deviceSwitch);
    if (majorNumber >= 0)
        deviceNode = devfs_make_node(makedev(majorNumber, 0), DEVFS_CHAR, 0, 0, 0666, "marble-haptics");
    if (!deviceNode) { error(ENXIO); return; }
    IOLockLock(lock);
    online = true;
    marble_haptics_runtime_stats[0] = 1;
    IOLog("MarbleHaptics: Actuator endpoint ready, PCM16 48000 Hz, envelope 2 ms\n");
    unsigned activeGeneration = generation, applied = 0;
    uint64_t expires = 0;
    while (!stopping) {
        const uint64_t now = mach_absolute_time();
        if (activeGeneration != generation) {
            activeGeneration = generation;
            if (applied) {
                IOLockUnlock(lock);
                int result = native_marble_haptics_quiet();
                IOLockLock(lock);
                if (result) error(result);
                ++marble_haptics_runtime_stats[8];
            }
            applied = 0;
            marble_haptics_runtime_stats[13] = 0;
        }
        if (count && queue[head].at <= now) {
            const Segment segment = queue[head];
            head = (head + 1) % kSegments;
            --count;
            ++marble_haptics_runtime_stats[6];
            if (segment.end + ticksPerSecond / 10 < now) {
                ++marble_haptics_runtime_stats[16];
                continue;
            }
            expires = segment.end + ticksPerSecond / 125; // 8 ms underflow deadline
            if (applied != segment.amplitude) {
                IOLockUnlock(lock);
                int result = segment.amplitude ? native_marble_haptics_drive(segment.amplitude) :
                    native_marble_haptics_quiet();
                IOLockLock(lock);
                if (result) {
                    error(result);
                    resetQueue();
                    IOLockUnlock(lock);
                    native_marble_haptics_quiet();
                    IOLockLock(lock);
                    applied = 0;
                } else {
                    if (!applied && segment.amplitude) {
                        ++marble_haptics_runtime_stats[7];
                        ++marble_haptics_play_count;
                    }
                    if (applied && !segment.amplitude) ++marble_haptics_runtime_stats[8];
                    applied = segment.amplitude;
                }
                marble_haptics_runtime_stats[13] = applied != 0;
                marble_haptics_runtime_stats[19] = applied;
            }
            continue;
        }
        if (applied && now >= expires) {
            IOLockUnlock(lock);
            int result = native_marble_haptics_quiet();
            IOLockLock(lock);
            if (result) error(result);
            ++marble_haptics_runtime_stats[8];
            applied = 0;
            marble_haptics_runtime_stats[13] = 0;
            continue;
        }
        uint64_t deadline = count ? queue[head].at : 0;
        if (applied && (!deadline || expires < deadline)) deadline = expires;
        if (deadline) IOLockSleepDeadline(lock, &count, deadline, THREAD_UNINT);
        else IOLockSleep(lock, &count, THREAD_UNINT);
    }
    online = false;
    resetQueue();
    IOLockUnlock(lock);
    native_marble_haptics_quiet();
    marble_haptics_runtime_stats[0] = 0;
    marble_haptics_runtime_stats[13] = 0;
}

extern "C" void native_marble_haptics_runtime_stop(void) {
    if (!lock) return;
    IOLockLock(lock);
    stopping = true;
    IOLockWakeup(lock, &count, false);
    IOLockUnlock(lock);
}
