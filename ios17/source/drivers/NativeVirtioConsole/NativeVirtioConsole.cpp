// SPDX-License-Identifier: BSD-2-Clause-Patent
// Modern VirtIO console port 0. OASIS VirtIO 1.2 sections 4.2 and 5.3.
// Binary transport only: this does not implement a GPU, compositor or display.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <kern/task.h>
#include "NativeVirtioConsoleTransport.h"
namespace {
constexpr unsigned queueSize=8, bufferSize=4096;
struct Descriptor { uint64_t address;uint32_t length;uint16_t flags,next; };
struct Used { uint32_t id,length; };
static_assert(sizeof(Descriptor)==16 && sizeof(Used)==8,"Actual VirtIO wire sizes");
inline void dmaBarrier() { __asm__ volatile("dmb osh" ::: "memory"); }
struct Queue {
    IOBufferMemoryDescriptor *ring=nullptr,*buffer=nullptr;
    uint64_t physical=0,dataPhysical=0;
    uint16_t available=0,used=0;
    unsigned bytes=0,offset=0;
    bool pending=false;
    unsigned char *memory() const {return static_cast<unsigned char *>(ring->getBytesNoCopy());}
    void *data() const {return buffer->getBytesNoCopy();}
};
}
extern "C" {
volatile uint32_t native_console_ready;
volatile uint32_t native_console_tx_requests,native_console_rx_requests,native_console_failures;
volatile uint64_t native_console_tx_bytes,native_console_rx_bytes,native_console_mmio;
}
class NativeVirtioConsoleClient;
class NativeVirtioConsole : public IOService {
    OSDeclareDefaultStructors(NativeVirtioConsole)
    friend class NativeVirtioConsoleClient;
    friend IOReturn native_console_send(IOService *,IOService *,const void *,size_t);
    friend IOReturn native_console_receive(IOService *,IOService *,void *,size_t *);
    IOMemoryMap *mapping=nullptr;
    IOLock *gate=nullptr;
    IOService *owner=nullptr;
    Queue queues[2];
    uintptr_t registers=0;
    bool online=false,ownsDevice=false;
    uint32_t read32(unsigned n) const {return *reinterpret_cast<volatile uint32_t *>(registers+n);}
    void write32(unsigned n,uint32_t v) {
        *reinterpret_cast<volatile uint32_t *>(registers+n)=v;
        __asm__ volatile("dsb oshst" ::: "memory");
    }
    void address(unsigned n,uint64_t v) {write32(n,(uint32_t)v);write32(n+4,(uint32_t)(v>>32));}
    bool reset();
    bool configure();
    bool configureQueue(unsigned);
    void post(unsigned,unsigned);
    IOReturn complete(unsigned,unsigned *);
    void cleanup();
    IOReturn send(const void *,size_t);
    IOReturn receive(void *,size_t *);
public:
    IOService *probe(IOService *,SInt32 *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    bool handleOpen(IOService *,IOOptionBits,void *) override;
    void handleClose(IOService *,IOOptionBits) override;
    bool handleIsOpen(const IOService *) const override;
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
};
class NativeVirtioConsoleClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeVirtioConsoleClient)
    NativeVirtioConsole *controller=nullptr;
    bool opened=false;
    static IOReturn sendAction(OSObject *,void *,IOExternalMethodArguments *);
    static IOReturn receiveAction(OSObject *,void *,IOExternalMethodArguments *);
public:
    bool initWithTask(task_t,void *,UInt32,OSDictionary *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *) override;
};
OSDefineMetaClassAndStructors(NativeVirtioConsole,IOService)
OSDefineMetaClassAndStructors(NativeVirtioConsoleClient,IOUserClient)

extern "C" IOReturn native_console_send(IOService *provider,IOService *owner,const void *bytes,size_t size) {
    NativeVirtioConsole *console=OSDynamicCast(NativeVirtioConsole,provider);
    return console && owner && console->isOpen(owner)?console->send(bytes,size):kIOReturnNotOpen;
}
extern "C" IOReturn native_console_receive(IOService *provider,IOService *owner,void *bytes,size_t *size) {
    NativeVirtioConsole *console=OSDynamicCast(NativeVirtioConsole,provider);
    return console && owner && console->isOpen(owner)?console->receive(bytes,size):kIOReturnNotOpen;
}

IOService *NativeVirtioConsole::probe(IOService *provider,SInt32 *score) {
    OSData *compatible=OSDynamicCast(OSData,provider->getProperty("compatible"));
    if(!compatible || compatible->getLength()!=12 || memcmp(compatible->getBytesNoCopy(),"virtio,mmio",12))return nullptr;
    IOMemoryMap *view=provider->mapDeviceMemoryWithIndex(0);
    if(!view)return nullptr;
    uintptr_t base=view->getVirtualAddress();
    bool matches=view->getLength()>=0x100 &&
        *reinterpret_cast<volatile uint32_t *>(base)==0x74726976 &&
        *reinterpret_cast<volatile uint32_t *>(base+4)==2 &&
        *reinterpret_cast<volatile uint32_t *>(base+8)==3;
    view->release();
    return matches?IOService::probe(provider,score):nullptr;
}
bool NativeVirtioConsole::reset() {
    online=false;if(ownsDevice)native_console_ready=0;
    if(!registers || !ownsDevice)return true;
    write32(0x70,0);uint64_t deadline;
    clock_interval_to_deadline(1000,kMillisecondScale,&deadline);
    while(read32(0x70)) {if(mach_absolute_time()>=deadline)return false;IODelay(1);}
    for(unsigned i=0;i<2;i++)queues[i].pending=false;
    return true;
}
bool NativeVirtioConsole::configureQueue(unsigned id) {
    Queue &q=queues[id];write32(0x30,id);
    if(read32(0x44) || read32(0x34)<queueSize)return false;
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    q.ring=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,options,4096,4096);
    q.buffer=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,options,bufferSize,4096);
    if(!q.ring || !q.buffer)return false;
    IOByteCount rn=0,bn=0;q.physical=q.ring->getPhysicalSegment(0,&rn);q.dataPhysical=q.buffer->getPhysicalSegment(0,&bn);
    if(!q.physical || !q.dataPhysical || rn<4096 || bn<bufferSize || (q.physical|q.dataPhysical)&4095)return false;
    bzero(q.memory(),4096);bzero(q.data(),bufferSize);
    *reinterpret_cast<volatile uint16_t *>(q.memory()+128)=1; // NO_INTERRUPT; bounded polling.
    dmaBarrier();write32(0x38,queueSize);address(0x80,q.physical);address(0x90,q.physical+128);address(0xa0,q.physical+256);write32(0x44,1);
    return true;
}
bool NativeVirtioConsole::configure() {
    if(!reset())return false;
    write32(0x70,1);write32(0x70,3);
    write32(0x14,1);if(!(read32(0x10)&1))return false;
    // Port zero only. No MULTIPORT, event-index, indirect descriptors or DMA remapping.
    write32(0x24,0);write32(0x20,0);write32(0x24,1);write32(0x20,1);
    write32(0x70,11);if((read32(0x70)&(8|64|128))!=8)return false;
    if(!configureQueue(0) || !configureQueue(1))return false;
    write32(0x70,15);online=(read32(0x70)&(15|64|128))==15;
    if(online)post(0,bufferSize);
    return online;
}
void NativeVirtioConsole::post(unsigned id,unsigned bytes) {
    Queue &q=queues[id];Descriptor *d=reinterpret_cast<Descriptor *>(q.memory());
    d[0]={q.dataPhysical,bytes,(uint16_t)(id==0?2:0),0};
    volatile uint16_t *available=reinterpret_cast<volatile uint16_t *>(q.memory()+128);
    available[2+(q.available%queueSize)]=0;
    dmaBarrier();available[1]=++q.available;dmaBarrier();q.pending=true;write32(0x50,id);
}
IOReturn NativeVirtioConsole::complete(unsigned id,unsigned *length) {
    Queue &q=queues[id];if(!q.pending)return kIOReturnNotReady;
    volatile uint16_t *used=reinterpret_cast<volatile uint16_t *>(q.memory()+256);
    uint64_t deadline;clock_interval_to_deadline(2000,kMillisecondScale,&deadline);
    while(used[1]==q.used) {
        if(read32(0x70)&(64|128)) {online=false;return kIOReturnIOError;}
        if(mach_absolute_time()>=deadline)return kIOReturnNotResponding;
        IODelay(50);
    }
    dmaBarrier();if((uint16_t)(used[1]-q.used)!=1) {online=false;return kIOReturnIOError;}
    volatile Used *entries=reinterpret_cast<volatile Used *>(q.memory()+260);
    uint32_t index=entries[q.used%queueSize].id,n=entries[q.used%queueSize].length;
    if(index || n>bufferSize) {online=false;return kIOReturnIOError;}
    ++q.used;q.pending=false;*length=n;
    uint32_t irq=read32(0x60);if(irq&3)write32(0x64,irq&3);
    return kIOReturnSuccess;
}
IOReturn NativeVirtioConsole::send(const void *data,size_t bytes) {
    if(!data || !bytes || bytes>bufferSize)return kIOReturnBadArgument;
    IOLockLock(gate);IOReturn status=kIOReturnNotReady;
    Queue &q=queues[1];
    if(online) {
        unsigned consumed=0;
        // A timed-out descriptor is still device-owned. Never overwrite it.
        if(q.pending)status=complete(1,&consumed);else status=kIOReturnSuccess;
        if(status==kIOReturnSuccess) {
            memcpy(q.data(),data,bytes);post(1,(unsigned)bytes);
            status=complete(1,&consumed);
            if(status==kIOReturnSuccess) {native_console_tx_requests=native_console_tx_requests+1;native_console_tx_bytes+=bytes;}
        }
    }
    if(status!=kIOReturnSuccess)native_console_failures=native_console_failures+1;
    IOLockUnlock(gate);return status;
}
IOReturn NativeVirtioConsole::receive(void *data,size_t *bytes) {
    if(!data || !bytes || !*bytes || *bytes>bufferSize)return kIOReturnBadArgument;
    size_t capacity=*bytes;*bytes=0;IOLockLock(gate);IOReturn status=kIOReturnNotReady;
    Queue &q=queues[0];
    if(online) {
        status=kIOReturnSuccess;
        if(q.pending) {
            unsigned n=0;status=complete(0,&n);
            if(status==kIOReturnSuccess) {q.bytes=n;q.offset=0;}
        }
        if(status==kIOReturnSuccess) {
            unsigned left=q.bytes-q.offset,n=left<capacity?left:(unsigned)capacity;
            memcpy(data,static_cast<unsigned char *>(q.data())+q.offset,n);q.offset+=n;*bytes=n;
            native_console_rx_requests=native_console_rx_requests+1;native_console_rx_bytes+=n;
            if(q.offset==q.bytes) {q.offset=q.bytes=0;post(0,bufferSize);}
        }
    }
    if(status!=kIOReturnSuccess)native_console_failures=native_console_failures+1;
    IOLockUnlock(gate);return status;
}
bool NativeVirtioConsole::start(IOService *provider) {
    uint64_t tcr,sctlr;__asm__ volatile("mrs %0,tcr_el1":"=r"(tcr));__asm__ volatile("mrs %0,sctlr_el1":"=r"(sctlr));
    if(((tcr>>14)&3)!=0 || ((tcr>>30)&3)!=2 || !(sctlr&1) || !IOService::start(provider))return false;
    gate=IOLockAlloc();mapping=provider->mapDeviceMemoryWithIndex(0);
    if(!gate || !mapping || mapping->getLength()<0x100)return false;
    registers=mapping->getVirtualAddress();
    if(read32(0)!=0x74726976 || read32(4)!=2 || read32(8)!=3)return false;
    ownsDevice=true;if(!configure())return false;
    native_console_mmio=mapping->getPhysicalAddress();native_console_ready=1;
    registerService();return true;
}
void NativeVirtioConsole::cleanup() {
    if(gate)IOLockLock(gate);
    bool stopped=reset();
    if(stopped) {
        for(unsigned i=0;i<2;i++) {OSSafeReleaseNULL(queues[i].ring);OSSafeReleaseNULL(queues[i].buffer);}
        OSSafeReleaseNULL(mapping);registers=0;ownsDevice=false;
    }
    // If reset fails, preserve device-reachable allocations instead of freeing
    // storage while the device may still DMA. This path requires recovery.
    if(gate)IOLockUnlock(gate);
}
void NativeVirtioConsole::stop(IOService *provider) {cleanup();IOService::stop(provider);}
void NativeVirtioConsole::free() {cleanup();if(gate){IOLockFree(gate);gate=nullptr;}IOService::free();}
bool NativeVirtioConsole::handleOpen(IOService *client,IOOptionBits,void *) {if(owner)return owner==client;owner=client;return true;}
void NativeVirtioConsole::handleClose(IOService *client,IOOptionBits) {if(owner==client)owner=nullptr;}
bool NativeVirtioConsole::handleIsOpen(const IOService *client) const {return client?owner==client:owner!=nullptr;}
IOReturn NativeVirtioConsole::newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **out) {
    if(!out || type || !online)return kIOReturnBadArgument;*out=nullptr;
    NativeVirtioConsoleClient *client=new NativeVirtioConsoleClient;if(!client)return kIOReturnNoMemory;
    bool attached=false;
    if(!client->initWithTask(task,security,type,properties) || !(attached=client->attach(this)) || !client->start(this)) {
        if(attached)client->detach(this);client->release();return kIOReturnNotOpen;
    }
    *out=client;return kIOReturnSuccess;
}
bool NativeVirtioConsoleClient::initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties) {
    if(type || clientHasPrivilege(security,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return false;
    return IOUserClient::initWithTask(task,security,type,properties);
}
bool NativeVirtioConsoleClient::start(IOService *provider) {
    controller=OSDynamicCast(NativeVirtioConsole,provider);
    if(!controller || !IOUserClient::start(provider))return false;
    opened=controller->open(this);return opened;
}
void NativeVirtioConsoleClient::stop(IOService *provider) {
    if(opened){controller->close(this);opened=false;}controller=nullptr;IOUserClient::stop(provider);
}
IOReturn NativeVirtioConsoleClient::clientClose() {terminate();return kIOReturnSuccess;}
IOReturn NativeVirtioConsoleClient::sendAction(OSObject *target,void *,IOExternalMethodArguments *args) {
    NativeVirtioConsoleClient *client=OSDynamicCast(NativeVirtioConsoleClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(args->structureInputDescriptor)return kIOReturnUnsupported;
    return client->controller->send(args->structureInput,args->structureInputSize);
}
IOReturn NativeVirtioConsoleClient::receiveAction(OSObject *target,void *,IOExternalMethodArguments *args) {
    NativeVirtioConsoleClient *client=OSDynamicCast(NativeVirtioConsoleClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(args->structureOutputDescriptor)return kIOReturnUnsupported;
    size_t size=args->structureOutputSize;
    IOReturn status=client->controller->receive(args->structureOutput,&size);args->structureOutputSize=(uint32_t)size;return status;
}
IOReturn NativeVirtioConsoleClient::externalMethod(uint32_t selector,IOExternalMethodArguments *args,IOExternalMethodDispatch *,OSObject *,void *) {
    static const IOExternalMethodDispatch methods[]={
        {sendAction,0,kIOUCVariableStructureSize,0,0},
        {receiveAction,0,0,0,kIOUCVariableStructureSize}};
    if(selector>=2)return kIOReturnUnsupported;
    return IOUserClient::externalMethod(selector,args,const_cast<IOExternalMethodDispatch *>(methods+selector),this,nullptr);
}
