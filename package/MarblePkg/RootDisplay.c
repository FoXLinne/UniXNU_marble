// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/RootStage.h>
#include "../MarbleScanoutWire.h"

BOOLEAN MarbleRootReadRegister(UINT32 *, CONST VOID *, UINTN, UINT64 *);
STATIC volatile MARBLE_SCANOUT *mDisplay;
STATIC UINT32 mPipeOffset[8];
STATIC UINT64 mDisplayFrequency;
STATIC UINT64 DisplayCounter(VOID);
/* Prefix ABI from QcomPkg/Include/Protocol/EFIClock.h. */
typedef struct MARBLE_DISPLAY_CLOCK MARBLE_DISPLAY_CLOCK;
struct MARBLE_DISPLAY_CLOCK {
  UINT64 Version;
  EFI_STATUS (EFIAPI *GetClockID)(MARBLE_DISPLAY_CLOCK *,CONST CHAR8 *,UINTN *);
  EFI_STATUS (EFIAPI *EnableClock)(MARBLE_DISPLAY_CLOCK *,UINTN);
};
STATIC EFI_STATUS DisplayRegisterClocks(VOID) {
  EFI_GUID Guid={0x241afae6,0x885f,0x4f6c,{0xa7,0xea,0xc2,0x8e,0xab,0x79,0xc3,0xe5}};
  MARBLE_DISPLAY_CLOCK *Clock=NULL;
  EFI_STATUS Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Clock);
  if (EFI_ERROR(Status)) return Status;
  /* DisplayDxe uses these two register-bus clocks before MDP access. */
  CONST CHAR8 *Names[]={"gcc_disp_ahb_clk","disp_cc_mdss_ahb_clk"};
  for (UINTN I=0;I<2;I++) {
    UINTN Id=0;
    Status=Clock->GetClockID(Clock,Names[I],&Id);
    if (EFI_ERROR(Status)) return Status;
    Status=Clock->EnableClock(Clock,Id);
    if (EFI_ERROR(Status)) return Status;
  }
  return EFI_SUCCESS;
}
CONST volatile MARBLE_SCANOUT *MarbleDisplayState(VOID) {return mDisplay;}
BOOLEAN MarbleDisplayClock(UINT32 Values[8]) {
  if (!mDisplay || !mDisplay->ready) return FALSE;
  UINT64 Fault[2]={0};UINT32 Active=0;
  if (!MarbleRootReadRegister(&Active,(VOID *)(UINTN)0xae160f4,0,Fault)) return FALSE;
  UINT32 Interface=(Active&2) ? 1 : (Active&4) ? 2 : 0;
  if (!Interface) return FALSE;
  UINT32 Base=0x35000+Interface*0x1000;
  Values[0]=(UINT32)DisplayCounter();Values[1]=(UINT32)mDisplayFrequency;
  Values[2]=Active;Values[3]=Interface;
  CONST UINT32 Offsets[4]={0xac,8,0xc,0x6c};
  for (UINTN I=0;I<4;I++)
    if (!MarbleRootReadRegister(&Values[I+4],(VOID *)(UINTN)(0xae00000+Base+Offsets[I]),0,Fault)) return FALSE;
  return TRUE;
}
STATIC CONST UINT32 mPipe[8]={0x5000,0x7000,0x9000,0xb000,0x25000,0x27000,0x29000,0x2b000};
STATIC CONST UINT32 mRegisters[8]={0,8,0x14,0x24,0x30,0x38,0x70,0x170};
STATIC UINT64 DisplayCounter(VOID) {
  UINT64 Value; __asm__ volatile("isb\n\tmrs %0,cntpct_el0":"=r"(Value)); return Value;
}
STATIC BOOLEAN DisplayRead(UINT32 Offset,UINT32 *Value) {
  UINT64 Fault[2]={0};
  mDisplay->checkpoint=Offset;
  __asm__ volatile("dc cvac,%0\n\tdsb sy"::"r"((UINTN)mDisplay):"memory");
  if (MarbleRootReadRegister(Value,(VOID *)(UINTN)(0xae00000+Offset),0,Fault)) return TRUE;
  mDisplay->error=0x10000000U|(UINT32)Fault[0]; return FALSE;
}
STATIC VOID DisplayPublish(VOID) {
  __asm__ volatile("dc cvac,%0\n\tdsb sy"::"r"((UINTN)mDisplay):"memory");
}
VOID MarbleDisplayPrepare(ROOT_STAGE_TABLE *Stage) {
  ROOT_STAGE_DISK *Kernel=&Stage->Disks[2];
  if (Kernel->Count!=1 || Kernel->Bytes<0x100000 || Kernel->Bytes>Kernel->Extents[0].Bytes) return;
  mDisplay=(VOID *)(UINTN)(Kernel->Extents[0].Address+Kernel->Bytes-0x100000+MARBLE_SCANOUT_OFFSET);
  ZeroMem((VOID *)mDisplay,sizeof(*mDisplay));mDisplay->magic=MARBLE_SCANOUT_MAGIC;mDisplay->shown=3;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop=NULL;
  if (EFI_ERROR(gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop)) || !Gop->Mode || !Gop->Mode->Info) {mDisplay->error=1;goto Done;}
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info=Gop->Mode->Info;
  mDisplay->base=Gop->Mode->FrameBufferBase;mDisplay->width=Info->HorizontalResolution;
  mDisplay->height=Info->VerticalResolution;mDisplay->stride=Info->PixelsPerScanLine*4;
  mDisplay->span=ALIGN_VALUE((UINT64)mDisplay->stride*mDisplay->height,4096);
  if (mDisplay->base!=0xb8000000ULL || !mDisplay->span || mDisplay->span>0x2b00000ULL/4 || Info->PixelFormat!=PixelBlueGreenRedReserved8BitPerColor) {mDisplay->error=2;goto Done;}
  EFI_STATUS ClockStatus=DisplayRegisterClocks();
  if (EFI_ERROR(ClockStatus)) {mDisplay->error=0x20000000U|(UINT32)(ClockStatus&0xffff);goto Done;}
  UINT32 Layers=0,Layers1=0,Extra=0,Extra1=0,High=0,High1=0,Top=0,Active=0,Version=0;
  if (!DisplayRead(0x16000,&Layers) || !DisplayRead(0x16004,&Layers1) ||
      !DisplayRead(0x16040,&High) || !DisplayRead(0x16044,&High1) ||
      !DisplayRead(0x16070,&Extra) || !DisplayRead(0x16074,&Extra1) ||
      !DisplayRead(0x16014,&Top) || !DisplayRead(0x160f4,&Active) || !DisplayRead(0x1000,&Version)) goto Done;
  mDisplay->version=Version;
  if ((Top&(1U<<17)) || !(Active&6)) {mDisplay->error=3;goto Done;}
  Layers|=Layers1;High|=High1;Extra|=Extra1;
  CONST UINT32 Shifts[6]={0,3,6,26,18,21},HighBits[6]={0,2,4,6,16,18};
  CONST UINT32 FlushBits[8]={0,1,2,18,11,12,24,25};
  for (UINTN I=0;I<8;I++) {
    BOOLEAN Used=I<6 ? (((Layers>>Shifts[I])&7) || (High&(1U<<HighBits[I]))) : ((Extra>>(4*(I-6)))&15);
    if (!Used) continue;
    UINT32 Source=0;
    if (!DisplayRead(mPipe[I]+0x14,&Source)) goto Done;
    if (Source<mDisplay->base || Source>=mDisplay->base+mDisplay->stride*mDisplay->height) continue;
    for (UINTN J=0;J<8;J++) {UINT32 Value=0;if (!DisplayRead(mPipe[I]+mRegisters[J],&Value)) goto Done;mDisplay->registers[I][J]=Value;}
    mPipeOffset[I]=(UINT32)(Source-mDisplay->base);
    mDisplay->pipe_mask|=1U<<I;mDisplay->flush_mask|=1U<<FlushBits[I];
  }
  if (!mDisplay->pipe_mask) {mDisplay->error=4;goto Done;}
  __asm__ volatile("mrs %0,cntfrq_el0":"=r"(mDisplayFrequency));
  mDisplay->ready=1;
Done:
  for (UINTN At=0;At<sizeof(*mDisplay);At+=64) __asm__ volatile("dc cvac,%0"::"r"((UINTN)mDisplay+At):"memory");
  __asm__ volatile("dsb sy":::"memory");
}
VOID MarbleDisplayService(VOID) {
  if (!mDisplay || !mDisplay->ready) return;
  __asm__ volatile("dc ivac,%0\n\tdsb sy"::"r"((UINTN)mDisplay+64):"memory");
  UINT32 Request=mDisplay->requested;
  if (Request==mDisplay->completed) return;
  UINT32 Slot=mDisplay->slot;
  if (Slot>=3 && Slot!=0xffffffffU) {mDisplay->error=5;goto Complete;}
  volatile UINT32 *Mdp=(VOID *)(UINTN)0xae00000;
  UINT32 Address=(UINT32)(mDisplay->base+(Slot<3 ? (Slot+1)*mDisplay->span : 0));
  UINT64 Began=DisplayCounter();
  if (Mdp[0x16018/4]&mDisplay->flush_mask) {mDisplay->error=6;goto Complete;}
  for (UINTN I=0;I<8;I++) if (mDisplay->pipe_mask&(1U<<I)) Mdp[(mPipe[I]+0x14)/4]=Address+mPipeOffset[I];
  __asm__ volatile("dsb sy":::"memory");Mdp[0x16018/4]=mDisplay->flush_mask;
  __asm__ volatile("dsb sy":::"memory");
  while (Mdp[0x16018/4]&mDisplay->flush_mask) {
    if (DisplayCounter()-Began>mDisplayFrequency/20) {mDisplay->error=7;mDisplay->timeouts++;goto Complete;}
    gBS->Stall(50);
  }
  UINT64 Us=(DisplayCounter()-Began)*1000000/mDisplayFrequency;
  if (Us>mDisplay->longest_us) mDisplay->longest_us=Us;
  mDisplay->shown=Slot;mDisplay->presented++;mDisplay->error=0;
Complete:
  mDisplay->completed=Request;DisplayPublish();
}
