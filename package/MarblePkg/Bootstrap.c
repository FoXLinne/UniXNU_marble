#include <Protocol/RootStage.h>
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/UniXNULoader.h>
#include "GunyahWatchdog.h"

// The existing marble BootShim places the appended blobs in this reserved window.
#define PAYLOAD_BASE 0xA8000000ULL
#define PAYLOAD_MAGIC 0x50554E58

typedef struct { UINT32 Magic, Size; } PAYLOAD_HEADER;
typedef struct { CHAR8 Name[16]; UINT32 Size, Reserved; } PAYLOAD_BLOB;

STATIC UINT8 *mVolume;
STATIC UINTN mVolumeSize;
STATIC BOOLEAN mRootStage, mRootStorage;
STATIC BOOLEAN mResidentUsb;
EFI_STATUS MarbleRootUsbEnableResident(VOID);
EFI_STATUS MarbleRootUsbStartResident(VOID);
EFI_STATUS MarbleRootStorage(VOID);
EFI_STATUS MarbleRootUsb(VOID);
EFI_STATUS MarbleRootUsbPrepare(ROOT_STAGE_TABLE *Stage);
EFI_STATUS MarbleRootUsbContinue(VOID);
EFI_STATUS MarbleRootUsbPoll(VOID);
EFI_STATUS MarbleRootUsbResume(ROOT_STAGE_TABLE *Stage);
STATIC EFI_TEXT_STRING mOutputString;
STATIC CHAR16 mBootText[32768];
STATIC UINTN mBootTextUsed;
STATIC UNIXNU_LOADER_BOOT mLoaderBoot;

STATIC VOID
CaptureBootScreen(UINT8 *Buffer, UINTN Capacity)
{
  EFI_GUID Guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  if (EFI_ERROR(gBS->LocateProtocol(&Guid, NULL, (VOID **)&Gop)) ||
      !Gop->Mode || !Gop->Mode->Info) return;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info = Gop->Mode->Info;
  if (Info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor ||
      !Info->HorizontalResolution || !Info->VerticalResolution ||
      Info->PixelsPerScanLine < Info->HorizontalResolution ||
      (UINT64)Info->PixelsPerScanLine * Info->VerticalResolution * 4 > Gop->Mode->FrameBufferSize)
    return;
  UINTN Scale = 1;
  while ((UINT64)(Info->HorizontalResolution / Scale) * (Info->VerticalResolution / Scale) * 3 + 32 > Capacity)
    Scale++;
  UINT32 Width = Info->HorizontalResolution / Scale, Height = Info->VerticalResolution / Scale;
  if (!Width || !Height) return;
  UINT32 Header[4] = {Width, Height, Width * 3, Width * Height * 3};
  CopyMem(Buffer, "ROOTRGB1", 8);
  CopyMem(Buffer + 8, Header, sizeof(Header));
  ZeroMem(Buffer + 24, 8);
  volatile UINT32 *Pixels = (VOID *)(UINTN)Gop->Mode->FrameBufferBase;
  UINT8 *Out = Buffer + 32;
  for (UINTN Y = 0; Y < Height; Y++) {
    for (UINTN X = 0; X < Width; X++) {
      UINT32 Pixel = Pixels[Y * Scale * Info->PixelsPerScanLine + X * Scale];
      *Out++ = Pixel >> 16;
      *Out++ = Pixel >> 8;
      *Out++ = Pixel;
    }
  }
}

STATIC EFI_STATUS EFIAPI
BootOutput(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *Text)
{
  for (UINTN Index = 0; Text[Index] && mBootTextUsed + 1 < ARRAY_SIZE(mBootText); Index++)
    mBootText[mBootTextUsed++] = Text[Index];
  return mOutputString(This, Text);
}

STATIC EFI_STATUS
BootReturn(EFI_STATUS Status, UINTN Checkpoint)
{
  STATIC ROOT_STAGE_TABLE Report;
  ZeroMem(&Report, sizeof(Report));
  Report.Magic = ROOT_STAGE_MAGIC;
  Report.Version = 2;
  Report.KernelBase = Checkpoint;
  Report.ReturnCount = Status;
  ROOT_STAGE_TABLE *Resident = NULL;
  EFI_GUID StageGuid = ROOT_STAGE_GUID;
  for (UINTN Index = 0; Index < gST->NumberOfTableEntries; Index++) {
    if (CompareGuid(&StageGuid, &gST->ConfigurationTable[Index].VendorGuid))
      Resident = gST->ConfigurationTable[Index].VendorTable;
  }
  if (Resident && Resident->Magic == ROOT_STAGE_MAGIC && Resident->Version == 2) {
    CopyMem(&Report, Resident, sizeof(Report));
    Report.ReturnCount = Status;
    for (UINTN Index = 0; Index < Resident->Disks[2].Count; Index++) {
      ROOT_STAGE_EXTENT *Extent = &Resident->Disks[2].Extents[Index];
      gBS->FreePages(Extent->Address, EFI_SIZE_TO_PAGES(Extent->Bytes));
    }
    ZeroMem(&Resident->Disks[2], sizeof(Resident->Disks[2]));
  }
  if (mOutputString) gST->ConOut->OutputString = mOutputString;
  Print(L"Marble boot checkpoint %u: %r\r\n", Checkpoint, Status);
  gBS->SetWatchdogTimer(0, 0, 0, NULL);
  MarbleWatchdogCall(0x86000005, 2, 0, NULL);
  VOID *Log = AllocatePages(EFI_SIZE_TO_PAGES(0x100000));
  if (Log) {
    ZeroMem(Log, 0x100000);
    CopyMem(Log, mBootText, mBootTextUsed * sizeof(CHAR16));
    CaptureBootScreen((UINT8 *)Log + 65536, 0x100000 - 65536);
    ZeroMem(&Report.Disks[2], sizeof(Report.Disks[2]));
    Report.Disks[2].Bytes = 0x100000;
    Report.Disks[2].Count = 1;
    Report.Disks[2].Extents[0].Address = (UINTN)Log;
    Report.Disks[2].Extents[0].Bytes = 0x100000;
    EFI_STATUS Host = MarbleRootUsbResume(&Report);
    if (Host == EFI_NOT_READY && Resident) {
      CopyMem(&Resident->Disks[2], &Report.Disks[2], sizeof(Resident->Disks[2]));
      return EFI_NOT_READY;
    }
  }
  return Status;
}

EFI_STATUS
MarbleRootStorageReturn(ROOT_STAGE_TABLE *Stage)
{
  ROOT_STAGE_DISK *Kernel = &Stage->Disks[2];
  if (Kernel->Count == 1 && Kernel->Bytes >= 0x100000) {
    UINT8 *Log = (VOID *)(UINTN)(Kernel->Extents[0].Address + Kernel->Bytes - 0x100000);
    ZeroMem(Log, 0x100000);
    CopyMem(Log, mBootText, mBootTextUsed * sizeof(CHAR16));
    CaptureBootScreen(Log + 65536, 0x100000 - 65536);
  }
  return MarbleRootUsbResume(Stage);
}

STATIC EFI_STATUS EFIAPI
BootLoader(UNIXNU_LOADER_PROTOCOL *This, CONST UNIXNU_LOADER_BOOT_REQUEST *Request)
{
  for (;;) {
    EFI_STATUS Status = BootReturn(mLoaderBoot(This, Request), 6);
    if (Status != EFI_NOT_READY) return Status;
    mBootTextUsed = 0;
    mBootText[0] = 0;
    gST->ConOut->OutputString = BootOutput;
  }
}

STATIC VOID EFIAPI
LoaderAvailable(EFI_EVENT Event, VOID *Context)
{
  EFI_GUID Guid = UNIXNU_LOADER_PROTOCOL_GUID;
  UNIXNU_LOADER_PROTOCOL *Loader;
  if (!EFI_ERROR(gBS->LocateProtocol(&Guid, NULL, (VOID **)&Loader)) &&
      Loader->Boot != BootLoader) {
    mLoaderBoot = Loader->Boot;
    Loader->Boot = BootLoader;
  }
}

STATIC EFI_BLOCK_IO_MEDIA mMedia = {
  .MediaId = 1, .MediaPresent = TRUE, .ReadOnly = TRUE, .BlockSize = 512, .IoAlign = 1
};
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mPath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof(VENDOR_DEVICE_PATH), 0}},
   {0x71335348, 0xb8be, 0x466b, {0x8b, 0xb4, 0x92, 0x68, 0x70, 0xaa, 0x59, 0xe7}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {END_DEVICE_PATH_LENGTH, 0}}
};

STATIC EFI_STATUS EFIAPI
DiskReset(EFI_BLOCK_IO_PROTOCOL *This, BOOLEAN ExtendedVerification)
{
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
DiskRead(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buffer)
{
  if (MediaId != mMedia.MediaId) return EFI_MEDIA_CHANGED;
  if (Size % mMedia.BlockSize) return EFI_BAD_BUFFER_SIZE;
  if (Lba > mMedia.LastBlock || Size > mVolumeSize - Lba * 512) return EFI_INVALID_PARAMETER;
  CopyMem(Buffer, mVolume + Lba * 512, Size);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
DiskWrite(EFI_BLOCK_IO_PROTOCOL *This, UINT32 MediaId, EFI_LBA Lba, UINTN Size, VOID *Buffer)
{
  return EFI_WRITE_PROTECTED;
}

STATIC EFI_STATUS EFIAPI
DiskFlush(EFI_BLOCK_IO_PROTOCOL *This)
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL mBlock = {
  EFI_BLOCK_IO_PROTOCOL_REVISION, &mMedia, DiskReset, DiskRead, DiskWrite, DiskFlush
};

STATIC BOOLEAN
FindVolume(VOID)
{
  PAYLOAD_HEADER *Header = (VOID *)PAYLOAD_BASE;
  UINT8 *Cursor = (VOID *)(Header + 1), *End = Cursor + Header->Size;
  if (Header->Magic != PAYLOAD_MAGIC) return FALSE;
  while (Cursor + sizeof(PAYLOAD_BLOB) <= End) {
    PAYLOAD_BLOB *Blob = (VOID *)Cursor;
    if (!Blob->Name[0] || Blob->Size > (UINTN)(End - Cursor - sizeof(*Blob))) break;
    if (CompareMem(Blob->Name, "root.storage", sizeof("root.storage")) == 0) mRootStorage = TRUE;
    if (CompareMem(Blob->Name, "root.usb", sizeof("root.usb")) == 0) mResidentUsb = TRUE;
    if (CompareMem(Blob->Name, "root.stage", sizeof("root.stage")) == 0) mRootStage = TRUE;
    if (CompareMem(Blob->Name, "volume.img", sizeof("volume.img")) == 0) {
      mVolume = (VOID *)(Blob + 1);
      mVolumeSize = Blob->Size;
      return mVolumeSize >= 512 && mVolumeSize % 512 == 0;
    }
    Cursor += sizeof(*Blob) + ALIGN_VALUE(Blob->Size, 16);
  }
  return FALSE;
}

EFI_STATUS EFIAPI
MarbleBootstrapEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HANDLE Disk = NULL, *Volumes = NULL;
  UINTN Count = 0;
  EFI_STATUS Status;
  EFI_EVENT LoaderEvent;
  VOID *Registration;
  EFI_GUID LoaderGuid = UNIXNU_LOADER_PROTOCOL_GUID;
  mOutputString = gST->ConOut->OutputString;
  gST->ConOut->OutputString = BootOutput;
  Status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, LoaderAvailable, NULL, &LoaderEvent);
  if (EFI_ERROR(Status)) return BootReturn(Status, 7);
  Status = gBS->RegisterProtocolNotify(&LoaderGuid, LoaderEvent, &Registration);
  if (EFI_ERROR(Status)) return BootReturn(Status, 8);
  Print(L"UniXNU marble\r\n");
  if (!FindVolume()) return BootReturn(EFI_NOT_FOUND, 1);
  // Dedicated panic log, inside marble's reserved 0xA8000000..0xB8000000 window.
  ZeroMem((VOID *)(UINTN)0xB7000000, 0x100000);
  if (mRootStorage) {
    if (mResidentUsb) {
      Status=MarbleRootUsbEnableResident();
      if (EFI_ERROR(Status)) return BootReturn(Status,9);
    }
    Status=MarbleRootUsbPrepare(NULL);
    if (EFI_ERROR(Status)) Print(L"RootStage USB: %r\r\n",Status);
    Status=MarbleRootStorage();
    if (EFI_ERROR(Status)) { Print(L"UFS boot: %r\r\n",Status); return Status; }
    Status=MarbleRootUsbStartResident();
    if (EFI_ERROR(Status)) return BootReturn(Status,10);
    for (UINTN Tick=0;Tick<500;Tick++) {
      Status=MarbleRootUsbPoll();
      if (EFI_ERROR(Status)) break;
      gBS->Stall(1000);
    }
    if (EFI_ERROR(Status)) Print(L"UFS USB handoff: %r\r\n",Status);
  }
  if (mRootStage) {
    Status=mRootStorage ? MarbleRootUsbContinue() : MarbleRootUsb();
    if (EFI_ERROR(Status)) { Print(L"RootStage: %r\r\n",Status); return Status; }
  }
  mMedia.LastBlock = mVolumeSize / 512 - 1;
  Status = gBS->InstallMultipleProtocolInterfaces(&Disk,
    &gEfiBlockIoProtocolGuid, &mBlock, &gEfiDevicePathProtocolGuid, &mPath, NULL);
  if (EFI_ERROR(Status)) return BootReturn(Status, 2);
  Status = gBS->ConnectController(Disk, NULL, NULL, TRUE);
  if (EFI_ERROR(Status)) return BootReturn(Status, 3);
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Volumes);
  if (EFI_ERROR(Status)) return BootReturn(Status, 4);
  Status = EFI_NOT_FOUND;
  for (UINTN i = 0; i < Count; i++) {
    EFI_DEVICE_PATH_PROTOCOL *Device = DevicePathFromHandle(Volumes[i]);
    if (!Device || CompareMem(Device, &mPath.Vendor, sizeof(mPath.Vendor))) continue;
    EFI_DEVICE_PATH_PROTOCOL *Path = FileDevicePath(Volumes[i], L"\\EFI\\UniXNU\\UniXNU.efi");
    EFI_HANDLE Application;
    Status = gBS->LoadImage(FALSE, ImageHandle, Path, NULL, 0, &Application);
    FreePool(Path);
    if (!EFI_ERROR(Status)) {
      Status = MarbleWatchdogStart(32000);
      Print(L"Marble watchdog: %r, timeout 32000 ms\r\n", Status);
      if (!EFI_ERROR(Status)) Status = gBS->StartImage(Application, NULL, NULL);
    }
    break;
  }
  FreePool(Volumes);
  Print(L"UniXNU returned: %r\r\n", Status);
  return BootReturn(Status, 5);
}
