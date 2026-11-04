// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "GunyahWatchdog.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

EFI_STATUS EFIAPI
MarbleWatchdogProbeEntry(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status = MarbleWatchdogStart(1000);
  Print(L"Marble watchdog: %r, timeout 1000 ms\r\n", Status);
  if (!EFI_ERROR(Status)) gBS->Stall(2000000);
  Print(L"Marble watchdog reset fallback\r\n");
  gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
  return EFI_DEVICE_ERROR;
}
