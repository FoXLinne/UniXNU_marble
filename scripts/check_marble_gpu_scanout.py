#!/usr/bin/env python3
"""Check marble GPU IRQ dispatch and render pixels through the buffered scanout client."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

DEVICE = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, default=DEVICE.parent.parent / 'outputs/UniXNU_marble/ios18_arm64e')
args = parser.parse_args()
sys.path.insert(0, str(DEVICE / 'scripts'))
from pack_ios18_ufs import merge_kernel_args

assert merge_kernel_args('marble-display-flip=1 cpus=7', ['marble-display-flip=0']) == 'cpus=7 marble-display-flip=0'
assert merge_kernel_args('a=1 a=2 b=3', ['a=0', 'a=4']) == 'b=3 a=4'
for invalid in ('', 'a=1 b=2', '\0', '=1'):
    try:
        merge_kernel_args('a=1', [invalid])
    except ValueError:
        continue
    raise AssertionError(repr(invalid))
print('PASS: boot argument replacement and rejected malformed overrides', flush=True)
driver = args.root.resolve() / 'gpu/AdrenoMetal'
source = (DEVICE / 'package/MarbleAdreno.cpp').read_text()
start = source.index('    static void finishPreemption(')
end = source.index('    static int gmuWrite(', start)
handlers = source[start:end]
fixture = r'''
#include <cassert>
#include <cstdint>
#include "MarbleAdrenoRegisters.h"
using thread_call_param_t = void *;
static bool inInterrupt;
static unsigned queued,finished,wakes;
volatile uint32_t marble_gpu_irq[8];
static bool native_adreno_preempt_irq() {assert(!inInterrupt);++finished;return true;}
static void thread_call_enter(void *) {assert(inInterrupt);++queued;}
using event_t = void *;
static void thread_wakeup(event_t) {++wakes;}
struct CommandProcessor {
    uint32_t status=0,cleared=0,interrupts=0;
    void *preemptCall=this;
    uint32_t read(uint32_t reg) {assert(reg==GEN7_RBBM_INT_0_STATUS);return status;}
    void write(uint32_t reg,uint32_t value) {assert(reg==GEN7_RBBM_INT_CLEAR_CMD);cleared=value;}
''' + handlers + r'''
};
int main() {
    static_assert(MarbleGpuInterruptMask==((1U<<8)|(1U<<20)));
    CommandProcessor cp;
    inInterrupt=true;cp.status=MarbleGpuFenceInterrupt;cp.interrupt(&cp);
    assert(cp.cleared==cp.status && wakes==1 && queued==0 && finished==0);
    assert(marble_gpu_irq[3]==1 && marble_gpu_irq[4]==0);
    cp.status=MarbleGpuFenceInterrupt|MarbleGpuPreemptInterrupt;cp.interrupt(&cp);
    assert(wakes==2 && queued==1 && finished==0);
    assert(marble_gpu_irq[3]==2 && marble_gpu_irq[4]==1);
    inInterrupt=false;cp.finishPreemption(nullptr,nullptr);
    assert(finished==1 && marble_gpu_irq[7]==1);
    inInterrupt=true;cp.status=0;cp.interrupt(&cp);
    assert(wakes==3 && queued==1 && cp.interrupts==3);
}
'''
with tempfile.TemporaryDirectory(prefix='marble-gpu-scanout-') as tmp:
    tmp = Path(tmp)
    test = tmp / 'irq.cpp'
    test.write_text(fixture)
    flags = ['-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined']
    subprocess.run(['xcrun', 'clang++', *flags, '-I', DEVICE / 'package', test, '-o', tmp / 'irq'], check=True)
    subprocess.run([tmp / 'irq'], check=True)
    print('PASS: Gen7 fence IRQ bit 20 and deferred preemption completion', flush=True)
    subprocess.run(['xcrun', 'clang++', *flags, '-fobjc-arc', '-Wno-unused-parameter',
                    '-Wno-deprecated-declarations', '-I', DEVICE / 'package', '-I', driver,
                    DEVICE / 'package/tests/MarbleScanoutHost.mm',
                    '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
                    '-o', tmp / 'scanout'], check=True)
    subprocess.run([tmp / 'scanout'], check=True)
