#!/usr/bin/env python3
"""Exercise the kernel scanout transaction without a firmware service loop."""
from pathlib import Path
import re
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
source = (device / 'package/MarbleScanout.cpp').read_text()
flip = source[source.index('IOReturn scanoutFlip('):source.index('IOReturn flip(')]
flip = re.sub(r'__asm__ volatile\(.*?\);', '(void)0;', flip, flags=re.S)
program = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "MarbleScanout.h"
#include "MarbleScanoutWire.h"
using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnBadArgument=-1,kIOReturnBusy=-2,kIOReturnTimeout=-3};
static uint32_t registers[0x30000/4],marble_scanout_registers[8][8];
static volatile uint32_t *scanoutMdp=registers;
static MARBLE_SCANOUT state;
static volatile MARBLE_SCANOUT *mailbox=&state;
static MarbleScanoutLayout layout;
static uint64_t ticks;
static int sleeps,latchedAfter=2;
static uint64_t mach_absolute_time() {return ticks;}
static void absolutetime_to_nanoseconds(uint64_t value,uint64_t *out) {*out=value;}
static void IOSleep(unsigned ms) {
 ticks+=ms*1000000ULL;
 if(latchedAfter>0&&++sleeps==latchedAfter)registers[0x16018/4]=0;
}
'''
cases = r'''
static void fresh() {
 memset(registers,0,sizeof(registers));memset(&state,0,sizeof(state));
 state.base=MarbleSplashBase;state.pipe_mask=(1<<0)|(1<<3);state.flush_mask=(1<<0)|(1<<18);
 assert(marbleScanoutLayout(state.base,1080,2400,4320,&layout));
 marble_scanout_registers[0][2]=uint32_t(state.base+32);
 marble_scanout_registers[3][2]=uint32_t(state.base+128);
 registers[0x7014/4]=0xaabbccdd;ticks=0;sleeps=0;latchedAfter=2;
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
}
'''
with tempfile.TemporaryDirectory(prefix='scanout-kernel-mmio-') as temporary:
    path = Path(temporary)
    (path / 'test.cpp').write_text(program + flip + cases)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(device / 'package'),
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Kernel scanout: active pipes, slot address, flush acknowledgement, busy, timeout and boot restoration passed')
