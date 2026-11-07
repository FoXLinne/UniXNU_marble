#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the display's actual fence-wait and cancellation methods with a fake GPU."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'NativeLegacyDisplay.cpp'
text = source.read_text()
methods = []
for start, end in [('    bool gpuWrote(', '    static void fenceFired('),
                   ('    bool cancelSwapsAfterTimeout()', '\n#endif')]:
    at = text.index(start)
    methods.append(text[at:text.index(end, at)].replace(' override', ''))

harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
struct OriginalLegacySwapRequest {};
static uint64_t clockNow=1000000;
static uint64_t mach_absolute_time() { return clockNow; }
static void absolutetime_to_nanoseconds(uint64_t time,uint64_t *out) { *out=time; }
static uint32_t native_legacy_direct[14];
static uint64_t native_legacy_swap_timeouts;
static bool queryKnown;
static uint32_t querySubmitted,queryRetired,queryReason;
static bool detail(uint32_t *submitted,uint32_t *retired,uint32_t *extra) {
    extra[2]=queryReason;
    if(queryKnown){*submitted=querySubmitted;*retired=queryRetired;}
    return queryKnown;
}
static bool legacy(uint32_t *submitted,uint32_t *retired) {
    uint32_t extra[3]={};return detail(submitted,retired,extra);
}
static bool (*queryDetail)(uint32_t *,uint32_t *,uint32_t *)=detail;
static bool (*queryLegacy)(uint32_t *,uint32_t *)=legacy;
#define native_adreno_fences_detail (*queryDetail)
#define native_adreno_fences (*queryLegacy)
enum {GpuTraceDisplayFence=22};
static unsigned lastReason;
template<class... Args> static void gpuTraceEvent(unsigned,unsigned,uint64_t,unsigned reason,Args...) {lastReason=reason;}
struct Timer {
    unsigned delay=0,polls=0;
    void setTimeoutUS(unsigned us) {delay=us;++polls;}
    void cancelTimeout() {delay=0;}
};
struct Display {
    const OriginalLegacySwapRequest *fenceFor=nullptr,*gpuRequest=nullptr,*validated[8]={};
    uint32_t fenceTarget=0;
    uint64_t fenceSince=0;
    bool fenceCaptured=false,fenceSlow=false,traceGpuComplete=false,gpuRetired=false,transferred=false;
    Timer timer;
    Timer *fenceTimer=&timer;
    unsigned waits=0,passed=0;
    uint64_t traceId(const OriginalLegacySwapRequest *) {return 1;}
    void stat(unsigned) {}
    void note(unsigned event,int,const OriginalLegacySwapRequest *) {if(event==2)++waits;if(event==3)++passed;}
#include "methods.inc"
};
static void query(bool known,uint32_t submitted=0,uint32_t retired=0,uint32_t reason=2) {
    queryKnown=known;querySubmitted=submitted;queryRetired=retired;queryReason=reason;
}
int main() {
    OriginalLegacySwapRequest a,b;
    Display d;
    // Reproduce request 41: known pending target, then a busy query.
    query(true,89,88);
    assert(!d.gpuWrote(&a) && d.fenceTarget==89 && d.timer.delay==200);
    query(false,0,0,1);
    assert(!d.gpuWrote(&a) && !d.traceGpuComplete && lastReason==4);
    assert(d.fenceFor==&a && d.fenceTarget==89 && d.waits==1);
    // A later submission must not move this frame's target.
    query(true,99,88);assert(!d.gpuWrote(&a) && d.fenceTarget==89);
    query(true,99,89);assert(d.gpuWrote(&a) && d.traceGpuComplete && d.passed==1);

    // Initial busy polls have no valid target, including zero.
    query(false,0,0,1);assert(!d.gpuWrote(&b) && !d.fenceCaptured && !d.traceGpuComplete);
    query(true,100,99);assert(!d.gpuWrote(&b) && d.fenceTarget==100);
    query(false,0,0,3);assert(!d.gpuWrote(&b) && d.fenceTarget==100);
    query(true,101,100);assert(d.gpuWrote(&b));
    query(false,0,0,0);assert(!d.gpuWrote(&a)); // Provider not yet published.
    query(true,0,0);assert(d.gpuWrote(&a)); // A successful empty snapshot is complete.

    // Passing 250 ms records the slow wait once, without presenting the frame.
    query(true,120,119);assert(!d.gpuWrote(&a));
    const uint32_t slow=native_legacy_direct[12];
    clockNow+=250000000;
    assert(!d.gpuWrote(&a) && !d.traceGpuComplete && native_legacy_direct[12]==slow+1);
    query(false,0,0,1);clockNow+=250000000;
    assert(!d.gpuWrote(&a) && native_legacy_direct[12]==slow+1 && d.fenceTarget==120);
    query(true,130,120);assert(d.gpuWrote(&a) && d.traceGpuComplete);

    // A new head discards the previous head's target and elapsed time.
    query(true,140,139);assert(!d.gpuWrote(&a));
    query(false,0,0,1);assert(!d.gpuWrote(&b) && !d.fenceCaptured && !d.fenceTarget && !d.fenceSlow);
    query(true,150,149);assert(!d.gpuWrote(&b) && d.fenceTarget==150);
    d.validated[0]=&b;d.gpuRequest=&b;d.transferred=true;d.gpuRetired=true;
    assert(d.cancelSwapsAfterTimeout() && !d.timer.delay && !d.fenceFor && !d.fenceCaptured);
    assert(!d.validated[0] && !d.gpuRequest && !d.transferred && !d.gpuRetired && !d.traceGpuComplete);
    // The family may reuse the cancelled request address.
    query(true,160,159);assert(!d.gpuWrote(&b) && d.fenceTarget==160);
    query(true,161,160);assert(d.gpuWrote(&b));

    // Completion comparison retains the GPU's wrapping 32-bit fence semantics.
    query(true,UINT32_MAX,UINT32_MAX-1);assert(!d.gpuWrote(&a));
    query(true,0,0);assert(d.gpuWrote(&a));
    query(true,0,UINT32_MAX);assert(!d.gpuWrote(&b));
    query(true,1,0);assert(d.gpuWrote(&b));

    // Legacy providers also defer failed queries. CPU-only builds keep their path.
    queryDetail=nullptr;
    query(false,0,0,1);assert(!d.gpuWrote(&a));
    query(true,170,169);assert(!d.gpuWrote(&a));
    query(true,171,170);assert(d.gpuWrote(&a));
    queryLegacy=nullptr;assert(d.gpuWrote(&b) && !d.traceGpuComplete && lastReason==0);
    std::puts("PASS: actual display methods defer busy/unknown/slow fences, preserve targets and clear cancelled waits");
}
'''

with tempfile.TemporaryDirectory(prefix='gop-fence-wait-') as directory:
    root = Path(directory)
    (root / 'methods.inc').write_text('\n'.join(methods))
    (root / 'test.cpp').write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
