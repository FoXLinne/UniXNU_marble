/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <stdarg.h>
#include "qbt.h"
#include "qbt_geni.h"
#include "qbt_transport.h"
#include "MarbleBluetoothPlatform.h"
#include "SmmuBypass.h"

#include "MarbleWcnPower.h"
#include "MarbleBluetoothFirmware.h"

extern "C" int armcpu_spi_attach(uint32_t,bool,void(*)(void *),void *,uint32_t *,uint64_t *);
extern "C" int armcpu_spi_share(uint32_t,void(*)(void *),void *);
extern "C" int armcpu_spi_mask(uint32_t,bool);
extern "C" volatile int marble_addon_install_result;
extern "C" {
volatile uint64_t native_bluetooth_stats[32];
/* Connection metadata survives controller power cycles. No addresses, link
 * keys, passkeys, ACL contents or advertising payloads enter these records.
 * Columns: sequence, time ns, direction (0 RX, 1 TX, 2 power), event/opcode,
 * handle, status, public capability flags, parameter length.
 */
volatile uint64_t native_bluetooth_link_events[128][8];
volatile uint32_t native_bluetooth_link_event_count;
/* Per-direction HCI ACL frame and byte counts, without ACL payloads. */
volatile uint64_t native_bluetooth_acl_stats[4];
/* The connection timeline survives bursts of scan reconfiguration. */
volatile uint64_t native_bluetooth_connection_events[64][8];
volatile uint32_t native_bluetooth_connection_event_count;
uint8_t native_bluetooth_boot_rx[512];
volatile uint32_t native_bluetooth_boot_rx_length;
uint8_t native_bluetooth_boot_tail[256];
volatile uint32_t native_bluetooth_boot_tail_length;
/* log-bluetooth=2: pins before TX, RX bytes before TX, then pins, engine status and download reports when startup returns. */
volatile uint32_t native_bluetooth_boot_state[12];
/* log-bluetooth=2: live pid and starts seen for bluetoothd, then for BlueTool. */
volatile uint32_t native_bluetooth_processes[4];
/* log-bluetooth=2: printable runs in controller output, which carry its assert text. */
char *volatile native_bluetooth_boot_text;
volatile uint32_t native_bluetooth_boot_text_length;
}
namespace {
struct Region {IOMemoryMap *map;volatile uint32_t *va;};
struct Bluetooth {
    Region gcc,uart,wrapper,tlmm,smmu;
    IOService *radio;
    IOBufferMemoryDescriptor *dma;
    struct smmu_plan domain;
    uint64_t dma_address;
    struct qbt_geni geni;
    struct qbt *core;
    uint64_t rx_sequence;
    uint32_t pending,rx_wake,power_request,started;
    unsigned log,baud;
    uint8_t address[6];
    bool resources,powered,gated,clocked,serial_irq,wake_irq,service,initializing,loopback;
    uint8_t loop_rx[16];unsigned loop_length;
} bt;
constexpr unsigned kRx=79,kBtEnable=81,kSwCtrl=82,kXo=204;
constexpr size_t kTlmmBytes=0x100000;
static_assert((kXo+1)*4096u<=kTlmmBytes,"Bluetooth GPIO exceeds the TLMM mapping");
constexpr uint32_t kBtEnableCfg=(7u<<6)|(1u<<9); // bt-en-pins: 16 mA, no bias, output
constexpr uint32_t kDmaBytes=32768;
constexpr uint32_t kOperatingBaud=3000000;
constexpr uint64_t kStartupGraceNs=5000000000ULL;
constexpr uint64_t kDisconnectGraceNs=500000000ULL;
bool map(Region &r,uint64_t base,size_t bytes) {
    if(r.va)return true;
    IODeviceMemory *memory=IODeviceMemory::withRange(base,bytes);
    r.map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    if(memory)memory->release();r.va=r.map?reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()):nullptr;
    return r.va!=nullptr;
}
void barrier(){__asm__ volatile("dsb sy" ::: "memory");}
uint64_t nowNs(void *){uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns;}
static bool connectionOpcode(unsigned opcode) {
    switch (opcode) {
    case 0x0405: case 0x0406: case 0x0408: case 0x0409: case 0x040a:
    case 0x0411: case 0x0413: case 0x0419: case 0x041b: case 0x041c:
    case 0x041d: case 0x041f: case 0x042b: case 0x0434: case 0x042c:
    case 0x042d: case 0x200d: case 0x200e: case 0x2043:
        return true;
    default:
        return false;
    }
}
static void putEvent(volatile uint64_t (*ring)[8], unsigned count, volatile uint32_t *counter,
    unsigned direction, unsigned code, unsigned handle, unsigned status, unsigned flags, unsigned length) {
    uint32_t sequence = __atomic_add_fetch(counter, 1u, __ATOMIC_RELAXED);
    volatile uint64_t *row = ring[(sequence - 1) % count];
    row[0] = 0;
    row[1] = nowNs(nullptr);
    row[2] = direction;
    row[3] = code;
    row[4] = handle;
    row[5] = status;
    row[6] = flags;
    row[7] = length;
    __atomic_store_n(&row[0], sequence, __ATOMIC_RELEASE);
}
void linkEvent(unsigned direction, unsigned code, unsigned handle, unsigned status, unsigned flags, unsigned length) {
    putEvent(native_bluetooth_link_events, 128, &native_bluetooth_link_event_count,
        direction, code, handle, status, flags, length);
    if (direction == 2 || (direction == 0 && code <= 0xff) || connectionOpcode(code))
        putEvent(native_bluetooth_connection_events, 64, &native_bluetooth_connection_event_count,
            direction, code, handle, status, flags, length);
}
static bool initializationOpcode(unsigned opcode) {
    return (opcode >= 0x0c00 && opcode < 0x2100) || (opcode & 0xfc00) == 0xfc00;
}
void traceHci(void *,bool tx,uint8_t type,const uint8_t *p,size_t n) {
    if (bt.initializing) return;
    if (type == 2 && n == 4) {
        unsigned dir = tx ? 2 : 0;
        native_bluetooth_acl_stats[dir] = native_bluetooth_acl_stats[dir] + 1;
        native_bluetooth_acl_stats[dir + 1] = native_bluetooth_acl_stats[dir + 1] + (p[2] | (unsigned)p[3] << 8);
        return;
    }
    if(tx && type==1 && n>=3) {
        unsigned op=p[0]|unsigned(p[1])<<8;
        if (!initializationOpcode(op) && !connectionOpcode(op)) return;
        unsigned handle=(op==0x0406 || op==0x0411) && n>=5 ? p[3]|unsigned(p[4])<<8 : 0;
        unsigned status=op==0x0434 && n>=10?p[9]:0;
        unsigned flags=op==0x042b && n>=12?p[9]|unsigned(p[10])<<8|unsigned(p[11])<<16:0;
        if (op == 0x0405 && n >= 16) {
            unsigned packet_type = p[9] | (unsigned)p[10] << 8;
            flags = packet_type | ((unsigned)(p[11] & 3) << 16) |
                ((unsigned)(p[14] >> 7) << 18) | ((unsigned)(p[15] & 1) << 19);
        } else if ((op == 0x2042 || op == 0x200c) && n >= 5) {
            flags = p[3] | ((unsigned)p[4] << 8);
        }
        linkEvent(1,op,handle,status,flags,(unsigned)n-3);return;
    }
    if(type!=4 || n<2)return;
    unsigned ev=p[0],handle=0,status=0,flags=0;
    if((ev==0x0e || ev==0x0f) && n>=6) {
        unsigned at=ev==0x0e?3:4,op=p[at]|unsigned(p[at+1])<<8;
        if (!initializationOpcode(op) && !connectionOpcode(op)) return;
        linkEvent(0,op,0,p[ev==0x0e?5:2],ev,(unsigned)n-2);return;
    }
    switch(ev) {
    case 0x03:case 0x05:case 0x06:case 0x08:
        if(n<5)return;
        status=p[2];handle=p[3]|unsigned(p[4])<<8;
        if(ev==0x05 && n>=6)flags=p[5];break;
    case 0x31:break;
    case 0x32:
        if(n<11)return;
        flags=p[8]|unsigned(p[9])<<8|unsigned(p[10])<<16;break;
    case 0x36:if(n<3)return;status=p[2];break;
    case 0x3e:
        if(n<6 || (p[2]!=1 && p[2]!=0x0a))return;
        flags=p[2];status=p[3];handle=p[4]|unsigned(p[5])<<8;break;
    default:return;
    }
    linkEvent(0,ev,handle,status,flags,(unsigned)n-2);
}
void wake(void *){__atomic_store_n(&bt.pending,1u,__ATOMIC_RELEASE);thread_wakeup((event_t)&bt.pending);}
void waitUntil(void *,uint64_t deadline) {
    if(__atomic_exchange_n(&bt.pending,0u,__ATOMIC_ACQ_REL))return;
    if(deadline==UINT64_MAX)assert_wait((event_t)&bt.pending,THREAD_UNINT);
    else {uint64_t absolute;nanoseconds_to_absolutetime(deadline,&absolute);assert_wait_deadline((event_t)&bt.pending,THREAD_UNINT,absolute);}
    if(__atomic_exchange_n(&bt.pending,0u,__ATOMIC_ACQ_REL))clear_wait(current_thread(),THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
}
uint32_t urd(void *,uint32_t reg){return bt.uart.va[reg/4];}
void uwr(void *,uint32_t reg,uint32_t value){bt.uart.va[reg/4]=value;barrier();}
uint32_t grd(void *,uint32_t reg){return bt.gcc.va[reg/4];}
void gwr(void *,uint32_t reg,uint32_t value){bt.gcc.va[reg/4]=value;barrier();}
void delay(void *,unsigned us){IODelay(us);}
void dmaBarrier(void *){barrier();}
void dmaSync(void *,const struct qbt_dma_buffer *buffer,size_t bytes,bool device,bool tx) {
    // Match the board's touch path: cached RAM behind the firmware bypass bank.
    if(bytes && (device || !tx)) {
        uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
        size_t line=4u<<((ctr>>16)&15);
        uintptr_t at=reinterpret_cast<uintptr_t>(buffer->bytes)&~(line-1);
        uintptr_t end=reinterpret_cast<uintptr_t>(buffer->bytes)+bytes;
        for(;at<end;at+=line) {
            if(tx)__asm__ volatile("dc cvac, %0"::"r"(at):"memory");
            else if(device)__asm__ volatile("dc civac, %0"::"r"(at):"memory");
            else __asm__ volatile("dc ivac, %0"::"r"(at):"memory");
        }
    }
    barrier();
}
uint32_t srd(void *,uint32_t reg){return bt.smmu.va[reg/4];}
void swr(void *,uint32_t reg,uint32_t value){bt.smmu.va[reg/4]=value;barrier();}
void irqMask(void *,bool masked) {
    if(bt.serial_irq)(void)armcpu_spi_mask(MARBLE_UART20_INTID,masked);
}
void serialIrq(void *){qbt_geni_irq(&bt.geni);}
void pin(unsigned gpio,uint32_t cfg,int output=-1) {
    if(output>=0)bt.tlmm.va[(gpio*4096+4)/4]=output?2:0;
    bt.tlmm.va[gpio*4096/4]=cfg;barrier();
}
void rxIrq(void *) {
    volatile uint32_t *r=bt.tlmm.va+kRx*4096/4;
    if (!(r[2] & 1) || !(r[3] & 1)) return;
    r[2]&=~1u;r[3]=0;barrier();
    __atomic_store_n(&bt.rx_wake,1u,__ATOMIC_RELEASE);wake(nullptr);
}
void disarmRxWake(){bt.tlmm.va[(kRx*4096+8)/4]&=~1u;bt.tlmm.va[(kRx*4096+12)/4]=0;pin(kRx,3u|(1u<<2));}
bool serialClock(bool enabled) {
    if(!marble_geni_uart20_vote(bt.gcc.va+0x62010/4,enabled))return false;
    if(!enabled){bt.clocked=false;return true;}
    for(unsigned i=0;i<1000;i++){if(!(grd(nullptr,0x2e610)&(1u<<31))){bt.clocked=true;return true;}IODelay(1);}
    return false;
}
size_t rxBytes(void *,const uint8_t *bytes,size_t n) {
    if(bt.loopback) {
        size_t count=sizeof(bt.loop_rx)-bt.loop_length;if(count>n)count=n;
        memcpy(bt.loop_rx+bt.loop_length,bytes,count);bt.loop_length+=(unsigned)count;
        return n;
    }
    if(bt.log>=2 && bt.initializing) {
        uint32_t at=__atomic_load_n(&native_bluetooth_boot_rx_length,__ATOMIC_RELAXED);
        size_t count=sizeof(native_bluetooth_boot_rx)-at;if(count>n)count=n;
        if(count) {
            memcpy(native_bluetooth_boot_rx+at,bytes,count);
            __atomic_store_n(&native_bluetooth_boot_rx_length,at+(uint32_t)count,__ATOMIC_RELEASE);
        }
        static char run[96];static unsigned runLength;
        for(size_t i=0;i<n;i++) {
            uint8_t c=bytes[i];
            if(c>=32 && c<127 && runLength<sizeof(run)-1){run[runLength++]=(char)c;continue;}
            if(runLength>=12 && memchr(run,' ',runLength)) {
                uint32_t at=native_bluetooth_boot_text_length;
                if(native_bluetooth_boot_text && at+runLength+1<8192) {
                    memcpy(native_bluetooth_boot_text+at,run,runLength);native_bluetooth_boot_text[at+runLength]='|';
                    __atomic_store_n(&native_bluetooth_boot_text_length,at+runLength+1,__ATOMIC_RELEASE);
                }
            }
            runLength=0;
        }
        uint32_t tail=__atomic_load_n(&native_bluetooth_boot_tail_length,__ATOMIC_RELAXED);
        for(size_t i=0;i<n;i++)native_bluetooth_boot_tail[(tail+i)%sizeof(native_bluetooth_boot_tail)]=bytes[i];
        __atomic_store_n(&native_bluetooth_boot_tail_length,tail+(uint32_t)n,__ATOMIC_RELEASE);
    }
    bt.rx_sequence+=n;return qbt_receive(bt.core,bytes,n);
}
size_t nativeRx(void *,const uint8_t *bytes,size_t n){return qbt_transport_receive(bytes,(uint32_t)n);}
size_t writeUart(void *,const uint8_t *p,size_t n){return qbt_geni_write(&bt.geni,p,n);}
bool drained(void *){return bt.gated || qbt_geni_drained(&bt.geni);}
int flow(void *,bool enabled){qbt_geni_set_flow(&bt.geni,enabled);return 0;}
void pump(void *) {
    if(bt.log>=2 && bt.initializing) {
        struct qbt_stats stats;qbt_get_stats(bt.core,&stats);
        native_bluetooth_stats[3]=stats.tx_bytes;native_bluetooth_stats[4]=stats.rx_bytes;
        native_bluetooth_boot_state[10]=(uint32_t)stats.tlv_acks|((uint32_t)stats.tlv_sent<<16);
        native_bluetooth_boot_state[11]=(uint32_t)stats.hardware_errors|((uint32_t)stats.last_hardware_error<<8)|((uint32_t)stats.tlv_final_status<<16);
    }
    if(__atomic_exchange_n(&bt.rx_wake,0u,__ATOMIC_ACQ_REL) && bt.powered) {
        qbt_rx_wake(bt.core);
        /* The GPIO consumed the first IBS wake byte while the UART was gated. */
        const uint8_t wakeByte=0xfd;(void)qbt_receive(bt.core,&wakeByte,1);
    }
    if(!bt.gated) {
        if(qbt_geni_service(&bt.geni))wake(nullptr);
        if(bt.geni.state==QBT_GENI_FAILED)qbt_fail(bt.core,bt.geni.error);
    }
}
int quiesce() {
    uint64_t end=nowNs(nullptr)+40000000ULL;
    for(;;) {
        int e=qbt_geni_quiesce(&bt.geni);if(!e)return 0;
        if(e!=-EINPROGRESS && e!=-EBUSY)return e;
        pump(nullptr);if(bt.geni.state==QBT_GENI_STOPPED)return 0;
        if(nowNs(nullptr)>=end)return -ETIMEDOUT;
        uint64_t next=qbt_geni_deadline(&bt.geni);if(next>end)next=end;waitUntil(nullptr,next);
    }
}
int clockVote(void *,bool enabled) {
    if(enabled) {
        if(!bt.gated)return 0;
        if(!serialClock(true))return -EIO;
        disarmRxWake();IODelay(10);bt.gated=false;
        return qbt_geni_resume(&bt.geni);
    }
    if(bt.gated)return 0;
    uint64_t sequence=bt.rx_sequence;
    int e=quiesce();if(e)return e;
    if(sequence!=bt.rx_sequence || qbt_transport_tx_pending()) {qbt_geni_resume(&bt.geni);return -EBUSY;}
    volatile uint32_t *r=bt.tlmm.va+kRx*4096/4;
    r[2]=0;r[3]=0;pin(kRx,3u);
    r[2]=(3u<<5)|(2u<<2)|(1u<<4)|1u;barrier();
    if(!(r[1]&1) || (r[3]&1)) {disarmRxWake();qbt_geni_resume(&bt.geni);return -EBUSY;}
    /* Leave RTS ready so the controller can send its IBS wake indication. */
    uwr(nullptr,0x2ac,0x80000001u);
    bt.gated=true;
    if(!serialClock(false)){bt.gated=false;disarmRxWake();qbt_geni_resume(&bt.geni);return -EIO;}
    return 0;
}
int setRate(void *,uint32_t baud) {
    int e=quiesce();if(e)return e;
    uint32_t hz=0;struct marble_geni_clock_io io={nullptr,grd,gwr,delay};
    e=marble_geni_uart20_source(&io,baud,&hz);
    if(!e)e=qbt_geni_set_rate(&bt.geni,baud,hz,16);
    if(!e)e=qbt_geni_resume(&bt.geni);
    if(!e)bt.baud=baud;return e;
}
void *allocate(void *,size_t n){return IOMallocData(n);}
void release(void *,void *p,size_t n){IOFreeData(p,n);}
int firmware(void *,const char *name,const uint8_t **data,size_t *size) {
    uint8_t *bytes=nullptr;int e=marble_bt_firmware_read(name,&bytes,size);*data=bytes;return -e;
}
void firmwarePut(void *,const uint8_t *data,size_t size){marble_bt_firmware_release(const_cast<uint8_t *>(data),size);}
void log(void *,int level,const char *line){if(level<=3 || bt.log)printf("[bluetooth] %s\n",line);}
bool address() {
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSData *id=chosen?OSDynamicCast(OSData,chosen->getProperty("unique-chip-id")):nullptr;
    if(!id || id->getLength()<8){if(chosen)chosen->release();return false;}
    uint64_t hash=0xcbf29ce484222325ULL;
    static const char domain[]="unixnu-bluetooth";
    for(unsigned i=0;i<sizeof(domain)-1;i++)hash=(hash^(uint8_t)domain[i])*0x100000001b3ULL;
    const uint8_t *seed=static_cast<const uint8_t *>(id->getBytesNoCopy());
    bool nonzero=false;
    for(unsigned i=0;i<id->getLength();i++){nonzero|=seed[i]!=0;hash=(hash^seed[i])*0x100000001b3ULL;}
    bool published=false;
    if(nonzero) {
        for(unsigned i=0;i<6;i++)bt.address[i]=(uint8_t)(hash>>(i*8));
        bt.address[0]=(bt.address[0]&0xfe)|2;
        IORegistryEntry *node=IORegistryEntry::fromPath("/arm-io/bluetooth",gIODTPlane);
        OSData *encoding=node?OSDynamicCast(OSData,node->getProperty("transport-encoding")):nullptr;
        OSData *speed=node?OSDynamicCast(OSData,node->getProperty("transport-speed")):nullptr;
        uint32_t wire=0,baud=0;
        if(encoding && encoding->getLength()==4)memcpy(&wire,encoding->getBytesNoCopy(),4);
        if(speed && speed->getLength()==4)memcpy(&baud,speed->getBytesNoCopy(),4);
        if(node && wire==1 && baud==kOperatingBaud) {
            published=chosen->setProperty("mac-address-bluetooth0",bt.address,6) &&
                node->setProperty("local-mac-address",bt.address,6);
            if(published && !bt.radio) {
                IOService *radio=new IOService;
                if(radio && radio->init()) {
                    radio->setName("bluetooth");
                    published=radio->setProperty("device_type","bluetooth") &&
                        radio->setProperty("transport-encoding",encoding) &&
                        radio->setProperty("transport-speed",speed) &&
                        radio->setProperty("local-mac-address",bt.address,6);
                    IOService *provider=IOService::getPlatform();
                    if(published && provider && radio->attach(provider)) {
                        bt.radio=radio;radio->registerService();
                    } else {published=false;radio->release();}
                } else {published=false;if(radio)radio->release();}
            }
        }
        if(node)node->release();
    }
    chosen->release();return published;

}
int resources() {
    if(bt.resources)return 0;
    if(!map(bt.gcc,0x100000,0x80000) || !map(bt.uart,MARBLE_UART20_BASE,0x4000) ||
        !map(bt.wrapper,MARBLE_QUP2_BASE,0x2000) || !map(bt.tlmm,0xf100000,kTlmmBytes) || !map(bt.smmu,0x15000000,0x100000))return -ENOMEM;
    if(!address())return -ENOENT;
    const IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(!bt.dma)bt.dma=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,options,kDmaBytes,4096);
    if(!bt.dma)return -ENOMEM;
    IOByteCount contiguous=0;
    uint64_t pa=bt.dma->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);
    if(!pa || contiguous<kDmaBytes)return -EIO;
    struct smmu_io io={nullptr,srd,swr};
    bt.domain=smmu_plan_bypass(&io,MARBLE_QUP2_SID,pa,kDmaBytes,78,120);
    const char *error=bt.domain.error?bt.domain.error:smmu_configure_bypass(&io,&bt.domain,MARBLE_QUP2_SID);
    if(error) {
        printf("[bluetooth dma] slot=%u bank=%u result=%s\n",bt.domain.slot,bt.domain.bank,error);
        return -EIO;
    }
    bt.dma_address=pa;
    printf("[bluetooth dma] bypass slot=%u bank=%u pa=%llx bytes=%u\n",bt.domain.slot,bt.domain.bank,pa,kDmaBytes);
    bt.resources=true;return 0;
}
int stopEngine() {
    irqMask(nullptr,true);uwr(nullptr,0xe1c,0);
    if((urd(nullptr,0x40)&0x1001) || (urd(nullptr,0xe40)&3)) {
        uwr(nullptr,0x604,2);uwr(nullptr,0x634,2);
        unsigned i=0;for(;i<1000 && (urd(nullptr,0x40)&0x1001);i++)IODelay(1);
        if(i==1000)return -ETIMEDOUT;
        uwr(nullptr,0xc58,1);uwr(nullptr,0xd58,1);
        for(i=0;i<1000 && (urd(nullptr,0xe40)&3);i++)IODelay(1);
        if(i==1000)return -ETIMEDOUT;
    }
    uwr(nullptr,0x618,UINT32_MAX);uwr(nullptr,0x648,UINT32_MAX);
    uwr(nullptr,0xc44,UINT32_MAX);uwr(nullptr,0xd44,UINT32_MAX);
    bt.geni.state=QBT_GENI_OFF;bt.geni.rx_busy=bt.geni.tx_busy=false;
    return 0;
}
uint32_t linePins() {
    static const uint8_t pins[]={76,77,78,79,kBtEnable,kSwCtrl};
    uint32_t bits=0;
    for(unsigned i=0;i<sizeof(pins);i++)bits|=(bt.tlmm.va[(pins[i]*4096u+4)/4]&1u)<<i;
    return bits;
}
int prepareEngine() {
    uint8_t *bytes=static_cast<uint8_t *>(bt.dma->getBytesNoCopy());
    struct qbt_dma_buffer tx={bytes,bt.dma_address,QBT_GENI_TX_SIZE},rx[QBT_GENI_RX_COUNT];
    for(unsigned i=0;i<QBT_GENI_RX_COUNT;i++)rx[i]={bytes+4096+i*QBT_GENI_RX_SIZE,bt.dma_address+4096+i*QBT_GENI_RX_SIZE,QBT_GENI_RX_SIZE};
    struct qbt_geni_ops ops={nullptr,urd,uwr,dmaBarrier,dmaSync,nowNs,irqMask,wake,rxBytes};
    int e=qbt_geni_prepare(&bt.geni,&ops,&tx,rx);if(e)return e;
    native_bluetooth_stats[14]=5;
    uint32_t hz;struct marble_geni_clock_io clocks={nullptr,grd,gwr,delay};
    e=marble_geni_uart20_source(&clocks,115200,&hz);if(e)return e;
    e=qbt_geni_set_rate(&bt.geni,115200,hz,16);if(e)return e;
    bt.baud=115200;return 0;
}
/* log-bluetooth=2, controller still in reset: GENI's internal loopback checks DMA,
 * cache maintenance and packing without the controller. Stats[30] holds received
 * length, bit 16 sent, bit 17 match; stats[31] the first received bytes. */
int loopbackCheck() {
    static const uint8_t pattern[16]={0x01,0x55,0xaa,0x00,0xff,0x04,0x0e,0x7e,0x81,0x42,0x24,0x18,0x10,0x20,0x40,0x80};
    uwr(nullptr,0x22c,3);bt.loopback=true;bt.loop_length=0;
    int e=qbt_geni_start(&bt.geni);
    size_t sent=e?0:qbt_geni_write(&bt.geni,pattern,sizeof(pattern));
    uint64_t end=nowNs(nullptr)+50000000ULL;
    while(sent && bt.loop_length<sizeof(pattern) && bt.geni.state==QBT_GENI_RUN && nowNs(nullptr)<end) {
        if(!qbt_geni_service(&bt.geni))IOSleep(1);
    }
    uint64_t first=0;memcpy(&first,bt.loop_rx,bt.loop_length<8?bt.loop_length:8);
    native_bluetooth_stats[31]=first;
    native_bluetooth_stats[30]=bt.loop_length|(sent?1u<<16:0)|(bt.loop_length==sizeof(pattern) && !memcmp(bt.loop_rx,pattern,sizeof(pattern))?1u<<17:0);
    /* Stopping exercises the same cancel path as a baud change. */
    e=bt.geni.state==QBT_GENI_RUN?quiesce():-EIO;
    bt.loopback=false;uwr(nullptr,0x22c,0);
    if(!e && bt.geni.state==QBT_GENI_STOPPED)return 0;
    e=stopEngine();if(e)return e;
    return prepareEngine();
}
int powerOn() {
    if(bt.powered)return 0;
    native_bluetooth_stats[14]=1;int e=resources();if(e)return e;
    native_bluetooth_stats[14]=2;
    e=marble_wcn_power_get(MARBLE_WCN_BLUETOOTH);if(e)return -e;
    bt.powered=true;bt.gated=true;
    native_bluetooth_stats[14]=3;
    /* The controller leaves reset with RTS low: UART function only after it has settled. */
    for(unsigned gpio=76;gpio<=79;gpio++)pin(gpio,gpio>=78?3u:1u);
    pin(kSwCtrl,1u);pin(kBtEnable,kBtEnableCfg,0);IOSleep(100);
    if(!marble_gcc_update32(bt.gcc.va+0x62018/4,0,1u))return -EIO;
    for(unsigned i=0;i<1000 && !(grd(nullptr,0)&(1u<<31));i++)IODelay(1);
    if(!(grd(nullptr,0)&(1u<<31)) || !serialClock(true))return -EIO;
    bt.gated=false;
    native_bluetooth_stats[11]=urd(nullptr,0x68);native_bluetooth_stats[12]=bt.wrapper.va[1];
    native_bluetooth_stats[13]=grd(nullptr,0x62010);native_bluetooth_stats[14]=4;
    if(bt.wrapper.va[1]<0x20050000)return -ENOTSUP;
    if(((urd(nullptr,0x68)>>8)&255)!=2)return -ENXIO;
    e=stopEngine();if(e)return e;
    e=prepareEngine();if(e)return e;
    if(!bt.serial_irq){e=armcpu_spi_attach(MARBLE_UART20_INTID,false,serialIrq,nullptr,nullptr,nullptr);if(e)return -EIO;bt.serial_irq=true;irqMask(nullptr,true);}
    if(!bt.wake_irq){e=armcpu_spi_share(240,rxIrq,nullptr);if(e)return -EIO;bt.wake_irq=true;}
    if(bt.log>=2){e=loopbackCheck();if(e)return e;}
    // Stock btpower asserts XO 100-200 us before BT_EN and holds it for 4-6 ms.
    pin(kXo,1u<<9,1);IODelay(200);
    pin(kBtEnable,kBtEnableCfg,1);IOSleep(5);
    pin(kXo,1u<<9,0);IOSleep(60);
    for(unsigned gpio=76;gpio<=79;gpio++)pin(gpio,(1u<<2)|(gpio>=78?3u:1u));
    disarmRxWake();
    e=qbt_geni_start(&bt.geni);if(e)return e;
    native_bluetooth_stats[14]=6;
    bt.initializing=true;
    if(bt.log>=2) {
        /* Bytes seen here precede the first command: controller output, not a reply. */
        for(unsigned i=0;i<20;i++){pump(nullptr);IOSleep(1);}
        native_bluetooth_boot_state[0]=linePins();
        native_bluetooth_boot_state[1]=__atomic_load_n(&native_bluetooth_boot_rx_length,__ATOMIC_ACQUIRE);
    }
    e=qbt_boot(bt.core,kOperatingBaud,bt.address);bt.initializing=false;
    if(bt.log>=2) {
        static const uint32_t regs[]={0x40,0x610,0x640,0xc40,0xd40,0x800,0xd54};
        native_bluetooth_boot_state[2]=linePins();
        for(unsigned i=0;i<7;i++)native_bluetooth_boot_state[3+i]=bt.clocked?urd(nullptr,regs[i]):0;
        struct qbt_stats stats;qbt_get_stats(bt.core,&stats);
        native_bluetooth_boot_state[10]=(uint32_t)stats.tlv_acks|((uint32_t)stats.tlv_sent<<16);
        native_bluetooth_boot_state[11]=(uint32_t)stats.hardware_errors|((uint32_t)stats.last_hardware_error<<8)|((uint32_t)stats.tlv_final_status<<16);
    }
    return e;
}
int powerOff() {
    if(!bt.powered)return 0;
    qbt_set_service(bt.core,false);bt.service=false;
    int e=0;
    if(bt.geni.state!=QBT_GENI_OFF) {
        if(bt.gated){int resumed=clockVote(nullptr,true);if(!e)e=resumed;}
        if(bt.clocked){int shutdown=qbt_shutdown(bt.core);if(!e)e=shutdown;}
    }
    if(bt.clocked && bt.geni.state!=QBT_GENI_OFF){int stopped=quiesce();if(!e)e=stopped;}
    irqMask(nullptr,true);disarmRxWake();pin(kBtEnable,kBtEnableCfg,0);IOSleep(100);
    int retired=bt.clocked?stopEngine():0;if(!e)e=retired;
    if(!retired)bt.gated=serialClock(false);
    bt.powered=false;__atomic_store_n(&bt.rx_wake,0u,__ATOMIC_RELEASE);
    int released=marble_wcn_power_put(MARBLE_WCN_BLUETOOTH);if(!e)e=-released;
    return e;
}
int requestPower(void *,bool enabled) {
    uint32_t expected=0;
    const uint32_t request = enabled ? 1u : 2u;
    if (!__atomic_compare_exchange_n(&bt.power_request, &expected, request, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return expected == request ? 0 : EBUSY;
    linkEvent(2,1,0,0,enabled,0);
    wake(nullptr);return 0;
}
void powerResult(bool enabled,int error) {
    linkEvent(2,2,0,(unsigned)error,enabled,0);
    struct qbt_info info;qbt_get_info(bt.core,&info);
    native_bluetooth_stats[17]=info.product;native_bluetooth_stats[18]=info.soc;
    native_bluetooth_stats[19]=((uint32_t)info.rom<<16)|info.patch;
    if(enabled && !error)native_bluetooth_stats[14]=7;
    qbt_transport_power_result(enabled,error<0?-error:error,info.firmware,info.baud);
    native_bluetooth_stats[20]=(unsigned)bt.powered|((unsigned)bt.gated<<1)|((unsigned)bt.clocked<<2);
    native_bluetooth_stats[21]=marble_wcn_power_owners();
    native_bluetooth_stats[0]=error?3:enabled?2:0;native_bluetooth_stats[1]=(uint64_t)(int64_t)error;
    printf("[bluetooth] power=%u result=%d baud=%u\n",enabled,error,info.baud);
    if(error && bt.geni.error)printf("[bluetooth uart] error=%d irq=%x/%x/%x/%x\n",bt.geni.error,bt.geni.fault_irq[0],bt.geni.fault_irq[1],bt.geni.fault_irq[2],bt.geni.fault_irq[3]);
}
void sampleController() {
    struct qbt_stats stats;
    qbt_get_stats(bt.core, &stats);
    native_bluetooth_stats[2] = bt.baud;
    native_bluetooth_stats[3] = stats.tx_bytes;
    native_bluetooth_stats[4] = stats.rx_bytes;
    native_bluetooth_stats[5] = bt.geni.stats.interrupts;
    native_bluetooth_stats[6] = stats.timeouts;
    native_bluetooth_stats[7] = stats.clock_sleeps;
    native_bluetooth_stats[8] = bt.domain.slot;
    native_bluetooth_stats[9] = bt.domain.bank;
    native_bluetooth_stats[15] = bt.geni.stats.line_errors;
    native_bluetooth_stats[16] = bt.geni.stats.dma_errors;
    native_bluetooth_stats[20] = unsigned(bt.powered) | (unsigned(bt.gated) << 1) | (unsigned(bt.clocked) << 2);
    native_bluetooth_stats[21] = marble_wcn_power_owners();
    for (unsigned i = 0; i < 4; i++) native_bluetooth_stats[22 + i] = bt.geni.fault_irq[i];
    native_bluetooth_stats[26] = stats.malformed;
    native_bluetooth_stats[27] = stats.unmatched_events;
    native_bluetooth_stats[28] = bt.geni.stats.rx_dma;
    native_bluetooth_stats[29] = bt.geni.stats.tx_dma;
}
void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "marble-bluetooth");
    native_bluetooth_stats[10] = 2;
    const uint64_t addonDeadline = nowNs(nullptr) + 30000000000ULL;
    int addon;
    while ((addon = __atomic_load_n(&marble_addon_install_result, __ATOMIC_ACQUIRE)) == 0 &&
           nowNs(nullptr) < addonDeadline) waitUntil(nullptr, addonDeadline);
    int error = addon < 0 ? -addon : addon == 0 ? ETIMEDOUT : address() ? 0 : ENOENT;
    if (error) {
        native_bluetooth_stats[0] = 3;
        native_bluetooth_stats[1] = (uint64_t)(int64_t)-error;
        printf("[bluetooth] configuration error=%d\n", error);
        return;
    }
    native_bluetooth_stats[10] = 3;
    struct qbt_platform ops={nullptr,allocate,release,nowNs,writeUart,drained,setRate,flow,clockVote,pump,waitUntil,nativeRx,firmware,firmwarePut,log,traceHci,QBT_WCN6855};
    bt.core = qbt_create(&ops);
    if (!bt.core) error = ENOMEM;
    struct qbt_transport_ops transport = {nullptr, wake, requestPower};
    if (!error) error = qbt_transport_init(&transport, kOperatingBaud);
    if (error) {
        if (bt.core) qbt_destroy(bt.core);
        native_bluetooth_stats[0] = 3;
        native_bluetooth_stats[1] = (uint64_t)(int64_t)-error;
        printf("[bluetooth] transport error=%d\n", error);
        return;
    }
    native_bluetooth_stats[0] = 0;
    native_bluetooth_stats[10] = 4;
    printf("[bluetooth] UART.SK registered, awaiting native session\n");
    uint64_t closeAt = 0;
    bool hadSession = false;
    for (;;) {
        const uint32_t command = __atomic_exchange_n(&bt.power_request, 0u, __ATOMIC_ACQ_REL);
        if (command) {
            const bool enabled = command == 1;
            error = enabled ? powerOn() : powerOff();
            if (error && enabled) (void)powerOff();
            powerResult(enabled, error);
            closeAt = bt.powered ? nowNs(nullptr) + kStartupGraceNs : 0;
            hadSession = false;
        }
        if (bt.powered) {
            pump(nullptr);
            const bool connected = qbt_transport_connected();
            if (connected != bt.service) {
                qbt_set_service(bt.core, connected);
                bt.service = connected;
            }
            if (connected) {
                hadSession = true;
                closeAt = 0;
            } else if (hadSession && !closeAt) {
                closeAt = nowNs(nullptr) + kDisconnectGraceNs;
            }
            uint8_t tx[2048];
            const size_t room = qbt_transmit_capacity(bt.core);
            const uint32_t count = room ? qbt_transport_tx_take(tx, room < sizeof(tx) ? (uint32_t)room : sizeof(tx)) : 0;
            if (count && qbt_transmit(bt.core, tx, count)) qbt_fail(bt.core, -EIO);
            qbt_step(bt.core);
            struct qbt_info info;
            qbt_get_info(bt.core, &info);
            const bool due = closeAt && nowNs(nullptr) >= closeAt && !qbt_transport_connected();
            if (info.state == 3 || bt.geni.state == QBT_GENI_FAILED || due) {
                error = info.state == 3 ? info.error : bt.geni.error;
                const int cleanup = powerOff();
                powerResult(false, error ? error : cleanup);
                closeAt = 0;
                hadSession = false;
            }
        }
        sampleController();
        uint64_t deadline = bt.powered ? qbt_deadline(bt.core) : UINT64_MAX;
        const uint64_t serialDeadline = bt.powered && !bt.gated ? qbt_geni_deadline(&bt.geni) : UINT64_MAX;
        if (serialDeadline < deadline) deadline = serialDeadline;
        if (closeAt && closeAt < deadline) deadline = closeAt;
        if (!deadline) {
            thread_yield_to_preemption();
            continue;
        }
        waitUntil(nullptr, deadline);
    }
}
}
extern "C" void native_marble_bluetooth_addon_ready(void) {
    wake(nullptr);
}
struct proc;
extern "C" {
void proc_iterate(unsigned,int (*)(struct proc *,void *),void *,int (*)(struct proc *,void *),void *);
int proc_pid(struct proc *);
}
int processSeen(struct proc *process,void *found) {
    static const char *const names[]={"bluetoothd","BlueTool"};
    const char *name=proc_best_name(process);
    for(unsigned i=0;name && i<2;i++)if(!strcmp(name,names[i]))static_cast<int *>(found)[i]=proc_pid(process);
    return 0;
}
void processWatch(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"marble-bluetooth-procs");
    for(;;) {
        int found[2]={0,0};
        proc_iterate(1u|4u,processSeen,found,nullptr,nullptr);
        for(unsigned i=0;i<2;i++) {
            if(found[i] && (uint32_t)found[i]!=native_bluetooth_processes[i*2])native_bluetooth_processes[i*2+1]=native_bluetooth_processes[i*2+1]+1;
            native_bluetooth_processes[i*2]=(uint32_t)found[i];
        }
        IOSleep(2000);
    }
}
extern "C" void native_marble_bluetooth_start(void) {
    unsigned enabled=0;
    if(!PE_parse_boot_argn("qbt",&enabled,sizeof(enabled)))
        PE_parse_boot_argn("bluetooth",&enabled,sizeof(enabled));
    if(!enabled)return;
    uint32_t expected=0;if(!__atomic_compare_exchange_n(&bt.started,&expected,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    native_bluetooth_stats[0]=1;
    native_bluetooth_stats[10]=1;
    PE_parse_boot_argn("log-bluetooth",&bt.log,sizeof(bt.log));
    if(bt.log>=2)native_bluetooth_boot_text=static_cast<char *>(IOMallocData(8192));
    thread_t thread=nullptr;
    if(kernel_thread_start(worker,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    else native_bluetooth_stats[0]=3;
    if(bt.log>=2 && kernel_thread_start(processWatch,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
}
