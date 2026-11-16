#!/usr/bin/env python3
"""Exercise the actual RAM kernel selection with mailbox and DSP retention."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'package/MarblePkg/RootStorage.c').read_text()
source=source[source.index('EFI_STATUS MarbleRootStoragePatch('):]
fixture=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include "RootStage.h"
#define EFI_STATUS int
#define VOID void
#define CONST const
#define UINT8 uint8_t
#define UINT64 uint64_t
#define UINTN uintptr_t
#define EFI_SUCCESS 0
#define EFI_COMPROMISED_DATA 1
#define EFI_BAD_BUFFER_SIZE 2
#define CompareMem memcmp
#define ZeroMem(p,n) memset(p,0,n)
#define CopyMem memcpy
#define Print(...) ((void)0)
static ROOT_STAGE_TABLE table,*mStorage=&table;
static uint64_t ReadUnaligned64(const uint64_t *p) {uint64_t v;memcpy(&v,p,8);return v;}
static unsigned char data[0x101000],patch[132],before[sizeof(data)];
static void set64(void *p,uint64_t v) {memcpy(p,&v,8);}
static void reset(void) {
 memset(data,0xa5,sizeof(data));memcpy(data+4096,"ROOTLOG1",8);
 set64(data+4096+104,256);
 memset(patch,0,sizeof(patch));memcpy(patch,"KPATCH01",8);
 set64(patch+8,64);set64(patch+16,4);
 memset(patch+24,0x42,32);memcpy(patch+64,"-v\0",4);memset(patch+68,0xcc,64);
 table.Disks[2].Count=1;table.Disks[2].Bytes=sizeof(data);
 table.Disks[2].Extents[0].Address=(uintptr_t)data;
 memcpy(before,data,sizeof(data));
}
'''
cases=r'''
int main(void) {
 reset();assert(MarbleRootStoragePatch(patch,sizeof(patch))==EFI_SUCCESS);
 assert(!memcmp(data,patch+68,64));for(unsigned i=64;i<256;i++) assert(!data[i]);
 assert(!memcmp(data+256,before+256,4096-256));
 assert(!memcmp(data+4096,before+4096,128));
 assert(!memcmp(data+4096+128,"-v\0",4));
 assert(!memcmp(data+4096+1128,before+4096+1128,sizeof(data)-4096-1128));
 assert(!strcmp(table.Disks[2].Identity,"42424242424242424242424242424242"));
 reset();patch[0]=0;assert(MarbleRootStoragePatch(patch,sizeof(patch))==EFI_COMPROMISED_DATA);
 assert(!memcmp(data,before,sizeof(data)));
 reset();set64(patch+16,1001);assert(MarbleRootStoragePatch(patch,sizeof(patch))==EFI_BAD_BUFFER_SIZE);
 assert(!memcmp(data,before,sizeof(data)));
 reset();set64(data+4096+104,32);memcpy(before,data,sizeof(data));
 assert(MarbleRootStoragePatch(patch,sizeof(patch))==EFI_COMPROMISED_DATA);
 assert(!memcmp(data,before,sizeof(data)));
 reset();patch[67]='x';assert(MarbleRootStoragePatch(patch,sizeof(patch))==EFI_BAD_BUFFER_SIZE);
}
'''
with tempfile.TemporaryDirectory(prefix='root-storage-overlay-') as temporary:
 p=Path(temporary);(p/'test.c').write_text(fixture+source+cases)
 subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(root/'package/Protocol'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('RAM kernel overlay: selected bytes, arguments, DSP/Data retention and malformed payload refusals passed')
