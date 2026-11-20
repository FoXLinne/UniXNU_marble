#!/usr/bin/env python3
"""Exercise resident watchdog handoff with a mocked hypervisor and counter."""
from pathlib import Path
import subprocess
import tempfile


repository = Path(__file__).resolve().parents[1]
source = (repository / 'package/MarblePkg/RootUsb.c').read_text()
helpers = source[source.index('STATIC VOID ServiceWatchdog'):source.index('typedef struct {')]
helpers = helpers.replace('#include \"RootBootloaderReset.h\"', '')
helpers = helpers.replace(
    '__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(mWatchdogFrequency));',
    'mWatchdogFrequency=100;')
helpers = helpers.replace(
    '__asm__ volatile("dc cvac, %0\\n\\tdsb sy" :: "r"((UINTN)&mStage->UsbHost) : "memory");',
    'published=TRUE;')
doorbell = source[source.index('STATIC VOID PublishControl('):source.index('STATIC BOOLEAN ServiceUpdatePoint(')]
doorbell = doorbell.replace(
    '__asm__ volatile("dc cvac, %0\\n\\tdsb sy" :: "r"((UINTN)&mStage->UsbHost) : "memory");',
    'published=TRUE;').replace(
    '__asm__ volatile("msr icc_sgi1r_el1, %0\\n\\tisb" :: "r"(Sgi) : "memory");',
    'ring(Sgi);').replace(
    '__asm__ volatile("msr icc_sgi1r_el1, %0\\n\\tisb" :: "r"(mBootCpuSgiTarget) : "memory");',
    'ring(mBootCpuSgiTarget);')
link = source[source.index('STATIC VOID ResetUsbSession('):source.index('STATIC EFI_STATUS UsbEvent(')]
preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include "MarbleUsbHealth.h"
#include "Protocol/RootStage.h"
#define STATIC static
#define VOID void
#define TRUE 1
#define FALSE 0
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef int BOOLEAN;
static ROOT_STAGE_TABLE stage;
static typeof(stage) *mStage=&stage;
static BOOLEAN mKernelRunning, published;
static BOOLEAN mServiceReady, mUsbDoorbellOnline;
static UINT64 mBootCpuSgiTarget=1, lastSgi;
static unsigned interrupts;
static void ring(UINT64 target) {
  assert(published && stage.UsbHost.Command);
  lastSgi=target;
  ++interrupts;
}
static UINT64 mWatchdogControl, mWatchdogStatus, mWatchdogPetStatus;
static UINT64 mWatchdogLastPet, mWatchdogFrequency, counter;
static BOOLEAN mWatchdogService, mRecoveryArmed, mServiceStarted;
static UINT64 mUsbLastCommand;
static UINT64 mUsbSession, mUsbConnects, mUsbDisconnects, mUsbErrors;
static UINT64 mUsbFrequency=100, mUsbRetryAt, mUsbDetachedAt;
static BOOLEAN mConnected, mReceivePending, mReceiveReady, mUsbDraining;
static BOOLEAN mUsbRecovery, mUsbReattachPending;
static int mReceiveBytes;
static size_t mExportBytes;
enum {EP_OUT=1, EP_IN=0x81};
static unsigned aborted;
static void abortTransfer(unsigned endpoint) {
  assert(endpoint==EP_OUT || endpoint==EP_IN);
  ++aborted;
}
static struct {void (*AbortXfer)(unsigned);} usb={abortTransfer}, *mUsb=&usb;
static int active, statusError, disableError, pets;
static UINT64 RootCounter(void) { return counter; }
void MarbleDisplayService(void) {}
void MarbleRtcService(void) {}
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
  active=1; disableError=0; mRecoveryArmed=1;
  RetainUsbWatchdog(); assert(active && pets==2);
  mRecoveryArmed=0; pets=1; disableError=1;
  counter=99; ServiceWatchdog(); assert(pets==1);
  counter=100; ServiceWatchdog(); assert(pets==2);
  counter=200; ServiceWatchdog(); assert(pets==3);
  counter=UINT64_MAX-49; RetainUsbWatchdog(); assert(pets==4);
  counter=50; ServiceWatchdog(); assert(pets==5);
  statusError=1; RetainUsbWatchdog(); assert(mWatchdogService);
  counter+=100; ServiceWatchdog(); assert(pets==6);
  mKernelRunning=1; stage.UsbHost.Heartbeat=1;
  counter+=100; published=0; ServiceWatchdog();
  assert(published && stage.UsbHost.Heartbeat==UINT32_C(0x80000003));
  stage.UsbHost.Sequence=7; stage.UsbHost.Command=2; stage.UsbKernel.Completed=6;
  counter+=100; ServiceWatchdog();
  assert(stage.UsbHost.Command==2 && stage.UsbHost.Sequence==7 && stage.UsbKernel.Completed==6 && stage.UsbHost.Heartbeat==UINT32_C(0x80000005));
  stage.UsbHost.Heartbeat=UINT32_MAX;
  counter+=100; ServiceWatchdog();
  assert(stage.UsbHost.Heartbeat==MARBLE_USB_HEARTBEAT_TAG);
  mRecoveryArmed=1; mServiceStarted=1; mKernelRunning=1;
  mUsbLastCommand=counter; counter+=100; ServiceWatchdog();
  int livePets=pets;
  counter+=1100; ServiceWatchdog(); assert(pets==livePets+1);
  counter+=6000; ServiceWatchdog(); assert(pets==livePets+2);
  mUsbLastCommand=counter; counter+=100; ServiceWatchdog(); assert(pets==livePets+3);
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

  const UINT32 touch=0x82000001U, power=0x85002001U;
  stage.UsbHost.Sequence=0; stage.UsbKernel.Ready=0; published=0;
  PublishControl(touch);
  assert(published && !interrupts && stage.UsbHost.Sequence==1);
  mServiceReady=1; mUsbDoorbellOnline=1; stage.UsbKernel.Ready=1;
  PublishControl(touch); assert(!interrupts);
  PublishControl(power); assert(interrupts==1 && lastSgi==1);
  stage.UsbKernel.Reserved[MARBLE_USB_KERNEL_DOORBELL]=MARBLE_USB_DOORBELL_TAG;
  published=0; PublishControl(touch);
  assert(interrupts==2 && lastSgi==(1ULL | ((UINT64)MARBLE_USB_DOORBELL_SGI<<24)));
  mUsbDoorbellOnline=0; PublishControl(power);
  assert(interrupts==2 && stage.UsbHost.Sequence==5);

  mServiceStarted=1;
  counter=1000; UsbState(1); assert(!mUsbReattachPending);
  UsbState(0); mReceivePending=mReceiveReady=1; mReceiveBytes=64; mExportBytes=64;
  counter=2000; UsbState(1);
  assert(mUsbReattachPending && !mConnected && !mReceivePending && !mReceiveReady);
  assert(mUsbDraining && !mReceiveBytes && !mExportBytes && aborted==4);
  counter=2150; UsbState(1); assert(mUsbDetachedAt==2000 && !RetryDetachedUsb());
  counter=2199; assert(!RetryDetachedUsb());
  counter=2200; assert(RetryDetachedUsb() && mUsbRecovery && mUsbRetryAt==counter);
  counter=9000; assert(!RetryDetachedUsb());
  mUsbRecovery=0; UsbState(0); UsbState(1);
  counter=9199; UsbState(0);
  counter=10000; assert(!RetryDetachedUsb() && !mUsbRecovery && mConnected);
  counter=UINT64_MAX-99; UsbState(1);
  counter=100; assert(RetryDetachedUsb() && mUsbRetryAt==100);
  mUsbRecovery=0; UsbState(0); UsbState(1); UsbFailure();
  counter=1000; assert(!RetryDetachedUsb() && mUsbRecovery && mUsbErrors==1);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='root-usb-watchdog-') as temporary:
    root = Path(temporary)
    program = root / 'watchdog.c'
    program.write_text(preamble + helpers + doorbell + link + cases)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(repository / 'package'), str(program),
                    '-o', str(root / 'watchdog')], check=True)
    subprocess.run([str(root / 'watchdog')], check=True)
print('RootStage watchdog: idle service, hypervisor failures, heartbeat publication, request preservation, stall deadline, counter wrap, clock restart, negotiated command interrupt and detach recovery passed')
