// SPDX-License-Identifier: GPL-2.0-only
// MiCode display-drivers sheng-u-oss: dsi_ctrl_async_timing_update,
// dsi_ctrl_hw_cmn_set_video_timing and sde_hw_intf_setup_timing_engine.
// marble_live.dts m16t_36_0d_0b: immediate VFP, 2400/12/4/16 at 120 Hz.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" {
extern volatile uint32_t native_legacy_direct[14];
bool native_gop_scanout_active(void);
IOReturn native_marble_panel_set_refresh(uint32_t);
volatile uint32_t marble_panel_state, marble_panel_error;
uint32_t marble_panel_registers[64];
void native_marble_panel_refresh_request(unsigned);
}
namespace {
constexpr uint32_t intf=0x36000, dsi=0x94000;
constexpr uint32_t offsets[]={0,4,8,0xc,0x14,0x1c,0x24,0x2c,0x34,0x60,0xa8,0xac,0xb0,0x170};
constexpr uint32_t dsiOffsets[]={0,4,8,0x10,0x24,0x28,0x2c,0x30,0x34,0x38,0x1e4,0x1e8};
void panelWorker(void *argument,wait_result_t) {
    const unsigned hz=unsigned(uintptr_t(argument));
    auto *memory=IODeviceMemory::withRange(0xae00000,0xa0000);
    auto *mapping=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    OSSafeReleaseNULL(memory);
    if(!mapping) {marble_panel_error=kIOReturnNoMemory;marble_panel_state=3;thread_terminate(current_thread());return;}
    auto *r=reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress());
    auto read=[&](uint32_t at){return r[at/4];};
    auto snapshot=[&](unsigned at){
        for(unsigned i=0;i<14;++i)marble_panel_registers[at+i]=read(intf+offsets[i]);
        for(unsigned i=0;i<12;++i)marble_panel_registers[at+14+i]=read(dsi+dsiOffsets[i]);
    };
    snapshot(0);
    IOReturn status=kIOReturnSuccess;
    if(hz) {
        const uint32_t oldTotal=(read(dsi+0x2c)>>16)+1;
        status=native_marble_panel_set_refresh(hz);
        marble_panel_registers[52]=oldTotal;
        marble_panel_registers[53]=(read(dsi+0x2c)>>16)+1;
    }
    snapshot(26);
    marble_panel_registers[54]=hz;
    marble_panel_error=status;
    mapping->release();
    __atomic_store_n(&marble_panel_state,status?3U:2U,__ATOMIC_RELEASE);
    thread_terminate(current_thread());
}
}
extern "C" void native_marble_panel_refresh_request(unsigned hz) {
    if(!native_legacy_direct[0] && !native_gop_scanout_active()) {marble_panel_error=kIOReturnNotReady;marble_panel_state=3;return;}
    unsigned previous=__atomic_load_n(&marble_panel_state,__ATOMIC_ACQUIRE);
    if(previous==1 || !__atomic_compare_exchange_n(&marble_panel_state,&previous,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    marble_panel_error=0;
    thread_t worker;
    const auto status=kernel_thread_start(panelWorker,reinterpret_cast<void *>(uintptr_t(hz)),&worker);
    if(!status)thread_deallocate(worker);
    else {marble_panel_error=status;marble_panel_state=3;}
}
