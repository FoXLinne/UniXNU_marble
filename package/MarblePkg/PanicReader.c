// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/RootStage.h>
#include "GunyahWatchdog.h"

EFI_STATUS MarbleRootUsbResume(ROOT_STAGE_TABLE *Stage);

EFI_STATUS EFIAPI
MarblePanicReaderEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  STATIC ROOT_STAGE_TABLE Stage;
  VOID *Log = AllocatePages(EFI_SIZE_TO_PAGES(0x100000));
  if (!Log) return EFI_OUT_OF_RESOURCES;
  CopyMem(Log, (VOID *)(UINTN)0xB7000000, 0x100000);
  Stage.Magic = ROOT_STAGE_MAGIC;
  Stage.Version = 2;
  Stage.KernelBase = 0xB7000000;
  Stage.ReturnCount = *(UINT32 *)Log;
  Stage.Disks[2].Bytes = 0x100000;
  Stage.Disks[2].Count = 1;
  Stage.Disks[2].Extents[0].Address = (UINTN)Log;
  Stage.Disks[2].Extents[0].Bytes = 0x100000;
  MarbleWatchdogStart(32000);
  MarbleRootUsbResume(&Stage);
  gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
  return EFI_DEVICE_ERROR;
}
