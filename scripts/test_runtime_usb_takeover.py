#!/usr/bin/env python3
"""Exercise DWC3 takeover against pending queues and delayed clock synchronization."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'package/RuntimeUsb.cpp').read_text()
start = source.index('    // Reset the inherited device queues')
end = source.index('    write(DWC3_GUSB2PHYCFG', start)
body = source[start:end]
harness = r'''
#include <cassert>
#include <cstdint>
#include "RuntimeUsbRegisters.h"
static uint32_t ctl, status, elapsed, resetDelay;
static bool resetSeen, stuck, missingHalt;
static uint32_t read(unsigned offset) {
    if (offset==DWC3_DCTL) return ctl;
    if (offset==DWC3_DSTS) return status;
    return 0;
}
static void write(unsigned offset, uint32_t value) {
    if (offset==DWC3_DCTL) {
        ctl=value;
        if (value&DWC3_DCTL_CSFTRST) {
            assert(!(value&DWC3_DCTL_RUN_STOP));
            resetSeen=true;
        }
    }
}
static void IOSleep(unsigned ms) {
    elapsed+=ms;
    if (resetSeen && !stuck && elapsed>=resetDelay) {
        ctl&=~DWC3_DCTL_CSFTRST;
        if (!missingHalt) status|=DWC3_DSTS_DEVCTRLHLT;
    }
}
#define IOLog(...) ((void)0)
static bool takeover() {
    unsigned id=DWC3_GSNPSID_USB31;
'''
cases = r'''
    return true;
}
static void setup() {
    ctl=DWC3_DCTL_RUN_STOP; status=0; elapsed=0; resetDelay=60;
    resetSeen=stuck=missingHalt=false;
}
int main() {
    setup(); assert(takeover() && resetSeen && elapsed>=410);
    setup(); stuck=true; assert(!takeover() && elapsed==200);
    setup(); missingHalt=true; assert(!takeover() && elapsed<400);
}
'''
with tempfile.TemporaryDirectory(prefix='runtime-usb-takeover-') as temporary:
    directory = Path(temporary)
    program = directory / 'takeover.cpp'
    program.write_text(harness + body + cases)
    binary = directory / 'takeover'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'package'),
                    str(program), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('RuntimeUSB takeover: pending firmware queues, 60 ms synchronization, reset timeout, halt timeout passed')
