// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/RootStage.h>

EFI_STATUS EFIAPI MarbleBootstrapEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);

STATIC EFI_STATUS
KernelReturned(VOID)
{
  return EFI_ABORTED;
}

STATIC EFI_STATUS
RunProbe(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable, BOOLEAN SelfTest)
{
  STATIC ROOT_STAGE_TABLE Stage;
  EFI_GUID Guid = ROOT_STAGE_GUID;
  Stage.Magic = ROOT_STAGE_MAGIC;
  Stage.Version = ROOT_STAGE_VERSION;
  Stage.KernelBytes = 0x80000000ULL;
  Stage.KernelBase = (UINTN)AllocateAlignedPages(
      EFI_SIZE_TO_PAGES(Stage.KernelBytes + 0x200000), 0x200000);
  if (!Stage.KernelBase) return EFI_OUT_OF_RESOURCES;
  Stage.WaitForHost = (UINTN)&KernelReturned;
  Stage.ReturnTest = SelfTest;
  EFI_STATUS Status = gBS->InstallConfigurationTable(&Guid, &Stage);
  if (EFI_ERROR(Status)) return Status;
  Print(L"Marble kernel probe: base=%lx bytes=%lx selftest=%lu\r\n",
      Stage.KernelBase, Stage.KernelBytes, Stage.ReturnTest);
  return MarbleBootstrapEntry(ImageHandle, SystemTable);
}

EFI_STATUS EFIAPI
MarbleKernelProbeEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  return RunProbe(ImageHandle, SystemTable, FALSE);
}

EFI_STATUS EFIAPI
MarbleResidentSelftestEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  return RunProbe(ImageHandle, SystemTable, TRUE);
}
