// SPDX-License-Identifier: BSD-2-Clause-Patent
// marble: QUPv3 SE15 I2C, AW88263 at 0x34/0x35, reset GPIO1/120;
// audio clocks and route from the board DTS and original ACF Music profiles.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include "MarbleMicrosecondDelay.h"
#include "MarbleHapticsAbi.h"
#include "audio/MarbleSpeakerAbi.h"
extern "C" {
#include "GeniI2c.h"
#include "MarbleAdsp.h"
#include "audio/ShengGpr.h"
#include "audio/ShengAudioReach.h"
volatile uint32_t marble_audio_stats[64];
volatile uint32_t marble_audio_amp_registers[2][128];
volatile uint32_t marble_audio_messages[16][16];
volatile uint32_t marble_audio_gpio[256];
volatile uint32_t marble_audio_smmu[8];
volatile uint32_t marble_audio_probe[64];
void native_marble_audio_start(void);
void native_marble_audio_stop(void);
void native_marble_audio_dma_prepare(uintptr_t);
volatile uint64_t marble_audio_dma_physical, marble_audio_dma_virtual;
}
namespace {
struct Window {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
    bool open(uint64_t physical, uint64_t size) {
        IODeviceMemory *memory = IODeviceMemory::withRange(physical, size);
        map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        va = map ? reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()) : nullptr;
        return va != nullptr;
    }
    uint32_t read(unsigned offset) { return va[offset / 4]; }
    void write(unsigned offset, uint32_t value) { va[offset / 4] = value; __asm__ volatile("dsb sy" ::: "memory"); }
};
Window gcc, se, tlmm;
IOBufferMemoryDescriptor *dmaBuffer, *dmaTables;
IOMemoryMap *dmaMapping;
uintptr_t dmaSmmu;
unsigned dmaContext;
unsigned dmaSlot;
bool dmaExtended;
constexpr unsigned kAudioIova = 0x30000000u, kAudioDmaBytes = 2u << 20;
#include "audio/MarbleAmplifierProfile.inc"
GeniI2c bus;
MarbleAdspChannel *channel;
IOLock *lock;
bool running, stopping, up;
struct Message { uint32_t size; uint8_t data[512]; };
Message inbox[16]; unsigned head, count;
uint32_t token = 100;
constexpr unsigned kPeriodFrames = 960, kPeriodBytes = kPeriodFrames * 8, kSlots = 8;
constexpr unsigned kRingFrames = 131072;
int32_t *speakerRing;
uint64_t ringRead, ringWritten;
bool endpointOpened, wanted, playing, graphsReady;
uint32_t requestGeneration, playbackGeneration;
uint32_t memoryHandle, inFlight, nextSlot, loudHistory;
bool slotInFlight[kSlots];
int speakerMajor = -1;
void *speakerNode;
void stopOutput();
void fail(unsigned, int);
bool command(const uint8_t *packet, unsigned bytes, uint32_t opcode) {
    if (!bytes || marble_adsp_send(channel, packet, bytes)) return false;
    ++marble_audio_stats[30];
    uint64_t deadline; clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
    IOLockLock(lock);
    for (;;) {
        if (stopping || !up) { IOLockUnlock(lock); return false; }
        if (count) {
            Message message = inbox[head]; head = (head + 1) % 16; --count;
            IOLockUnlock(lock);
            ArAnswer answer = {};
            int parsed = ar_parse(message.data, message.size, &answer);
            marble_audio_stats[31] = answer.opcode;
            marble_audio_stats[32] = answer.status;
            marble_audio_stats[33] = answer.result_opcode;
            if (parsed != ArBad && answer.token == token && answer.opcode == AR_OP_MEM_MAP_DONE) {
                memoryHandle = answer.handle; marble_audio_stats[45] = answer.handle; return true;
            }
            if (parsed != ArBad && answer.token == token &&
                ((opcode == AR_OP_PRM_REQUEST && answer.opcode == AR_OP_PRM_REQUEST_DONE) ||
                 (opcode == AR_OP_PRM_RELEASE && answer.opcode == AR_OP_PRM_RELEASE_DONE))) return answer.status == 0;
            if (parsed != ArBad && answer.token == token && answer.opcode == GPR_OP_BASIC_RESULT && answer.result_opcode == opcode)
                return answer.status == 0;
            IOLockLock(lock);
            continue;
        }
        if (IOLockSleepDeadline(lock, &count, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
            ++marble_audio_stats[34]; IOLockUnlock(lock); return false;
        }
    }
}
void openGraphs() {
    uint8_t packet[512]; unsigned bytes;
    marble_audio_stats[0] = 5;
    bytes = ar_open_marble_tdm_device(packet, sizeof(packet), 0x3001, ++token);
    if (!command(packet, bytes, AR_OP_GRAPH_OPEN)) { fail(5, int(marble_audio_stats[32])); return; }
    marble_audio_stats[0] = 6;
    const ArFormat format = {96000, 32, 2};
    bytes = ar_config_marble_tdm_device(packet, sizeof(packet), 0x3001, ++token, &format);
    if (!command(packet, bytes, AR_OP_SET_CFG)) { fail(6, int(marble_audio_stats[32])); return; }
    marble_audio_stats[0] = 7;
    bytes = ar_open_playback_stream(packet, sizeof(packet), 0x3001, ++token);
    if (!command(packet, bytes, AR_OP_GRAPH_OPEN)) { fail(7, int(marble_audio_stats[32])); return; }
    marble_audio_stats[0] = 8;
    bytes = ar_config_playback_stream(packet, sizeof(packet), 0x3001, ++token, &format);
    if (!command(packet, bytes, AR_OP_SET_CFG)) { fail(8, int(marble_audio_stats[32])); return; }
    marble_audio_stats[0] = 9;
    bytes = ar_config_playback_shmem(packet, sizeof(packet), 0x3001, ++token, &format);
    if (!command(packet, bytes, AR_OP_SET_CFG)) { fail(9, int(marble_audio_stats[32])); return; }
    marble_audio_stats[0] = 10;
    if (marble_audio_stats[40] == 8) {
        bytes = ar_map(packet, sizeof(packet), 0x3001, ++token, uint64_t(kAudioIova) | (1ULL << 32), 65536);
        if (!command(packet, bytes, AR_OP_MEM_MAP)) { fail(10, int(marble_audio_stats[32])); return; }
        graphsReady = true;
        marble_audio_stats[0] = 11;
    }
}
uint32_t regRead(void *context, uint32_t offset) { return static_cast<Window *>(context)->read(offset); }
void regWrite(void *context, uint32_t offset, uint32_t value) { static_cast<Window *>(context)->write(offset, value); }
void pin(unsigned gpio, unsigned config, int level) {
    if (level >= 0) tlmm.write(gpio * 4096 + 4, level ? 2 : 0);
    tlmm.write(gpio * 4096, config);
    if (gpio < 128) {
        marble_audio_gpio[gpio * 2] = tlmm.read(gpio * 4096);
        marble_audio_gpio[gpio * 2 + 1] = tlmm.read(gpio * 4096 + 4);
    }
}
void snapshotSmmu() {
    if (!dmaSmmu) return;
    const uintptr_t cb = dmaSmmu + dmaContext;
    auto read = [cb](unsigned offset) { return *reinterpret_cast<volatile uint32_t *>(cb + offset); };
    marble_audio_smmu[0] = read(0x58);
    marble_audio_smmu[1] = read(0x60);
    marble_audio_smmu[2] = read(0x64);
    marble_audio_smmu[3] = read(0x68);
    marble_audio_smmu[4] = read(0);
    marble_audio_smmu[5] = read(0x30);
    ++marble_audio_smmu[7];
}
int transaction() {
    for (unsigned i = 0; i < 500; ++i) {
        int result = geni_i2c_poll(&bus);
        if (result == GeniDone) return 0;
        if (result == GeniFailed) { marble_audio_stats[10] = bus.error_status; return -1; }
        marbleMicrosecondDelay(20);
    }
    for (unsigned i = 0; i < 3 && geni_i2c_busy(&bus); ++i) {
        geni_i2c_timeout(&bus);
        for (unsigned j = 0; j < 100 && geni_i2c_busy(&bus); ++j) {
            geni_i2c_poll(&bus); marbleMicrosecondDelay(10);
        }
    }
    ++marble_audio_stats[11];
    return -2;
}
int readAmp(unsigned address, uint8_t reg, uint16_t &value) {
    bus.address = uint8_t(address);
    if (geni_i2c_start_read_bytes(&bus, &reg, 1, 2)) return -3;
    int result = transaction();
    if (!result) value = uint16_t((bus.rx & 255) << 8 | (bus.rx >> 8 & 255));
    return result;
}
int writeAmp(unsigned address, uint8_t reg, uint16_t value) {
    bus.address = uint8_t(address);
    const uint8_t bytes[] = {reg, uint8_t(value >> 8), uint8_t(value)};
    if (geni_i2c_start_write_bytes(&bus, bytes, sizeof(bytes))) return -3;
    return transaction();
}
bool updateAmp(unsigned amp, unsigned reg, uint16_t clear, uint16_t set) {
    uint16_t value = 0;
    return !readAmp(0x34 + amp, uint8_t(reg), value) &&
        !writeAmp(0x34 + amp, uint8_t(reg), uint16_t((value & ~clear) | set));
}
void muteAmps() {
    for (unsigned amp = 0; amp < 2; ++amp) {
        updateAmp(amp, 5, 0, 0x10);
        updateAmp(amp, 7, 1, 0);
        updateAmp(amp, 4, 0, 3);
    }
}
bool configureAmps() {
    if (marble_audio_stats[6] != 0x2032 || marble_audio_stats[7] != 0x2032) return false;
    for (unsigned amp = 0; amp < 2; ++amp) {
        for (const auto &row : kMusicRegisters[amp]) {
            uint16_t reg = row[0], value = row[1];
            if (reg == 4) value |= 3;  // program with amplifier and device powered down
            if (reg == 5) value |= 0x10; // keep the stock volume; program muted
            if (writeAmp(0x34 + amp, uint8_t(reg), value)) { muteAmps(); return false; }
        }
        uint16_t currentTrim = 0, voltageTrim = 0;
        if (readAmp(0x34 + amp, 0x7a, currentTrim) || readAmp(0x34 + amp, 0x78, voltageTrim)) return false;
        int current = currentTrim & 0x3ff, voltage = voltageTrim & 0x3ff;
        if (current & 0x200) current -= 0x400;
        if (voltage & 0x200) voltage -= 0x400;
        const int calibration = 8192 * (1000 + current) / (1000 + voltage);
        if (writeAmp(0x34 + amp, 0x56, uint16_t(calibration))) return false;
    }
    return true;
}
bool startAmps() {
    for (unsigned amp = 0; amp < 2; ++amp) {
        if (!updateAmp(amp, 4, 1, 0)) { muteAmps(); return false; }
        bool locked = false;
        for (unsigned i = 0; i < 20; ++i) {
            uint16_t status = 0;
            if (readAmp(0x34 + amp, 1, status)) break;
            marble_audio_stats[46 + amp] = status;
            if ((status & 0x11) == 0x11) { locked = true; break; }
            IOSleep(2);
        }
        if (!locked || !updateAmp(amp, 4, 2, 0)) { muteAmps(); return false; }
        uint16_t status = 0;
        IOSleep(2);
        if (readAmp(0x34 + amp, 1, status)) { muteAmps(); return false; }
        // Smart boost can remain in its quiet state for the priming silence.
        // Gate unmute on a locked, fault-free clock; observe boost/switching
        // after real samples arrive instead of requiring load during silence.
        constexpr uint16_t fatalStatus = (1u << 14) | (1u << 11) | (1u << 3) | (1u << 1);
        if ((status & 0x11) != 0x11 || (status & fatalStatus)) {
            for (unsigned reg = 1; reg < 16; ++reg) {
                uint16_t value = 0;
                if (!readAmp(0x34 + amp, uint8_t(reg), value)) marble_audio_amp_registers[amp][reg] = value;
            }
            muteAmps(); return false;
        }
        marble_audio_stats[46 + amp] = status;
        if (!updateAmp(amp, 5, 0x10, 0)) { muteAmps(); return false; }
        for (unsigned reg = 1; reg < 16; ++reg) {
            uint16_t value = 0;
            if (!readAmp(0x34 + amp, uint8_t(reg), value)) marble_audio_amp_registers[amp][reg] = value;
        }
    }
    return true;
}
// Sample TDM pads and amplifier sense while periods are all-silent or all-loud:
// [0..3] signal / [4..7] silence transitions on GPIO121..124, [8..11] signal
// high samples, [12]/[13] samples; per amp at 16 + 24 * amp, see capture script.
void probeSample() {
    const bool signal = (loudHistory & 0xff) == 0xff;
    if (!signal && (loudHistory & 0xff)) return;
    volatile uint32_t *p = marble_audio_probe;
    constexpr unsigned kPins[] = {121u, 122u, 123u, 124u}, kSamples = 2048;
    uint32_t last[4];
    for (unsigned i = 0; i < 4; ++i) last[i] = tlmm.read(kPins[i] * 4096 + 4) & 1;
    for (unsigned n = 0; n < kSamples; ++n) {
        for (unsigned i = 0; i < 4; ++i) {
            const uint32_t level = tlmm.read(kPins[i] * 4096 + 4) & 1;
            p[(signal ? 0 : 4) + i] += level != last[i];
            if (signal) p[8 + i] += level;
            last[i] = level;
        }
    }
    p[signal ? 12 : 13] += kSamples;
    static const uint8_t kRegs[] = {0x01, 0x02, 0x10, 0x21, 0x22, 0x23, 0x57, 0x58};
    for (unsigned amp = 0; amp < 2; ++amp) {
        volatile uint32_t *a = p + 16 + amp * 24;
        uint16_t v[8] = {};
        bool ok = true;
        for (unsigned i = 0; i < 8; ++i) ok = !readAmp(0x34 + amp, kRegs[i], v[i]) && ok;
        if (!ok) { ++a[23]; continue; }
        const int32_t isn = int16_t(v[6]), vsn = int16_t(v[7]);
        const unsigned range = signal ? 2 : 6, polls = a[signal ? 0 : 1]++;
        auto extend = [&](unsigned at, int32_t value) {
            if (!polls || value < int32_t(a[at])) a[at] = uint32_t(value);
            if (!polls || value > int32_t(a[at + 1])) a[at + 1] = uint32_t(value);
        };
        extend(range, isn); extend(range + 2, vsn);
        const uint32_t boost = v[2] & 0x3f;
        if (boost > a[signal ? 10 : 11]) a[signal ? 10 : 11] = boost;
        if (signal) { a[12] |= v[0]; a[13] = polls ? a[13] & v[0] : v[0]; }
        a[14] |= v[1]; a[15] |= v[3];
        a[16] = v[4]; a[17] = v[5]; a[18] = v[2]; a[19] = uint32_t(isn); a[20] = uint32_t(vsn); a[21] = v[0];
    }
}
bool graphControl(uint32_t opcode) {
    uint8_t packet[512];
    const unsigned first = opcode == AR_OP_GRAPH_STOP ? AR_SG_PLAY : AR_SG_SPEAKER;
    const unsigned second = first == AR_SG_PLAY ? AR_SG_SPEAKER : AR_SG_PLAY;
    unsigned bytes = ar_graph_command(packet, sizeof(packet), 0x3001, ++token, opcode, first);
    bool success = command(packet, bytes, opcode);
    bytes = ar_graph_command(packet, sizeof(packet), 0x3001, ++token, opcode, second);
    return command(packet, bytes, opcode) && success;
}
void startOutput() {
    if (!graphsReady || !memoryHandle || marble_audio_stats[40] != 8) return;
    // tert_tdm: 121=mi2s2_sck, 122=mi2s2_data0 (amp feedback in),
    // 123=mi2s2_ws, 124=mi2s2_data1 (speaker data out to the amps).
    const unsigned pins[] = {121u, 122u, 123u};
    for (unsigned gpio : pins) pin(gpio, (1u << 2) | (3u << 6), -1);
    pin(124, (1u << 2) | (3u << 6) | 1u, -1);
    if (!configureAmps()) { fail(12, -1); return; }
    uint8_t packet[512];
    unsigned bytes = ar_prm_clock(packet, sizeof(packet), 0x3001, ++token, 0x204, 96000 * 4 * 32);
    if (!command(packet, bytes, AR_OP_PRM_REQUEST)) { fail(13, int(marble_audio_stats[32])); muteAmps(); return; }
    if (!graphControl(AR_OP_GRAPH_PREPARE) || !graphControl(AR_OP_GRAPH_START)) {
        fail(14, int(marble_audio_stats[32])); muteAmps(); return;
    }
    // Feed silence before unmuting: the endpoint clocks need to lock first.
    bzero(reinterpret_cast<void *>(marble_audio_dma_virtual), 65536);
    inFlight = nextSlot = loudHistory = 0;
    bzero(slotInFlight, sizeof(slotInFlight));
    for (unsigned slot = 0; slot < 4; ++slot) {
        unsigned size = ar_write(packet, sizeof(packet), 0x3001, slot,
            uint64_t(kAudioIova + slot * kPeriodBytes) | (1ULL << 32), memoryHandle, kPeriodBytes);
        if (!size || marble_adsp_send(channel, packet, size)) {
            fail(14, -1); muteAmps(); graphControl(AR_OP_GRAPH_STOP); return;
        }
        slotInFlight[slot] = true; ++inFlight; ++nextSlot;
        ++marble_audio_stats[50];
    }
    if (!startAmps()) { fail(15, int(marble_audio_stats[46])); graphControl(AR_OP_GRAPH_STOP); return; }
    playing = true;
    snapshotSmmu();
    marble_audio_stats[0] = 16;
    ++marble_audio_stats[48];
    IOLog("MarbleAudio: output started %u\n", marble_audio_stats[48]);
}
void stopOutput() {
    if (!playing) return;
    muteAmps();
    graphControl(AR_OP_GRAPH_STOP);
    snapshotSmmu();
    playing = false;
    inFlight = 0;
    bzero(slotInFlight, sizeof(slotInFlight));
    uint8_t packet[512];
    unsigned bytes = ar_prm_clock(packet, sizeof(packet), 0x3001, ++token, 0x204, 0);
    command(packet, bytes, AR_OP_PRM_RELEASE);
    ++marble_audio_stats[49];
    IOLog("MarbleAudio: output stopped %u wanted=%d up=%d\n", marble_audio_stats[49], wanted, up);
}
void submitPeriods() {
    while (playing && inFlight < 4) {
        unsigned slot = nextSlot++ % kSlots;
        if (slotInFlight[slot]) break;
        auto *destination = reinterpret_cast<int32_t *>(marble_audio_dma_virtual + slot * kPeriodBytes);
        IOLockLock(lock);
        int32_t peak = 0;
        for (unsigned frame = 0; frame < kPeriodFrames; ++frame) {
            bool available = ringRead < ringWritten;
            unsigned index = unsigned(ringRead % kRingFrames) * 2;
            destination[frame * 2] = available ? speakerRing[index] : 0;
            destination[frame * 2 + 1] = available ? speakerRing[index + 1] : 0;
            if (available) ++ringRead;
            else ++marble_audio_stats[52];
            const int32_t left = destination[frame * 2];
            if (left > peak) peak = left;
        }
        IOLockUnlock(lock);
        loudHistory = loudHistory << 1 | (peak > (1 << 24));
        __asm__ volatile("dsb sy" ::: "memory");
        uint8_t packet[512];
        unsigned bytes = ar_write(packet, sizeof(packet), 0x3001, slot,
            (uint64_t(kAudioIova + slot * kPeriodBytes) | (1ULL << 32)), memoryHandle, kPeriodBytes);
        if (!bytes || marble_adsp_send(channel, packet, bytes)) { fail(16, -1); stopOutput(); return; }
        slotInFlight[slot] = true;
        ++inFlight; ++marble_audio_stats[50];
    }
}
int speakerOpen(dev_t, int, int, struct proc *) {
    IOLockLock(lock);
    int error = !graphsReady ? ENXIO : endpointOpened ? EBUSY : 0;
    if (!error) { endpointOpened = true; ++marble_audio_stats[53]; }
    IOLockUnlock(lock);
    return error;
}
int speakerClose(dev_t, int, int, struct proc *) {
    IOLockLock(lock); endpointOpened = false; wanted = false;
    ++requestGeneration;
    ringRead = ringWritten = 0; IOLockWakeup(lock, &count, false); IOLockUnlock(lock);
    return 0;
}
int speakerWrite(dev_t, struct uio *uio, int) {
    const auto size = uio_resid(uio);
    if (size < 32 || size > int(32 + MARBLE_SPEAKER_MAX_FRAMES * 8)) return EINVAL;
    auto *buffer = static_cast<uint8_t *>(IOMalloc(size_t(size)));
    if (!buffer) return ENOMEM;
    int error = uiomove(reinterpret_cast<char *>(buffer), int(size), uio);
    const auto *packet = reinterpret_cast<MarbleHapticsPacket *>(buffer);
    if (!error && (packet->magic != MARBLE_SPEAKER_MAGIC || packet->version != 1 || packet->reserved ||
        packet->rate != MARBLE_SPEAKER_RATE || packet->frames > MARBLE_SPEAKER_MAX_FRAMES ||
        size != int(32 + packet->frames * 8) || packet->flags > 2 || (packet->flags && packet->frames))) error = EINVAL;
    if (!error) {
        IOLockLock(lock);
        if (!endpointOpened || !graphsReady || stopping) error = ENXIO;
        else if (packet->flags) {
            wanted = packet->flags == MARBLE_HAPTICS_START;
            ++requestGeneration;
            ringRead = ringWritten = 0;
        } else if (ringWritten - ringRead + packet->frames > kRingFrames) error = EWOULDBLOCK;
        else {
            const auto *samples = reinterpret_cast<const int32_t *>(packet + 1);
            for (unsigned frame = 0; frame < packet->frames; ++frame) {
                unsigned index = unsigned(ringWritten++ % kRingFrames) * 2;
                speakerRing[index] = samples[frame * 2];
                speakerRing[index + 1] = samples[frame * 2 + 1];
            }
            ++marble_audio_stats[54]; marble_audio_stats[55] += packet->frames;
        }
        IOLockWakeup(lock, &count, false); IOLockUnlock(lock);
    }
    if (error) marble_audio_stats[56] = uint32_t(error);
    IOFree(buffer, size_t(size));
    return error;
}
struct cdevsw speakerSwitch = {
    .d_open = speakerOpen, .d_close = speakerClose, .d_read = eno_rdwrt,
    .d_write = speakerWrite, .d_ioctl = eno_ioctl, .d_stop = eno_stop,
    .d_reset = eno_reset, .d_ttys = nullptr, .d_select = eno_select,
    .d_mmap = eno_mmap, .d_strategy = eno_strat,
    .d_reserved_1 = eno_getc, .d_reserved_2 = eno_putc, .d_type = 0
};
void state(void *, int value) {
    IOLog("MarbleAudio: DSP channel %s\n", value ? "up" : "down");
    IOLockLock(lock); up = value != 0; marble_audio_stats[20] = up;
    if (!up) { wanted = false; ++requestGeneration; }
    IOLockWakeup(lock, &count, false); IOLockUnlock(lock);
}
void receive(void *, const uint8_t *data, uint32_t bytes) {
    IOLockLock(lock);
    if (bytes <= 512 && count < 16) {
        Message &message = inbox[(head + count) % 16];
        memcpy(message.data, data, bytes); message.size = bytes; ++count;
        const unsigned index = marble_audio_stats[21] % 16;
        bzero(const_cast<uint32_t *>(marble_audio_messages[index]), 64);
        memcpy(const_cast<uint32_t *>(marble_audio_messages[index]), data, bytes < 64 ? bytes : 64);
        ++marble_audio_stats[21]; IOLockWakeup(lock, &count, false);
    } else ++marble_audio_stats[22];
    IOLockUnlock(lock);
}
void fail(unsigned stage, int error) {
    marble_audio_stats[0] = uint32_t(-int(stage)); marble_audio_stats[1] = uint32_t(error);
    IOLog("MarbleAudio: stage=%d error=%d\n", -int(stage), error);
}
void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "marble-audio");
    marble_audio_stats[0] = 1;
    if (!gcc.open(0x100000, 0x100000) || !se.open(0x880000, 0x4000) || !tlmm.open(0xf100000, 0x100000)) {
        fail(1, -1); running = false; return;
    }
    marble_audio_stats[2] = se.read(GENI_FW_REVISION);
    marble_audio_stats[3] = se.read(GENI_CLK_CFG);
    marble_audio_stats[4] = gcc.read(0x62010);
    gcc.write(0x62010, gcc.read(0x62010) | (1u << 4) | (1u << 2) | (1u << 1));
    for (unsigned i = 0; i < 100 && (gcc.read(0x2e00c) & 0x80000000); ++i) IOSleep(1);
    pin(56, 3u | (1u << 2), -1); pin(57, 3u | (1u << 2), -1);
    // Stock Music keeps the receiver/speaker switch low.
    pin(52, (1u << 9) | 1u, 0);
    pin(1, (1u << 9) | 1u, 0); pin(120, (1u << 9) | 1u, 0);
    IOSleep(2);
    pin(1, (1u << 9) | 1u, 1); pin(120, (1u << 9) | 1u, 1);
    IOSleep(5);
    se.write(GENI_CLK_CFG, (2u << 4) | 1u);
    se.write(GENI_SCL_COUNTERS, (5u << 20) | (11u << 10) | 22u);
    GeniIo io = {.read = regRead, .write = regWrite, .ctx = &se}; uint32_t config = 0;
    if (geni_i2c_prepare(&bus, &io, 0x34, &config)) { fail(2, int(config)); running = false; return; }
    marble_audio_stats[5] = config;
    for (unsigned amp = 0; amp < 2; ++amp) {
        uint16_t value = 0;
        int result = readAmp(0x34 + amp, 0, value);
        marble_audio_stats[6 + amp] = value;
        marble_audio_stats[8 + amp] = uint32_t(result);
        IOLog("MarbleAudio: amplifier %u address=%x id=%x error=%d\n", amp, 0x34 + amp, value, result);
        if (!result && value == 0x2032) {
            for (unsigned reg = 1; reg < 0x80; ++reg) {
                if (!readAmp(0x34 + amp, uint8_t(reg), value)) marble_audio_amp_registers[amp][reg] = value;
            }
        }
    }
    marble_audio_stats[0] = 3;
    static const MarbleAdspClient client = {state, receive};
    channel = marble_adsp_channel("adsp_apps", 512, 20, 1, &client, nullptr);
    if (!channel) { fail(3, -1); running = false; return; }
    GprSpfProbe probe; gpr_spf_init(&probe, 3, 60);
    bool graphsAttempted = false;
    IOLockLock(lock);
    uint64_t deadline = 0, retryAt = 0;
    while (!stopping) {
        while (count) {
            Message message = inbox[head]; head = (head + 1) % 16; --count;
            IOLockUnlock(lock);
            ArAnswer answer = {};
            if (ar_parse(message.data, message.size, &answer) != ArBad &&
                answer.opcode == AR_OP_WRITE_DONE && answer.token < kSlots && slotInFlight[answer.token]) {
                slotInFlight[answer.token] = false;
                if (inFlight) --inFlight;
                ++marble_audio_stats[51];
                marble_audio_stats[57] = answer.status;
                if (playing && !(marble_audio_stats[51] % 5)) probeSample();
                if (playing && !(marble_audio_stats[51] % 20)) {
                    snapshotSmmu();
                    bool healthy = true;
                    for (unsigned amp = 0; amp < 2; ++amp) {
                        uint16_t status = 0;
                        if (readAmp(0x34 + amp, 1, status) || (status & 0x480a) || (status & 0x11) != 0x11)
                            healthy = false;
                        marble_audio_stats[46 + amp] = status;
                    }
                    if (!healthy) { fail(17, int(marble_audio_stats[46])); wanted = false; stopOutput(); }
                }
            }
            const bool wasReady = probe.state == SpfReady;
            int result = gpr_spf_receive(&probe, message.data, message.size);
            if (probe.state == SpfIdle) clock_interval_to_deadline(1000, kMillisecondScale, &retryAt);
            marble_audio_stats[23] = uint32_t(result);
            marble_audio_stats[24] = probe.state; marble_audio_stats[25] = probe.last_status;
            marble_audio_stats[26] = probe.last_opcode;
            if (!wasReady && probe.state == SpfReady) { marble_audio_stats[0] = 4; IOLog("MarbleAudio: DSP audio framework ready\n"); }
            IOLockLock(lock);
        }
        if (up && !graphsAttempted && (probe.state == SpfReady || probe.state == SpfGaveUp)) {
            graphsAttempted = true;
            IOLockUnlock(lock);
            openGraphs();
            if (graphsReady) {
                speakerRing = reinterpret_cast<int32_t *>(marble_audio_dma_virtual + 65536);
                bzero(speakerRing, kRingFrames * 8);
                speakerMajor = cdevsw_add(-1, &speakerSwitch);
                if (speakerMajor >= 0) speakerNode = devfs_make_node(makedev(speakerMajor, 0), DEVFS_CHAR,
                    0, 0, 0666, "marble-speaker");
                marble_audio_stats[58] = speakerNode != nullptr;
            }
            IOLockLock(lock);
        }
        if (graphsReady && (wanted != playing || (wanted && requestGeneration != playbackGeneration))) {
            bool start = wanted;
            const uint32_t generation = requestGeneration;
            IOLockUnlock(lock);
            stopOutput();
            if (start) startOutput();
            IOLockLock(lock);
            playbackGeneration = generation;
            if (start && !playing && generation == requestGeneration) wanted = false;
        }
        if (playing && wanted && requestGeneration == playbackGeneration) {
            IOLockUnlock(lock); submitPeriods(); IOLockLock(lock);
        }
        uint64_t now = mach_absolute_time();
        if (up && probe.state != SpfReady && probe.state != SpfGaveUp && (!retryAt || now >= retryAt)) {
            uint8_t packet[24]; uint32_t bytes = gpr_spf_next(&probe, packet);
            if (bytes) {
                IOLockUnlock(lock); int result = marble_adsp_send(channel, packet, bytes); IOLockLock(lock);
                marble_audio_stats[27] = uint32_t(result); ++marble_audio_stats[28];
                clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
            }
        }
        // Callbacks can queue replies while graph/amplifier work releases the
        // lock. Their wakeups precede this point, so drain that work before sleep.
        if (count || (graphsReady && (wanted != playing || (wanted && requestGeneration != playbackGeneration)))) continue;
        if (probe.state == SpfWaiting) {
            int result = IOLockSleepDeadline(lock, &count, deadline, THREAD_UNINT);
            if (result == THREAD_TIMED_OUT) {
                gpr_spf_timeout(&probe); ++marble_audio_stats[29];
                clock_interval_to_deadline(1000, kMillisecondScale, &retryAt);
            }
        } else if (up && probe.state == SpfIdle && retryAt) IOLockSleepDeadline(lock, &count, retryAt, THREAD_UNINT);
        else IOLockSleep(lock, &count, THREAD_UNINT);
    }
    IOLockUnlock(lock);
    stopOutput();
    muteAmps();
    running = false;
}
}
extern "C" void native_marble_audio_start(void) {
    unsigned enabled = 0; PE_parse_boot_argn("marble-audio", &enabled, sizeof(enabled));
    if (!enabled || running) return;
    lock = IOLockAlloc(); if (!lock) return;
    running = true; stopping = false;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread)) running = false;
    else thread_deallocate(thread);
}
extern "C" void native_marble_audio_stop(void) {
    if (!lock) return;
    IOLockLock(lock); stopping = true; IOLockWakeup(lock, &count, false); IOLockUnlock(lock);
    for (unsigned i = 0; i < 40 && __atomic_load_n(&running, __ATOMIC_ACQUIRE); ++i) IOSleep(5);
    // Block the DSP stream before resident firmware can reuse kernel memory.
    if (dmaSmmu) {
        *reinterpret_cast<volatile uint32_t *>(dmaSmmu + 0xc00 + 4 * dmaSlot) =
            (2u << 16) | (dmaExtended ? 1u << 10 : 0);
        __asm__ volatile("dsb sy" ::: "memory");
        *reinterpret_cast<volatile uint32_t *>(dmaSmmu + dmaContext) = 0;
        __asm__ volatile("dsb sy" ::: "memory");
    }
}

// Called with UFS quiesced, during its initial apps-SMMU configuration. The
// audio stream owns a separate context bank and an IOVA window; storage and
// display retain their mappings. DTS msm-audio-ion supplies SID 0x1801.
extern "C" void native_marble_audio_dma_prepare(uintptr_t smmu) {
    unsigned enabled = 0; PE_parse_boot_argn("marble-audio", &enabled, sizeof(enabled));
    if (enabled < 2 || dmaBuffer) return;
    auto read = [smmu](unsigned offset) { return *reinterpret_cast<volatile uint32_t *>(smmu + offset); };
    auto write = [smmu](unsigned offset, uint32_t value) {
        *reinterpret_cast<volatile uint32_t *>(smmu + offset) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    };
    const uint32_t id0 = read(0x20), id1 = read(0x24), id2 = read(0x28);
    const unsigned groups = id0 & 255, banks = id1 & 255;
    const unsigned pages = 1u << (((id1 >> 28) & 7) + 1);
    const bool extended = (id0 & (1u << 8)) && (read(0) & (1u << 3));
    if (!groups || groups > 128 || !banks || banks > 128 || (id1 >> 31) ||
        (pages + banks) * 4096 > 0xd0000 || !(id2 & (1u << 12))) { marble_audio_stats[40] = 1; return; }
    bool used[128] = {};
    int slot = -1;
    for (unsigned i = 0; i < groups; ++i) {
        uint32_t smr = read(0x800 + 4 * i), s2cr = read(0xc00 + 4 * i);
        bool valid = extended ? (s2cr & (1u << 10)) : (smr & 0x80000000u);
        if (!valid) { if (slot < 0) slot = int(i); continue; }
        if (((s2cr >> 16) & 3) == 0) used[s2cr & 127] = true;
        unsigned mask = (smr >> 16) & (extended ? 0xffff : 0x7fff);
        if (((smr ^ 0x1801u) & 0xffff & ~mask) == 0) {
            if (mask) { marble_audio_stats[40] = 2; return; }
            slot = int(i);
        }
    }
    int bank = -1;
    for (int i = int(banks) - 1; i >= 0; --i)
        if (!used[i] && !(read((pages + unsigned(i)) * 4096) & 1)) { bank = i; break; }
    if (slot < 0 || bank < 0) { marble_audio_stats[40] = 3; return; }
    dmaBuffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, kAudioDmaBytes, kAudioDmaBytes);
    dmaTables = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, 8192, 4096);
    if (!dmaBuffer || !dmaTables) { marble_audio_stats[40] = 4; return; }
    const uint64_t physical = dmaBuffer->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    const uint64_t tables = dmaTables->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if ((physical & (kAudioDmaBytes - 1)) || physical >= (1ULL << 40)) { marble_audio_stats[40] = 5; return; }
    bzero(dmaBuffer->getBytesNoCopy(), kAudioDmaBytes);
    auto *l1 = static_cast<uint64_t *>(dmaTables->getBytesNoCopy());
    auto *l2 = l1 + 512;
    bzero(l1, 8192);
    l1[0] = (tables + 4096) | 3;
    l2[kAudioIova >> 21] = physical | (1ULL << 11) | (1ULL << 10) | (3ULL << 8) | (1ULL << 6) | 1;
    for (unsigned offset = 0; offset < 8192; offset += 64)
        __asm__ volatile("dc civac, %0" :: "r"(reinterpret_cast<uint8_t *>(l1) + offset) : "memory");
    for (unsigned offset = 0; offset < kAudioDmaBytes; offset += 64)
        __asm__ volatile("dc civac, %0" :: "r"(static_cast<uint8_t *>(dmaBuffer->getBytesNoCopy()) + offset) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    const unsigned cb = (pages + unsigned(bank)) * 4096;
    dmaSmmu = smmu; dmaContext = cb; dmaSlot = unsigned(slot); dmaExtended = extended;
    write(0xc00 + 4 * unsigned(slot), (2u << 16) | (extended ? 1u << 10 : 0));
    write(cb, 0); write(0x1800 + 4 * unsigned(bank), 1); write(0x1000 + 4 * unsigned(bank), 0x1f300);
    *reinterpret_cast<volatile uint64_t *>(smmu + cb + 0x20) = tables;
    *reinterpret_cast<volatile uint64_t *>(smmu + cb + 0x28) = 0;
    write(cb + 0x10, (7u << 15) | ((id2 >> 4) & 15) | (1u << 4));
    write(cb + 0x30, (2u << 30) | (2u << 12) | 32u | (1u << 23)); write(cb + 0x38, 0xff);
    write(cb + 0x3c, 0); write(cb + 0x58, 0xffffffffu);
    write(cb + 0x618, 0); write(cb + 0x7f0, 0);
    for (unsigned spin = 0; (read(cb + 0x7f4) & 1) && spin < 10000; ++spin) marbleMicrosecondDelay(1);
    if (read(cb + 0x7f4) & 1) { marble_audio_stats[40] = 9; return; }
    write(cb, 0x1067);
    write(0x800 + 4 * unsigned(slot), (extended ? 0 : 0x80000000u) | 0x1801u);
    write(0xc00 + 4 * unsigned(slot), unsigned(bank) | (extended ? 1u << 10 : 0));
    const bool hardwareAtos = (id0 & (1u << 30)) && !(id0 & (1u << 26));
    if (hardwareAtos) *reinterpret_cast<volatile uint64_t *>(smmu + cb + 0x800) = kAudioIova;
    __asm__ volatile("dsb sy" ::: "memory");
    unsigned tries = 0;
    while ((read(cb + 0x8f0) & 1) && ++tries < 1000) marbleMicrosecondDelay(1);
    const uint64_t translated = hardwareAtos ? *reinterpret_cast<volatile uint64_t *>(smmu + cb + 0x50) : 0;
    marble_audio_stats[41] = unsigned(slot); marble_audio_stats[42] = unsigned(bank);
    marble_audio_stats[43] = uint32_t(translated); marble_audio_stats[44] = uint32_t(translated >> 32);
    marble_audio_stats[59] = id2; marble_audio_stats[60] = read(cb);
    marble_audio_stats[61] = read(0x1000 + 4 * unsigned(bank));
    marble_audio_stats[62] = read(cb + 0x30); marble_audio_stats[63] = id0;
    if (hardwareAtos && ((translated & 1) || (translated & 0x0000fffffffff000ULL) != physical)) { marble_audio_stats[40] = 6; return; }
    dmaMapping = dmaBuffer->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
    if (!dmaMapping) { marble_audio_stats[40] = 7; return; }
    marble_audio_dma_physical = physical;
    marble_audio_dma_virtual = dmaMapping->getVirtualAddress();
    marble_audio_stats[40] = 8;
    IOLog("MarbleAudio: DMA stream=1801 IOVA=%x PA=%llx bank=%d ATOS=%llx\n", kAudioIova, physical, bank, translated);
}
