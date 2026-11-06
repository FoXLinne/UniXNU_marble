#!/usr/bin/env python3
"""Exercise resident watchdog handoff with a mocked hypervisor and counter."""
from pathlib import Path
import subprocess
import tempfile


repository = Path(__file__).resolve().parents[1]
source = (repository / 'package/MarblePkg/RootUsb.c').read_text()
helpers = source[source.index('STATIC VOID ServiceWatchdog'):source.index('typedef struct {')]
helpers = helpers.replace(
    '__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(mWatchdogFrequency));',
    'mWatchdogFrequency=100;')
helpers = helpers.replace(
    '__asm__ volatile("dc cvac, %0\\n\\tdsb sy" :: "r"((UINTN)Heartbeat) : "memory");',
    'published=TRUE;')
preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include "MarbleUsbHealth.h"
#define STATIC static
#define VOID void
#define TRUE 1
#define FALSE 0
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef int BOOLEAN;
static struct { UINT64 ReturnTest; } stage;
static typeof(stage) *mStage=&stage;
static BOOLEAN mKernelRunning, published;
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
  RetainUsbWatchdog(); assert(mWatchdogService && !pets);
  active=1; RetainUsbWatchdog(); assert(!active && mWatchdogService && !pets);
  active=1; disableError=1; RetainUsbWatchdog();
  assert(active && mWatchdogService && pets==1);
  counter=99; ServiceWatchdog(); assert(pets==1);
  counter=100; ServiceWatchdog(); assert(pets==2);
  counter=200; ServiceWatchdog(); assert(pets==3);
  counter=UINT64_MAX-49; RetainUsbWatchdog(); assert(pets==4);
  counter=50; ServiceWatchdog(); assert(pets==5);
  statusError=1; RetainUsbWatchdog(); assert(mWatchdogService);
  counter+=100; ServiceWatchdog(); assert(pets==6);
  mKernelRunning=1; stage.ReturnTest=UINT64_C(1)<<32;
  counter+=100; published=0; ServiceWatchdog();
  assert(published && (stage.ReturnTest>>32)==UINT32_C(0x80000003));
  *(UINT32 *)&stage.ReturnTest=2;
  counter+=100; ServiceWatchdog();
  assert((UINT32)stage.ReturnTest==2 && (stage.ReturnTest>>32)==UINT32_C(0x80000005));
  stage.ReturnTest=UINT64_C(0xffffffff)<<32;
  counter+=100; ServiceWatchdog();
  assert((stage.ReturnTest>>32)==MARBLE_USB_HEARTBEAT_TAG);
  struct MarbleUsbHealth health={0};
  assert(!marble_usb_health_tick(&health,1,0));
  assert(!marble_usb_health_tick(&health,1,500));
  assert(!marble_usb_health_tick(&health,MARBLE_USB_HEARTBEAT_TAG,500));
  assert(!marble_usb_health_tick(&health,MARBLE_USB_HEARTBEAT_TAG,529));
  assert(marble_usb_health_tick(&health,MARBLE_USB_HEARTBEAT_TAG,530));
  assert(!marble_usb_health_tick(&health,MARBLE_USB_HEARTBEAT_TAG+2,531));
  for (UINT64 second=532;second<800;++second)
    assert(!marble_usb_health_tick(&health,MARBLE_USB_HEARTBEAT_TAG+(UINT32)second*2,second));
  assert(!marble_usb_health_tick(&health,0,900) && !health.armed);
  assert(!marble_usb_health_tick(&health,UINT32_MAX,900));
  assert(!marble_usb_health_tick(&health,MARBLE_USB_HEARTBEAT_TAG,901));
  assert(!marble_usb_health_tick(&health,MARBLE_USB_HEARTBEAT_TAG,5));
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='root-usb-watchdog-') as temporary:
    root = Path(temporary)
    program = root / 'watchdog.c'
    program.write_text(preamble + helpers + cases)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(repository / 'package'), str(program),
                    '-o', str(root / 'watchdog')], check=True)
    subprocess.run([str(root / 'watchdog')], check=True)
print('RootStage watchdog: idle service, hypervisor failures, heartbeat publication, request preservation, stall deadline, counter wrap and clock restart passed')
