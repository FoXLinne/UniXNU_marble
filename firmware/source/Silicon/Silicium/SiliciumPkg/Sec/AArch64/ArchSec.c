/**
  Copyright (c) 2012-2013, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>

#include "Sec.h"

VOID
SecCommonExceptionEntry (
  IN UINT32 Type,
  IN UINTN  LR,
  IN UINTN  Link,
  IN UINTN  Stack)
{
  CHAR8 Buffer[256];
  UINT64 Level, Esr, Far;
  UINTN CharCount;

  // Set Exception Message
  switch (Type) {
    case EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "Synchronous Exception at 0x%llx\n", LR);
      break;

    case EXCEPT_AARCH64_IRQ:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "IRQ Exception at 0x%llx\n", LR);
      break;

    case EXCEPT_AARCH64_FIQ:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "FIQ Exception at 0x%llx\n", LR);
      break;

    case EXCEPT_AARCH64_SERROR:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "SError/Abort Exception at 0x%llx\n", LR);
      break;

    default:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "Unknown Exception at 0x%llx\n", LR);
      break;
  }

  // Show Exception Message
  SerialPortWrite ((UINT8 *)Buffer, CharCount);

  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (Level));
  if (Level == 8) {
    __asm__ volatile ("mrs %0, esr_el2" : "=r" (Esr));
    __asm__ volatile ("mrs %0, far_el2" : "=r" (Far));
  } else {
    __asm__ volatile ("mrs %0, esr_el1" : "=r" (Esr));
    __asm__ volatile ("mrs %0, far_el1" : "=r" (Far));
  }
  CharCount = AsciiSPrint (Buffer, sizeof (Buffer),
    "ESR=%016lx FAR=%016lx\nLR=%016lx SP=%016lx EL=%lu\n",
    Esr, Far, (UINT64)Link, (UINT64)Stack, Level >> 2);
  SerialPortWrite ((UINT8 *)Buffer, CharCount);

  // CPU Dead Loop
  CpuDeadLoop ();
}
