// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MARBLE_ROOT_BOOTLOADER_RESET_H
#define MARBLE_ROOT_BOOTLOADER_RESET_H

// PMK8350 SDAM boot mode: same ownership and readback contract as RuntimeUsb.
STATIC BOOLEAN ResetRead(UINTN Address, UINT32 *Value)
{
  UINT64 Fault[2]={0};
  return MarbleRootReadRegister(Value,(CONST VOID *)Address,0,Fault);
}
STATIC VOID ResetWrite(UINTN Address, UINT32 Value)
{
  *(volatile UINT32 *)Address=Value;
  __asm__ volatile("dsb sy" ::: "memory");
}
STATIC BOOLEAN ResetTransfer(UINTN Channel, UINT32 Command)
{
  ResetWrite(Channel,Command);
  UINT64 Frequency,Began=RootCounter();
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(Frequency));
  do {
    UINT32 Status;
    if (!ResetRead(Channel+8,&Status)) return FALSE;
    if (Status&1) return (Status&15)==1;
  } while (RootCounter()-Began<Frequency/1000);
  return FALSE;
}
STATIC EFI_STATUS PrepareBootloaderReset(VOID)
{
  UINT32 Version;
  if (!ResetRead(0xc400000,&Version) || Version<0x50000000) return EFI_UNSUPPORTED;
  BOOLEAN V7=Version>=0x70000000;
  UINTN Count=V7?1024:512;
  for (UINTN Index=0;Index<Count;Index++) {
    UINT32 Map,Owner;
    if (!ResetRead(0xc400000+(V7?0x2000:0x900)+4*Index,&Map) ||
        !ResetRead(0xc42d000+(V7?0:0x700)+4*Index,&Owner)) return EFI_DEVICE_ERROR;
    if (((Map>>8)&0xfff)!=0x71 || (Owner&7)!=0) continue;
    UINTN Reader=0xc440000+Index*(V7?0x20:0x80);
    UINTN Writer=0xc500000+Index*(V7?0x1000:0x10000);
    UINT32 Previous,Current;
    if (!ResetTransfer(Reader,(1U<<27)|(0x48U<<4)) ||
        !ResetRead(Reader+24,&Previous)) return EFI_DEVICE_ERROR;
    ResetWrite(Writer+16,(Previous&1)|(2U<<1));
    if (!ResetTransfer(Writer,0x48U<<4) ||
        !ResetTransfer(Reader,(1U<<27)|(0x48U<<4)) ||
        !ResetRead(Reader+24,&Current) || ((Current>>1)&127)!=2) return EFI_DEVICE_ERROR;
    ResetWrite(0x146aa65c,0x77665500);
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
STATIC EFI_STATUS ArmRecoveryWatchdog(VOID)
{
  EFI_STATUS Status=PrepareBootloaderReset();
  if (EFI_ERROR(Status)) return Status;
  Status=MarbleWatchdogStart(32000);
  if (!EFI_ERROR(Status)) mRecoveryArmed=TRUE;
  return Status;
}
STATIC VOID FirmwareBootloaderReset(VOID)
{
  // PMIC boot mode is verified before acknowledgement. PSCI resets all CPUs.
  MarbleWatchdogCall(0x84000009,0,0,NULL);
}
#endif
