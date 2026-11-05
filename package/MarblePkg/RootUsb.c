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
#include "../MarblePac.h"
#include "GunyahWatchdog.h"

BOOLEAN MarbleRootReadMemory(VOID *Destination, CONST VOID *Source, UINTN Bytes, UINT64 *Fault);

#define TRANSFER_BYTES 0x400000
#define EP_OUT 0x01
#define EP_IN 0x81
#define KERNEL_ALIGNMENT_SLACK 0x200000ULL

STATIC ROOT_USB_PROTOCOL *mUsb;
STATIC EFI_CPU_ARCH_PROTOCOL *mCpu;
STATIC UINT8 *mRx, *mTx;
STATIC ROOT_STAGE_TABLE *mStage;
STATIC UINTN mDisk;
STATIC UINTN mLastBytes;
STATIC UINT64 mLastOffset;
STATIC BOOLEAN mConnected;
STATIC BOOLEAN mReceivePending;
STATIC BOOLEAN mReceiveReady;
STATIC INTN mReceiveBytes;
STATIC BOOLEAN mDirty[3];
STATIC BOOLEAN mRuntimeUsb;
STATIC UINT8 *mExport;
STATIC UINTN mExportBytes;
STATIC CONST VOID *mFirmwareLog;

VOID MarbleRootUsbSetFirmwareLog(CONST VOID *Log)
{
  mFirmwareLog=Log;
}
STATIC UINT64 mWatchdogControl, mWatchdogStatus, mWatchdogPetStatus;
STATIC UINT64 mWatchdogLastPet, mWatchdogFrequency;
STATIC BOOLEAN mWatchdogService;
STATIC BOOLEAN mResidentService, mServiceStarted;
STATIC volatile UINT32 mServiceReady, mHostAction, mKernelRunning;
STATIC ROOT_STAGE_TABLE *mReturnedStage;
UINT64 MarbleRootUsbContext[16] __attribute__((aligned(64)));
#define mServiceContext MarbleRootUsbContext
STATIC VOID *mServiceStack;
VOID MarbleRootUsbCore(VOID);

STATIC VOID ServiceUpdatePoint(VOID)
{
  ROOT_STAGE_TABLE *Stage=__atomic_load_n(&mReturnedStage,__ATOMIC_ACQUIRE);
  if (!Stage) return;
  __atomic_store_n(&mReturnedStage,NULL,__ATOMIC_RELEASE);
  mStage=Stage;
  mDisk=2;
  mLastOffset=Stage->Disks[2].Bytes-0x100000;
  mLastBytes=0x100000;
  __atomic_store_n(&mKernelRunning,0,__ATOMIC_RELEASE);
}

STATIC UINT64 RootCounter(VOID)
{
  UINT64 Counter;
  __asm__ volatile("mrs %0, cntpct_el0" : "=r"(Counter));
  return Counter;
}

STATIC VOID ServiceWatchdog(VOID)
{
  if (!mWatchdogService) return;
  UINT64 Counter=RootCounter();
  if (Counter-mWatchdogLastPet<mWatchdogFrequency) return;
  mWatchdogPetStatus=MarbleWatchdogCall(0x86000007,0,0,NULL);
  mWatchdogLastPet=Counter;
}

STATIC VOID RetainUsbWatchdog(VOID)
{
  // The resident kernel can change the watchdog independently of the loader.
  // Read back control after disabling it; service an active timer while waiting
  // for the host, including periods with no USB transfers.
  // Firmware and the kernel can enable the VM watchdog after this status read.
  // Its pet operation stays on the USB owner's event loop for the session.
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(mWatchdogFrequency));
  mWatchdogLastPet=RootCounter();
  mWatchdogService=TRUE;
  mWatchdogControl=0;
  mWatchdogStatus=MarbleWatchdogCall(0x86000006,0,0,&mWatchdogControl);
  if (mWatchdogStatus || !(mWatchdogControl&1)) return;
  MarbleWatchdogCall(0x86000005,2,0,NULL);
  mWatchdogStatus=MarbleWatchdogCall(0x86000006,0,0,&mWatchdogControl);
  if (!mWatchdogStatus && !(mWatchdogControl&1)) return;
  mWatchdogPetStatus=MarbleWatchdogCall(0x86000007,0,0,NULL);
}

typedef struct {
  CHAR8 Magic[8];
  UINT32 Version, PageSize;
  UINT64 Logical, IndexAddress, IndexBytes;
  CHAR8 Uuid[37];
} ROOT_DATA_EXPORT;

STATIC BOOLEAN OwnedRange(UINT64 Address, UINT64 Bytes)
{
  UINT64 Capacity=mStage->KernelBytes+KERNEL_ALIGNMENT_SLACK;
  return Address>=mStage->KernelBase && Bytes<=Capacity &&
    Address-mStage->KernelBase<=Capacity-Bytes;
}

STATIC EFI_STATUS ReadData(UINT64 Offset, UINTN Bytes, BOOLEAN Index)
{
  ROOT_STAGE_DISK *Kernel=&mStage->Disks[2];
  if (Kernel->Count!=1 || Kernel->Bytes<0x100000) return EFI_NOT_READY;
  ROOT_DATA_EXPORT *Header=(VOID *)(UINTN)(Kernel->Extents[0].Address+Kernel->Bytes-0x100000+768*1024);
  if (CompareMem(Header->Magic,"ROOTDAT1",8) || Header->Version!=1 || Header->PageSize!=4096 ||
      !Header->Logical || Header->Logical>0x80000000ULL || (Header->Logical&4095) ||
      Header->IndexBytes!=Header->Logical/4096*8 || !OwnedRange(Header->IndexAddress,Header->IndexBytes))
    return EFI_NOT_READY;
  UINT64 Size=Index ? Header->IndexBytes : Header->Logical;
  if (!Bytes || Bytes>TRANSFER_BYTES || Offset>Size || Bytes>Size-Offset) return EFI_INVALID_PARAMETER;
  if (!mExport) {
    EFI_STATUS Status=mUsb->AllocateTransferBuffer(TRANSFER_BYTES,(VOID **)&mExport);
    if (EFI_ERROR(Status)) return Status;
  }
  if (Index) CopyMem(mExport,(VOID *)(UINTN)(Header->IndexAddress+Offset),Bytes);
  else {
    UINT64 *Pages=(VOID *)(UINTN)Header->IndexAddress;
    for (UINTN Done=0;Done<Bytes;) {
      UINT64 At=Offset+Done, Address=Pages[At/4096];
      UINTN Within=At&4095, Count=MIN(Bytes-Done,4096-Within);
      if (Address && !OwnedRange(Address,4096)) return EFI_COMPROMISED_DATA;
      if (Address) CopyMem(mExport+Done,(VOID *)(UINTN)(Address+Within),Count);
      else ZeroMem(mExport+Done,Count);
      Done+=Count;
    }
  }
  mExportBytes=Bytes;
  return EFI_SUCCESS;
}

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
    ServiceUpdatePoint();
    ServiceWatchdog();
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
  if (!Bytes || (Index < 2 && Bytes % 512) || (Index < 2 && Bytes > ROOT_STAGE_CHUNK * ROOT_STAGE_EXTENTS)) return EFI_INVALID_PARAMETER;
  if (Disk->Count) return EFI_ALREADY_STARTED;
  for (UINT64 Offset = 0; Offset < Bytes;) {
    UINTN Size = (UINTN)MIN(Index == 2 ? Bytes : ROOT_STAGE_CHUNK, Bytes - Offset);
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
  if (Disk->Count == 1) return (UINT8 *)(UINTN)(Disk->Extents[0].Address + Offset);
  return (UINT8 *)(UINTN)(Disk->Extents[Offset / ROOT_STAGE_CHUNK].Address + Offset % ROOT_STAGE_CHUNK);
}

EFI_STATUS MarbleRootUsb(VOID);
EFI_STATUS MarbleRootUsbContinue(VOID);

STATIC VOID UsbServiceLoop(VOID)
{
  __atomic_store_n(&mServiceReady,1,__ATOMIC_RELEASE);
  __asm__ volatile("sev" ::: "memory");
  for (;;) MarbleRootUsbContinue();
}

STATIC EFI_STATUS UsbServiceEnter(VOID)
{
  if (!mServiceStarted) {
    // iOS 18 exposes CPU 0 to XNU; affinity 0x700 remains the USB owner.
    UINT64 Value;
#define SAVE_USB_REGISTER(Slot, Register) \
    __asm__ volatile("mrs %0, " #Register : "=r"(Value)); mServiceContext[Slot]=Value
    SAVE_USB_REGISTER(0,sctlr_el1);
    SAVE_USB_REGISTER(1,ttbr0_el1);
    SAVE_USB_REGISTER(2,ttbr1_el1);
    SAVE_USB_REGISTER(3,tcr_el1);
    SAVE_USB_REGISTER(4,mair_el1);
    SAVE_USB_REGISTER(5,vbar_el1);
    SAVE_USB_REGISTER(6,cpacr_el1);
    SAVE_USB_REGISTER(10,tpidr_el1);
    SAVE_USB_REGISTER(11,tpidr_el0);
    SAVE_USB_REGISTER(12,tpidrro_el0);
#undef SAVE_USB_REGISTER
    mServiceContext[7]=(UINTN)mServiceStack+65536;
    mServiceContext[8]=(UINTN)&UsbServiceLoop;
    __asm__ volatile("mov %0, x18" : "=r"(Value)); mServiceContext[9]=Value;
    mServiceReady=0; mKernelRunning=1; mServiceStarted=TRUE;
    for (UINTN Offset=0;Offset<sizeof(mServiceContext);Offset+=64)
      __asm__ volatile("dc cvac, %0" :: "r"((UINTN)mServiceContext+Offset) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    UINT64 Result=MarbleWatchdogCall(0xc4000003,0x700,(UINTN)&MarbleRootUsbCore,NULL);
    if (Result) { mServiceStarted=FALSE; return EFI_DEVICE_ERROR; }
    UINT64 Began=RootCounter(), Frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(Frequency));
    while (!__atomic_load_n(&mServiceReady,__ATOMIC_ACQUIRE)) {
      if (RootCounter()-Began>Frequency) { mServiceStarted=FALSE; return EFI_TIMEOUT; }
      __asm__ volatile("yield");
    }
  }
  __atomic_store_n(&mKernelRunning,1,__ATOMIC_RELEASE);
  return EFI_SUCCESS;
}

STATIC VOID UsbServiceResume(VOID)
{
  // Keep servicing the controller while the boot CPU restores firmware state.
  // Kernel-mode command restrictions also protect firmware return allocations.
  __asm__ volatile("dmb ish" ::: "memory");
}

EFI_STATUS MarbleRootUsbStartResident(VOID)
{
  return mResidentService ? UsbServiceEnter() : EFI_SUCCESS;
}

EFI_STATUS MarbleRootUsbEnableResident(VOID)
{
  STATIC ROOT_STAGE_SERVICE Service={ROOT_STAGE_SERVICE_MAGIC,(UINTN)&UsbServiceEnter,(UINTN)&UsbServiceResume};
  EFI_GUID Guid=ROOT_STAGE_SERVICE_GUID;
  mServiceStack=AllocatePages(16);
  if (!mServiceStack) return EFI_OUT_OF_RESOURCES;
  mResidentService=TRUE;
  return gBS->InstallConfigurationTable(&Guid,&Service);
}

STATIC EFI_STATUS InitializeUsb(VOID)
{
  if (mResidentService && mUsb) return EFI_SUCCESS;
  EFI_STATUS Status;
  EFI_EVENT Event;
  EFI_GUID InitGuid = {0x1c0cffce,0xfc8d,0x4e44,{0x8c,0x78,0x9c,0x9e,0x5b,0x53,0x0d,0x36}};
  EFI_GUID TableGuid = ROOT_STAGE_GUID;
  EFI_GUID UsbGuid = {0xd9d9ce48,0x44b8,0x4f49,{0x8e,0x3e,0x2a,0x3b,0x92,0x7d,0xc6,0xc1}};
  STATIC VOID *Configs[] = {mConfig}, *SsConfigs[] = {mSsConfig};
  STATIC VOID *Strings[] = {mLanguage,mManufacturer,mProduct,mSerial,mInterface};
  STATIC ROOT_USB_DESCRIPTOR_SET Set = {mDevice,Configs,mSsDevice,SsConfigs,mQualifier,mBos,5,Strings};
  gBS->SetWatchdogTimer(0, 0, 0, NULL);
  RetainUsbWatchdog();
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,UsbNotify,NULL,&InitGuid,&Event);
  if (!EFI_ERROR(Status)) { gBS->SignalEvent(Event); gBS->CloseEvent(Event); }
  Status = gBS->LocateProtocol(&UsbGuid,NULL,(VOID **)&mUsb);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->LocateProtocol(&gEfiCpuArchProtocolGuid,NULL,(VOID **)&mCpu);
  if (EFI_ERROR(Status)) return Status;
  if (mStage) goto StartUsb;
  for (UINTN I=0;I<gST->NumberOfTableEntries;I++) {
    if (CompareGuid(&TableGuid,&gST->ConfigurationTable[I].VendorGuid)) {
      ROOT_STAGE_TABLE *Installed=gST->ConfigurationTable[I].VendorTable;
      if (!Installed || Installed->Magic!=ROOT_STAGE_MAGIC || Installed->Version!=2 ||
          !Installed->KernelBase || !Installed->KernelBytes) return EFI_COMPROMISED_DATA;
      mStage=Installed;
      goto StartUsb;
    }
  }
  mStage = AllocatePages(EFI_SIZE_TO_PAGES(sizeof(*mStage)));
  if (!mStage) return EFI_OUT_OF_RESOURCES;
  ZeroMem(mStage, sizeof(*mStage));
  mStage->Magic = ROOT_STAGE_MAGIC;
  mStage->Version = 2;
  mStage->WaitForHost = (UINTN)&MarbleRootUsb;
  mStage->KernelBytes = 0x200000000ULL;
  // DarwinBoot preserves the kernel virtual base's offset within a 2 MiB block.
  mStage->KernelBase = (UINTN)AllocateAlignedPages(EFI_SIZE_TO_PAGES(mStage->KernelBytes+KERNEL_ALIGNMENT_SLACK),0x200000);
  if (!mStage->KernelBase) return EFI_OUT_OF_RESOURCES;
  Print(L"Kernel RAM: 0x%lx bytes\r\n",mStage->KernelBytes);
  Print(L"Kernel base: %p\r\n",(VOID *)(UINTN)mStage->KernelBase);
  Status = gBS->InstallConfigurationTable(&TableGuid,mStage);
  if (EFI_ERROR(Status)) return Status;
StartUsb:
  mConnected = FALSE;
  mReceivePending = FALSE;
  mReceiveReady = FALSE;
  if (mRuntimeUsb) mUsb->Stop();
  Status = mUsb->StartEx(&Set);
  if (EFI_ERROR(Status)) return Status;
  if (!mRx) Status = mUsb->AllocateTransferBuffer(TRANSFER_BYTES,(VOID **)&mRx);
  if (EFI_ERROR(Status)) return Status;
  if (!mTx) Status = mUsb->AllocateTransferBuffer(TRANSFER_BYTES,(VOID **)&mTx);
  if (EFI_ERROR(Status)) return Status;
  return EFI_SUCCESS;
}

EFI_STATUS MarbleRootUsbPrepare(ROOT_STAGE_TABLE *Stage)
{
  mStage = Stage;
  EFI_STATUS Status = InitializeUsb();
  if (EFI_ERROR(Status)) return Status;
  mRuntimeUsb = TRUE;
  UINTN ConnectedTicks=0;
  for (UINTN Tick=0;Tick<5000;Tick++) {
    USB_DEVICE_EVENT Event=UsbDeviceEventNoEvent;
    USB_DEVICE_EVENT_DATA Data;
    UINTN Size=0;
    Status=mUsb->HandleEvent(&Event,&Size,&Data);
    if (EFI_ERROR(Status) && Status!=EFI_NOT_READY) return Status;
    if (Event==UsbDeviceEventDeviceStateChange) {
      mConnected=Data.DeviceState==0;
    }
    // Service enumeration requests after the physical connection event.
    if (!mConnected) ConnectedTicks=0;
    else if (++ConnectedTicks>=500) {
      Status=mUsb->Send(EP_OUT,64,mRx);
      if (!EFI_ERROR(Status)) mReceivePending=TRUE;
      return Status;
    }
    gBS->Stall(1000);
  }
  return EFI_TIMEOUT;
}

EFI_STATUS MarbleRootUsbPoll(VOID)
{
  // After CPU 7 starts, it is the sole controller event consumer.
  if (mServiceStarted) return EFI_SUCCESS;
  if (!mUsb) return EFI_NOT_READY;
  ServiceWatchdog();
  for (UINTN Tick=0;Tick<64;Tick++) {
    USB_DEVICE_EVENT Event=UsbDeviceEventNoEvent;
    USB_DEVICE_EVENT_DATA Data;
    UINTN Size=0;
    EFI_STATUS Status=mUsb->HandleEvent(&Event,&Size,&Data);
    if (EFI_ERROR(Status) && Status!=EFI_NOT_READY) return Status;
    if (Event==UsbDeviceEventNoEvent) break;
    if (Event==UsbDeviceEventDeviceStateChange) {
      mConnected=Data.DeviceState==0;
      if (!mConnected) { mReceivePending=FALSE; mReceiveReady=FALSE; }
    } else if (Event==UsbDeviceEventTransferNotification &&
               Data.TransferOutcome.EndpointIndex==EP_OUT && mReceivePending) {
      mReceiveBytes=Data.TransferOutcome.Status==UsbDeviceTransferStatusCompleteOK ?
        (INTN)Data.TransferOutcome.BytesCompleted : -1;
      mReceiveReady=TRUE;
    }
  }
  if (mConnected && !mReceivePending) {
    EFI_STATUS Status=mUsb->Send(EP_OUT,64,mRx);
    if (EFI_ERROR(Status)) return Status;
    mReceivePending=TRUE;
  }
  return EFI_SUCCESS;
}

EFI_STATUS MarbleRootUsbContinue(VOID)
{
  EFI_STATUS Status;
  if (!mServiceStarted) Print(L"UniXNU RootStage USB\r\n");
  for (;;) {
    ServiceUpdatePoint();
    if (!mConnected) { WaitTransfer(0xff); continue; }
    INTN Size;
    if (mReceivePending) {
      mReceivePending=FALSE;
      if (mReceiveReady) { Size=mReceiveBytes; mReceiveReady=FALSE; }
      else Size=WaitTransfer(EP_OUT);
    } else Size=Transfer(EP_OUT,mRx,64);
    if (Size <= 0) continue;
    mRx[Size] = 0;
    CHAR8 *Command = (CHAR8 *)mRx;
    ROOT_STAGE_DISK *Disk = &mStage->Disks[mDisk];
    if (AsciiStrCmp(Command,"upload")) mExportBytes=0;
    if (!AsciiStrCmp(Command,"getvar:version")) Reply("OKAY","RootStage2");
    else if (!AsciiStrCmp(Command,"getvar:phase")) Reply("OKAY",mKernelRunning ? "kernel" : "firmware");
    else if (!AsciiStrCmp(Command,"getvar:kernel-ready")) {
      __asm__ volatile("dc civac, %0\n\tdsb sy" :: "r"((UINTN)&mStage->ReturnTest) : "memory");
      Reply("OKAY",mKernelRunning && (mStage->ReturnTest==2 || (mStage->ReturnTest>>32)==1) ? "ready" : "pending");
    }
    else if (!AsciiStrCmp(Command,"oem interrupt")) {
      if (!mKernelRunning) { Reply("OKAY","firmware"); continue; }
      *(volatile UINT32 *)&mStage->ReturnTest=1;
      __asm__ volatile("dc cvac, %0\n\tdsb sy" :: "r"((UINTN)&mStage->ReturnTest) : "memory");
      Reply("OKAY","interrupt requested");
    }
    else if (!AsciiStrCmp(Command,"getvar:watchdog")) {
      CHAR8 Text[51];
      Hex(Text,mWatchdogStatus,16); Text[16]=':';
      Hex(Text+17,mWatchdogControl,16); Text[33]=':';
      Hex(Text+34,mWatchdogPetStatus,16); Reply("OKAY",Text);
    }
    else if (!AsciiStrCmp(Command,"getvar:cpu-features")) {
      UINT64 Isar1, Isar2, Level;
      __asm__ volatile("mrs %0, ID_AA64ISAR1_EL1" : "=r"(Isar1));
      __asm__ volatile("mrs %0, S3_0_C0_C6_2" : "=r"(Isar2));
      __asm__ volatile("mrs %0, CurrentEL" : "=r"(Level));
      CHAR8 Text[51];
      Hex(Text,Isar1,16); Text[16]=':';
      Hex(Text+17,Isar2,16); Text[33]=':';
      Hex(Text+34,Level,16); Reply("OKAY",Text);
    }
    else if (!AsciiStrCmp(Command,"getvar:pac-el1")) {
      struct MarblePacResult Result = {0};
      marbleProbePac(&Result);
      CHAR8 Text[34];
      Hex(Text,Result.keys,16); Text[16]=':';
      Hex(Text+17,Result.authenticated,16); Reply("OKAY",Text);
    }
    else if (!AsciiStrCmp(Command,"getvar:features")) Reply("OKAY","runtime-usb,data-export,dirty-cache,zero-fill,memory-read");
    else if (!AsciiStrnCmp(Command,"oem memory-read:",16)) {
      if (AsciiStrLen(Command)!=41 || Command[32]!=':') {
        Reply("FAIL","invalid memory request"); continue;
      }
      UINT64 Address=AsciiStrHexToUint64(Command+16);
      UINTN Bytes=AsciiStrHexToUintn(Command+33);
      if (!Bytes || Bytes>TRANSFER_BYTES || !OwnedRange(Address,Bytes)) {
        Reply("FAIL","outside retained kernel memory"); continue;
      }
      if (!mExport) Status=mUsb->AllocateTransferBuffer(TRANSFER_BYTES,(VOID **)&mExport);
      else Status=EFI_SUCCESS;
      if (EFI_ERROR(Status)) { Reply("FAIL","memory export allocation failed"); continue; }
      UINT64 Fault[2]={0};
      if (!MarbleRootReadMemory(mExport,(VOID *)(UINTN)Address,Bytes,Fault)) {
        CHAR8 Text[48];
        CopyMem(Text,"memory fault ",13);
        Hex(Text+13,Fault[0],16); Text[29]=':';
        Hex(Text+30,Fault[1],16);
        Reply("FAIL",Text); continue;
      }
      mExportBytes=Bytes;
      Reply("OKAY","");
    }
    else if (!AsciiStrnCmp(Command,"oem runtime-usb:",16) && (Command[16]=='0' || Command[16]=='1') && !Command[17]) {
      mRuntimeUsb=Command[16]=='1'; Reply("OKAY","");
    }
    else if ((!AsciiStrnCmp(Command,"oem data-map:",13) || !AsciiStrnCmp(Command,"oem data-read:",14))) {
      BOOLEAN Index=!AsciiStrnCmp(Command,"oem data-map:",13);
      UINTN Start=Index ? 13 : 14, Digits=Index ? 8 : 16;
      if (AsciiStrLen(Command)!=Start+Digits+9 || Command[Start+Digits]!=':') {
        Reply("FAIL","invalid Data request"); continue;
      }
      UINT64 Offset=AsciiStrHexToUint64(Command+Start);
      UINTN Bytes=AsciiStrHexToUintn(Command+Start+Digits+1);
      Status=ReadData(Offset,Bytes,Index);
      Reply(EFI_ERROR(Status) ? "FAIL" : "OKAY",EFI_ERROR(Status) ? "Data snapshot unavailable" : "");
    }
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
    } else if (!AsciiStrCmp(Command,"getvar:handoff-translation")) {
      EFI_GUID Guid = ROOT_STAGE_FAULT_GUID;
      UINT64 Empty[16] = {0}, *Fault = Empty;
      for (UINTN i=0;i<gST->NumberOfTableEntries;i++) {
        if (CompareGuid(&Guid,&gST->ConfigurationTable[i].VendorGuid))
          Fault=gST->ConfigurationTable[i].VendorTable;
      }
      CHAR8 Text[34];
      Hex(Text,Fault[14],16); Text[16]=':';
      Hex(Text+17,Fault[15],16); Reply("OKAY",Text);
    }
    else if (!AsciiStrCmp(Command,"getvar:returns")) {
      CHAR8 Text[17]; Hex(Text,mStage->ReturnCount,16); Reply("OKAY",Text);
    }
    else if (!AsciiStrCmp(Command,"getvar:max-download-size")) Reply("OKAY","0x00400000");
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
      mFirmwareLog=NULL;
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
    } else if (!AsciiStrnCmp(Command,"oem zero:",9) && AsciiStrLen(Command)==17) {
      UINTN Bytes=AsciiStrHexToUintn(Command+9);
      if (!Bytes || Bytes>TRANSFER_BYTES || Disk->Received>Disk->Bytes || Bytes>Disk->Bytes-Disk->Received) { Reply("FAIL","invalid zero fill size"); continue; }
      UINTN First=MIN(Bytes,(UINTN)(ROOT_STAGE_CHUNK-Disk->Received%ROOT_STAGE_CHUNK));
      ZeroMem(DiskAddress(Disk,Disk->Received),First);
      if (First<Bytes) ZeroMem(DiskAddress(Disk,Disk->Received+First),Bytes-First);
      mLastOffset=Disk->Received; mLastBytes=Bytes; Disk->Received+=Bytes;
      mDirty[mDisk]=TRUE;
      Reply("OKAY","");
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
      mDirty[mDisk]=TRUE;
      Reply("OKAY","");
    } else if (!AsciiStrCmp(Command,"upload")) {
      if (mExportBytes) {
        DataReply(mExportBytes); Transfer(EP_IN,mExport,mExportBytes); Reply("OKAY","");
        mExportBytes=0; continue;
      }
      if (mFirmwareLog && !mKernelRunning) {
        DataReply(0x100000);
        CopyMem(mTx,mFirmwareLog,0x100000);
        Transfer(EP_IN,mTx,0x100000); Reply("OKAY","");
        continue;
      }
      if (!mLastBytes) { Reply("FAIL","no staged data"); continue; }
      DataReply(mLastBytes);
      UINTN First=MIN(mLastBytes,(UINTN)(ROOT_STAGE_CHUNK-mLastOffset%ROOT_STAGE_CHUNK));
      CopyMem(mTx,DiskAddress(Disk,mLastOffset),First);
      if (First<mLastBytes) CopyMem(mTx+First,DiskAddress(Disk,mLastOffset+First),mLastBytes-First);
      Transfer(EP_IN,mTx,mLastBytes); Reply("OKAY","");
    } else if (!AsciiStrCmp(Command,"continue") || !AsciiStrCmp(Command,"selftest") || !AsciiStrCmp(Command,"oem retry")) {
      if (mKernelRunning) { Reply("FAIL","use oem interrupt to enter firmware"); continue; }
      BOOLEAN Retry=!AsciiStrCmp(Command,"oem retry");
      mStage->ReturnTest=!AsciiStrCmp(Command,"selftest");
      BOOLEAN Complete=Retry || mStage->DiskCount>0;
      for (UINTN i=0;i<mStage->DiskCount;i++) Complete &= mStage->Disks[i].Bytes && mStage->Disks[i].Received==mStage->Disks[i].Bytes;
      if (mStage->Disks[2].Bytes) Complete &= mStage->Disks[2].Received==mStage->Disks[2].Bytes;
      if (!Complete && !mStage->ReturnTest) { Reply("FAIL","root images incomplete"); continue; }
      Print(L"RootStage cache writeback\r\n");
      Status = EFI_SUCCESS;
      for (UINTN i=0;i<3 && !EFI_ERROR(Status);i++) {
        if (!mDirty[i]) continue;
        for (UINTN j=0;j<mStage->Disks[i].Count;j++) {
          ROOT_STAGE_EXTENT *Extent=&mStage->Disks[i].Extents[j];
          Status=mCpu->FlushDataCache(mCpu,Extent->Address,Extent->Bytes,EfiCpuFlushTypeWriteBack);
          if (EFI_ERROR(Status)) break;
        }
      }
      if (!EFI_ERROR(Status)) ZeroMem(mDirty,sizeof(mDirty));
      if (!EFI_ERROR(Status)) Status=mCpu->FlushDataCache(mCpu,(UINTN)mStage,sizeof(*mStage),EfiCpuFlushTypeWriteBack);
      if (EFI_ERROR(Status)) { Reply("FAIL","root cache writeback failed"); continue; }
      Reply("OKAY","");
      if (mServiceStarted) {
        __atomic_store_n(&mKernelRunning,1,__ATOMIC_RELEASE);
        __atomic_store_n(&mHostAction,Retry ? 1 : 2,__ATOMIC_RELEASE);
        __asm__ volatile("sev" ::: "memory");
        continue;
      }
      if (!mRuntimeUsb) mUsb->Stop();
      return Retry ? EFI_NOT_READY : EFI_SUCCESS;
    } else if (!AsciiStrCmp(Command,"reboot")) {
      Reply("OKAY",""); mUsb->Stop(); gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,0,NULL);
    } else Reply("FAIL","unknown command");
  }
}

EFI_STATUS MarbleRootUsb(VOID)
{
  EFI_STATUS Status=InitializeUsb();
  return EFI_ERROR(Status) ? Status : MarbleRootUsbContinue();
}

EFI_STATUS MarbleRootUsbResume(ROOT_STAGE_TABLE *Stage)
{
  if (mServiceStarted) {
    RetainUsbWatchdog();
    __atomic_store_n(&mHostAction,0,__ATOMIC_RELEASE);
    __atomic_store_n(&mReturnedStage,Stage,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&mHostAction,__ATOMIC_ACQUIRE)) __asm__ volatile("wfe");
    return mHostAction==1 ? EFI_NOT_READY : EFI_SUCCESS;
  }
  mStage=Stage;
  mDisk=2;
  mLastOffset=Stage->Disks[2].Bytes-0x100000;
  mLastBytes=0x100000;
  return MarbleRootUsb();
}
