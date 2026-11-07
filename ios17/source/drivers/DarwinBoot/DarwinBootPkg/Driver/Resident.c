// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/RootStage.h>

UINT64 DarwinResidentContext[48] __attribute__((aligned(64)));
UINT64 DarwinResidentFaultInfo[16] __attribute__((aligned(64)));
VOID DarwinResidentVectors(VOID);
VOID DarwinResidentFault(VOID);
UINTN DarwinResidentSave(VOID) __attribute__((returns_twice));
VOID DarwinResidentReturn(VOID);
VOID DarwinEnter(UINT64,UINT64,UINT64,UINT64,UINT64,UINT64);

// MiCode marble-s-oss drivers/virt/gunyah/gh_virt_wdt.c, vendor-hypervisor SMCCC32 calls.
#define VM_WDT_CONTROL 0x86000005U
#define VM_WDT_STATUS  0x86000006U
#define VM_WDT_PET     0x86000007U
#define VM_WDT_TIME    0x86000008U
STATIC BOOLEAN mWatchdogArmed;
STATIC BOOLEAN mInterruptsSaved;
STATIC UINT64 *mReturnTables;

STATIC VOID *KernelPointer(UINT64 Address, UINTN Bytes, UINT64 Base, UINT64 Size, UINT64 Virtual)
{
  UINTN Bits=64-((DarwinResidentFaultInfo[15]>>16)&63);
  if (Bits<25 || Bits>48) Bits=39;
  UINT64 Mask=(1ULL<<Bits)-1;
  UINT64 WalkAddress=Address&Mask;
  UINT64 Table=DarwinResidentFaultInfo[14]&0x0000fffffffff000ULL;
  UINTN First=4-(Bits-12+8)/9;
  for (UINTN Level=First;Level<4;Level++) {
    if (Table<Base || Table-Base>Size-EFI_PAGE_SIZE) return NULL;
    UINTN Shift=12+9*(3-Level);
    UINT64 Entry=((UINT64 *)(UINTN)Table)[(WalkAddress>>Shift)&511];
    if (!(Entry&1)) return NULL;
    if (Level==3 || (Entry&3)==1) {
      UINT64 BlockMask=(1ULL<<Shift)-1;
      UINT64 Physical=(Entry&0x0000fffffffff000ULL&~BlockMask)|(WalkAddress&BlockMask);
      if (Physical<Base || Bytes>Size || Physical-Base>Size-Bytes) return NULL;
      return (VOID *)(UINTN)Physical;
    }
    Table=Entry&0x0000fffffffff000ULL;
  }
  return NULL;
}

STATIC BOOLEAN ReadKernel(UINT64 Address, VOID *Buffer, UINTN Bytes, UINT64 Base, UINT64 Size, UINT64 Virtual)
{
  UINTN Done=0;
  while (Done<Bytes) {
    UINTN Count=MIN(Bytes-Done,EFI_PAGE_SIZE-((Address+Done)&EFI_PAGE_MASK));
    VOID *Source=KernelPointer(Address+Done,Count,Base,Size,Virtual);
    if (!Source) return FALSE;
    CopyMem((UINT8 *)Buffer+Done,Source,Count);
    Done+=Count;
  }
  return TRUE;
}

STATIC VOID DumpKernelString(CONST CHAR16 *Name, UINT64 Address, UINT64 Base, UINT64 Size, UINT64 Virtual)
{
  CHAR8 Text[256];
  if (!ReadKernel(Address,Text,sizeof(Text)-1,Base,Size,Virtual)) return;
  Text[sizeof(Text)-1]=0;
  Print(L"Resident %s: %a\r\n",Name,Text);
}

STATIC VOID DumpKernelFault(UINT64 Base, UINT64 Size, UINT64 Args)
{
  UINT64 Virtual=*(UINT64 *)(UINTN)(Args+8);
  Print(L"Resident x0=%lx x1=%lx x2=%lx x3=%lx\r\n",DarwinResidentFaultInfo[3],
      DarwinResidentFaultInfo[4],DarwinResidentFaultInfo[5],DarwinResidentFaultInfo[6]);
  Print(L"Resident x4=%lx x5=%lx x6=%lx x7=%lx\r\n",DarwinResidentFaultInfo[7],
      DarwinResidentFaultInfo[8],DarwinResidentFaultInfo[9],DarwinResidentFaultInfo[10]);
  DumpKernelString(L"message",DarwinResidentFaultInfo[4],Base,Size,Virtual);
  DumpKernelString(L"format",DarwinResidentFaultInfo[5],Base,Size,Virtual);
  UINT64 Arguments[8],Values[8];
  if (ReadKernel(DarwinResidentFaultInfo[6],Arguments,sizeof(Arguments),Base,Size,Virtual)) {
    Print(L"Resident arguments: %lx %lx %lx %lx %lx %lx %lx %lx\r\n",
        Arguments[0],Arguments[1],Arguments[2],Arguments[3],Arguments[4],Arguments[5],Arguments[6],Arguments[7]);
    if (ReadKernel(Arguments[0],Values,sizeof(Values),Base,Size,Virtual)) {
      Print(L"Resident varargs: %lx %lx %lx %lx %lx %lx %lx %lx\r\n",
          Values[0],Values[1],Values[2],Values[3],Values[4],Values[5],Values[6],Values[7]);
      for (UINTN Index=0;Index<4;Index++) DumpKernelString(L"argument string",Values[Index],Base,Size,Virtual);
    }
  }
  Print(L"Resident fp=%lx lr=%lx sp=%lx\r\n",DarwinResidentFaultInfo[11],
      DarwinResidentFaultInfo[12],DarwinResidentFaultInfo[13]);
  Print(L"Resident ttbr1=%lx tcr=%lx\r\n",DarwinResidentFaultInfo[14],DarwinResidentFaultInfo[15]);
  UINT64 Frame=DarwinResidentFaultInfo[11];
  for (UINTN Index=0;Index<16;Index++) {
    UINT64 Record[2];
    if (!ReadKernel(Frame,Record,sizeof(Record),Base,Size,Virtual)) break;
    Print(L"Resident frame %u: fp=%lx lr=%lx\r\n",Index,Record[0],Record[1]);
    if (Record[0]==Frame) break;
    Frame=Record[0];
  }
}

STATIC EFI_STATUS PrepareProbeControl(UINT64 Base, UINT64 End, UINT64 Args)
{
  for (UINT64 Address=Base;Address+40<=End;Address+=8) {
    UINT64 *Control=(VOID *)(UINTN)Address;
    if (Control[0]!=0x5250454c4252414dULL || Control[1]!=0x315347524145424fULL) continue;
    if (!mReturnTables) mReturnTables=AllocatePages(2);
    if (!mReturnTables) return EFI_OUT_OF_RESOURCES;
    ZeroMem(mReturnTables,2*EFI_PAGE_SIZE);
    UINT64 Fault=(UINTN)&DarwinResidentFault, Return=(UINTN)&DarwinResidentReturn;
    UINT64 Block=Fault&~0x1fffffULL;
    if ((Return&~0x1fffffULL)!=Block || ((UINTN)DarwinResidentContext&~0x1fffffULL)!=Block)
      return EFI_UNSUPPORTED;
    mReturnTables[(Fault>>30)&511]=(UINTN)(mReturnTables+512)|3;
    mReturnTables[512+((Fault>>21)&511)]=Block|0x601;
    Control[0]=Args;
    Control[1]=*(UINT64 *)(UINTN)(Args+8)+Args-Base;
    Control[2]=Fault;
    Control[3]=Return;
    Control[4]=(UINTN)mReturnTables;
    Print(L"Resident probe control=%lx return-table=%lx\r\n",Address,Control[4]);
    return EFI_SUCCESS;
  }
  return EFI_SUCCESS;
}
STATIC UINT64 VmWatchdogCall(UINT32 Function, UINT32 Arg1, UINT32 Arg2, UINT64 *State)
{
  register UINT64 X0 __asm__("x0")=Function;
  register UINT64 X1 __asm__("x1")=Arg1;
  register UINT64 X2 __asm__("x2")=Arg2;
  __asm__ volatile("smc #0" : "+r"(X0), "+r"(X1), "+r"(X2) ::
      "x3","x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
  if (State) *State=X1;
  return X0;
}
STATIC EFI_STATUS ArmVmWatchdog(VOID)
{
  UINT64 Control=0, Status=VmWatchdogCall(VM_WDT_STATUS,0,0,&Control);
  if (Status) return EFI_DEVICE_ERROR;
  Status=VmWatchdogCall(VM_WDT_CONTROL,2,0,NULL);
  if (Status && !(Control&1)) Status=0;
  if (!Status) Status=VmWatchdogCall(VM_WDT_TIME,32000,32000,NULL);
  if (!Status) Status=VmWatchdogCall(VM_WDT_CONTROL,3,0,NULL);
  if (!Status) Status=VmWatchdogCall(VM_WDT_PET,0,0,NULL);
  Print(L"VM watchdog bark=32000 bite=32000 status=%lx\r\n",Status);
  mWatchdogArmed=Status==0;
  return Status ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
STATIC VOID DisarmVmWatchdog(VOID)
{
  if (mWatchdogArmed) {
    VmWatchdogCall(VM_WDT_CONTROL,2,0,NULL);
    mWatchdogArmed=FALSE;
  }
}

#define DIST 0x17100000UL
#define REDIST 0x17190000UL
STATIC UINT32 mControl, mEnabled[32], mConfig[64];
STATIC UINT8 mPriority[1024], mLocalPriority[32];
STATIC UINT64 mRoutes[1024];
STATIC UINT32 mLocalEnabled, mLocalConfig, mLocalGroup, mLocalGroupModifier, mBanks;
// Use base-only MMIO instructions; firmware traps on post-indexed accesses.
STATIC UINT32 Read32(UINTN Address) {
  UINT32 Value;
  __asm__ volatile("ldr %w0, [%1]" : "=r"(Value) : "r"(Address) : "memory");
  return Value;
}
STATIC VOID Write32(UINTN Address, UINT32 Value) {
  __asm__ volatile("str %w1, [%0]" :: "r"(Address), "r"(Value) : "memory");
}
STATIC UINT8 Read8(UINTN Address) {
  UINT32 Value;
  __asm__ volatile("ldrb %w0, [%1]" : "=r"(Value) : "r"(Address) : "memory");
  return (UINT8)Value;
}
STATIC VOID Write8(UINTN Address, UINT8 Value) {
  __asm__ volatile("strb %w1, [%0]" :: "r"(Address), "r"((UINT32)Value) : "memory");
}
STATIC UINT64 Read64(UINTN Address) {
  UINT64 Value;
  __asm__ volatile("ldr %0, [%1]" : "=r"(Value) : "r"(Address) : "memory");
  return Value;
}
STATIC VOID Write64(UINTN Address, UINT64 Value) {
  __asm__ volatile("str %1, [%0]" :: "r"(Address), "r"(Value) : "memory");
}

STATIC VOID SaveInterrupts(VOID)
{
  mControl=Read32(DIST);
  mBanks=MIN((Read32(DIST+4)&31)+1,32);
  for (UINTN i=1;i<mBanks;i++) {
    mEnabled[i]=Read32(DIST+0x100+4*i);
  }
  // Preserve the firmware's active SPIs; unused IDs can belong to secure firmware.
  for (UINTN i=32;i<MIN(mBanks*32,1020);i++) {
    if ((mEnabled[i/32] & (1U << (i%32))) == 0) continue;
    mPriority[i]=Read8(DIST+0x400+i);
    mConfig[i/16]=Read32(DIST+0xc00+4*(i/16));
    mRoutes[i]=Read64(DIST+0x6000+8*i);
  }
  mLocalEnabled=Read32(REDIST+0x100);
  mLocalGroup=Read32(REDIST+0x80);
  mLocalGroupModifier=Read32(REDIST+0xd00);
  for (UINTN i=0;i<32;i++)
    if (mLocalEnabled & (1U << i)) mLocalPriority[i]=Read8(REDIST+0x400+i);
  mLocalConfig=Read32(REDIST+0xc04);
}

STATIC VOID RestoreInterrupts(VOID)
{
  for (UINTN i=1;i<mBanks;i++) {
    // Retire kernel sources before restoring the firmware's interrupt handlers.
    UINT32 Enabled=Read32(DIST+0x100+4*i)|mEnabled[i];
    if (!Enabled) continue;
    Write32(DIST+0x180+4*i,Enabled);
    Write32(DIST+0x280+4*i,Enabled);
    Write32(DIST+0x380+4*i,Enabled);
  }
  for (UINTN i=32;i<MIN(mBanks*32,1020);i++) {
    if ((mEnabled[i/32] & (1U << (i%32))) == 0) continue;
    Write8(DIST+0x400+i,mPriority[i]);
    Write32(DIST+0xc00+4*(i/16),mConfig[i/16]);
    Write64(DIST+0x6000+8*i,mRoutes[i]);
  }
  for (UINTN i=1;i<mBanks;i++)
    if (mEnabled[i]) Write32(DIST+0x100+4*i,mEnabled[i]);
  UINT32 LocalEnabled=Read32(REDIST+0x100)|mLocalEnabled;
  Write32(REDIST+0x180,LocalEnabled); Write32(REDIST+0x280,LocalEnabled); Write32(REDIST+0x380,LocalEnabled);
  Write32(REDIST+0x80,mLocalGroup); Write32(REDIST+0xd00,mLocalGroupModifier);
  for (UINTN i=0;i<32;i++)
    if (mLocalEnabled & (1U << i)) Write8(REDIST+0x400+i,mLocalPriority[i]);
  Write32(REDIST+0xc04,mLocalConfig); Write32(REDIST+0x100,mLocalEnabled);
  Write32(DIST,mControl);
  __asm__ volatile("dsb sy\n\tisb" ::: "memory");
}

EFI_STATUS DarwinResidentRun(ROOT_STAGE_TABLE *Stage, UINT64 Args, UINT64 Entry,
    UINT64 Base, UINT64 Size, UINT64 Video, UINT64 VideoBytes,
    EFI_MEMORY_DESCRIPTOR *Map, UINTN MapSize, UINTN Stride)
{
  ROOT_STAGE_SERVICE *Service=NULL;
  EFI_GUID ServiceGuid=ROOT_STAGE_SERVICE_GUID;
  for (UINTN Index=0;Index<gST->NumberOfTableEntries;Index++) {
    if (CompareGuid(&ServiceGuid,&gST->ConfigurationTable[Index].VendorGuid)) {
      Service=gST->ConfigurationTable[Index].VendorTable;
      if (!Service || Service->Magic!=ROOT_STAGE_SERVICE_MAGIC || !Service->Enter || !Service->Resume)
        return EFI_COMPROMISED_DATA;
      break;
    }
  }
  EFI_STATUS Status=EFI_SUCCESS;
  if (Service) {
    VmWatchdogCall(VM_WDT_CONTROL,2,0,NULL);
    mWatchdogArmed=FALSE;
  } else {
    Status=ArmVmWatchdog();
    if (EFI_ERROR(Status)) return Status;
  }
  Stage->ReturnStub=(UINTN)&DarwinResidentReturn;
  UINT64 LoadedEnd=*(UINT64 *)(UINTN)(Args+0x20);
  Status=PrepareProbeControl(Base,LoadedEnd,Args);
  if (EFI_ERROR(Status)) { DisarmVmWatchdog(); return Status; }
  *(UINT64 *)(UINTN)(Args+0x800)=(UINTN)&DarwinResidentFault;
  *(UINT64 *)(UINTN)(Args+0x808)=Stage->ReturnStub;
  Print(L"Resident run=%p save=%p return=%p\r\n", (VOID *)(UINTN)&DarwinResidentRun,
      (VOID *)(UINTN)&DarwinResidentSave, (VOID *)(UINTN)&DarwinResidentReturn);
  ZeroMem(DarwinResidentFaultInfo,sizeof(DarwinResidentFaultInfo));
  mInterruptsSaved=FALSE;
  if (DarwinResidentSave()==0) {
    __asm__ volatile("msr daifset, #15" ::: "memory");
    // Preserve the return frame before accessing interrupt-controller registers.
    UINT64 Stack;
    __asm__ volatile("mov %0, sp" : "=r"(Stack));
    for (UINT64 P=(Stack-0x2000)&~63ULL;P<Stack+0x2000;P+=64)
      __asm__ volatile("dc cvac, %0" :: "r"(P) : "memory");
    for (UINT64 P=(UINTN)DarwinResidentContext;P<(UINTN)(DarwinResidentContext+48);P+=64)
      __asm__ volatile("dc cvac, %0" :: "r"(P) : "memory");
    __asm__ volatile("dsb sy\n\tmsr vbar_el1, %0\n\tisb" :: "r"((UINTN)&DarwinResidentVectors) : "memory");
    SaveInterrupts();
    mInterruptsSaved=TRUE;
    // The kernel owns only its allocated window. Retained firmware must reach RAM.
    for (UINTN i=0;i<MapSize;i+=Stride) {
      EFI_MEMORY_DESCRIPTOR *D=(VOID *)((UINT8 *)Map+i);
      if ((D->Type>=EfiLoaderCode && D->Type<=EfiRuntimeServicesData) || D->Type==EfiACPIReclaimMemory || D->Type==EfiACPIMemoryNVS) {
        for (UINT64 P=D->PhysicalStart;P<D->PhysicalStart+EFI_PAGES_TO_SIZE(D->NumberOfPages);P+=64) {
          if (P>=Base && P<Base+Size) {
            P=Base+Size-64;
            continue;
          }
          __asm__ volatile("dc cvac, %0" :: "r"(P) : "memory");
        }
        VmWatchdogCall(VM_WDT_PET,0,0,NULL);
      }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    VmWatchdogCall(VM_WDT_PET,0,0,NULL);
    if (Service) {
      Status=((EFI_STATUS (*)(VOID))(UINTN)Service->Enter)();
      if (EFI_ERROR(Status)) { DisarmVmWatchdog(); return Status; }
    }
    DarwinEnter(Args,Stage->ReturnTest ? Stage->ReturnStub : Entry,Base,LoadedEnd-Base,Video,VideoBytes);
  }
  if (Service) ((VOID (*)(VOID))(UINTN)Service->Resume)();
  DisarmVmWatchdog();
  if (!DarwinResidentFaultInfo[1] && DarwinResidentFaultInfo[3])
    DumpKernelString(L"panic",DarwinResidentFaultInfo[3],Base,Size,*(UINT64 *)(UINTN)(Args+8));
  if (DarwinResidentFaultInfo[1]) {
    Print(L"Resident fault ESR=%lx PC=%lx FAR=%lx\r\n",
        DarwinResidentFaultInfo[0],DarwinResidentFaultInfo[1],DarwinResidentFaultInfo[2]);
    DumpKernelFault(Base,Size,Args);
  }
  if (mInterruptsSaved) RestoreInterrupts();
  Stage->ReturnTest=0;
  Stage->ReturnCount++;
  __asm__ volatile("msr daif, %0" :: "r"(DarwinResidentContext[8]) : "memory");
  return EFI_SUCCESS;
}
