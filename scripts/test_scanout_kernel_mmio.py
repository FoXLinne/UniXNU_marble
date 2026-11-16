#!/usr/bin/env python3
"""Exercise the kernel scanout transaction without a firmware service loop."""
from pathlib import Path
import re
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
source = (device / 'package/MarbleScanout.cpp').read_text()
def function(name):
    start=source.index('IOReturn '+name+'(')
    opening=source.index('{',start)
    depth=1;end=opening+1
    while depth:
        if source[end]=='{':depth+=1
        elif source[end]=='}':depth-=1
        end+=1
    return source[start:end]
flip = '\n'.join(function(name) for name in ('scanoutCommit','scanoutSettle','scanoutFlip','flip'))
flip = re.sub(r'__asm__ volatile\(.*?\);', '(void)0;', flip, flags=re.S)
program = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "MarbleScanout.h"
#include "MarbleScanoutWire.h"
using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnBusy=-2,kIOReturnTimeout=-3,kIOReturnIOError=-4};
volatile uint64_t marble_scanout_wait_stats[6];
static uint32_t registers[0x30000/4],marble_scanout_registers[8][8];
static volatile uint32_t *scanoutMdp=registers;
static MARBLE_SCANOUT state;
static volatile MARBLE_SCANOUT *mailbox=&state;
static MarbleScanoutLayout layout;
static uint64_t ticks;
static int sleeps,latchedAfter=2;
static uint64_t schedulingDelay;
static int mailboxLatchedAfter,mailboxError;
static void refresh() {}
static uint64_t mach_absolute_time() {return ticks;}
static void absolutetime_to_nanoseconds(uint64_t value,uint64_t *out) {*out=value;}
static void IOSleep(unsigned ms) {
 ticks+=ms*1000000ULL+schedulingDelay;
 ++sleeps;
 if(latchedAfter>0&&sleeps==latchedAfter)registers[0x16018/4]=0;
 if(mailboxLatchedAfter>0&&sleeps==mailboxLatchedAfter) {state.completed=state.requested;state.error=mailboxError;}
}
'''
cases = r'''
static void fresh() {
 memset(registers,0,sizeof(registers));memset(&state,0,sizeof(state));
 state.base=MarbleSplashBase;state.pipe_mask=(1<<0)|(1<<3);state.flush_mask=(1<<0)|(1<<18);
 assert(marbleScanoutLayout(state.base,1080,2400,4320,&layout));
 marble_scanout_registers[0][2]=uint32_t(state.base+32);
 marble_scanout_registers[3][2]=uint32_t(state.base+128);
 registers[0x7014/4]=0xaabbccdd;ticks=0;sleeps=0;latchedAfter=2;schedulingDelay=0;
 mailboxLatchedAfter=mailboxError=0;
 for(auto &value:marble_scanout_wait_stats)value=0;
}
int main() {
 fresh();assert(scanoutFlip(1)==kIOReturnSuccess);
 assert(registers[0x5014/4]==state.base+2*layout.slotBytes+32);
 assert(registers[0xb014/4]==state.base+2*layout.slotBytes+128);
 assert(registers[0x7014/4]==0xaabbccdd&&!registers[0x16018/4]);
 fresh();assert(scanoutFlip(0xffffffffU)==kIOReturnSuccess);
 assert(registers[0x5014/4]==state.base+32);
 fresh();assert(scanoutFlip(3)==kIOReturnBadArgument&&!registers[0x5014/4]);
 fresh();marble_scanout_registers[3][2]=uint32_t(state.base+layout.slotBytes);
 assert(scanoutFlip(0)==kIOReturnBadArgument&&!registers[0x5014/4]);
 fresh();registers[0x16018/4]=state.flush_mask;
 assert(scanoutFlip(0)==kIOReturnBusy&&!registers[0x5014/4]);
 fresh();latchedAfter=0;assert(scanoutFlip(0)==kIOReturnTimeout);
 assert(registers[0x16018/4]==state.flush_mask&&ticks==50000000);
 fresh();state.pipe_mask=1U<<8;assert(scanoutFlip(0)==kIOReturnBadArgument);
 // A commit returns before the latch; the next commit is refused until it settles.
 fresh();assert(scanoutCommit(0)==kIOReturnSuccess&&registers[0x16018/4]==state.flush_mask&&!sleeps);
 assert(registers[0x5014/4]==state.base+layout.slotBytes+32);
 assert(scanoutCommit(1)==kIOReturnBusy&&registers[0x5014/4]==state.base+layout.slotBytes+32);
 assert(scanoutSettle()==kIOReturnSuccess&&!registers[0x16018/4]);
 assert(scanoutCommit(1)==kIOReturnSuccess&&registers[0x5014/4]==state.base+2*layout.slotBytes+32);
 // Hardware acknowledges during sleep; a delayed CPU wake must observe it.
 fresh();latchedAfter=1;schedulingDelay=75000000;
 assert(scanoutFlip(1)==kIOReturnSuccess&&!registers[0x16018/4]);
 assert(marble_scanout_wait_stats[1]==1&&!marble_scanout_wait_stats[2]);
 fresh();mailboxLatchedAfter=1;schedulingDelay=75000000;
 assert(flip(1,50000000)==kIOReturnSuccess&&state.requested==state.completed);
 fresh();assert(flip(0,5000000)==kIOReturnTimeout&&state.requested!=state.completed);
 fresh();state.requested=1;assert(flip(0)==kIOReturnBusy);
 fresh();mailboxLatchedAfter=1;mailboxError=1;assert(flip(0)==kIOReturnIOError);
}
'''
with tempfile.TemporaryDirectory(prefix='scanout-kernel-mmio-') as temporary:
    path = Path(temporary)
    (path / 'test.cpp').write_text(program + flip + cases)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(device / 'package'),
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Kernel scanout: pipelined commit and settle, address validation, real deadlines, delayed-wake acknowledgements, mailbox errors and boot restoration passed')
