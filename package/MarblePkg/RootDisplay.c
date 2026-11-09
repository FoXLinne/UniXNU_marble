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
STATIC MARBLE_SCANOUT mBootDisplay;
STATIC BOOLEAN mBootDisplayReady;
STATIC UINTN mDisplayBanks[2];
STATIC UINT32 mDisplayBankState[2][6];
STATIC UINTN mDisplayBankCount;
STATIC UINT64 DisplayCounter(VOID);
STATIC BOOLEAN mPanelAsleep;
STATIC UINT32 mPanelBrightness=0x133;
STATIC UINT64 mPanelReadAt;
STATIC UINT32 mPanelHz=60;
STATIC VOID PanelPublishState(VOID) {
  UINT32 State=0x80000000U|(mPanelAsleep?1U<<30:0)|(mPanelHz<<16)|mPanelBrightness;
  mDisplay->panel_status[2]=((UINT64)State<<32)|(UINT32)mDisplay->panel_status[2];
}
STATIC VOID PanelImportState(VOID) {
  if (!mDisplay || mDisplay->magic!=MARBLE_SCANOUT_MAGIC) return;
  __asm__ volatile("dc ivac,%0\n\tdsb sy"::"r"(&mDisplay->panel_status[0]):"memory");
  UINT32 State=(UINT32)(mDisplay->panel_status[2]>>32);
  if (!(State&0x80000000U)) return;
  mPanelAsleep=(State&(1U<<30))!=0;mPanelBrightness=State&0xfffU;
  UINT32 Hz=(State>>16)&255U;
  if (Hz==60 || Hz==90 || Hz==120) mPanelHz=Hz;
}
/* Qualcomm dsi_ctrl_copy_and_pad_cmd / kickoff_fifo_command, sheng-u-oss
 * 6461221b68908b990e1fa2b60feaa812cb944ef5. The panel wiring and DCS levels
 * come from marble's m16t_36_0d_0b DT node. */
STATIC BOOLEAN PanelPacketMode(UINT32 Header, UINT32 Payload, UINT32 Length, UINT32 *Response,
  BOOLEAN LowPower, VOID (*Commit)(VOID *), VOID *Context) {
  UINT64 Fault[2]={0};UINT32 Control=0;
  volatile UINT32 *Dsi=(VOID *)(UINTN)0xae94000;
  mDisplay->panel_status[3]=(1ULL<<63)|Header;
  if (!MarbleRootReadRegister(&Control,(CONST VOID *)(UINTN)0xae94004,0,Fault)) return FALSE;
  mDisplay->panel_status[4]=Control;
  if (!(Control&1)) return FALSE;
  UINT32 Dma=Dsi[0x3c/4],Pattern=Dsi[0x15c/4];
  UINT32 Interrupts=Dsi[0x110/4]&0xaaeeaafeU;
  /* Qualcomm clear_error_status: read timeouts and ACK errors are W1C;
   * ACK requires a following zero write. Clear the observed receive errors. */
  UINT32 Ack=Dsi[0x68/4]&0x1193bfffU,Timeout=Dsi[0xc0/4]&0x110U;
  if (Ack) {Dsi[0x68/4]=Ack;__asm__ volatile("dsb sy":::"memory");Dsi[0x68/4]=0;}
  if (Timeout) Dsi[0xc0/4]=Timeout;
  Dsi[4/4]=Control|4U;
  Dsi[0x1ec/4]=1;__asm__ volatile("dsb sy":::"memory");gBS->Stall(1);Dsi[0x1ec/4]=0;
  /* enable_status_interrupts enables CMD_DMA_DONE before kickoff. Its mask
   * can be zero after firmware's last command; preserve the caller's mask. */
  Dsi[0x110/4]=Interrupts|(1U<<1)|(Response?(1U<<21):0)|1U|(1U<<20);
  Dsi[0x15c/4]=0x30006;
  if (Response) {Dsi[0x1d4/4]=1;__asm__ volatile("dsb sy":::"memory");Dsi[0x1d4/4]=0;}
  Dsi[0x17c/4]=Header;
  Dsi[0x17c/4]=Payload;
  Dsi[0x3c/4]=(Dma&~((1U<<31)|(1U<<30)|(1U<<26)))|(1U<<28)|(LowPower?(1U<<26):0);
  Dsi[0x4c/4]=Length;__asm__ volatile("dsb sy":::"memory");Dsi[0x90/4]=1;
  if (Commit) Commit(Context);
  UINT64 Began=DisplayCounter();BOOLEAN Done=FALSE;
  do {
    if ((Dsi[0x110/4]&1U) && (!Response || (Dsi[0x1d4/4]>>16))) {Done=TRUE;break;}
    gBS->Stall(10);
  /* Video mode schedules command DMA inside a frame. Qualcomm's
   * DSI_CTRL_TX_TO_MS permits 200 ms; a 5 ms window can reset a transfer
   * before its scheduled scanline, including the sleep-out command. */
  } while (DisplayCounter()-Began<mDisplayFrequency/5);
  mDisplay->panel_status[3]=Header|(Done?0:(1ULL<<63));
  mDisplay->panel_status[4]=Control|((UINT64)Dma<<32);
  mDisplay->panel_status[5]=((UINT64)Interrupts<<32)|Dsi[0x110/4];
  mDisplay->panel_status[6]=((UINT64)Dsi[0x68/4]<<32)|Dsi[0xc0/4];
  mDisplay->panel_status[7]=((UINT64)Dsi[0x8/4]<<32)|Dsi[0x1d4/4];
  Dsi[0x110/4]=Interrupts|1U|(1U<<20);
  if (Response) *Response=Done ? __builtin_bswap32(Dsi[0x6c/4]) : 0;
  Dsi[0x15c/4]=0;Dsi[0x1ec/4]=1;__asm__ volatile("dsb sy":::"memory");gBS->Stall(1);Dsi[0x1ec/4]=0;
  Dsi[0x15c/4]=Pattern;Dsi[0x3c/4]=Dma;Dsi[4/4]=Control;__asm__ volatile("dsb sy":::"memory");
  return Done;
}
STATIC BOOLEAN PanelPacket(UINT32 Header, UINT32 Payload, UINT32 Length, UINT32 *Response) {
  return PanelPacketMode(Header,Payload,Length,Response,TRUE,NULL,NULL);
}
STATIC BOOLEAN PanelCommand(UINT8 Command, UINT8 First, UINT8 Second, UINT32 Length) {
  /* The selected m16t panel DT encodes power commands as 0x39 long writes
   * with a trailing zero, including 0x11/0x29 and 0x28/0x10. */
  return PanelPacket(0xc0390000U|Length,
    0xff000000U|((Length==3 ? Second : 0xffU)<<16)|(First<<8)|Command,8,NULL);
}
/* Short DCS responses: the controller returns network-byte-order packets.
 * Keep their data type and payload together so a failed read stays observable.
 * DCS 0x0a bit 2 is display-on, bit 4 is sleep-out; 0x52 returns brightness.
 */
STATIC VOID PanelReadState(VOID) {
  UINT32 Brightness=0,Power=0;
  BOOLEAN Max=PanelPacket(0x80370002U,0,4,NULL);
  BOOLEAN B=Max && PanelPacket(0xa0060052U,0,4,&Brightness);
  BOOLEAN P=Max && PanelPacket(0xa006000aU,0,4,&Power);
  mDisplay->panel_status[0]=B ? Brightness : 0;
  mDisplay->panel_status[1]=P ? Power : 0;
  mDisplay->panel_status[2]++;
  mPanelReadAt=DisplayCounter();
}
STATIC BOOLEAN PanelBrightness(UINT32 Level) {
  if (Level<4) Level=4;
  if (Level>4095) Level=4095;
  if (!mPanelAsleep && !PanelCommand(0x51,(UINT8)(Level>>8),(UINT8)Level,3)) return FALSE;
  mPanelBrightness=Level;
  if (!mPanelAsleep && DisplayCounter()-mPanelReadAt>=mDisplayFrequency) PanelReadState();
  return TRUE;
}
STATIC BOOLEAN PanelPower(BOOLEAN On) {
  if (On==!mPanelAsleep) return TRUE;
  if (On) {
    if (!PanelCommand(0x11,0,0,2)) return FALSE;
    gBS->Stall(120000);
    if (!PanelCommand(0x29,0,0,2)) return FALSE;
    mPanelAsleep=FALSE;
    BOOLEAN Done=PanelBrightness(mPanelBrightness);PanelReadState();return Done;
  }
  if (!PanelCommand(0x28,0,0,2)) return FALSE;
  mPanelAsleep=TRUE;
  gBS->Stall(20000);
  /* A sleeping panel can stop answering reads. Read display-off while its
   * command receiver is awake, then enter sleep without a BTA transaction. */
  PanelReadState();
  if (!PanelCommand(0x10,0,0,2)) return FALSE;
  gBS->Stall(120000);return TRUE;
}
typedef struct {
  volatile UINT32 *Mdp;
  UINT32 Ctl;
} PANEL_MODE_COMMIT;
STATIC VOID PanelModeCommit(VOID *Context) {
  PANEL_MODE_COMMIT *Commit=Context;
  Commit->Mdp[(Commit->Ctl+0x110)/4]|=2U;
  __asm__ volatile("dsb sy":::"memory");
  Commit->Mdp[(Commit->Ctl+0x18)/4]|=1U<<31;
  __asm__ volatile("dsb sy":::"memory");
}
STATIC BOOLEAN PanelRefresh(UINT32 Hz) {
  /* M16T PB: dsi_display.c overrides VFP to 2416/812/16 for 60/90/120.
   * sde_encoder_kickoff waits for active scanout and queues the panel's HS
   * FPS gamma command before its CTL kickoff. The DT marks that command
   * asynchronous (0x10), so trigger DMA, commit timing, then await DMA done. */
  UINT32 Vfp=Hz==60?2416:Hz==90?812:Hz==120?16:0;
  if (!Vfp || mPanelAsleep) return FALSE;
  volatile UINT32 *Mdp=(VOID *)(UINTN)0xae00000,*Dsi=(VOID *)(UINTN)0xae94000;
  CONST UINT32 Intf=0x36000,Total=2400+12+4+Vfp;
  UINT32 Htotal=Mdp[(Intf+8)/4]>>16,OldPeriod=Mdp[(Intf+0xc)/4];
  UINT32 OldDsiTotal=Dsi[0x2c/4],OldTotal=(OldDsiTotal>>16)+1;
  UINT32 Active=Dsi[0x28/4],Ctl=0,OldFetch=Mdp[(Intf+0x170)/4];
  for (UINT32 I=0;I<4;I++) if (Mdp[(0x16000+I*0x1000+0xf4)/4]&2U) {Ctl=0x16000+I*0x1000;break;}
  if (!Ctl || !Htotal || !Mdp[Intf/4] || !(Dsi[4/4]&2) ||
      (Active>>16)-(Active&0xffff)!=2400 || OldPeriod!=Htotal*OldTotal) return FALSE;
  /* Preserve the firmware's fetch phase: it starts on line 2417, the
   * second blanking line. Recomputing it from a clamped VFP moved it to
   * line 2416, where this panel's first 120 Hz trial recorded underrun.
   * The inherited start must remain outside active data and in this mode. */
  if ((Mdp[(Intf+4)/4]&(1U<<31)) &&
      (OldFetch<(Active>>16)*Htotal || OldFetch>=Total*Htotal)) return FALSE;
  volatile UINT32 *Status=mDisplay->panel_mode_status;
  Status[0]=Hz;Status[1]=Hz==120?2:Hz==90?1:0;Status[2]=1;
  Status[3]=OldTotal;Status[4]=Total;Status[5]=Mdp[(Intf+0x268)/4];
  Status[10]=Mdp[(Intf+0xac)/4];
  UINT64 Began=DisplayCounter();
  UINT32 ActiveStart=(Active&0xffff),ActiveEnd=Active>>16;
  while (Mdp[(Intf+0xb0)/4]<ActiveStart || Mdp[(Intf+0xb0)/4]>=ActiveEnd) {
    if (DisplayCounter()-Began>mDisplayFrequency/20) {Status[2]=2;return FALSE;}
    gBS->Stall(10);
  }
  UINT32 OldDb=Dsi[0x1e8/4];
  Dsi[0x1e8/4]=1;
  Dsi[0x2c/4]=((Total-1)<<16)|(OldDsiTotal&0xffff);
  Dsi[0x1e4/4]=1;
  Mdp[(Intf+0xc)/4]=Total*Htotal;
  PANEL_MODE_COMMIT Commit={Mdp,Ctl};
  BOOLEAN Done=PanelPacketMode(0xc0390002U,0xffff006cU|(Status[1]<<8),8,NULL,
    FALSE,PanelModeCommit,&Commit);
  Began=DisplayCounter();
  while (Done && ((Mdp[(Ctl+0x18)/4]&(1U<<31)) ||
      (UINT32)(Mdp[(Intf+0xac)/4]-Status[10])<3)) {
    if (DisplayCounter()-Began>mDisplayFrequency/20) {Done=FALSE;break;}
    gBS->Stall(10);
  }
  Status[6]=Mdp[(Intf+0x268)/4];Status[7]=Dsi[8/4];Status[8]=Dsi[0x68/4];
  Status[9]=Mdp[(Ctl+0x18)/4];Status[11]=Mdp[(Intf+0xac)/4];
  if (Status[6]!=Status[5]) Done=FALSE;
  if (Done) {mPanelHz=Hz;Status[2]=0;return TRUE;}
  Status[2]=3;
  Dsi[0x1e8/4]=1;Dsi[0x2c/4]=OldDsiTotal;Dsi[0x1e4/4]=1;
  Mdp[(Intf+0xc)/4]=OldPeriod;Mdp[(Intf+0x170)/4]=OldFetch;
  UINT32 Gamma=mPanelHz==120?2:mPanelHz==90?1:0;
  PanelPacketMode(0xc0390002U,0xffff006cU|(Gamma<<8),8,NULL,FALSE,PanelModeCommit,&Commit);
  Dsi[0x1e8/4]=OldDb;
  return FALSE;
}
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
VOID MarbleDisplaySuspend(VOID) {PanelImportState();mDisplay=NULL;}
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
  __asm__ volatile("dc cvac,%0"::"r"((UINTN)&mDisplay->panel_mode_status[0]):"memory");
  __asm__ volatile("dc cvac,%0"::"r"((UINTN)&mDisplay->panel_status[0]):"memory");
  __asm__ volatile("dc cvac,%0\n\tdsb sy"::"r"((UINTN)mDisplay):"memory");
}
STATIC BOOLEAN DisplayReadPhysical(UINTN Address,UINT32 *Value) {
  UINT64 Fault[2]={0};
  return MarbleRootReadRegister(Value,(VOID *)Address,0,Fault);
}
STATIC BOOLEAN DisplaySaveSmmu(VOID) {
  mDisplayBankCount=0;
  UINT32 Idr0=0,Idr1=0;
  if (!DisplayReadPhysical(0x15000020,&Idr0) || !DisplayReadPhysical(0x15000024,&Idr1)) return FALSE;
  UINTN Groups=MIN((UINTN)(Idr0&255),128U),Banks=Idr1&255;
  UINTN Pages=1U<<(((Idr1>>28)&7)+1);
  CONST UINT32 Offsets[6]={0,0x20,0x24,0x10,0x30,0x38};
  for (UINTN I=0;I<Groups;I++) {
    UINT32 Smr=0,S2cr=0;
    if (!DisplayReadPhysical(0x15000800+4*I,&Smr)) return FALSE;
    if (!(Smr&0x80000000U)) continue;
    UINT32 Mask=(Smr>>16)&0x7fff;
    if (((Smr^0x2800U)&0x7fff&~Mask) && ((Smr^0x2c00U)&0x7fff&~Mask)) continue;
    if (!DisplayReadPhysical(0x15000c00+4*I,&S2cr)) return FALSE;
    if (((S2cr>>16)&3)!=0 || (S2cr&255)>=Banks) continue;
    UINTN Bank=0x15000000+(Pages+(S2cr&255))*4096;
    if (mDisplayBankCount && mDisplayBanks[0]==Bank) continue;
    if (mDisplayBankCount==2) return FALSE;
    mDisplayBanks[mDisplayBankCount]=Bank;
    for (UINTN J=0;J<6;J++)
      if (!DisplayReadPhysical(Bank+Offsets[J],&mDisplayBankState[mDisplayBankCount][J])) return FALSE;
    mDisplayBankCount++;
  }
  return TRUE;
}
STATIC BOOLEAN DisplayRestore(VOID) {
  if (mPanelAsleep && !PanelPower(TRUE)) return FALSE;
  CONST UINT32 Offsets[6]={0,0x20,0x24,0x10,0x30,0x38};
  /* The boot snapshot belongs to the firmware, outside replaceable KernelSeed. */
  for (UINTN I=0;I<mDisplayBankCount;I++) {
    volatile UINT32 *Bank=(VOID *)mDisplayBanks[I];
    Bank[0]=mDisplayBankState[I][0];
    __asm__ volatile("dsb sy\n\tisb":::"memory");
    for (UINTN J=1;J<6;J++) Bank[Offsets[J]/4]=mDisplayBankState[I][J];
    UINT32 Fault=Bank[0x58/4];
    if (Fault&(1U<<30)) Bank[0x8/4]=1;
    if (Fault&0xc00001feU) Bank[0x58/4]=Fault;
    Bank[0x618/4]=0;Bank[0x7f0/4]=0;
    __asm__ volatile("dsb sy":::"memory");
    UINT64 Began=DisplayCounter();
    while (Bank[0x7f4/4]&1) if (DisplayCounter()-Began>mDisplayFrequency/20) return FALSE;
  }
  volatile UINT32 *Mdp=(VOID *)(UINTN)0xae00000;
  for (UINTN I=0;I<8;I++) if (mDisplay->pipe_mask&(1U<<I))
    for (UINTN J=0;J<8;J++) Mdp[(mPipe[I]+mRegisters[J])/4]=mDisplay->registers[I][J];
  __asm__ volatile("dsb sy":::"memory");Mdp[0x16018/4]=mDisplay->flush_mask;
  __asm__ volatile("dsb sy":::"memory");
  UINT64 Began=DisplayCounter();
  while (Mdp[0x16018/4]&mDisplay->flush_mask)
    if (DisplayCounter()-Began>mDisplayFrequency/20) return FALSE;
  return TRUE;
}
VOID MarbleDisplayPrepare(ROOT_STAGE_TABLE *Stage) {
  PanelImportState();
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
  if (mBootDisplayReady) {
    CopyMem((VOID *)mDisplay,&mBootDisplay,sizeof(*mDisplay));
    mDisplay->ready=0;
    if (!DisplayRestore()) {mDisplay->error=8;goto Done;}
    mDisplay->ready=1;
    goto Done;
  }
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
  if (!DisplaySaveSmmu()) {mDisplay->error=9;goto Done;}
  mDisplay->ready=1;
  {
    UINT64 Fault[2]={0};UINT32 DsiControl=0;
    if (MarbleRootReadRegister(&DsiControl,(CONST VOID *)(UINTN)0xae94004,0,Fault) && (DsiControl&1)) {
      mDisplay->reserved[4]=MARBLE_PANEL_DCS_CAP;
      mDisplay->reserved[3]=MARBLE_PANEL_REFRESH_CAP;
    }
  }
  CopyMem(&mBootDisplay,(CONST VOID *)mDisplay,sizeof(mBootDisplay));
  mBootDisplayReady=TRUE;
Done:
  if (mDisplay) PanelPublishState();
  for (UINTN At=0;At<sizeof(*mDisplay);At+=64) __asm__ volatile("dc cvac,%0"::"r"((UINTN)mDisplay+At):"memory");
  __asm__ volatile("dsb sy":::"memory");
}
VOID MarbleDisplayService(VOID) {
  if (!mDisplay || !mDisplay->ready) return;
  __asm__ volatile("dc ivac,%0\n\tdsb sy"::"r"((UINTN)mDisplay+64):"memory");
  UINT32 Request=mDisplay->requested;
  if (Request==mDisplay->completed) return;
  UINT32 Slot=mDisplay->slot;
  if (Slot==MARBLE_PANEL_BRIGHTNESS) {
    mDisplay->error=PanelBrightness((UINT32)mDisplay->reserved[0]) ? 0 : 0x30000001U;
    goto Complete;
  }
  if (Slot==MARBLE_PANEL_POWER) {
    mDisplay->error=PanelPower(mDisplay->reserved[0]!=0) ? 0 : 0x30000002U;
    goto Complete;
  }
  if (Slot==MARBLE_PANEL_REFRESH) {
    mDisplay->error=PanelRefresh((UINT32)mDisplay->reserved[0]) ? 0 : 0x30000003U;
    goto Complete;
  }
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
  PanelPublishState();
  mDisplay->completed=Request;DisplayPublish();
}
