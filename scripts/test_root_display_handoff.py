#!/usr/bin/env python3
"""Exercise RootDisplay.c across KernelSeed replacement with mapped MMIO fixtures."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'package/MarblePkg/RootDisplay.c').read_text()
source = re.sub(r'^#include .*\n', '', source, flags=re.M)
source = re.sub(r'STATIC UINT64 DisplayCounter\(VOID\) \{.*?\n\}',
                'STATIC UINT64 DisplayCounter(VOID) { return TestCounter(); }', source, flags=re.S)
source = source.replace('__asm__ volatile("mrs %0,cntfrq_el0":"=r"(mDisplayFrequency));',
                        'mDisplayFrequency=19200000;')
source = re.sub(r'__asm__ volatile\(.*?\);', '(void)0;', source, flags=re.S)
preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "MarbleScanoutWire.h"
#define STATIC static
#define VOID void
#define CONST const
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_ERROR(x) ((x)!=0)
#define EFIAPI
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ALIGN_VALUE(a,b) (((a)+(b)-1)&~((b)-1))
typedef uint8_t UINT8;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef int BOOLEAN;
typedef char CHAR8;
typedef int EFI_STATUS;
typedef struct {UINT32 Data1;unsigned short Data2,Data3;UINT8 Data4[8];} EFI_GUID;
typedef struct {UINT64 Address,Bytes;} EXTENT;
typedef struct {UINT64 Bytes;UINT32 Count;EXTENT Extents[1];} DISK;
typedef DISK ROOT_STAGE_DISK;
typedef struct {DISK Disks[3];} ROOT_STAGE_TABLE;
typedef struct {UINT32 HorizontalResolution,VerticalResolution,PixelsPerScanLine,PixelFormat;} INFO;
typedef INFO EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
typedef struct {INFO *Info;UINT64 FrameBufferBase;} MODE;
typedef struct {MODE *Mode;} EFI_GRAPHICS_OUTPUT_PROTOCOL;
#define PixelBlueGreenRedReserved8BitPerColor 1
#define CopyMem(d,s,n) memcpy(d,s,n)
#define ZeroMem(d,n) memset(d,0,n)
static EFI_GUID gEfiGraphicsOutputProtocolGuid={1,0,0,{0}};
static INFO info={1080,2400,1080,1};
static MODE mode={&info,0xb8000000};
static EFI_GRAPHICS_OUTPUT_PROTOCOL gop={&mode};
static UINT64 ticks;
static UINT8 *fixtureMdp,*fixtureSmmu;
static int holdFlush;
static UINT64 TestCounter(void) {
  if(!holdFlush) *(volatile UINT32 *)(uintptr_t)0xae16018=0;
  ticks+=19200;
  return ticks;
}
static int ClockId(void *self,const char *name,UINTN *id){(void)self;(void)name;*id=1;return 0;}
static int ClockEnable(void *self,UINTN id){(void)self;assert(id==1);return 0;}
static struct {UINT64 version;void *id,*enable;} clockProtocol={1,ClockId,ClockEnable};
static int Locate(EFI_GUID *guid,void *registration,void **out) {
  (void)registration;*out=guid->Data1==1?(void *)&gop:(void *)&clockProtocol;return 0;
}
static void Stall(UINTN us){ticks+=us*192/10;}
static struct {int (*LocateProtocol)(EFI_GUID *,void *,void **);void (*Stall)(UINTN);} bs={Locate,Stall};
static typeof(bs) *gBS=&bs;
BOOLEAN MarbleRootReadRegister(UINT32 *out,const void *address,UINTN width,UINT64 *fault) {
  (void)width;(void)fault;*out=*(const volatile UINT32 *)address;return TRUE;
}
'''
cases = r'''
int main(void) {
  fixtureMdp=calloc(1,0xa0000);fixtureSmmu=calloc(1,0xd0000);
  assert(fixtureMdp&&fixtureSmmu);
  volatile UINT32 *mdp=(void *)0xae00000,*smmu=(void *)0x15000000;
  smmu[0x20/4]=1;smmu[0x24/4]=75;
  smmu[0x800/4]=0x80002800;smmu[0xc00/4]=74;
  volatile UINT32 *bank=(void *)(uintptr_t)(0x15000000+(2+74)*4096);
  bank[0x20/4]=0xabc000;bank[0x30/4]=32;
  mdp[0x16000/4]=1;mdp[0x160f4/4]=2;mdp[0x1000/4]=0x80050000;
  mdp[0x5000/4]=0x9600438;mdp[0x5014/4]=0xb8000000;
  mdp[0x5024/4]=4320;mdp[0x5030/4]=0x1234;
  UINT32 saved[8];for(unsigned j=0;j<8;j++)saved[j]=mdp[(0x5000+mRegisters[j])/4];
  UINT8 *first=calloc(1,2*1024*1024),*second=calloc(1,3*1024*1024);assert(first&&second);
  ROOT_STAGE_TABLE stage={0};stage.Disks[2]=(DISK){2*1024*1024,1,{{(UINTN)first,2*1024*1024}}};
  MarbleDisplayPrepare(&stage);assert(mDisplay->ready&&mDisplay->pipe_mask==1&&mDisplayBankCount==1);
  MARBLE_SCANOUT *old=(void *)mDisplay;
  MarbleDisplaySuspend();assert(!MarbleDisplayState());
  memset(old,0xa5,sizeof(*old));
  for(unsigned j=0;j<8;j++)mdp[(0x5000+mRegisters[j])/4]=0xdeadbeef;
  bank[0]=0x61;bank[0x20/4]=0xdead000;bank[0x30/4]=0x800020;
  stage.Disks[2]=(DISK){3*1024*1024,1,{{(UINTN)second,3*1024*1024}}};
  MarbleDisplayPrepare(&stage);
  assert((UINT8 *)mDisplay==second+2*1024*1024+MARBLE_SCANOUT_OFFSET);
  assert(mDisplay->ready&&!mDisplay->error&&!mDisplay->requested&&!mDisplay->completed);
  assert(bank[0]==0&&bank[0x20/4]==0xabc000&&bank[0x30/4]==32);
  for(unsigned j=0;j<8;j++)assert(mdp[(0x5000+mRegisters[j])/4]==saved[j]);
  mDisplay->slot=1;mDisplay->requested=1;MarbleDisplayService();
  assert(mDisplay->completed==1&&mDisplay->presented==1&&!mDisplay->error);
  assert(mdp[0x5014/4]==0xb8000000+2*mDisplay->span);
  MarbleDisplayPrepare(&stage);assert(mDisplay->ready&&mdp[0x5014/4]==0xb8000000);
  holdFlush=1;MarbleDisplayPrepare(&stage);assert(!mDisplay->ready&&mDisplay->error==8);
  holdFlush=0;MarbleDisplayPrepare(&stage);assert(mDisplay->ready&&!mDisplay->error);
  free(first);free(second);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='marble-display-handoff-') as temporary:
    directory = Path(temporary)
    program = directory / 'display.c'
    def fixture_address(match):
        value = int(match[0], 16)
        for base, length, name in ((0xae00000, 0xa0000, 'fixtureMdp'),
                                   (0x15000000, 0xd0000, 'fixtureSmmu')):
            if base <= value < base + length:
                return f'((UINTN){name}+{value-base:#x})'
        return match[0]
    program.write_text(re.sub(r'0x[0-9a-fA-F]+', fixture_address, preamble + source + cases))
    executable = directory / 'display'
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(ROOT / 'package'), str(program), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('RootDisplay: KernelSeed relocation, SMMU and pipe restoration, present, timeout and recovery passed')
