#!/usr/bin/env python3
"""Exercise firmware SPMI boot mode programming against ownership and bus failures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'package/MarblePkg/RootBootloaderReset.h').read_text()
source = source[source.index('STATIC BOOLEAN ResetRead'):source.index('STATIC EFI_STATUS ArmRecoveryWatchdog')]
start = source.index('STATIC VOID ResetWrite(')
end = source.index('STATIC BOOLEAN ResetTransfer(', start)
source = source[:start] + '#define ResetWrite ModelWrite\n' + source[end:]
source = source.replace('__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(Frequency));', 'Frequency=1000000;')
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define STATIC static
#define BOOLEAN int
#define VOID void
#define CONST const
#define UINT64 uint64_t
#define UINT32 uint32_t
#define UINTN uintptr_t
#define EFI_STATUS int
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_UNSUPPORTED 1
#define EFI_DEVICE_ERROR 2
#define EFI_NOT_FOUND 3
static uint32_t version,reason,pendingReason;
static int ownerFailure,readFailure,busFailure,readbackFailure,imemWrites,writes;
static uint64_t ticks;
static uint64_t RootCounter(void) {return ++ticks;}
static uintptr_t Reader(void) {return 0xc440000+2*(version>=0x70000000?0x20:0x80);}
static uintptr_t Writer(void) {return 0xc500000+2*(version>=0x70000000?0x1000:0x10000);}
static int MarbleRootReadRegister(uint32_t *out,const void *ptr,size_t n,uint64_t *fault) {
  (void)n;(void)fault;
  uintptr_t at=(uintptr_t)ptr;
  if(readFailure) return 0;
  unsigned map=version>=0x70000000?0x2000:0x900;
  unsigned owner=version>=0x70000000?0:0x700;
  if(at==0xc400000) *out=version;
  else if(at==0xc400000+map+4 || at==0xc400000+map+8) *out=0x7100;
  else if(at>=0xc400000+map && at<0xc400000+map+4096) *out=0;
  else if(at==0xc42d000+owner+4) *out=1;
  else if(at==0xc42d000+owner+8) *out=ownerFailure?1:0;
  else if(at>=0xc42d000+owner && at<0xc42d000+owner+4096) *out=0;
  else if(at==Reader()+8 || at==Writer()+8) *out=busFailure?3:1;
  else if(at==Reader()+24) *out=reason;
  else {assert(0);return 0;}
  return 1;
}
static void ModelWrite(uintptr_t at,uint32_t value) {
  if(at==Writer()+16) {pendingReason=value;++writes;}
  else if(at==Writer()) {assert(value==0x480);if(!readbackFailure)reason=pendingReason;}
  else if(at==Reader()) assert(value==((1u<<27)|0x480));
  else if(at==0x146aa65c) {assert(value==0x77665500);++imemWrites;}
  else assert(0);
}
'''
cases = r'''
int main(void) {
  for(unsigned generation=5;generation<=7;generation+=2) {
    version=generation<<28; reason=0xff; writes=imemWrites=0;
    assert(PrepareBootloaderReset()==EFI_SUCCESS);
    assert(reason==5 && writes==1 && imemWrites==1);
    readbackFailure=1;reason=0xff;writes=imemWrites=0;
    assert(PrepareBootloaderReset()==EFI_DEVICE_ERROR && writes==1 && !imemWrites);
    readbackFailure=0;busFailure=1;writes=imemWrites=0;
    assert(PrepareBootloaderReset()==EFI_DEVICE_ERROR && !writes && !imemWrites);
    busFailure=0;ownerFailure=1;
    assert(PrepareBootloaderReset()==EFI_NOT_FOUND && !writes && !imemWrites);
    ownerFailure=0;readFailure=1;
    assert(PrepareBootloaderReset()==EFI_UNSUPPORTED && !writes && !imemWrites);
    readFailure=0;
  }
  version=0x40000000;
  assert(PrepareBootloaderReset()==EFI_UNSUPPORTED);
}
'''
with tempfile.TemporaryDirectory(prefix='firmware-bootloader-reset-') as temporary:
    directory = Path(temporary)
    program = directory / 'reset.c'
    program.write_text(fixture + source + cases)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(program), '-o', str(directory / 'reset')], check=True)
    subprocess.run([str(directory / 'reset')], check=True)
print('Firmware reset: SPMI v5/v7 ownership, restart-bit preservation, readback, bus and mapping failures passed')
