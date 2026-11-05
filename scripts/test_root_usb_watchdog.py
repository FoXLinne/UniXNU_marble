#!/usr/bin/env python3
"""Exercise resident watchdog handoff with a mocked hypervisor and counter."""
from pathlib import Path
import subprocess
import tempfile


source = (Path(__file__).resolve().parents[1] / 'package/MarblePkg/RootUsb.c').read_text()
helpers = source[source.index('STATIC VOID ServiceWatchdog'):source.index('typedef struct {')]
helpers = helpers.replace(
    '__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(mWatchdogFrequency));',
    'mWatchdogFrequency=100;')
preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define STATIC static
#define VOID void
#define TRUE 1
#define FALSE 0
typedef uint64_t UINT64;
typedef int BOOLEAN;
static UINT64 mWatchdogControl, mWatchdogStatus, mWatchdogPetStatus;
static UINT64 mWatchdogLastPet, mWatchdogFrequency, counter;
static BOOLEAN mWatchdogService;
static int active, statusError, disableError, pets;
static UINT64 RootCounter(void) { return counter; }
static UINT64 MarbleWatchdogCall(unsigned call, unsigned a, unsigned b, UINT64 *state) {
  assert(b==0);
  if (call==0x86000006) { *state=active; return statusError; }
  if (call==0x86000005) { assert(a==2); if (!disableError) active=0; return disableError; }
  assert(call==0x86000007); ++pets; return 0;
}
'''
cases = r'''
int main(void) {
  RetainUsbWatchdog(); assert(!mWatchdogService && !pets);
  active=1; RetainUsbWatchdog(); assert(!active && !mWatchdogService && !pets);
  active=1; disableError=1; RetainUsbWatchdog();
  assert(active && mWatchdogService && pets==1);
  counter=99; ServiceWatchdog(); assert(pets==1);
  counter=100; ServiceWatchdog(); assert(pets==2);
  counter=200; ServiceWatchdog(); assert(pets==3);
  counter=UINT64_MAX-49; RetainUsbWatchdog(); assert(pets==4);
  counter=50; ServiceWatchdog(); assert(pets==5);
  statusError=1; RetainUsbWatchdog(); assert(!mWatchdogService);
  ServiceWatchdog(); assert(pets==5);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='root-usb-watchdog-') as temporary:
    root = Path(temporary)
    program = root / 'watchdog.c'
    program.write_text(preamble + helpers + cases)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(program),
                    '-o', str(root / 'watchdog')], check=True)
    subprocess.run([str(root / 'watchdog')], check=True)
print('RootStage watchdog: disabled, active, disable failure, idle service, counter wrap, status failure passed')
