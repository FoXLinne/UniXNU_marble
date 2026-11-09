#!/usr/bin/env python3
"""Managed RAM debug reads must use the existing physical map without VM remaps."""
from pathlib import Path
import subprocess
import tempfile

device=Path(__file__).resolve().parents[1]
source=(device/'package/MarbleKernelUsb.inc').read_text()
functions=source[source.index('static bool rootRange('):source.index('static void rootSessionReset(')]
program=r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
#include "Protocol/RootStage.h"
static uint8_t bytes[65536];
static unsigned mappings;
static ROOT_STAGE_TABLE stage;
static const ROOT_STAGE_TABLE *native_resident_table(){return &stage;}
static uintptr_t ml_static_ptovirt(uint64_t physical) {
 return physical>=0x100000&&physical<0x110000?reinterpret_cast<uintptr_t>(bytes)+physical-0x100000:0;
}
struct IOMemoryMap {uintptr_t getVirtualAddress(){return reinterpret_cast<uintptr_t>(bytes);}};
struct IOMemoryDescriptor {
 static IOMemoryDescriptor *withAddressRange(uint64_t,unsigned,int,void *) {++mappings;static IOMemoryDescriptor m;return &m;}
 IOMemoryMap *map(int) {static IOMemoryMap m;return &m;}
};
static IOMemoryMap *rootExport;
static const uint8_t *rootExportPointer;
static unsigned rootExportBytes;
enum {kIODirectionIn=1,kIOMemoryMapperNone=2,kIOMapAnywhere=4,kIOMapCopybackCache=8};
#define OSSafeReleaseNULL(p) ((p)=nullptr)
'''
cases=r'''
int main(){
 stage.KernelBase=0x100000;stage.KernelBytes=65536;
 for(unsigned i=0;i<128;++i){assert(rootSelect(0x100000+(i%16)*4096,4096));assert(rootExportPointer==bytes+(i%16)*4096);}
 assert(!mappings&&rootExportBytes==4096);
 assert(!rootSelect(0x100000,0));assert(!rootSelect(0x100000,0x400001));
 assert(!rootSelect(UINT64_MAX-1,8));assert(!rootSelect(0x110000,4096));assert(!mappings);
 stage.Disks[2].Count=1;stage.Disks[2].Bytes=0x200000;stage.Disks[2].Extents[0].Address=0x800000;
 assert(rootSelect(0x900000,4096));assert(mappings==1&&rootExportPointer==bytes);
}
'''
with tempfile.TemporaryDirectory(prefix='native-usb-physmap-') as temporary:
    path=Path(temporary);(path/'test.cpp').write_text(program+functions+cases)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(device/'package'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Native USB RAM reads: existing physical map, repeat reads, bounds and separate firmware output passed')
