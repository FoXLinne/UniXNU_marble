#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleFileSystem.h>

// The existing marble BootShim places the appended blobs in this reserved window.
#define PAYLOAD_BASE 0xA8000000ULL
#define PAYLOAD_MAGIC 0x50554E58

typedef struct { UINT32 Magic, Size; } PAYLOAD_HEADER;
typedef struct { CHAR8 Name[16]; UINT32 Size, Reserved; } PAYLOAD_BLOB;

STATIC UINT8 *mVolume;
STATIC UINTN mVolumeSize;
STATIC BOOLEAN mRootStage, mRootStorage;
EFI_STATUS MarbleRootStorage(VOID);
EFI_STATUS MarbleRootUsb(VOID);
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
  Print(L"UniXNU marble\r\n");
  if (!FindVolume()) return EFI_NOT_FOUND;
  // Dedicated panic log, inside marble's reserved 0xA8000000..0xB8000000 window.
  ZeroMem((VOID *)(UINTN)0xB7000000, 0x100000);
  if (mRootStage) {
    Status = MarbleRootUsb();
    if (EFI_ERROR(Status)) { Print(L"RootStage: %r\r\n",Status); return Status; }
  }
  if (mRootStorage) {
    Status = MarbleRootStorage();
    if (EFI_ERROR(Status)) { Print(L"UFS boot: %r\r\n",Status); return Status; }
  }
  mMedia.LastBlock = mVolumeSize / 512 - 1;
  Status = gBS->InstallMultipleProtocolInterfaces(&Disk,
    &gEfiBlockIoProtocolGuid, &mBlock, &gEfiDevicePathProtocolGuid, &mPath, NULL);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->ConnectController(Disk, NULL, NULL, TRUE);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Volumes);
  if (EFI_ERROR(Status)) return Status;
  Status = EFI_NOT_FOUND;
  for (UINTN i = 0; i < Count; i++) {
    EFI_DEVICE_PATH_PROTOCOL *Device = DevicePathFromHandle(Volumes[i]);
    if (!Device || CompareMem(Device, &mPath.Vendor, sizeof(mPath.Vendor))) continue;
    EFI_DEVICE_PATH_PROTOCOL *Path = FileDevicePath(Volumes[i], L"\\EFI\\UniXNU\\UniXNU.efi");
    EFI_HANDLE Application;
    Status = gBS->LoadImage(FALSE, ImageHandle, Path, NULL, 0, &Application);
    FreePool(Path);
    if (!EFI_ERROR(Status)) Status = gBS->StartImage(Application, NULL, NULL);
    break;
  }
  FreePool(Volumes);
  Print(L"UniXNU returned: %r\r\n", Status);
  return Status;
}
