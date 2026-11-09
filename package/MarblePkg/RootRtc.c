/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/RootStage.h>
#include "../MarbleRtc.h"

STATIC volatile MARBLE_RTC *mRtc;
STATIC UINT64 mRtcFrequency, mRtcLastService;
VOID MarbleRtcSuspend(VOID) {mRtc=NULL;}

STATIC UINT64 RtcCounter(VOID) {
  UINT64 Value;
  __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(Value));
  return Value;
}
/* PMK8350 RTC: SID 0, peripheral 0x61, read data 0x6148.
 * The SPMI observer lane issues read transactions only. */
STATIC INT64 mRtcOffset;
STATIC volatile UINT32 *mRtcObserver;
STATIC EFI_STATUS RtcRaw(UINT32 *Seconds) {
  if (!mRtcObserver) {
    volatile UINT32 *Core=(VOID *)(UINTN)0xc400000;
    UINT32 Version=Core[0];
    if (Version<0x50000000) return EFI_UNSUPPORTED;
    BOOLEAN V7=Version>=0x70000000;
    UINTN Selected=V7 ? 1024 : 512;
    for (UINTN Index=0;Index<(V7 ? 1024U : 512U);Index++) {
      UINT32 Map=Core[((V7 ? 0x2000 : 0x900)+Index*4)/4];
      if (Map && ((Map>>8)&0xfff)==0x61) Selected=Index;
    }
    if (Selected==(V7 ? 1024U : 512U)) return EFI_NOT_FOUND;
    mRtcObserver=(VOID *)(UINTN)(0xc440000+Selected*(V7 ? 0x20 : 0x80));
  }
  for (UINTN Attempt=0;Attempt<3;Attempt++) {
    UINT8 Bytes[5];
    for (UINTN Index=0;Index<5;Index++) {
      UINTN Reg=0x48+(Index%4);
      __asm__ volatile("dsb sy" ::: "memory");
      mRtcObserver[0]=(1U<<27)|(Reg<<4);
      __asm__ volatile("dsb sy" ::: "memory");
      UINTN Wait;
      for (Wait=0;Wait<1000;Wait++) {
        UINT32 Status=mRtcObserver[2];
        if (Status&1) {
          if (Status&14) return EFI_DEVICE_ERROR;
          break;
        }
        gBS->Stall(1);
      }
      if (Wait==1000) return EFI_TIMEOUT;
      Bytes[Index]=(UINT8)mRtcObserver[6];
    }
    if (Bytes[4]>=Bytes[0]) {
      *Seconds=(UINT32)Bytes[0]|((UINT32)Bytes[1]<<8)|((UINT32)Bytes[2]<<16)|((UINT32)Bytes[3]<<24);
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_READY;
}

VOID MarbleRtcLoadOffset(CONST VOID *Header) {
  CONST UINT64 *Cal=(CONST UINT64 *)((CONST UINT8 *)Header+MARBLE_RTC_CAL_OFFSET);
  if (Cal[0]==MARBLE_RTC_CAL_MAGIC && Cal[2]==~Cal[1]) mRtcOffset=(INT64)Cal[1];
}

EFI_STATUS MarbleRtcRead(UINT64 *Seconds, UINT32 *Nanoseconds) {
  UINT32 Raw=0;
  EFI_STATUS Status=RtcRaw(&Raw);
  if (!EFI_ERROR(Status)) { *Seconds=(UINT64)((INT64)Raw+mRtcOffset); *Nanoseconds=0; }
  return Status;
}

STATIC VOID Publish(EFI_STATUS Status, UINT64 Seconds, UINT32 Nanoseconds, UINT64 Counter, UINT32 Completed) {
  if (!mRtc) return;
  mRtc->sequence++;
  __asm__ volatile("dmb ishst" ::: "memory");
  mRtc->magic=MARBLE_RTC_MAGIC;
  mRtc->error=(UINT32)(Status & 0x7fffffff);
  mRtc->seconds=Seconds; mRtc->nanoseconds=Nanoseconds;
  mRtc->counter=Counter; mRtc->frequency=mRtcFrequency;
  mRtc->completed=Completed;
  __asm__ volatile("dmb ishst" ::: "memory");
  mRtc->sequence++;
  __asm__ volatile("dc cvac, %0\n\tdsb sy" :: "r"((UINTN)mRtc) : "memory");
}

STATIC EFI_STATUS RtcSet(UINT64 Seconds, UINT32 Nanoseconds, UINT32 Completed) {
  UINT32 Raw=0;
  EFI_STATUS Status=RtcRaw(&Raw);
  if (!EFI_ERROR(Status)) mRtcOffset=(INT64)Seconds-Raw;
  if (mRtc) {
    mRtc->reserved[1]=Raw;
    Publish(Status,Seconds,Nanoseconds,RtcCounter(),Completed);
  }
  return Status;
}

EFI_STATUS MarbleRtcSet(UINT64 Seconds, UINT32 Nanoseconds) {
  return RtcSet(Seconds,Nanoseconds,mRtc ? mRtc->completed : 0);
}

EFI_STATUS MarbleRtcRequest(UINT64 Seconds) {
  if (!mRtc || Seconds<1577836800ULL || Seconds>=253402300800ULL) return EFI_INVALID_PARAMETER;
  mRtc->reserved[0]=Seconds;
  __asm__ volatile("dc cvac, %0\n\tdsb sy" :: "r"((UINTN)mRtc) : "memory");
  return EFI_SUCCESS;
}

VOID MarbleRtcPrepare(ROOT_STAGE_TABLE *Stage) {
  ROOT_STAGE_DISK *Kernel=&Stage->Disks[2];
  if (Kernel->Count!=1 || Kernel->Bytes<0x100000 || Kernel->Bytes>Kernel->Extents[0].Bytes) return;
  UINT8 *Mailbox=(UINT8 *)(UINTN)(Kernel->Extents[0].Address+Kernel->Bytes-0x100000);
  if (CompareMem(Mailbox,"ROOTLOG1",8)) return;
  mRtc=(volatile MARBLE_RTC *)(Mailbox+MARBLE_RTC_OFFSET);
  ZeroMem((VOID *)mRtc,sizeof(*mRtc));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(mRtcFrequency));
  UINT64 Seconds=0;
  UINT32 Nanoseconds=0;
  EFI_STATUS Status=MarbleRtcRead(&Seconds,&Nanoseconds);
  UINT32 Raw=0;
  (VOID)RtcRaw(&Raw);
  mRtc->reserved[1]=Raw;
  Publish(Status,Seconds,Nanoseconds,RtcCounter(),0);
  Print(L"MarbleRTC: firmware UTC %lu status %r\r\n",Seconds,Status);
}

VOID MarbleRtcService(VOID) {
  if (!mRtc || !mRtcFrequency) return;
  UINT64 Counter=RtcCounter();
  if (Counter-mRtcLastService<mRtcFrequency) return;
  mRtcLastService=Counter;
  __asm__ volatile("dc ivac, %0\n\tdsb sy" :: "r"((UINTN)mRtc+64) : "memory");
  UINT32 Request=mRtc->requested;
  if (Request==mRtc->completed || (Request&1)) return;
  __asm__ volatile("dmb ishld" ::: "memory");
  UINT64 Seconds=mRtc->request_seconds, Tick=mRtc->request_counter;
  UINT64 Nanoseconds=mRtc->request_nanoseconds;
  __asm__ volatile("dmb ishld" ::: "memory");
  if (Request!=mRtc->requested || Counter<Tick) return;
  UINT64 Delta=Counter-Tick;
  Seconds+=Delta/mRtcFrequency;
  Nanoseconds+=(Delta%mRtcFrequency)*1000000000/mRtcFrequency;
  Seconds+=Nanoseconds/1000000000;
  (VOID)RtcSet(Seconds,(UINT32)(Nanoseconds%1000000000),Request);
}
