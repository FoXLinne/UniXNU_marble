#!/usr/bin/env python3
"""Exercise the actual loader's retained DRAM reservation and reload contract."""
import argparse
from pathlib import Path
import subprocess
import tempfile

DEVICE = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--loader', required=True, type=Path)
args = parser.parse_args()
source = args.loader.read_text()
start = source.index('STATIC ROOT_STAGE_DRAM_TABLE mRetainedDram;')
end = source.index('STATIC\nEFI_STATUS\nEFIAPI\nBoot (', start)
production = source[start:end]

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "RootStage.h"
typedef unsigned long long UINT64, EFI_PHYSICAL_ADDRESS;
typedef unsigned long UINTN;
typedef unsigned EFI_STATUS;
typedef unsigned char UINT8;
typedef struct { unsigned a,b,c; unsigned char d[8]; } EFI_GUID;
typedef struct { EFI_GUID VendorGuid; void *VendorTable; } CONFIG;
typedef struct { UINTN NumberOfTableEntries; CONFIG ConfigurationTable[1]; } SYSTEM;
static SYSTEM system_table, *gST=&system_table;
#define STATIC static
#define EFI_SUCCESS 0
#define EFI_COMPROMISED_DATA 1
#define EFI_BAD_BUFFER_SIZE 2
#define EFI_OUT_OF_RESOURCES 3
#define EFI_ERROR(v) ((v)!=EFI_SUCCESS)
#define AllocateAddress 0
#define EfiLoaderData 2
#define EFI_SIZE_TO_PAGES(v) ((v)/4096)
#define ZeroMem(p,n) memset((p),0,(n))
#define Print(...) ((void)0)
static unsigned allocations, fail_at;
static EFI_STATUS allocate(unsigned type,unsigned memory,UINTN pages,EFI_PHYSICAL_ADDRESS *address) {
  assert(type==AllocateAddress && memory==EfiLoaderData && pages>0 && (*address%4096)==0);
  allocations++;
  return allocations==fail_at ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
}
static EFI_STATUS install(EFI_GUID *guid,void *table) {
  gST->NumberOfTableEntries=1;
  gST->ConfigurationTable[0]=(CONFIG){*guid,table};
  return EFI_SUCCESS;
}
static struct { EFI_STATUS (*AllocatePages)(unsigned,unsigned,UINTN,EFI_PHYSICAL_ADDRESS *);
                EFI_STATUS (*InstallConfigurationTable)(EFI_GUID *,void *); } services={allocate,install}, *gBS=&services;
static int CompareGuid(const EFI_GUID *a,const EFI_GUID *b) { return !memcmp(a,b,sizeof(*a)); }
static UINT64 DarwinRangeBase(const UINT8 *table,UINTN at) { UINT64 v;memcpy(&v,table+16*at,8);return v; }
static UINT64 DarwinRangeSize(const UINT8 *table,UINTN at) { UINT64 v;memcpy(&v,table+16*at+8,8);return v; }
static void DarwinRangeSet(UINT8 *table,UINTN at,UINT64 base,UINT64 bytes) {
  memcpy(table+16*at,&base,8);memcpy(table+16*at+8,&bytes,8);
}
'''
cases = r'''
int main(void) {
  UINT8 table[64*16]={0}; UINTN count=2; UINT64 bytes=0;
  DarwinRangeSet(table,0,0x80000000,0x100000);
  DarwinRangeSet(table,1,0x90000000,0x200000);
  assert(ReserveResidentDram(table,64,&count,&bytes)==EFI_SUCCESS);
  assert(count==2 && bytes==0x300000 && allocations==2 && gST->NumberOfTableEntries==1);
  memset(table,0,sizeof(table)); count=0;
  assert(ReserveResidentDram(table,64,&count,&bytes)==EFI_SUCCESS);
  assert(count==2 && bytes==0x300000 && allocations==2 && DarwinRangeBase(table,1)==0x90000000);
  assert(ReserveResidentDram(table,1,&count,&bytes)==EFI_COMPROMISED_DATA);
  gST->NumberOfTableEntries=0; allocations=0; fail_at=2; count=2;
  DarwinRangeSet(table,0,0x80000000,0x100000); DarwinRangeSet(table,1,0x90000000,0x200000);
  assert(ReserveResidentDram(table,64,&count,&bytes)==EFI_SUCCESS);
  assert(count==1 && bytes==0x100000 && mRetainedDram.Count==1 && DarwinRangeSize(table,1)==0);
  mRetainedDram.Magic=0;
  assert(ReserveResidentDram(table,64,&count,&bytes)==EFI_COMPROMISED_DATA);
  puts("PASS: reserved DRAM ownership, reload identity, capacity refusal, partial allocation, malformed table");
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='marble-ios27-memory-') as temporary:
    root = Path(temporary)
    c, executable = root / 'reservation.c', root / 'reservation'
    c.write_text(fixture + production + cases)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-I', str(DEVICE / 'package/Protocol'),
                    str(c), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
