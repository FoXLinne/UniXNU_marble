// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board wiring: marble live DT. Register layout: gcc-waipio and pinctrl-cape.
// Goodix wire format: MiCode gold-s-oss, GT9916R.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "GeniSpi.h"
#include "SmmuBypass.h"
void native_touch_publish(unsigned count, const uint8_t *points);
void native_marble_touch_start(void);
}

namespace {
unsigned touchStage, touchEvents, touchFrames, touchInvalid, touchIrq;
uint32_t touchHeader;
int touchResult;
struct Region {
    IOMemoryMap *mapping = nullptr;
    uintptr_t address = 0;
    bool map(uint64_t physical, uint64_t bytes) {
        auto *memory = IODeviceMemory::withRange(physical, bytes);
        mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        address = mapping ? mapping->getVirtualAddress() : 0;
        return address != 0;
    }
    ~Region() { OSSafeReleaseNULL(mapping); }
};
uint32_t read32(void *cookie, uint32_t offset) {
    return *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(cookie) + offset);
}
void write32(void *cookie, uint32_t offset, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(cookie) + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
void delay(void *, unsigned us) { IODelay(us); }
void cacheRange(void *, void *data, size_t bytes) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4U << ((ctr >> 16) & 15);
    uintptr_t end = reinterpret_cast<uintptr_t>(data) + bytes;
    for (uintptr_t at = reinterpret_cast<uintptr_t>(data) & ~(line - 1); at < end; at += line)
        __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
uint16_t little16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | unsigned(p[1]) << 8); }
uint32_t little32(const uint8_t *p) { return little16(p) | uint32_t(little16(p + 2)) << 16; }
bool checksum(const uint8_t *data, size_t bytes) {
    if (bytes < 2) return false;
    unsigned sum = 0;
    for (size_t i = 0; i < bytes - 2; ++i) sum += data[i];
    return (sum & 65535) == little16(data + bytes - 2);
}
// RPMh VRM resources and AP DRV2 wiring come from the marble device tree.
struct TouchPower {
    Region database, controller;
    uint32_t resource(const char *name) {
        auto *db = reinterpret_cast<const uint8_t *>(database.address);
        const uint8_t magic[] = {0xdb, 0x30, 0x03, 0x0c};
        if (memcmp(db + 4, magic, sizeof(magic))) return 0;
        for (unsigned group = 0; group < 8; ++group) {
            const uint8_t *header = db + 8 + group * 16;
            unsigned offset = 144 + little16(header + 2), count = little16(header + 6);
            if (offset + count * 24 > 0x20000) return 0;
            for (unsigned index = 0; index < count; ++index) {
                const uint8_t *entry = db + offset + index * 24;
                if (!strncmp(reinterpret_cast<const char *>(entry), name, 8)) return little32(entry + 16);
            }
        }
        return 0;
    }
    int enable(uint32_t address, unsigned millivolts) {
        auto *base = reinterpret_cast<void *>(controller.address);
        // The first three TCS slots belong to active requests on DRV2.
        unsigned slot = 0;
        for (; slot < 3; ++slot)
            if (read32(base, 0xd00 + slot * 672 + 0x18)) break;
        if (slot == 3) return GENI_ERR_IO;
        unsigned tcs = 0xd00 + slot * 672;
        uint32_t control = read32(base, tcs + 0x14) & ~(1U << 24);
        write32(base, tcs + 0x14, control);
        write32(base, tcs + 0x14, control & ~(1U << 16));
        write32(base, 0xd08, 1U << slot);
        write32(base, tcs + 0x1c, 0);
        write32(base, tcs + 0x10, 7);
        const uint32_t offsets[] = {0, 8, 4};
        const uint32_t values[] = {millivolts, 7, 1};
        for (unsigned i = 0; i < 3; ++i) {
            unsigned cmd = tcs + i * 20;
            write32(base, cmd + 0x30, 0x10108);
            write32(base, cmd + 0x34, address + offsets[i]);
            write32(base, cmd + 0x38, values[i]);
        }
        write32(base, tcs + 0x1c, 7);
        write32(base, tcs + 0x14, 1U << 16);
        write32(base, tcs + 0x14, (1U << 16) | (1U << 24));
        unsigned elapsed = 0;
        for (; elapsed < 100; ++elapsed) {
            if (read32(base, 0xd04) & (1U << slot)) break;
            IOSleep(1);
        }
        uint32_t status[3];
        for (unsigned i = 0; i < 3; ++i) status[i] = read32(base, tcs + 0x3c + i * 20);
        IOLog("MarbleTouch: power resource=%x mV=%u TCS=%u status=%x/%x/%x elapsed=%u\n",
            address, millivolts, slot, status[0], status[1], status[2], elapsed);
        if (elapsed == 100) return GENI_ERR_TIMEOUT;
        write32(base, tcs + 0x1c, 0);
        write32(base, 0xd08, 1U << slot);
        write32(base, tcs + 0x14, 1U << 16);
        return (status[0] & status[1] & status[2] & (1U << 16)) ? 0 : GENI_ERR_IO;
    }
    int start() {
        if (!database.map(0x80860000, 0x20000) || !controller.map(0x17a20000, 0x10000)) return GENI_ERR_IO;
        uint32_t iovdd = resource("ldoc2"), avdd = resource("ldoc9");
        IOLog("MarbleTouch: RPMh ldoc2=%x ldoc9=%x config=%x\n", iovdd, avdd,
            read32(reinterpret_cast<void *>(controller.address), 0xc));
        if (!iovdd || !avdd) return GENI_ERR_IO;
        int result = enable(iovdd, 1800);
        if (result) return result;
        IOSleep(3);
        result = enable(avdd, 3224);
        IOSleep(15);
        return result;
    }
};
struct Touch {
    Region gcc, pins, engine, wrapper, smmu;
    IOBufferMemoryDescriptor *dma = nullptr;
    geni_spi spi = {};
    uint8_t tx[1033] = {}, rx[1033] = {};
    int prepareDma() {
        IOLog("MarbleTouch: preparing QUP DMA\n");
        dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIODirectionInOut | kIOMemoryPhysicallyContiguous, 8192, 0xfffffff000ULL);
        if (!dma || dma->prepare() != kIOReturnSuccess || !smmu.map(0x15000000, 0x100000)) return GENI_ERR_IO;
        addr64_t length = 0, physical = dma->getPhysicalSegment(0, &length);
        if (length < 8192) return GENI_ERR_IO;
        smmu_io io = {reinterpret_cast<void *>(smmu.address), read32, write32};
        IOLog("MarbleTouch: SMMU id=%x/%x/%x control=%x\n", io.rd(io.ctx, 0x20),
            io.rd(io.ctx, 0x24), io.rd(io.ctx, 0x28), io.rd(io.ctx, 0));
        auto plan = smmu_plan_bypass(&io, 0x5a3, physical, 8192, 78, 120);
        IOLog("MarbleTouch: SMMU id=%x control=%x extended=%d bank=%u slot=%u\n",
            io.rd(io.ctx, 0x20), io.rd(io.ctx, 0), plan.extended, plan.bank, plan.slot);
        const char *error = smmu_configure_bypass(&io, &plan, 0x5a3);
        IOLog("MarbleTouch: DMA pa=%llx SMMU bank=%u slot=%u status=%s\n",
            physical, plan.bank, plan.slot, error ? error : "ready");
        if (error) return GENI_ERR_IO;
        auto *memory = static_cast<uint8_t *>(dma->getBytesNoCopy());
        spi.use_dma = 1;
        spi.dma = {nullptr, cacheRange, cacheRange, cacheRange,
            memory, memory + 4096, physical, physical + 4096, 4096};
        return 0;
    }
    int read(uint32_t address, uint8_t *data, size_t bytes) {
        if (bytes > sizeof(tx) - 8) return GENI_ERR_PARAM;
        memset(tx, 0xff, bytes + 8);
        tx[0] = 0xf1;
        for (unsigned i = 0; i < 4; ++i) tx[i + 1] = static_cast<uint8_t>(address >> (24 - 8 * i));
        int result = geni_spi_transfer(&spi, tx, rx, bytes + 8, 0);
        if (!result) memcpy(data, rx + 8, bytes);
        else IOLog("MarbleTouch: SPI read=%d address=%x irq=%x status=%x tx=%x rx=%x\n",
            result, address, spi.snap_irq, spi.snap_status, spi.snap_txfs, spi.snap_rxfs);
        return result;
    }
    int write(uint32_t address, const uint8_t *data, size_t bytes) {
        if (bytes > sizeof(tx) - 5) return GENI_ERR_PARAM;
        tx[0] = 0xf0;
        for (unsigned i = 0; i < 4; ++i) tx[i + 1] = static_cast<uint8_t>(address >> (24 - 8 * i));
        memcpy(tx + 5, data, bytes);
        return geni_spi_transfer(&spi, tx, nullptr, bytes + 5, 0);
    }
    int acknowledge(uint32_t address) {
        const uint8_t zero = 0;
        return write(address, &zero, 1);
    }
    int initialize() {
        if (!gcc.map(0x100000, 0x70000) || !pins.map(0xf110000, 0x6000) ||
            !wrapper.map(0x9c0000, 0x1000) || !engine.map(0x990000, 0x4000)) return GENI_ERR_IO;
        auto *g = reinterpret_cast<void *>(gcc.address);
        write32(g, 0x62018, read32(g, 0x62018) | 1);
        write32(g, 0x62008, read32(g, 0x62008) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9) | (1 << 14));
        IOSleep(1);
        auto *p = reinterpret_cast<void *>(pins.address);
        for (unsigned pin = 0; pin < 4; ++pin)
            write32(p, pin * 0x1000, 4 | ((pin == 3 ? 7 : 2) << 6) | (pin == 3 ? 3 : 0));
        write32(p, 0x5000, (3 << 6) | 3);
        write32(p, 0x4004, 0);
        write32(p, 0x4000, (1 << 9) | (3 << 6) | 3);
        IOSleep(10);
        TouchPower power;
        int powered = power.start();
        if (powered) return powered;
        write32(p, 0x4004, 2);
        IOSleep(100);
        spi.se = {reinterpret_cast<void *>(engine.address), read32, write32, delay};
        spi.wrapper = {reinterpret_cast<void *>(wrapper.address), read32, write32, delay};
        spi.poll_only = 2;
        spi.poll_limit = 10000;
        int result = geni_spi_read_info(&spi);
        if (!result && spi.info.fifo_disabled) result = prepareDma();
        if (!result) result = geni_spi_init(&spi);
        IOLog("MarbleTouch: GENI protocol=%u FIFO=%u disabled=%d init=%d\n",
            spi.info.protocol, spi.info.tx_fifo_depth, spi.info.fifo_disabled, result);
        if (result) return result;
        result = geni_spi_setup(&spi, 0, 0);
        if (result) return result;
        // GENI selects an existing DFS performance level; its source rate comes from GCC.
        unsigned level = 0;
        uint64_t rate = 0;
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t cfg = read32(g, 0x274e4 + 0x1c + 4 * i);
            unsigned source = (cfg >> 8) & 7, divider = cfg & 31;
            uint64_t hz = source == 0 ? 19200000 : source == 1 ? 600000000 : source == 6 ? 300000000 : 0;
            if (divider) hz = hz * 2 / (divider + 1);
            if ((cfg >> 12) & 3) {
                uint32_t m = read32(g, 0x274e4 + 0x5c + 4 * i) & 65535;
                uint32_t n = ((~read32(g, 0x274e4 + 0x9c + 4 * i)) & 65535) + m;
                hz = n ? hz * m / n : 0;
            }
            if (hz && (!rate || hz < rate)) { level = i; rate = hz; }
        }
        if (!rate) return GENI_ERR_IO;
        return geni_spi_set_clock(&spi, 1000000, static_cast<unsigned>(rate), level);
    }
    int run() {
        int result = initialize();
        if (result) return result;
        touchStage = 2;
        uint8_t confirm[8], returned[8];
        memset(confirm, 0xaa, sizeof(confirm));
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            result = write(0x10000, confirm, sizeof(confirm));
            if (!result) result = read(0x10000, returned, sizeof(returned));
            if (result) return result;
            IOLog("MarbleTouch: confirm=%08x/%08x tx=%u rx=%u\n", little32(returned),
                little32(returned + 4), spi.last_tx_in, spi.last_rx_in);
            if (!memcmp(confirm, returned, sizeof(confirm))) break;
            if (attempt == 2) return GENI_ERR_PROTO;
            IOSleep(5);
        }
        uint8_t version[28];
        result = read(0x10014, version, sizeof(version));
        if (result) return result;
        IOLog("MarbleTouch: version checksum=%u PID=%.8s sensor=%u\n",
            checksum(version, sizeof(version)), version + 10, version[23]);
        if (!checksum(version, sizeof(version))) return GENI_ERR_PROTO;
        uint8_t info[1024];
        result = read(0x10070, info, 2);
        if (result) return result;
        size_t bytes = little16(info);
        IOLog("MarbleTouch: IC info length=%lu\n", bytes);
        if (bytes < 34 || bytes > sizeof(info)) return GENI_ERR_PROTO;
        result = read(0x10070, info, bytes);
        if (result) return result;
        if (!checksum(info, bytes)) return GENI_ERR_PROTO;
        size_t at = 2 + 16 + 10 + 4;
        for (unsigned i = 0; i < 5; ++i) {
            if (at >= bytes) return GENI_ERR_PROTO;
            at += 1 + 2 * info[at];
        }
        if (at + 52 > bytes) return GENI_ERR_PROTO;
        uint32_t events = little32(info + at + 44);
        IOLog("MarbleTouch: IC info=%lu event=%x head=%u point=%u\n", bytes,
            events, little16(info + at + 48), little16(info + at + 50));
        if (!events || little16(info + at + 48) != 8 || little16(info + at + 50) != 8) return GENI_ERR_PROTO;
        touchStage = 3;
        uint8_t frame[90];
        for (;;) {
            touchIrq = read32(reinterpret_cast<void *>(pins.address), 0x5004) & 1;
            if (touchIrq) { IOSleep(8); continue; }
            // Match the controller driver's two-contact pre-read; extend for additional contacts.
            result = read(events, frame, 26);
            if (result) return result;
            touchEvents++;
            touchHeader = little32(frame);
            if (!checksum(frame, 8)) {
                touchInvalid++;
                IOSleep(1);
                continue; // Preserve the pending event while its header is being updated.
            }
            unsigned count = frame[2] & 15;
            bool touchEvent = (frame[0] & 0x80) && count <= 10;
            if (touchEvent && count > 2) {
                result = read(events + 26, frame + 26, (count - 2) * 8);
                if (result) return result;
            }
            bool valid = touchEvent && (!count || checksum(frame + 8, count * 8 + 2));
            // Release the controller before entering the HID work loop.
            result = acknowledge(events);
            if (result) return result;
            if (valid) {
                // Raw frame trail for the coordinate mapping: one line per contact-count change and every eighth frame.
                static unsigned framesLogged, lastCount = ~0u;
                if (count != lastCount || (++framesLogged & 7) == 0) {
                    IOLog("MarbleTouch: frame n=%u id=%u x=%u y=%u\n", count,
                        count ? frame[8] >> 4 : 0xff, count ? little16(frame + 10) : 0,
                        count ? little16(frame + 12) : 0);
                    lastCount = count;
                }
                touchFrames++;
                native_touch_publish(count, frame + 8);
            } else touchInvalid++;
            IOSleep(1);
        }
    }
};
void worker(void *, wait_result_t) {
    auto *touch = new Touch;
    int result = touch ? touch->run() : GENI_ERR_IO;
    touchResult = result;
    touchStage = 4;
    IOLog("MarbleTouch: stopped status=%d\n", result);
    delete touch;
}
}
extern "C" void native_marble_touch_start(void) {
    static bool started;
    if (__atomic_exchange_n(&started, true, __ATOMIC_RELAXED)) return;
    touchStage = 1;
    thread_t thread;
    kern_return_t result = kernel_thread_start(worker, nullptr, &thread);
    if (!result) thread_deallocate(thread);
    else IOLog("MarbleTouch: thread start=%d\n", result);
}
extern "C" void native_marble_touch_report(char *out, unsigned size) {
    snprintf(out, size, "MarbleTouch stage=%u result=%d irq=%u events=%u frames=%u invalid=%u header=%08x",
        touchStage, touchResult, touchIrq, touchEvents, touchFrames, touchInvalid, touchHeader);
}
