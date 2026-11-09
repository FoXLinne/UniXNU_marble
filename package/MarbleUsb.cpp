// SPDX-License-Identifier: BSD-2-Clause-Patent
// Marble board glue for UniXNU QcomDwc3. Endpoint and transfer handling belongs
// to the original iOS AppleUSBXDCI and IOUSBDeviceFamily implementations.
#include "OriginalUsbXdciABI.h"
#include "QcomEusb2Phy.h"
#include "QcomRpmh.h"
#include "GeniI2c.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <Protocol/RootStage.h>
extern "C" const ROOT_STAGE_TABLE *native_resident_table(void);
extern "C" int armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *);
extern "C" int armcpu_spi_mask(uint32_t,bool);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" unsigned int real_ncpus;
extern "C" void marble_usb_static_init(void) __asm__("__GLOBAL__sub_I_MarbleUsb.cpp");
extern "C" {volatile uint32_t native_usb_stats[54];}
namespace {
constexpr uint64_t kCore=0xa600000,kAppleWindow=kCore+0xc000,kSmmu=0x15000000;
constexpr uint32_t kIrq=165,kSid=0;
struct Region {IOMemoryMap *map=nullptr;volatile uint32_t *va=nullptr;};
Region gGcc,gScratch,gPhy,gCoreMap,gSmmu;
bool gCoreClocked,gSmmuInstalled;
uint32_t gPhyCalibration[13];
constexpr uint16_t kPhyCalibrationOffsets[]=ROOT_STAGE_USB_PHY_CALIBRATION_OFFSETS;
IOLock *gRoleLock;
volatile bool gStopping;
int gWanted=1,gApplied=-1;
void stage(int value){native_usb_stats[0]=uint32_t(value);IOLog("MarbleUSB: stage=%d\n",value);}
void barrier(){__asm__ volatile("dsb sy" ::: "memory");}
bool mapRegion(Region &r,uint64_t address,uint64_t size){
 auto *m=IODeviceMemory::withRange(address,size);
 r.map=m?m->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;OSSafeReleaseNULL(m);
 r.va=r.map?reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()):nullptr;
 return r.va!=nullptr;
}
bool waitBits(volatile uint32_t *reg,uint32_t mask,uint32_t value,unsigned ms){
 for(unsigned i=0;i<ms*10;++i){if((*reg&mask)==value)return true;IODelay(100);}
 return (*reg&mask)==value;
}
void vbusOverride(bool on){
 auto *reg=gScratch.va+0x810/4;const uint32_t mask=(1u<<20)|(1u<<28);
    auto *ss=gScratch.va+0x830/4;const uint32_t present=1u<<24;
    *ss=on?*ss|present:*ss&~present;
    *reg=on?*reg|mask:*reg&~mask;barrier();native_usb_stats[10]=*reg;
}
// This lock protects the USB stream transaction; other drivers retain their
// stream entries. Existing firmware USB entries are saved for resident return.

struct SavedStream {uint32_t index,smr,s2cr;};
SavedStream gSavedStreams[128];unsigned gSavedCount;
void saveUsbStreams(){
 unsigned groups=gSmmu.va[0x20/4]&255;if(groups>128)groups=128;
 for(unsigned i=0;i<groups;++i){
  uint32_t smr=gSmmu.va[(0x800+4*i)/4];
  if((smr&0x80000000u)&&!((smr^kSid)&0xffff&~((smr>>16)&0x7fff))){
   gSavedStreams[gSavedCount++]={i,smr,gSmmu.va[(0xc00+4*i)/4]};
   gSmmu.va[(0x800+4*i)/4]=0;
  }
 }
 barrier();
}
// Apple's controller driver does no cache maintenance: Apple's fabric is coherent. Here coherence comes from the SMMU's output
// attributes, as in Linux (dma-coherent, IOMMU_CACHE page tables): a stage 1 context with an identity map of DRAM in 1 GiB blocks,
// Normal write-back inner shareable. The bypass bank keeps device attributes and leaves this master non-coherent. No IOMapper:
// IODMACommand hands the physical address to the core, which the identity map turns back into the same address.
IOBufferMemoryDescriptor *gSmmuTable;
unsigned gSmmuBank, gSmmuPages, gSmmuAsid;
constexpr uint32_t kCbSctlr = 0x0, kCbTcr2 = 0x10, kCbTtbr0 = 0x20, kCbTtbr1 = 0x28, kCbTcr = 0x30, kCbMair0 = 0x38, kCbMair1 = 0x3c, kCbFsr = 0x58,
    kCbTlbiAsid = 0x610, kCbTlbSync = 0x7f0, kCbTlbStatus = 0x7f4;
bool smmuIdentity() {
    if (!mapRegion(gSmmu, kSmmu, 0xd0000)) return false;
    saveUsbStreams();
    const uint32_t idr0 = gSmmu.va[0x20 / 4], idr1 = gSmmu.va[0x24 / 4];
    unsigned groups = idr0 & 255;
    if (groups > 128) groups = 128;
    const unsigned banks = idr1 & 255, pages = 1u << (((idr1 >> 28) & 7) + 1);
    if ((idr1 >> 31) || !groups || banks < 2 || banks > 128 || pages > 128 || (pages + banks) * 4096u > 0xd0000u) return false;
    uint64_t used[2] = {};
    for(unsigned i=0;i<gSavedCount;++i){
        const uint32_t route=gSavedStreams[i].s2cr;
        if(((route>>16)&3)==0)used[(route&127)/64]|=1ull<<((route&127)%64);
    }
    unsigned slot = 0xffffffffu;
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t smr = gSmmu.va[(0x800 + 4 * i) / 4];
        if (!(smr & 0x80000000u)) {if (slot == 0xffffffffu) slot = i; continue;}
        const uint32_t s2cr = gSmmu.va[(0xc00 + 4 * i) / 4];
        if (((smr ^ kSid) & 0xffff & ~((smr >> 16) & 0x7fff)) == 0) {IOLog("qcom-dwc3: stream already mapped %x %x\n", smr, s2cr); return false;}
        if (((s2cr >> 16) & 3) == 0) used[(s2cr & 127) / 64] |= 1ull << ((s2cr & 127) % 64);
    }
    if (slot == 0xffffffffu) return false;
    // The last bank is the shared bypass bank. Take the highest bank no stream uses and no firmware left enabled.
    unsigned bank = 0xffffffffu;
    for (unsigned b = banks - 1; b-- > 0;) {
        if ((used[b / 64] >> (b % 64)) & 1) continue;
        if (gSmmu.va[(pages + b) * 1024] & 1u) continue;
        bank = b;
        break;
    }
    if (bank == 0xffffffffu) return false;
    // ASIDs share a namespace across the enabled banks on this SMMU. Preserve
    // the firmware's ASID 0 translations across the native controller session.
    unsigned asid=bank+1;
    for(;;++asid) {
        bool usedAsid=false;
        for(unsigned b=0;b<banks;++b) {
            const auto *context=gSmmu.va+(pages+b)*1024;
            if((context[kCbSctlr/4]&1u)&&(context[kCbTtbr0/4+1]>>16)==asid){usedAsid=true;break;}
        }
        if(!usedAsid)break;
        if(asid==65535)return false;
    }
    gSmmuTable = IOBufferMemoryDescriptor::withOptions((IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, 4096, 4096);
    if (!gSmmuTable || gSmmuTable->prepare() != kIOReturnSuccess) return false;
    uint64_t *table = static_cast<uint64_t *>(gSmmuTable->getBytesNoCopy());
    bzero(table, 4096);
    // T0SZ 28: 64 GiB, the walk starts at level 1. Blocks: AF, inner shareable, AP unprivileged read/write, attribute 0. Below 2 GiB
    // there is no DRAM, and a stray access should fault.
    for (uint64_t i = 2; i < 64; ++i) table[i] = i << 30 | 1ull << 10 | 3ull << 8 | 1ull << 6 | 1ull;
    for (uintptr_t at = reinterpret_cast<uintptr_t>(table); at < reinterpret_cast<uintptr_t>(table) + 4096; at += 64) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
    barrier();
    const uint64_t tablePa = gSmmuTable->getPhysicalSegment(0, nullptr, 0);
    volatile uint32_t *cb = gSmmu.va + (pages + bank) * 1024;
    gSmmu.va[(0x1800 + 4 * bank) / 4] = 1;                                      // CBA2R: AArch64
    gSmmu.va[(0x1000 + 4 * bank) / 4] = 1u << 16 | 0xfu << 12 | 3u << 8;        // CBAR: stage 1 with stage 2 bypass
    cb[kCbTcr2 / 4] = 2u | 7u << 15;                                            // 40-bit output, upstream sign extension
    cb[kCbTcr / 4] = 28u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23;            // walks cacheable inner shareable, 4 KiB, no TTBR1
    cb[kCbTtbr0 / 4] = (uint32_t)tablePa; cb[kCbTtbr0 / 4 + 1] = (uint32_t)(tablePa >> 32)|(asid<<16);
    cb[kCbTtbr1 / 4] = 0; cb[kCbTtbr1 / 4 + 1] = 0;
    cb[kCbMair0 / 4] = 0xff; cb[kCbMair1 / 4] = 0;                              // attribute 0: Normal write-back
    barrier();
    cb[kCbTlbiAsid / 4] = asid;
    cb[kCbTlbSync / 4] = 0;
    barrier();
    if(!waitBits(cb+kCbTlbStatus/4,1u,0,10))return false;
    cb[kCbFsr / 4] = cb[kCbFsr / 4];
    cb[kCbSctlr / 4] = 1u | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12;             // M, TRE, AFE, CFRE, ASIDPNE
    barrier();
    gSmmu.va[(0xc00 + 4 * slot) / 4] = bank;                                    // S2CR translate to the bank, then the match
    barrier();
    gSmmu.va[(0x800 + 4 * slot) / 4] = 0x80000000u | kSid;
    barrier();
    native_usb_stats[7] = slot | bank << 16;
    gSmmuBank = bank; gSmmuPages = pages;gSmmuAsid=asid;native_usb_stats[37]=asid;
    gSmmuInstalled = true;
    const bool ok = (cb[kCbSctlr / 4] & 1u) && (gSmmu.va[(0xc00 + 4 * slot) / 4] & 255) == bank &&
                    gSmmu.va[(0x800 + 4 * slot) / 4] == (0x80000000u | kSid);
    if (!ok) IOLog("qcom-dwc3: SMMU context readback %x %x %x\n", cb[kCbSctlr / 4], gSmmu.va[(0xc00 + 4 * slot) / 4], gSmmu.va[(0x800 + 4 * slot) / 4]);
    return ok;
}
}  // namespace

// A controller's provider: a register window and the core interrupt, in the IOService terms IOFilterInterruptEventSource uses. Device and
// host have a nub each and share the one line: it goes to the nub of the role the core is in.
class QcomDwc3Nub;
static QcomDwc3Nub *volatile gActiveNub;
static bool gLineAttached;
class QcomDwc3Nub : public IOService {
    OSDeclareDefaultStructors(QcomDwc3Nub)
    IOInterruptAction action = nullptr;
    OSObject *target = nullptr;
    void *refcon = nullptr;
    bool enabled = false;
    static void primary(void *) {
        QcomDwc3Nub *self = gActiveNub;
        native_usb_stats[11] = native_usb_stats[11] + 1;
        if (self && self->action) self->action(self->target, self->refcon, self, 0);
    }
    void unmask() {if (gActiveNub == this) armcpu_spi_mask(kIrq, !enabled);}
public:
    bool serializeProperties(OSSerialize *serializer) const override {
        auto *self=const_cast<QcomDwc3Nub *>(this);
        self->setProperty("NativeUSBStage",native_usb_stats[0],32);
        self->setProperty("NativeUSBInterrupts",native_usb_stats[11],32);
        self->setProperty("NativeUSBControllerID",native_usb_stats[1],32);
        self->setProperty("NativeUSBPowerOn",native_usb_stats[12],32);
        self->setProperty("NativeUSBPowerOff",native_usb_stats[13],32);
        self->setProperty("NativeCPUCount",real_ncpus,32);
        return IOService::serializeProperties(serializer);
    }
    static void route(QcomDwc3Nub *nub) {
        armcpu_spi_mask(kIrq, true);
        gActiveNub = nub;
        __asm__ volatile("dmb ish" ::: "memory");
        if (nub) nub->unmask();
    }
    IOReturn getInterruptType(int source, int *type) override {
        if (source != 0 || !type) return kIOReturnNoInterrupt;
        *type = kIOInterruptTypeLevel;
        return kIOReturnSuccess;
    }
    IOReturn registerInterrupt(int source, OSObject *owner, IOInterruptAction handler, void *context) override {
        if (source != 0 || !handler) return kIOReturnNoInterrupt;
        if (action) return kIOReturnNoResources;
        target = owner; refcon = context; action = handler; enabled = false;
        __asm__ volatile("dmb ish" ::: "memory");
        if (!gLineAttached) {
            if (armcpu_spi_attach(kIrq, false, primary, nullptr, nullptr, nullptr) != kIOReturnSuccess) {action = nullptr; return kIOReturnNoResources;}
            gLineAttached = true;
        }
        unmask();
        return kIOReturnSuccess;
    }
    IOReturn unregisterInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        unmask();
        action = nullptr; target = nullptr; refcon = nullptr;
        return kIOReturnSuccess;
    }
    IOReturn enableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = true;
        unmask();
        return kIOReturnSuccess;
    }
    IOReturn disableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        unmask();
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(QcomDwc3Nub, IOService)

class QcomDwc3Xdci : public AppleUSBXDCI {
    OSDeclareDefaultStructors(QcomDwc3Xdci)
    uint32_t read(uint32_t offset) {return (*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x240))[offset / 4];}
    void write(uint32_t offset, uint32_t value) {(*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x240))[offset / 4] = value; barrier();}
public:
    bool start(IOService *provider) override {
        uint8_t *self = reinterpret_cast<uint8_t *>(this);
        IOMemoryMap *map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
        if (!map) return false;
        *reinterpret_cast<IOMemoryMap **>(self + 0x238) = map;
        *reinterpret_cast<uint64_t *>(self + 0x240) = map->getVirtualAddress();
        return AppleUSBXDCI::start(provider);
    }
    // True keeps the family's already selected configuration list (IOUSBDeviceController builds it when this is false): Apple's SoC
    // classes answer from their IOPort transport object, and this class has none. The USB 3 descriptors follow the speed instead.
    bool XDCI_KEEP_CONFIGURATIONS() override {return false;}
    bool XDCI_TRANSPORT_ACTIVE() override {return true;}
    IOReturn XDCI_POWER_ON() override {
        native_usb_stats[12] = native_usb_stats[12] + 1;
        if (gStopping) return kIOReturnNotReady;
        vbusOverride(true);
        native_usb_stats[15] = read(0x70c);
        return kIOReturnSuccess;
    }
    IOReturn XDCI_POWER_OFF() override {
        native_usb_stats[13] = native_usb_stats[13] + 1;
        vbusOverride(false);
        return kIOReturnSuccess;
    }
    // One event buffer and interrupter; the Linux driver has one as well. The scale and the periodic count are read in the reset.
    void XDCI_PARAMETERS() override {
        uint32_t *fields = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x2418);
        fields[0] = 1; fields[1] = 0; fields[2] = 4; fields[3] = 4;
    }
    IOReturn XDCI_CONTROLLER_RESET() override {
        const IOReturn result = AppleUSBXDCI::XDCI_CONTROLLER_RESET();
        native_usb_stats[14] = (uint32_t)result;
        if (result != kIOReturnSuccess) return result;
        uint32_t *fields = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x2418);
        // dwc3_core_setup_global_control keeps the power down scale the hardware resets to when there is no suspend clock.
        const uint32_t gctl = read(0x110);
        fields[1] = gctl >> 19;
        write(0x110, (gctl & ~((3u << 12)|(1u<<1))) | (2u << 12) | 1u);
        // dwc3_gadget_setup_nump: RAM2 bytes less 24 for the event overhead and 16 per setup, in 1 KiB packets, at most 16.
        const uint32_t hw0 = read(0x140), hw6 = read(0x158), hw7 = read(0x15c);
        const uint32_t mdwidth = ((hw0 >> 8) & 0xffu) | ((hw6 >> 8) & 3u) << 8;
        const uint32_t ram2 = hw7 >> 16;
        uint32_t nump = ram2 * mdwidth / 8 > 40 ? (ram2 * mdwidth / 8 - 40) / 1024 : 1;
        fields[2] = nump > 16 ? 16 : nump ? nump : 1;
        native_usb_stats[2] = hw0; native_usb_stats[3] = read(0x14c); native_usb_stats[4] = hw7;
        // snps,dis_u2_susphy_quirk, snps,dis_enblslpm_quirk: this board never suspends the eUSB2 PHY clock from the core.
        write(0x200, read(0x200) & ~(1u << 6 | 1u << 8));
        write(0x2c0, read(0x2c0) & ~(1u << 17));
        write(0x700, read(0x700) & ~7u);   // High speed
        native_usb_stats[16] = read(0x700); native_usb_stats[17] = read(0x110); native_usb_stats[18] = read(0x200);
        return result;
    }
    // Bandwidth accounting of Apple's fabric: nothing to vote for on this SoC, the SN2 vote covers the peak.
    // iOS 18 IOUSBDeviceController owns its USB bus state on its command gate.
    IOReturn cable(bool connected) {
        return connected?XDCI_CABLE_CONNECT():XDCI_CABLE_DISCONNECT();
    }
    uint32_t XDCI_PERIODIC_USAGE() override {return *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x37c) != 0;}
    void XDCI_PERIODIC_SET(uint32_t) override {}
};
OSDefineMetaClassAndStructors(QcomDwc3Xdci, AppleUSBXDCI)

namespace {
QcomDwc3Xdci *controller;
QcomDwc3Nub *provider;
bool powerRails() {
    Region database, rsc;
    if (!mapRegion(database,0x80860000,0x20000) || !mapRegion(rsc,0x17a20000,0x10000)) return false;
    auto *db=static_cast<uint8_t *>(IOMallocData(0x20000));
    if (!db) return false;
    for (unsigned i=0;i<0x20000;i+=4) {uint32_t v=database.va[i/4];memcpy(db+i,&v,4);}
    const QcomRscLayout layout=qcom_rsc_layout(rsc.va[0]);
    const unsigned tcs=2;
    auto send=[&](uint32_t address,uint32_t value) {
        QcomRpmhWrite writes[16];
        const unsigned count=qcom_rpmh_active_request(&layout,tcs,&address,&value,1,writes,16);
        if (!count) return false;
        for(unsigned i=0;i<count;++i)rsc.va[(0xd00+writes[i].offset)/4]=writes[i].value;
        barrier();
        IOSleep(2);
        // TCS2 has no IRQ_ENABLE vote. Its command response remains available
        // in CMD_STATUS; IRQ_STATUS is masked by IRQ_ENABLE on this RSC.
        auto *response=rsc.va+(0xd00+tcs*layout.tcs_stride+layout.cmd_data+4)/4;
        const uint32_t complete=(1u<<8)|(1u<<16);
        const bool completed=waitBits(response,complete,complete,100);
        native_usb_stats[23]=address;native_usb_stats[24]=value;
        native_usb_stats[25]=*response;
        const unsigned finish=qcom_rpmh_finish(&layout,tcs,writes,16);
        for(unsigned i=0;i<finish;++i)rsc.va[(0xd00+writes[i].offset)/4]=writes[i].value;
        barrier();return completed;
    };
    // marble_live.dts: PHY L5B/L10C; NXP repeater S10B/L2B.
    // NXP eusb2-repeater requires 3.075..3.3 V on vdd3.
    const struct {const char *name;uint32_t mv;} rails[]={{"ldob5",880},{"ldoc10",1200},{"smpb10",1800},{"ldob2",3104}};
    bool ok=true;
    for(unsigned i=0;i<4&&ok;++i) {
        QcomRpmhResource resource;
        const int found=qcom_cmd_db_find(db,0x20000,rails[i].name,&resource);
        native_usb_stats[22]=uint32_t(found);
        ok=!found &&
            send(resource.address+8,7) && send(resource.address,rails[i].mv) && send(resource.address+4,1);
        if(ok)native_usb_stats[9]|=1u<<i;
    }
    QcomRpmhResource reference;
    if(ok)ok=!qcom_cmd_db_find(db,0x20000,"xo.lvl",&reference)&&send(reference.address,3);
    QcomRpmhResource bandwidth;
    if(ok&&!qcom_cmd_db_find(db,0x20000,"SN2",&bandwidth)){
        uint32_t vote=0x400;
        const uint32_t unit=bandwidth.levels[0]|uint32_t(bandwidth.levels[1])<<16;
        if(bandwidth.level_count>=3&&unit&&bandwidth.levels[2]){
            uint64_t value=(((2500000ull*bandwidth.levels[2]+15)/16)*1000+unit-1)/unit;
            vote=value<1?1:value>0x3fff?0x3fff:uint32_t(value);
        }
        ok=send(bandwidth.address,(1u<<30)|(1u<<29)|(vote<<14)|vote);
    }
    IOFreeData(db,0x20000);database.map->release();rsc.map->release();return ok;
}
bool boardClocks() {
    // MiCode marble-s-oss gcc-waipio.c: cape uses the 0x49000 USB block.
    auto *domain=gGcc.va+0x49004/4;
    *domain=(*domain&~1u)|(1u<<11);barrier();
    if(!waitBits(domain,1u<<31,1u<<31,100))return false;
    native_usb_stats[5]=*domain;
    const uint32_t sources[][2]={{0x49028,0x105},{0x49040,0}};
    for(const auto &source:sources){
        auto *reg=gGcc.va+source[0]/4;
        if((reg[1]&0x71f)!=source[1]){
            reg[1]=(reg[1]&~0x71fu)|source[1];barrier();reg[0]|=1u;barrier();
            if(!waitBits(reg,1u,0,10))return false;
        }
    }
    const unsigned branches[]={0x49088,0x49084,0x49018,0x49024,0x49020,0x9c00c};
    for(unsigned offset:branches) {
        auto *reg=gGcc.va+offset/4;*reg|=1u;barrier();
        if(!waitBits(reg,1u<<31,0,100))return false;
    }
    return true;
}
uint32_t i2cRead(void *context,uint32_t reg){return static_cast<volatile uint32_t *>(context)[reg/4];}
void i2cWrite(void *context,uint32_t reg,uint32_t value){static_cast<volatile uint32_t *>(context)[reg/4]=value;barrier();}
Region gpioObserver,gpioWriter;
uint8_t gpioSaved[7];bool gpioChanged;
bool spmiTransfer(volatile uint32_t *channel,uint32_t command) {
    channel[0]=command;barrier();
    for(unsigned us=0;us<1000;++us){uint32_t status=channel[2];if(status&1)return (status&15)==1;IODelay(1);}
    return false;
}
bool gpioRead(unsigned reg,uint8_t &value) {
    if(!spmiTransfer(gpioObserver.va,(1u<<27)|(reg<<4)))return false;
    value=uint8_t(gpioObserver.va[6]);return true;
}
bool gpioWrite(unsigned reg,uint8_t value) {
    gpioWriter.va[4]=value;barrier();return spmiTransfer(gpioWriter.va,reg<<4);
}
bool resetRepeater() {
    Region core,config;
    if(!mapRegion(core,0xc400000,0x3000)||!mapRegion(config,0xc42d000,0x4000))return false;
    const bool v7=core.va[0]>=0x70000000;
    const unsigned count=v7?1024:512,mapOffset=v7?0x2000:0x900,ownerOffset=v7?0:0x700;
    int apid=-1;
    for(unsigned i=0;i<count;++i){
        const uint32_t entry=core.va[(mapOffset+4*i)/4];
        // The APID map's bit 24 describes interrupt ownership. GPIO register
        // access follows the independent write-owner table (EE 0).
        if(((entry>>8)&0xfff)==0x28e&&(config.va[(ownerOffset+4*i)/4]&7)==0){apid=int(i);break;}
    }
    core.map->release();config.map->release();
    if(apid<0||!mapRegion(gpioObserver,0xc440000+uint64_t(apid)*(v7?0x20:0x80),0x20)||
       !mapRegion(gpioWriter,0xc500000+uint64_t(apid)*(v7?0x1000:0x10000),0x1000))return false;
    uint8_t subtype=0;
    if(!gpioRead(5,subtype))return false;
    for(unsigned i=0;i<7;++i)if(!gpioRead(0x40+i,gpioSaved[i]))return false;
    gpioChanged=true;
    const bool lv=subtype>=0x10&&subtype<=0x13;
    // MiCode pinctrl-spmi-gpio: PM8350C GPIO7, normal CMOS output, VIN1.
    if(!gpioWrite(0x41,1)||!gpioWrite(0x42,5)||!gpioWrite(0x45,2)||
       !gpioWrite(0x40,lv?2:0x21)||(lv&&!gpioWrite(0x44,0x80))||!gpioWrite(0x46,0x80))return false;
    IODelay(50);return true;
}
bool repeater() {
    if(!resetRepeater())return false;
    Region engine;
    if(!mapRegion(engine,0x994000,0x4000))return false;
    // The firmware configures SE5 as I2C. Retain its timing and select the
    // upstream FIFO transfer state machine for the NXP parameter writes.
    gGcc.va[0x62008/4]|=(1u<<15)|(1u<<8)|(1u<<9)|(1u<<6)|(1u<<7);barrier();
    GeniI2c bus={};GeniIo io={i2cRead,i2cWrite,const_cast<uint32_t *>(engine.va)};
    uint32_t config=0;
    bool ok=geni_i2c_prepare(&bus,&io,0x4f,&config)==0;
    const uint8_t sequence[][2]={{0x08,0x61},{0x07,0x00}};
    for(unsigned i=0;i<2&&ok;++i) {
        ok=geni_i2c_start_write(&bus,sequence[i][0],sequence[i][1])==0;
        for(unsigned phase=0;phase<3&&ok&&geni_i2c_busy(&bus);++phase) {
            for(unsigned tick=0;tick<2500&&geni_i2c_busy(&bus);++tick) {
                const int result=geni_i2c_poll(&bus);
                if(result==GeniFailed){ok=false;break;}
                if(result==GeniDone)break;
                IODelay(10);
            }
            if(geni_i2c_busy(&bus)&&geni_i2c_timeout(&bus)==GeniFailed)ok=false;
        }
        ok=ok&&!geni_i2c_busy(&bus)&&!bus.error;
    }
    native_usb_stats[8]=ok?1u:uint32_t(bus.error_status);
    engine.map->release();return ok;
}
void resetCore() {
    // gcc-waipio.c GCC_USB30_PRIM_BCR. The block reset stops DMA independently
    // of the PHY clock; DCTL.CSFTRST requires the initialized PHY clocks.
    auto *reset=gGcc.va+0x49000/4;
    *reset|=1u;barrier();IODelay(10);
    *reset&=~1u;barrier();IODelay(100);
    ++native_usb_stats[36];
}
bool hardwareStart() {
    stage(1);
    if(const auto *table=native_resident_table())
        memcpy(gPhyCalibration,table->UsbHost.Reserved,sizeof(gPhyCalibration));
    native_usb_stats[45]=gPhyCalibration[0]==ROOT_STAGE_USB_PHY_CALIBRATION_MAGIC;
    if(native_usb_stats[45]) {
        native_usb_stats[46]=gPhyCalibration[1];native_usb_stats[47]=gPhyCalibration[3];
        native_usb_stats[48]=gPhyCalibration[4];native_usb_stats[49]=gPhyCalibration[11];
        native_usb_stats[50]=gPhyCalibration[12];
    }
    if(!mapRegion(gGcc,0x100000,0x1f0000)||!mapRegion(gScratch,kCore+0xf8000,0x1000)||
       !mapRegion(gPhy,0x88e3000,0x1000)||!mapRegion(gCoreMap,kCore+0xc000,0x1000))return false;
    stage(2);if(!powerRails())return false;
    stage(3);if(!boardClocks())return false;
    gCoreClocked=true;
    vbusOverride(false);
    stage(4);resetCore();
    stage(5);if(!smmuIdentity())return false;
    stage(6);if(!repeater())return false;
    stage(7);
    auto *reset=gGcc.va+0x22000/4;
    *reset|=1u;barrier();IODelay(20000);*reset&=~1u;barrier();
    for(const auto &row:kQcomEusb2Init) {
        uint32_t value=row.value;
        if(native_usb_stats[45]&&(row.offset!=0x54||row.mask==0x70)&&
           (row.offset!=0x64||row.mask==0x10)) {
            for(unsigned i=0;i<12;++i)
                if(kPhyCalibrationOffsets[i]==row.offset){value=gPhyCalibration[i+1]&row.mask;break;}
        }
        auto *reg=gPhy.va+row.offset/4;*reg=(*reg&~uint32_t(row.mask))|value;barrier();
    }
    if(native_usb_stats[45]) {
        gPhy.va[0x64/4]=gPhyCalibration[11];gPhy.va[0x3c/4]=gPhyCalibration[12];barrier();
    }
    native_usb_stats[51]=gPhy.va[0x54/4];native_usb_stats[52]=gPhy.va[0x5c/4];
    auto *clock=gScratch.va+0x808/4;
    *clock|=1u<<8;barrier();IODelay(1000);*clock|=1u|(1u<<3);barrier();
    IODelay(1000);*clock&=~(1u<<8);barrier();
    native_usb_stats[1]=gCoreMap.va[0x120/4];
    stage(9);return true;
}
void cableWorker(void *,wait_result_t) {
    for(;;) {
        IOLockLock(gRoleLock);
        while(gWanted==gApplied&&!gStopping)IOLockSleep(gRoleLock,&gWanted,THREAD_UNINT);
        if(gStopping){IOLockUnlock(gRoleLock);break;}
        const int connected=gWanted;IOLockUnlock(gRoleLock);
        if(connected)IOService::getResourceService()->setProperty("AppleUSBCableType","USBHost");
        else IOService::getResourceService()->removeProperty("AppleUSBCableType");
        native_usb_stats[20]=uint32_t(controller->cable(connected));
        IOLockLock(gRoleLock);gApplied=connected;IOLockUnlock(gRoleLock);
        ++native_usb_stats[35];
    }
    thread_terminate(current_thread());
}
void startWorker(void *,wait_result_t) {
    const OSReturn loaded=OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
    native_usb_stats[21]=uint32_t(loaded);
    if(loaded!=kOSReturnSuccess&&loaded!=kOSKextReturnStartStopError){stage(-10);thread_terminate(current_thread());}
    void *module=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!module){stage(-11);thread_terminate(current_thread());}
    marble_usb_static_init();
    if(OSMetaClass::postModLoad(module)){stage(-12);thread_terminate(current_thread());}
    provider=OSTypeAlloc(QcomDwc3Nub);
    auto *range=IODeviceMemory::withRange(kAppleWindow,0x1000);
    auto *ranges=range?OSArray::withObjects((const OSObject **)&range,1):nullptr;
    uint8_t number=0;
    if(!provider||!provider->init()||!ranges){stage(-13);thread_terminate(current_thread());}
    provider->setDeviceMemory(ranges);provider->setName("usb-drd0");
    provider->setProperty("bus-number",&number,1);
    provider->setProperty("configuration-string",(void *)"standardMuxOnly",16);
    provider->setProperty("u1-init-disable",true);provider->setProperty("u2-init-disable",true);
    provider->setProperty("transport","interrupt-dma");
    if(!provider->attach(IOService::getPlatform())){stage(-14);thread_terminate(current_thread());}
    provider->registerService();ranges->release();range->release();
    QcomDwc3Nub::route(provider);
    controller=OSTypeAlloc(QcomDwc3Xdci);
    if(!controller||!controller->init()||!controller->attach(provider)||!controller->start(provider)){
        stage(-15);thread_terminate(current_thread());
    }
    native_usb_stats[19]=1;
    thread_t thread;
    if(kernel_thread_start(cableWorker,nullptr,&thread)!=KERN_SUCCESS){stage(-16);thread_terminate(current_thread());}
    thread_deallocate(thread);stage(12);thread_terminate(current_thread());
}
}

extern "C" void native_marble_usb_vbus(int online) {
    if(!gRoleLock||gStopping)return;
    IOLockLock(gRoleLock);gWanted=online!=0;IOLockWakeup(gRoleLock,&gWanted,false);IOLockUnlock(gRoleLock);
}
extern "C" void native_marble_usb_start(void) {
    static unsigned started;
    if(__atomic_exchange_n(&started,1,__ATOMIC_ACQ_REL))return;
    gRoleLock=IOLockAlloc();
    if(!gRoleLock||!hardwareStart()){stage(-int(native_usb_stats[0]));return;}
    thread_t thread;
    if(kernel_thread_start(startWorker,nullptr,&thread)!=KERN_SUCCESS){stage(-17);return;}
    thread_deallocate(thread);
}
extern "C" bool native_marble_usb_configured(void){return controller&&!gStopping&&controller->getPowerState()>=4;}
extern "C" void native_marble_usb_quiesce(void) {
    gStopping=true;
    armcpu_spi_mask(kIrq,true);
}
extern "C" void native_marble_usb_stop(void) {
    native_marble_usb_quiesce();
    if(gCoreClocked) {
        vbusOverride(false);
        resetCore();
        // Release the USB wire before the firmware publishes its descriptors.
        // The eUSB2 PHY retains pull-up/session state across a core-only reset.
        auto *phyReset=gGcc.va+0x22000/4;
        *phyReset|=1u;barrier();IODelay(20000);*phyReset&=~1u;barrier();
    }
    if(gSmmu.va) {
        if(gSmmuInstalled) {
            const unsigned slot=native_usb_stats[7]&65535;
            gSmmu.va[(0x800+4*slot)/4]=0;barrier();
            auto *cb=gSmmu.va+(gSmmuPages+gSmmuBank)*1024;
            cb[kCbSctlr/4]=0;barrier();
            cb[kCbTlbiAsid/4]=gSmmuAsid;cb[kCbTlbSync/4]=0;barrier();
            waitBits(cb+kCbTlbStatus/4,1u,0,10);
            gSmmuInstalled=false;
        }
        for(unsigned i=0;i<gSavedCount;++i){const auto &s=gSavedStreams[i];
            gSmmu.va[(0xc00+4*s.index)/4]=s.s2cr;barrier();gSmmu.va[(0x800+4*s.index)/4]=s.smr;}
        barrier();
    }
    if(gpioChanged){
        for(unsigned i=0;i<7;++i)gpioWrite(0x40+i,gpioSaved[i]);
        gpioChanged=false;
    }
}
extern "C" void native_marble_usb_report(char *out,unsigned size) {
    if(gCoreClocked) {
        native_usb_stats[26]=gCoreMap.va[0x704/4];native_usb_stats[27]=gCoreMap.va[0x70c/4];
        native_usb_stats[28]=gCoreMap.va[0x40c/4];native_usb_stats[29]=gCoreMap.va[0x408/4];
        native_usb_stats[38]=gCoreMap.va[0x720/4];native_usb_stats[39]=gCoreMap.va[0x80c/4];
        native_usb_stats[40]=gCoreMap.va[0x100/4];native_usb_stats[41]=gCoreMap.va[0x700/4];
        native_usb_stats[42]=gCoreMap.va[0x800/4];native_usb_stats[43]=gCoreMap.va[0x804/4];
        native_usb_stats[44]=gCoreMap.va[0x808/4];
    }
    if(gSmmuInstalled) {
        native_usb_stats[30]=gSmmu.va[0x48/4];
        native_usb_stats[31]=gSmmu.va[(gSmmuPages+gSmmuBank)*1024+kCbFsr/4];
    }
    snprintf(out,size,"MarbleUSB stage=%d controller=%x IRQ=%u power=%u/%u cable=%d/%d SMMU=%x ASID=%u ctl=%x sts=%x evt=%x/%x fault=%x/%x ep=%x cmd=%x bus=%x cfg=%x par=%x/%x/%x",
        int(native_usb_stats[0]),native_usb_stats[1],native_usb_stats[11],native_usb_stats[12],native_usb_stats[13],
        gApplied,gWanted,native_usb_stats[7],gSmmuAsid,native_usb_stats[26],native_usb_stats[27],
        native_usb_stats[28],native_usb_stats[29],native_usb_stats[30],native_usb_stats[31],
        native_usb_stats[38],native_usb_stats[39],native_usb_stats[40],native_usb_stats[41],
        native_usb_stats[42],native_usb_stats[43],native_usb_stats[44]);
}

