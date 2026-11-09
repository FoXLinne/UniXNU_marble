#!/usr/bin/env python3
"""Exercise disconnect, stale completion, timeout and return during USB waits."""
from pathlib import Path
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
source = (device / 'package/MarblePkg/RootUsb.c').read_text()
state = source[source.index('STATIC VOID ResetUsbSession('):source.index('STATIC VOID UsbIdle(')]
wait = source[source.index('STATIC INTN WaitTransfer('):source.index('STATIC BOOLEAN Reply(')]
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "Protocol/RootStage.h"
#define STATIC static
#define VOID void
#define BOOLEAN int
#define TRUE 1
#define FALSE 0
#define UINT8 uint8_t
#define UINT32 uint32_t
#define UINT64 uint64_t
#define UINTN size_t
#define INTN long
#define EFI_STATUS long
#define EFI_ERROR(x) ((x)<0)
#define EFI_NOT_READY -2
#define ZeroMem(p,n) memset(p,0,n)
#define EFIAPI
#define EP_OUT 1
#define EP_IN 0x81
#include "MarblePkg/RootUsb.h"
static BOOLEAN mConnected,mReceivePending,mReceiveReady,mUsbRecovery,mUsbDraining;
static INTN mReceiveBytes;
static UINTN mExportBytes;
static UINT64 mUsbSession,mUsbErrors,mUsbRetryAt,mUsbFrequency=10;
static UINT64 mUsbBusyTicks,mUsbPolls,mUsbTimeouts,counter;
static UINT64 mUsbConnects,mUsbDisconnects;
static int update,failSend,restarts,indexEvent,countEvent;
static char buffer[64];
struct sample {USB_DEVICE_EVENT event; USB_DEVICE_EVENT_DATA data; EFI_STATUS status;} samples[8];
static UINT64 RootCounter(void) {return counter;}
static void ServiceWatchdog(void) {}
static BOOLEAN ServiceUpdatePoint(void) {if (!update) return FALSE; update=0; return TRUE;}
static void UsbIdle(BOOLEAN command) {(void)command; ++counter;}
static EFI_STATUS RestartUsb(void) {++restarts;mUsbRecovery=FALSE;return 0;}
static EFI_STATUS event(USB_DEVICE_EVENT *kind,UINTN *size,USB_DEVICE_EVENT_DATA *data) {
 assert(*size==sizeof(*data)); ++counter;
 if (indexEvent==countEvent) {*kind=UsbDeviceEventNoEvent;return EFI_NOT_READY;}
 struct sample *sample=&samples[indexEvent++]; *kind=sample->event;*data=sample->data;return sample->status;
}
static EFI_STATUS send(UINT8 endpoint,UINTN size,VOID *data) {
 (void)endpoint;(void)size;(void)data;return failSend ? -1 : 0;
}
static EFI_STATUS abortTransfer(UINT8 endpoint) {(void)endpoint;return 0;}
static ROOT_USB_PROTOCOL usb={.Send=send,.HandleEvent=event,.AbortXfer=abortTransfer};
static ROOT_USB_PROTOCOL *mUsb=&usb;
'''
cases = r'''
static void fresh(void) {
 mConnected=TRUE;mUsbRecovery=FALSE;mUsbDraining=FALSE;mReceivePending=TRUE;mReceiveReady=TRUE;
 mReceiveBytes=64;mExportBytes=4096;counter=0;update=0;indexEvent=0;countEvent=1;
 memset(samples,0,sizeof(samples));failSend=0;
}
static void completion(unsigned size,void *data) {
 samples[0].event=UsbDeviceEventTransferNotification;
 samples[0].data.TransferOutcome.Status=UsbDeviceTransferStatusCompleteOK;
 samples[0].data.TransferOutcome.EndpointIndex=EP_IN;
 samples[0].data.TransferOutcome.BytesCompleted=size;
 samples[0].data.TransferOutcome.DataBuffer=data;
}
int main(void) {
 _Static_assert(sizeof(ROOT_STAGE_USB_HOST)==64,"host cache line");
 _Static_assert(sizeof(ROOT_STAGE_USB_KERNEL)==64,"kernel cache line");
 _Static_assert(offsetof(ROOT_STAGE_TABLE,UsbHost)%64==0,"host alignment");
 _Static_assert(offsetof(ROOT_STAGE_TABLE,UsbKernel)%64==0,"kernel alignment");
 fresh(); samples[0].event=UsbDeviceEventDeviceStateChange;samples[0].data.DeviceState=1;
 UINT64 session=mUsbSession;
 assert(WaitTransfer(EP_OUT,buffer,64,TRUE)==-1);
 assert(!mConnected&&!mReceivePending&&!mReceiveReady&&!mReceiveBytes&&!mExportBytes);
 assert(mUsbSession==session+1&&!mUsbRecovery);
 fresh(); UsbState(1);UsbState(0);completion(8,buffer);
 assert(WaitTransfer(EP_IN,buffer,8,FALSE)==8&&mConnected);
 fresh(); completion(9,buffer);assert(WaitTransfer(EP_IN,buffer,8,FALSE)==-1&&mUsbRecovery);
 fresh(); completion(8,buffer+1);assert(WaitTransfer(EP_IN,buffer,8,FALSE)==-1&&mUsbRecovery);
 fresh(); samples[0].status=-1;assert(WaitTransfer(EP_OUT,buffer,64,TRUE)==-1&&mUsbRecovery);
 fresh(); countEvent=0;UINT64 timeouts=mUsbTimeouts;
 assert(WaitTransfer(EP_IN,buffer,8,FALSE)==-1&&mUsbRecovery&&mUsbTimeouts==timeouts+1);
 fresh(); update=1;assert(WaitTransfer(EP_OUT,buffer,64,TRUE)==-1&&!mUsbRecovery);
 fresh(); failSend=1;assert(Transfer(EP_IN,buffer,8)==-1&&mUsbRecovery);
 fresh(); UsbFailure();counter=mUsbRetryAt;int before=restarts;
 assert(WaitTransfer(0xff,NULL,0,TRUE)==-1&&!mUsbRecovery&&restarts==before+1);
 fresh();mConnected=FALSE;before=indexEvent;assert(Transfer(EP_IN,buffer,8)==-1&&before==indexEvent);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='root-usb-reconnect-') as temporary:
    path = Path(temporary)
    (path / 'test.c').write_text(program + state + wait + cases)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(device / 'package'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('RootStage USB: disconnect cleanup, completion bounds, buffer ownership, timeout, return and recovery passed')
