// SPDX-License-Identifier: BSD-2-Clause-Patent
// Polled EP0 diagnostics. PHY and clocks are inherited from the marble loader.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <sys/param.h>
#include "RuntimeUsbRegisters.h"

extern "C" unsigned native_root_runtime_read(unsigned, unsigned, void *, unsigned);
extern "C" void native_hid_request_home(void);
extern "C" void native_marble_touch_start(void);
extern "C" void native_marble_gpu_start(void);
extern "C" int native_marble_adreno_start(void);
extern "C" volatile uint32_t native_legacy_output_blank;
extern "C" void native_marble_gmu_stop(void);
extern "C" void native_hid_request_tap(unsigned, unsigned);
extern "C" void native_hid_request_contact(unsigned, unsigned, bool);
extern "C" void native_root_block_request_snapshot(void);
extern "C" void native_root_block_capture_log(void);
extern "C" void native_runtime_usb_start(void);
extern "C" void native_marble_watchdog_start(void);
extern "C" void native_runtime_usb_stop(void);
extern "C" void native_runtime_usb_report(char *, unsigned);

namespace {
constexpr unsigned dmaBytes = 16384, eventBytes = 4096, responseBytes = 4096;
constexpr unsigned trbOffset = 4096, controlOffset = 8192, setupOffset = 12288;
constexpr uint64_t controllerAddress = 0x0a600000;
IOMemoryMap *registerMap, *dmaMap;
IOBufferMemoryDescriptor *dma;
volatile uint32_t *registers;
uint8_t *bytes;
uint8_t response[responseBytes];
uint64_t physical;
unsigned eventPosition, controlState, resource[2];
unsigned statusEndpoint;
bool resourceOwned[2];
unsigned events, requests, failures, configuration, returnTicks, rebootTicks;
unsigned controlTicks;
unsigned enumerationTicks;
uint32_t controllerId, initialControl, initialStatus, initialPhy, lastCommand;
bool threeStage, active, controllerOwned, panicOnFailure;
bool watchdogActive;
unsigned watchdogTicks;
volatile bool watchdogRuntimeOwned;
uint64_t watchdogCall(uint64_t function, uint64_t *control = nullptr) {
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1) ::
        "x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
    if (control) *control = x1;
    return x0;
}
IOMemoryMap *rebootMap(uint64_t address, unsigned length) {
    auto *memory=IOMemoryDescriptor::withAddressRange(address,length,kIODirectionInOut | kIOMemoryMapperNone,nullptr);
    auto *mapping=memory ? memory->map(kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    return mapping;
}
bool rebootTransfer(volatile uint32_t *channel, uint32_t command) {
    channel[0]=command;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned tick=0;tick<1000;++tick) {
        uint32_t status=channel[2];
        if (status&1) return (status&15)==1;
        IODelay(1);
    }
    return false;
}
void rebootBootloader() {
    IOMemoryMap *core=rebootMap(0xc400000,0x3000), *config=rebootMap(0xc42d000,0x4000);
    IOMemoryMap *observer=rebootMap(0xc440000,0x80000), *writer=nullptr, *imem=nullptr;
    bool ready=false;
    if (core && config && observer) {
        auto *table=reinterpret_cast<volatile uint32_t *>(core->getVirtualAddress());
        auto *owners=reinterpret_cast<volatile uint32_t *>(config->getVirtualAddress());
        uint32_t version=table[0];
        bool v7=version>=0x70000000;
        unsigned apid=v7 ? 1024 : 512;
        if (version>=0x50000000) for (unsigned index=0;index<(v7?1024u:512u);++index) {
            unsigned offset=(v7?0x2000:0x900)+4*index;
            unsigned owner=(v7?0:0x700)+4*index;
            if (((table[offset/4]>>8)&0xfff)==0x71 && (owners[owner/4]&7)==0) apid=index;
        }
        if (apid<(v7?1024u:512u)) {
            writer=rebootMap(0xc500000ULL+apid*(v7?0x1000:0x10000),0x1000);
            if (writer) {
                auto *rd=reinterpret_cast<volatile uint32_t *>(observer->getVirtualAddress()+apid*(v7?0x20:0x80));
                auto *wr=reinterpret_cast<volatile uint32_t *>(writer->getVirtualAddress());
                // marble's PMK8350 SDAM restart cell is SID 0, address 0x7148, bits 1..7.
                if (rebootTransfer(rd,(1u<<27)|(0x48u<<4))) {
                    uint32_t previous=rd[6]&255;
                    wr[4]=(previous&1)|(2u<<1);
                    __asm__ volatile("dsb sy" ::: "memory");
                    ready=rebootTransfer(wr,0x48u<<4) && rebootTransfer(rd,(1u<<27)|(0x48u<<4)) && ((rd[6]>>1)&127)==2;
                    IOLog("MarbleReboot: arbiter=%x apid=%u reason=%x->%x ready=%u\n",version,apid,previous,rd[6]&255,ready);
                }
            }
        }
        if (ready) {
            imem=rebootMap(0x146aa000,0x1000);
            if (imem) {
                reinterpret_cast<volatile uint32_t *>(imem->getVirtualAddress())[0x65c/4]=0x77665500;
                __asm__ volatile("dsb sy" ::: "memory");
            }
            native_root_block_capture_log();
            native_runtime_usb_stop();
            watchdogCall(0x84000009);
        }
    }
    IOLog("MarbleReboot: bootloader reset returned ready=%u\n",ready);
    OSSafeReleaseNULL(imem); OSSafeReleaseNULL(writer); OSSafeReleaseNULL(observer);
    OSSafeReleaseNULL(config); OSSafeReleaseNULL(core);
}
uint8_t setup[8];

uint32_t read(unsigned offset) { return registers[offset / 4]; }
void write(unsigned offset, uint32_t value) {
    registers[offset / 4] = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
bool command(unsigned ep, uint32_t op, uint32_t p0=0, uint32_t p1=0, uint32_t p2=0) {
    unsigned base = DWC3_DEP_BASE(ep);
    lastCommand = ep<<16 | op;
    write(base + DWC3_DEPCMDPAR0, p0);
    write(base + DWC3_DEPCMDPAR1, p1);
    write(base + DWC3_DEPCMDPAR2, p2);
    write(base + DWC3_DEPCMD, op | DWC3_DEPCMD_CMDACT);
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t result = read(base + DWC3_DEPCMD);
        if (!(result & DWC3_DEPCMD_CMDACT)) {
            if (DWC3_DEPCMD_STATUS(result)) {
                IOLog("RuntimeUSB: endpoint=%u command=%x result=%x state=%u\n",ep,op,result,controlState);
                failures++; return false;
            }
            if (DWC3_DEPCMD_CMD(op) == DWC3_DEPCMD_STARTTRANSFER) {
                resource[ep] = DWC3_DEPCMD_GET_RSC_IDX(result);
                resourceOwned[ep] = true;
            }
            return true;
        }
        IODelay(1);
    }
    failures++;
    IOLog("RuntimeUSB: endpoint=%u command=%x timed out state=%u\n",ep,op,controlState);
    return false;
}
void end(unsigned ep) {
    if (resourceOwned[ep]) {
        command(ep, DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_HIPRI_FORCERM |
                    DWC3_DEPCMD_PARAM(resource[ep]));
        IODelay(100);
        resource[ep] = 0;
        resourceOwned[ep] = false;
    }
}
bool configure(unsigned ep, bool modify) {
    uint32_t p0 = DWC3_DEPCFG_MAX_PACKET_SIZE(64) |
        (modify ? DWC3_DEPCFG_ACTION_MODIFY : DWC3_DEPCFG_ACTION_INIT);
    return command(ep, DWC3_DEPCMD_SETEPCONFIG, p0,
        DWC3_DEPCFG_EP_NUMBER(ep) | DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_NOT_READY_EN);
}
bool startConfiguration() {
    return command(0,DWC3_DEPCMD_DEPSTARTCFG) &&
        command(0,DWC3_DEPCMD_SETTRANSFRESOURCE,1) &&
        command(1,DWC3_DEPCMD_SETTRANSFRESOURCE,1);
}
bool transfer(unsigned ep, unsigned offset, unsigned length, uint32_t type) {
    auto *trb = reinterpret_cast<DWC3_TRB *>(bytes + trbOffset + ep * 32);
    uint64_t address = physical + offset;
    volatile uint32_t *words = reinterpret_cast<volatile uint32_t *>(trb);
    words[0]=(uint32_t)address; words[1]=address>>32; words[2]=length;
    words[3]=type | DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_LST | DWC3_TRB_CTRL_ISP_IMI;
    __asm__ volatile("dsb sy" ::: "memory");
    uint64_t trbAddress = physical + trbOffset + ep * 32;
    return command(ep, DWC3_DEPCMD_STARTTRANSFER, trbAddress >> 32, (uint32_t)trbAddress);
}
bool receiveSetup() {
    end(1); end(0);
    controlState = 0;
    controlTicks = 0;
    if (!transfer(0, setupOffset, 8, DWC3_TRBCTL_CONTROL_SETUP)) {
        if (panicOnFailure) panic("RuntimeUSB: setup transfer failed events=%u requests=%u",events,requests);
        active=false;
        return false;
    }
    return true;
}
void stall() {
    command(0, DWC3_DEPCMD_SETSTALL);
    receiveSetup();
}
unsigned descriptor(unsigned type, unsigned index, uint8_t *out) {
    static const uint8_t device[] = {18,1,0,2,0,0,0,64,0xd1,0x18,0x0e,0xd0,0,1,1,2,3,1};
    static const uint8_t config[] = {9,2,18,0,1,1,0,0x80,250,9,4,0,0,0,0xff,0x42,0x42,0};
    static const uint8_t qualifier[] = {10,6,0,2,0,0,0,64,1,0};
    if (type == 1) { memcpy(out,device,sizeof(device)); return sizeof(device); }
    if (type == 2 || type == 7) { memcpy(out,config,sizeof(config)); out[1]=type; return sizeof(config); }
    if (type == 6) { memcpy(out,qualifier,sizeof(qualifier)); return sizeof(qualifier); }
    if (type != 3) return 0;
    if (!index) { const uint8_t language[] = {4,3,9,4}; memcpy(out,language,4); return 4; }
    const char *value = index == 1 ? "UniXNU" : index == 2 ? "RuntimeRoot" : index == 3 ? "marble" : nullptr;
    if (!value) return 0;
    unsigned length = strlen(value);
    out[0] = 2 + length * 2; out[1] = 3;
    for (unsigned i=0; i<length; i++) { out[2+i*2]=value[i]; out[3+i*2]=0; }
    return out[0];
}
#ifndef MARBLE_IOS18
volatile bool adrenoStarting;
void startAdreno(void *, wait_result_t) {
    IOLog("MarbleGPU: command processor worker entered\n");
    int status = native_marble_adreno_start();
    IOLog("MarbleGPU: command processor result=%d\n", status);
    __atomic_store_n(&adrenoStarting, false, __ATOMIC_RELEASE);
    thread_terminate(current_thread());
}
void requestAdreno() {
    IOLog("MarbleGPU: request adreno\n");
    if (__atomic_exchange_n(&adrenoStarting, true, __ATOMIC_ACQ_REL)) return;
    thread_t thread;
    kern_return_t status = kernel_thread_start(startAdreno, nullptr, &thread);
    if (status == KERN_SUCCESS) thread_deallocate(thread);
    else {
        __atomic_store_n(&adrenoStarting, false, __ATOMIC_RELEASE);
        IOLog("MarbleGPU: command processor worker start=%d\n", status);
    }
}
#endif
void handleSetup() {
    for (unsigned i=0; i<8; i++) setup[i]=reinterpret_cast<volatile uint8_t *>(bytes)[setupOffset+i];
    unsigned value = setup[2] | setup[3]<<8, index = setup[4] | setup[5]<<8;
    unsigned length = setup[6] | setup[7]<<8, count = 0;
    threeStage = length != 0;
    controlState = threeStage ? 1 : 2;
    requests++;
    uint8_t *out = response;
    if ((setup[0] & 0x60) == 0) {
        switch (setup[1]) {
        case 6:
            count = descriptor(value >> 8, value & 255, out);
            if (!count) { stall(); return; }
            break;
        case 5:
            write(DWC3_DCFG,(read(DWC3_DCFG)&~DWC3_DCFG_DEVADDR_MASK)|DWC3_DCFG_DEVADDR(value&127));
            return;
        case 9: __atomic_store_n(&configuration, value, __ATOMIC_RELEASE); return;
        case 8: out[0] = configuration; count = 1; break;
        case 0: out[0] = out[1] = 0; count = 2; break;
        case 1: case 3: case 11: return;
        default: stall(); return;
        }
    } else if (setup[0] == 0xc0 && (setup[1] == 1 || setup[1] == 2 || setup[1] == 5 || setup[1] == 6 || setup[1] == 7 || setup[1] == 8 || setup[1] == 16)) {
        count = native_root_runtime_read(setup[1], value | index<<16, out, MIN(length,responseBytes));
    } else if (setup[0] == 0x40 && !length && setup[1] == 3) {
#ifdef MARBLE_IOS18
        stall(); return;
#else
        native_hid_request_home(); return;
#endif
    } else if (setup[0] == 0x40 && !length && setup[1] == 4) {
        returnTicks = 50; return;
    } else if (setup[0] == 0x40 && !length && setup[1] == 17) {
        rebootTicks = 50; return;
    } else if (setup[0] == 0x40 && !length && setup[1] == 8) {
        native_marble_touch_start(); return;

#ifndef MARBLE_IOS18
    } else if (setup[0] == 0x40 && !length && setup[1] == 9) {
        native_marble_gpu_start(); return;
    } else if (setup[0] == 0x40 && !length && setup[1] == 15) {
        __atomic_store_n(&native_legacy_output_blank, value, __ATOMIC_RELEASE); return;
    } else if (setup[0] == 0x40 && !length && setup[1] == 14) {
        requestAdreno(); return;

#endif
    } else if (setup[0] == 0x40 && !length && setup[1] == 10) {
        native_hid_request_tap(value, index); return;
    } else if (setup[0] == 0x40 && !length && setup[1] == 11) {
        native_root_block_request_snapshot(); return;
    } else if (setup[0] == 0x40 && !length && (setup[1] == 12 || setup[1] == 13)) {
        native_hid_request_contact(value, index, setup[1] == 12); return;
    } else { stall(); return; }
    count = MIN(count,length);
    for (unsigned i=0; i<count; i++) reinterpret_cast<volatile uint8_t *>(bytes)[controlOffset+i]=response[i];
    // Host requests bounded chunks, including the exact final length.
    if (!transfer(1,controlOffset,count,DWC3_TRBCTL_CONTROL_DATA)) stall();
}
void handleEvent(uint32_t event) {
    events++;
    if (events <= 32) IOLog("RuntimeUSB: event=%x state=%u resources=%u/%u\n",event,controlState,resource[0],resource[1]);
    if (!DWC3_EVENT_IS_ENDPOINT(event)) {
        if (DWC3_DEVT_CATEGORY(event) != DWC3_EVENT_TYPE_DEV) return;
        switch (DWC3_DEVT_TYPE(event)) {
        case DWC3_DEVICE_EVENT_RESET:
            __atomic_store_n(&configuration, 0, __ATOMIC_RELEASE);
            end(1);
            if (controlState != 0) end(0);
            controlState=0;
            controlTicks=0;
            command(0,DWC3_DEPCMD_CLEARSTALL);
            command(1,DWC3_DEPCMD_CLEARSTALL);
            write(DWC3_DCFG,read(DWC3_DCFG)&~DWC3_DCFG_DEVADDR_MASK);
            break;
        case DWC3_DEVICE_EVENT_CONNECT_DONE: {
            bool rearm=!resourceOwned[0];
            if ((rearm && !startConfiguration()) || !configure(0,!rearm) || !configure(1,!rearm)) {
                active=false;
                break;
            }
            if (rearm) receiveSetup();
            IOLog("RuntimeUSB: connected speed=%x\n",read(DWC3_DSTS)&7);
            break;
        }
        case DWC3_DEVICE_EVENT_DISCONNECT: __atomic_store_n(&configuration, 0, __ATOMIC_RELEASE); break;
        }
        return;
    }
    unsigned ep = DWC3_DEPEVT_EPNUMBER(event), type = DWC3_DEPEVT_TYPE(event);
    if (ep > 1) return;
    if (type == DWC3_DEPEVT_XFERCOMPLETE) {
        if (!resourceOwned[ep]) return;
        __asm__ volatile("dsb sy" ::: "memory");
        resourceOwned[ep]=false;
        resource[ep]=0;
        if (controlState == 0 && ep == 0) handleSetup();
        else if (controlState == 1 && ep == 1) controlState=2;
        else if (controlState == 3 && ep == statusEndpoint) receiveSetup();
    } else if (type == DWC3_DEPEVT_XFERNOTREADY &&
               !(DWC3_DEPEVT_STATUS(event) & DEPEVT_STATUS_TRANSFER_ACTIVE) &&
               DEPEVT_STATUS_CONTROL_PHASE(DWC3_DEPEVT_STATUS(event)) == DEPEVT_STATUS_CONTROL_STATUS &&
               controlState == 2) {
        controlState=3;
        statusEndpoint=ep;
        if (!transfer(ep,controlOffset,0,threeStage ? DWC3_TRBCTL_CONTROL_STATUS3 : DWC3_TRBCTL_CONTROL_STATUS2)) stall();
    }
}
bool initialize() {
    auto *memory = IOMemoryDescriptor::withAddressRange(controllerAddress,0x10000,kIODirectionInOut | kIOMemoryMapperNone,nullptr);
    registerMap = memory ? memory->map(kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    if (!registerMap) return false;
    registers = reinterpret_cast<volatile uint32_t *>(registerMap->getVirtualAddress());
    uint32_t id=read(DWC3_GSNPSID);
    controllerId=id; initialControl=read(DWC3_DCTL); initialStatus=read(DWC3_DSTS); initialPhy=read(DWC3_GUSB2PHYCFG(0));
    IOLog("RuntimeUSB: id=%x ctl=%x status=%x phy=%x\n",id,read(DWC3_DCTL),read(DWC3_DSTS),read(DWC3_GUSB2PHYCFG(0)));
    if ((id&DWC3_GSNPSID_MASK)!=DWC3_GSNPSID_USB3 && (id&DWC3_GSNPSID_MASK)!=DWC3_GSNPSID_USB31) return false;
    controllerOwned=true;
    dma = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut|kIOMemoryPhysicallyContiguous|kIOMapInhibitCache,dmaBytes,4096);
    dmaMap = dma ? dma->map(kIOMapInhibitCache) : nullptr;
    if (!dmaMap) return false;
    bytes = reinterpret_cast<uint8_t *>(dmaMap->getVirtualAddress());
    physical = dmaMap->getPhysicalAddress();
    for (unsigned i=0; i<dmaBytes/4; i++) reinterpret_cast<volatile uint32_t *>(bytes)[i]=0;
    // Reset the inherited device queues while clearing RUN_STOP. Firmware can
    // leave a bulk OUT request pending, so waiting for halt before reset stalls
    // takeover. DWC_usb31 also needs time for its clock domains to synchronize.
    write(DWC3_DEVTEN,0);
    write(DWC3_DCTL,(read(DWC3_DCTL)&~(DWC3_DCTL_RUN_STOP|DWC3_DCTL_ULSTCHNGREQ_MASK))|DWC3_DCTL_CSFTRST);
    unsigned wait=0;
    for (; wait<40 && (read(DWC3_DCTL)&DWC3_DCTL_CSFTRST); wait++) IOSleep(5);
    if (wait==40) { IOLog("RuntimeUSB: inherited device reset timeout\n"); return false; }
    if ((id&DWC3_GSNPSID_MASK)==DWC3_GSNPSID_USB31) IOSleep(50);
    unsigned stopped=0;
    for (; stopped<100 && !(read(DWC3_DSTS)&DWC3_DSTS_DEVCTRLHLT); stopped++) IOSleep(1);
    if (stopped==100) { IOLog("RuntimeUSB: reset device halt timeout\n"); return false; }
    // RootStage and RuntimeRoot have distinct descriptors. Give the host a disconnect interval.
    IOSleep(300);
    write(DWC3_GCTL,(read(DWC3_GCTL)&~DWC3_GCTL_PRTCAPDIR(3))|DWC3_GCTL_PRTCAPDIR(2)|DWC3_GCTL_DSBLCLKGTNG);
    write(DWC3_GUSB2PHYCFG(0),read(DWC3_GUSB2PHYCFG(0))&~(DWC3_GUSB2PHYCFG_SUSPHY|DWC3_GUSB2PHYCFG_ENBLSLPM));
    write(DWC3_DCFG,(read(DWC3_DCFG)&~(DWC3_DCFG_SPEED_MASK|DWC3_DCFG_DEVADDR_MASK))|DWC3_DCFG_HIGHSPEED);
    write(DWC3_GEVNTADRLO(0),(uint32_t)physical);
    write(DWC3_GEVNTADRHI(0),physical>>32);
    write(DWC3_GEVNTSIZ(0),DWC3_GEVNTSIZ_INTMASK|eventBytes);
    write(DWC3_GEVNTCOUNT(0),read(DWC3_GEVNTCOUNT(0))&DWC3_GEVNTCOUNT_MASK);
    write(DWC3_DEVTEN,DWC3_DEVTEN_DISCONNEVTEN|DWC3_DEVTEN_USBRSTEN|DWC3_DEVTEN_CONNECTDONEEN);
    write(DWC3_DALEPENA,0);
    if (!startConfiguration()) return false;
    if (!configure(0,false) || !configure(1,false)) return false;
    write(DWC3_DALEPENA,3);
    if (!receiveSetup()) return false;
    active=true;
    write(DWC3_DCTL,read(DWC3_DCTL)|DWC3_DCTL_RUN_STOP);
    IOLog("RuntimeUSB: ready dma=%llx\n",physical);
    return true;
}
void startupWatchdog(void *, wait_result_t) {
    uint64_t control = 0;
    uint64_t status = watchdogCall(0x86000006, &control);
    IOLog("MarbleWatchdog: startup active=%u status=%llx\n", !status && (control & 1), status);
    if (!status && (control & 1)) {
        while (!__atomic_load_n(&watchdogRuntimeOwned, __ATOMIC_ACQUIRE)) {
            watchdogCall(0x86000007);
            IOSleep(1000);
        }
    }
    thread_terminate(current_thread());
}
void worker(void *, wait_result_t) {
    bool initialized=active;
    if (initialized) {
        unsigned maintenanceTicks = 0;
        uint64_t control = 0;
        uint64_t status = watchdogCall(0x86000006, &control);
        watchdogActive = !status && (control & 1);
        IOLog("MarbleWatchdog: active=%u status=%llx\n", watchdogActive, status);
        __atomic_store_n(&watchdogRuntimeOwned, true, __ATOMIC_RELEASE);
        while (active) {
            const bool maintenance = ++maintenanceTicks == 10;
            if (maintenance) maintenanceTicks = 0;
            if (maintenance && watchdogActive && ++watchdogTicks >= 100) {
                watchdogCall(0x86000007);
                watchdogTicks = 0;
            }
            unsigned count = read(DWC3_GEVNTCOUNT(0)) & DWC3_GEVNTCOUNT_MASK;
            if (count > eventBytes || (count&3)) {
                IOLog("RuntimeUSB: invalid event count=%u\n",count); break;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            for (unsigned i=0; i<count; i+=4) {
                uint32_t event=reinterpret_cast<volatile uint32_t *>(bytes)[eventPosition/4];
                eventPosition=(eventPosition+4)%eventBytes;
                write(DWC3_GEVNTCOUNT(0),4);
                handleEvent(event);
            }
            if (maintenance && panicOnFailure && controlState && ++controlTicks >= 500) {
                native_root_block_capture_log();
                panic("RuntimeUSB: control transfer timeout state=%u events=%u requests=%u failures=%u",
                    controlState, events, requests, failures);
            }
            if (configuration) enumerationTicks=0;
            else if (maintenance && panicOnFailure && ++enumerationTicks >= 1000) {
                native_root_block_capture_log();
                panic("RuntimeUSB: enumeration timeout state=%u events=%u requests=%u failures=%u",
                    controlState, events, requests, failures);
            }
            if (maintenance && returnTicks && !--returnTicks) {
                native_root_block_capture_log();
                panic("RootStage runtime return requested");
            }
            if (maintenance && rebootTicks && !--rebootTicks) rebootBootloader();
            IOSleep(1);
        }
    }
    IOLog("RuntimeUSB: stopped events=%u requests=%u failures=%u\n",events,requests,failures);
    if (initialized && panicOnFailure) {
        native_root_block_capture_log();
        panic("RuntimeUSB: event processing stopped events=%u requests=%u failures=%u", events, requests, failures);
    }
    native_runtime_usb_stop();
    // DMA allocations stay owned until the controller is quiescent.
    thread_terminate(current_thread());
}
}
extern "C" void native_runtime_usb_report(char *out,unsigned length) {
    uint32_t control=controllerOwned ? read(DWC3_DCTL) : 0;
    uint32_t state=controllerOwned ? read(DWC3_DSTS) : 0;
    snprintf(out,length,"RuntimeUSB active=%u id=%x initial=%x/%x/%x current=%x/%x events=%u requests=%u failures=%u last_command=%x dma=%llx",
        active,controllerId,initialControl,initialStatus,initialPhy,control,state,events,requests,failures,lastCommand,physical);
}
extern "C" void native_runtime_usb_stop(void) {
#ifndef MARBLE_IOS18
    native_marble_gmu_stop();
#endif
    active=false;
    if (!controllerOwned) return;
    write(DWC3_DEVTEN,0);
    write(DWC3_DCTL,read(DWC3_DCTL)&~DWC3_DCTL_RUN_STOP);
    for (unsigned i=0; i<10000 && !(read(DWC3_DSTS)&DWC3_DSTS_DEVCTRLHLT); i++) IODelay(1);
}
extern "C" void native_runtime_usb_start(void) {
    unsigned enabled=0;
    if (!PE_parse_boot_argn("rootstage-runtime-usb",&enabled,sizeof(enabled)) || (enabled!=1 && enabled!=2)) return;
    panicOnFailure=enabled==2;
    if (!initialize()) {
        if (panicOnFailure) {
            char report[256]; native_runtime_usb_report(report,sizeof(report));
            panic("%s",report);
        }
        native_runtime_usb_stop();
        return;
    }
    thread_t thread;
    kern_return_t status=kernel_thread_start(worker,nullptr,&thread);
    if (status!=KERN_SUCCESS) panic("RuntimeUSB: event worker start failed %x",status);
    thread_deallocate(thread);
}

#ifndef MARBLE_IOS18
extern "C" void native_marble_watchdog_start(void) {
    thread_t thread;
    if (kernel_thread_start(startupWatchdog, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}

#endif
extern "C" bool native_runtime_usb_configured(void) {return __atomic_load_n(&configuration, __ATOMIC_ACQUIRE) != 0;}
