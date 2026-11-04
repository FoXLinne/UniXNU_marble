// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MARBLE_GUNYAH_WATCHDOG_H
#define MARBLE_GUNYAH_WATCHDOG_H

#include <Uefi.h>
#include <Library/UefiLib.h>

STATIC UINT64
MarbleWatchdogCall(UINT64 Function, UINT64 Argument1, UINT64 Argument2, UINT64 *Control)
{
  register UINT64 X0 __asm__("x0") = Function;
  register UINT64 X1 __asm__("x1") = Argument1;
  register UINT64 X2 __asm__("x2") = Argument2;
  __asm__ volatile("smc #0" : "+r"(X0), "+r"(X1), "+r"(X2) ::
    "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
    "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  if (Control) *Control = X1;
  return X0;
}

STATIC EFI_STATUS
MarbleWatchdogStart(UINT32 TimeoutMs)
{
  UINT64 Control = 0;
  UINT64 Result;
  if (!TimeoutMs || TimeoutMs > 32000) return EFI_INVALID_PARAMETER;
  if (MarbleWatchdogCall(0x86000006, 0, 0, &Control)) return EFI_UNSUPPORTED;
  Result = MarbleWatchdogCall(0x86000005, 2, 0, NULL);
  Print(L"Marble watchdog disable=%lx control=%lx\r\n", Result, Control);
  if (Result && (Control & 1)) return EFI_DEVICE_ERROR;
  Result = MarbleWatchdogCall(0x86000008, TimeoutMs, TimeoutMs, NULL);
  Print(L"Marble watchdog timeout=%lx\r\n", Result);
  if (Result) return EFI_DEVICE_ERROR;
  Result = MarbleWatchdogCall(0x86000005, 3, 0, NULL);
  Print(L"Marble watchdog enable=%lx\r\n", Result);
  if (Result) return EFI_DEVICE_ERROR;
  Result = MarbleWatchdogCall(0x86000007, 0, 0, NULL);
  Print(L"Marble watchdog ping=%lx\r\n", Result);
  if (Result) return EFI_DEVICE_ERROR;
  if (MarbleWatchdogCall(0x86000006, 0, 0, &Control) || !(Control & 1))
    return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

#endif
