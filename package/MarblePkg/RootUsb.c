#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include "RootUsb.h"
#include <Protocol/RootStage.h>
#include <Protocol/Cpu.h>

#define TRANSFER_BYTES 0x100000
#define EP_OUT 0x01
#define EP_IN 0x81

STATIC ROOT_USB_PROTOCOL *mUsb;
STATIC EFI_CPU_ARCH_PROTOCOL *mCpu;
STATIC UINT8 *mRx, *mTx;
STATIC ROOT_STAGE_TABLE *mStage;
STATIC UINTN mDisk;
STATIC UINTN mLastBytes;
STATIC UINT64 mLastOffset;
STATIC BOOLEAN mConnected;

// Same fastboot USB descriptors as the working marble HackiOS loader.
STATIC UINT8 mDevice[] = {18,1,0x10,2,0,0,0,64,0xd1,0x18,0x0d,0xd0,0,1,1,2,3,1};
STATIC UINT8 mSsDevice[] = {18,1,0,3,0,0,0,9,0xd1,0x18,0x0d,0xd0,0,1,1,2,3,1};
STATIC UINT8 mConfig[] = {9,2,32,0,1,1,0,0x80,0x50,9,4,0,0,2,0xff,0x42,3,4,
  7,5,EP_IN,2,0,2,0,7,5,EP_OUT,2,0,2,0};
STATIC UINT8 mSsConfig[] = {9,2,44,0,1,1,0,0x80,0x10,9,4,0,0,2,0xff,0x42,3,4,
  7,5,EP_IN,2,0,4,0,6,0x30,4,0,0,0,7,5,EP_OUT,2,0,4,0,6,0x30,4,0,0,0};
STATIC UINT8 mQualifier[] = {10,6,0,2,0xff,0xff,0xff,64,1,0};
STATIC UINT8 mBos[] = {5,0x0f,42,0,3,7,0x10,2,6,0,0,0,10,0x10,3,0,0x0e,0,1,7,0x65,0,
  20,0x10,0x0a,0,1,0,0,0,0,0x11,0,0,0x30,0x40,0x0a,0,0xb0,0x40,0x0a,0};
STATIC UINT8 mLanguage[] = {4,3,9,4};
STATIC UINT8 mManufacturer[] = {14,3,'U',0,'n',0,'i',0,'X',0,'N',0,'U',0};
STATIC UINT8 mProduct[] = {20,3,'R',0,'o',0,'o',0,'t',0,'S',0,'t',0,'a',0,'g',0,'e',0};
STATIC UINT8 mSerial[] = {14,3,'m',0,'a',0,'r',0,'b',0,'l',0,'e',0};
STATIC UINT8 mInterface[] = {18,3,'f',0,'a',0,'s',0,'t',0,'b',0,'o',0,'o',0,'t',0};

STATIC VOID EFIAPI UsbNotify(EFI_EVENT Event, VOID *Context) {}

STATIC INTN WaitTransfer(UINT8 Endpoint)
{
  for (;;) {
    USB_DEVICE_EVENT Event = UsbDeviceEventNoEvent;
    USB_DEVICE_EVENT_DATA Data;
    UINTN Size = 0;
    EFI_STATUS Status = mUsb->HandleEvent(&Event, &Size, &Data);
    if (EFI_ERROR(Status) && Status != EFI_NOT_READY) return -1;
    if (Event == UsbDeviceEventDeviceStateChange) {
      mConnected = Data.DeviceState == 0;
      return -1;
    }
    if (Event == UsbDeviceEventTransferNotification && Data.TransferOutcome.EndpointIndex == Endpoint) {
      return Data.TransferOutcome.Status == UsbDeviceTransferStatusCompleteOK ? (INTN)Data.TransferOutcome.BytesCompleted : -1;
    }
    gBS->Stall(1000);
  }
}

STATIC INTN Transfer(UINT8 Endpoint, VOID *Buffer, UINTN Bytes)
{
  EFI_STATUS Status = mUsb->Send(Endpoint, Bytes, Buffer);
  if (EFI_ERROR(Status)) return -1;
  return WaitTransfer(Endpoint);
}

STATIC VOID Reply(CONST CHAR8 *Code, CONST CHAR8 *Text)
{
  UINTN Prefix = AsciiStrLen(Code), Length = AsciiStrLen(Text);
  CopyMem(mTx,Code,Prefix);
  CopyMem(mTx+Prefix,Text,Length);
  UINTN Size = Prefix+Length;
  Transfer(EP_IN, mTx, Size);
}

STATIC VOID Hex(CHAR8 *Text, UINT64 Value, UINTN Digits)
{
  for (UINTN i=0;i<Digits;i++) Text[i]="0123456789abcdef"[(Value>>((Digits-i-1)*4))&15];
  Text[Digits]=0;
}

STATIC VOID DataReply(UINTN Size)
{
  CHAR8 Text[16];
  Hex(Text,Size,8);
  Reply("DATA", Text);
}

STATIC EFI_STATUS AllocateDisk(UINTN Index, UINT64 Bytes)
{
  ROOT_STAGE_DISK *Disk = &mStage->Disks[Index];
  if (!Bytes || (Index < 2 && Bytes % 512) || Bytes > (Index == 2 ? 0x4000000ULL : ROOT_STAGE_CHUNK * ROOT_STAGE_EXTENTS)) return EFI_INVALID_PARAMETER;
  if (Disk->Count) return EFI_ALREADY_STARTED;
  for (UINT64 Offset = 0; Offset < Bytes;) {
    UINTN Size = (UINTN)MIN(ROOT_STAGE_CHUNK, Bytes - Offset);
    EFI_PHYSICAL_ADDRESS Address = 0;
    EFI_STATUS Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderData, EFI_SIZE_TO_PAGES(Size), &Address);
    if (EFI_ERROR(Status)) {
      for (UINTN i = 0; i < Disk->Count; i++) gBS->FreePages(Disk->Extents[i].Address, EFI_SIZE_TO_PAGES(Disk->Extents[i].Bytes));
      ZeroMem(Disk, sizeof(*Disk));
      return Status;
    }
    Disk->Extents[Disk->Count++] = (ROOT_STAGE_EXTENT){Address, Size};
    Offset += Size;
  }
  Disk->Bytes = Bytes;
  return EFI_SUCCESS;
}

STATIC UINT8 *DiskAddress(ROOT_STAGE_DISK *Disk, UINT64 Offset)
{
  return (UINT8 *)(UINTN)(Disk->Extents[Offset / ROOT_STAGE_CHUNK].Address + Offset % ROOT_STAGE_CHUNK);
}

EFI_STATUS MarbleRootUsb(VOID)
{
  EFI_STATUS Status;
  EFI_EVENT Event;
  EFI_GUID InitGuid = {0x1c0cffce,0xfc8d,0x4e44,{0x8c,0x78,0x9c,0x9e,0x5b,0x53,0x0d,0x36}};
  EFI_GUID TableGuid = ROOT_STAGE_GUID;
  EFI_GUID UsbGuid = {0xd9d9ce48,0x44b8,0x4f49,{0x8e,0x3e,0x2a,0x3b,0x92,0x7d,0xc6,0xc1}};
  VOID *Configs[] = {mConfig}, *SsConfigs[] = {mSsConfig};
  VOID *Strings[] = {mLanguage,mManufacturer,mProduct,mSerial,mInterface};
  ROOT_USB_DESCRIPTOR_SET Set = {mDevice,Configs,mSsDevice,SsConfigs,mQualifier,mBos,5,Strings};
  gBS->SetWatchdogTimer(0, 0, 0, NULL);
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,UsbNotify,NULL,&InitGuid,&Event);
  if (!EFI_ERROR(Status)) { gBS->SignalEvent(Event); gBS->CloseEvent(Event); }
  Status = gBS->LocateProtocol(&UsbGuid,NULL,(VOID **)&mUsb);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->LocateProtocol(&gEfiCpuArchProtocolGuid,NULL,(VOID **)&mCpu);
  if (EFI_ERROR(Status)) return Status;
  if (mStage) goto StartUsb;
  mStage = AllocatePages(EFI_SIZE_TO_PAGES(sizeof(*mStage)));
  if (!mStage) return EFI_OUT_OF_RESOURCES;
  ZeroMem(mStage, sizeof(*mStage));
  mStage->Magic = ROOT_STAGE_MAGIC;
  mStage->Version = 2;
  mStage->WaitForHost = (UINTN)&MarbleRootUsb;
  mStage->KernelBytes = 0x100000000ULL;
  // DarwinBoot preserves the kernel virtual base's offset within a 2 MiB block.
  mStage->KernelBase = (UINTN)AllocateAlignedPages(EFI_SIZE_TO_PAGES(mStage->KernelBytes+0x200000),0x200000);
  if (!mStage->KernelBase) return EFI_OUT_OF_RESOURCES;
  Print(L"Kernel RAM: 0x%lx bytes\r\n",mStage->KernelBytes);
  Print(L"Kernel base: %p\r\n",(VOID *)(UINTN)mStage->KernelBase);
  Status = gBS->InstallConfigurationTable(&TableGuid,mStage);
  if (EFI_ERROR(Status)) return Status;
StartUsb:
  mConnected = FALSE;
  Status = mUsb->StartEx(&Set);
  if (EFI_ERROR(Status)) return Status;
  if (!mRx) Status = mUsb->AllocateTransferBuffer(TRANSFER_BYTES,(VOID **)&mRx);
  if (EFI_ERROR(Status)) return Status;
  if (!mTx) Status = mUsb->AllocateTransferBuffer(TRANSFER_BYTES,(VOID **)&mTx);
  if (EFI_ERROR(Status)) return Status;
  Print(L"UniXNU RootStage USB\r\n");
  for (;;) {
    if (!mConnected) { WaitTransfer(0xff); continue; }
    INTN Size = Transfer(EP_OUT,mRx,64);
    if (Size <= 0) continue;
    mRx[Size] = 0;
    CHAR8 *Command = (CHAR8 *)mRx;
    ROOT_STAGE_DISK *Disk = &mStage->Disks[mDisk];
    if (!AsciiStrCmp(Command,"getvar:version")) Reply("OKAY","RootStage2");
    else if (!AsciiStrCmp(Command,"getvar:handoff-fault")) {
      EFI_GUID Guid = ROOT_STAGE_FAULT_GUID;
      UINT64 Empty[3] = {0}, *Fault = Empty;
      for (UINTN i=0;i<gST->NumberOfTableEntries;i++) {
        if (CompareGuid(&Guid,&gST->ConfigurationTable[i].VendorGuid))
          Fault=gST->ConfigurationTable[i].VendorTable;
      }
      CHAR8 Text[51];
      Hex(Text,Fault[0],16); Text[16]=':';
      Hex(Text+17,Fault[1],16); Text[33]=':';
      Hex(Text+34,Fault[2],16); Reply("OKAY",Text);
    }
    else if (!AsciiStrCmp(Command,"getvar:returns")) {
      CHAR8 Text[17]; Hex(Text,mStage->ReturnCount,16); Reply("OKAY",Text);
    }
    else if (!AsciiStrCmp(Command,"getvar:max-download-size")) Reply("OKAY","0x00100000");
    else if (!AsciiStrCmp(Command,"getvar:kernel-memory")) {
      CHAR8 Text[34]; Hex(Text,mStage->KernelBase,16); Text[16]=':'; Hex(Text+17,mStage->KernelBytes,16); Reply("OKAY",Text);
    } else if (!AsciiStrCmp(Command,"getvar:received")) {
      CHAR8 Text[34]; Hex(Text,Disk->Received,16); Text[16]=':'; Hex(Text+17,Disk->Bytes,16); Reply("OKAY",Text);
    } else if (!AsciiStrnCmp(Command,"oem disks:",10) && (Command[10]=='1' || Command[10]=='2') && !Command[11]) {
      mStage->DiskCount=Command[10]-'0'; Reply("OKAY", "");
    } else if (!AsciiStrCmp(Command,"getvar:image-id")) {
      Reply("OKAY",Disk->Identity);
    } else if (!AsciiStrnCmp(Command,"oem select:",11) && Command[11]>='0' && Command[11]<='2' && !Command[12]) {
      mDisk=Command[11]-'0'; Reply("OKAY", "");
    } else if (!AsciiStrnCmp(Command,"oem begin:",10) && Command[10]>='0' && Command[10]<='2' && Command[11]==':' && AsciiStrLen(Command)==61 && Command[28]==':') {
      UINTN Index = Command[10]-'0'; UINT64 Bytes = AsciiStrHexToUint64(Command+12);
      ROOT_STAGE_DISK *Selected = &mStage->Disks[Index];
      if (Selected->Bytes != Bytes || AsciiStrCmp(Selected->Identity,Command+29)) {
        for (UINTN i=0;i<Selected->Count;i++)
          gBS->FreePages(Selected->Extents[i].Address,EFI_SIZE_TO_PAGES(Selected->Extents[i].Bytes));
        ZeroMem(Selected,sizeof(*Selected));
      }
      Status = Selected->Count ? EFI_SUCCESS : AllocateDisk(Index,Bytes);
      if (!EFI_ERROR(Status)) {
        CopyMem(Selected->Identity,Command+29,33);
        if (Index<2) mStage->DiskCount=MAX(mStage->DiskCount,Index+1);
      }
      if (EFI_ERROR(Status)) Reply("FAIL","RAM allocation failed");
      else { mDisk=Index; mLastBytes=0; Reply("OKAY",""); Print(L"RootStage disk %u: %lu bytes\r\n",Index,Bytes); }
    } else if (!AsciiStrnCmp(Command,"download:",9)) {
      UINTN Bytes = AsciiStrHexToUintn(Command+9);
      if (!Bytes || Bytes>TRANSFER_BYTES || Disk->Received>Disk->Bytes || Bytes>Disk->Bytes-Disk->Received) { Reply("FAIL","invalid download size"); continue; }
      DataReply(Bytes);
      UINTN Done=0;
      while (Done<Bytes) {
        INTN Got=Transfer(EP_OUT,mRx,Bytes-Done);
        if (Got<=0) break;
        UINT64 Offset=Disk->Received+Done;
        UINTN First=MIN((UINTN)Got,(UINTN)(ROOT_STAGE_CHUNK-Offset%ROOT_STAGE_CHUNK));
        CopyMem(DiskAddress(Disk,Offset),mRx,First);
        if (First<(UINTN)Got) CopyMem(DiskAddress(Disk,Offset+First),mRx+First,(UINTN)Got-First);
        Done+=(UINTN)Got;
      }
      if (Done!=Bytes) { Reply("FAIL","short download"); continue; }
      mLastOffset=Disk->Received; mLastBytes=Bytes; Disk->Received+=Bytes;
      Reply("OKAY","");
    } else if (!AsciiStrCmp(Command,"upload")) {
      if (!mLastBytes) { Reply("FAIL","no staged data"); continue; }
      DataReply(mLastBytes);
      UINTN First=MIN(mLastBytes,(UINTN)(ROOT_STAGE_CHUNK-mLastOffset%ROOT_STAGE_CHUNK));
      CopyMem(mTx,DiskAddress(Disk,mLastOffset),First);
      if (First<mLastBytes) CopyMem(mTx+First,DiskAddress(Disk,mLastOffset+First),mLastBytes-First);
      Transfer(EP_IN,mTx,mLastBytes); Reply("OKAY","");
    } else if (!AsciiStrCmp(Command,"continue") || !AsciiStrCmp(Command,"selftest")) {
      mStage->ReturnTest=!AsciiStrCmp(Command,"selftest");
      BOOLEAN Complete=mStage->DiskCount>0;
      for (UINTN i=0;i<mStage->DiskCount;i++) Complete &= mStage->Disks[i].Bytes && mStage->Disks[i].Received==mStage->Disks[i].Bytes;
      if (mStage->Disks[2].Bytes) Complete &= mStage->Disks[2].Received==mStage->Disks[2].Bytes;
      if (!Complete && !mStage->ReturnTest) { Reply("FAIL","root images incomplete"); continue; }
      Print(L"RootStage cache writeback\r\n");
      Status = EFI_SUCCESS;
      for (UINTN i=0;i<3 && !EFI_ERROR(Status);i++) {
        for (UINTN j=0;j<mStage->Disks[i].Count;j++) {
          ROOT_STAGE_EXTENT *Extent=&mStage->Disks[i].Extents[j];
          Status=mCpu->FlushDataCache(mCpu,Extent->Address,Extent->Bytes,EfiCpuFlushTypeWriteBack);
          if (EFI_ERROR(Status)) break;
        }
      }
      if (!EFI_ERROR(Status)) Status=mCpu->FlushDataCache(mCpu,(UINTN)mStage,sizeof(*mStage),EfiCpuFlushTypeWriteBack);
      if (EFI_ERROR(Status)) { Reply("FAIL","root cache writeback failed"); continue; }
      Reply("OKAY",""); mUsb->Stop(); return EFI_SUCCESS;
    } else if (!AsciiStrCmp(Command,"reboot")) {
      Reply("OKAY",""); mUsb->Stop(); gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);
    } else Reply("FAIL","unknown command");
  }
}
