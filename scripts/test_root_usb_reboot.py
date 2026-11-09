#!/usr/bin/env python3
"""Exercise the firmware reboot command with successful and failed USB replies."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'package/MarblePkg/RootUsb.c').read_text()
reply = source[source.index('STATIC BOOLEAN Reply('):source.index('STATIC VOID Hex(')]
start = source.index('    else if (!AsciiStrCmp(Command,"reboot-bootloader")) {')
end = source.index('    else if (!AsciiStrCmp(Command,"getvar:watchdog"))', start)
command = source[start:end]
command = 'if (0) {}\n' + command
command = command.replace(
    '__asm__ volatile("dc cvac, %0\\n\\tdsb sy" :: "r"((UINTN)&mStage->ReturnTest) : "memory");',
    'published=1;')
preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define STATIC static
#define VOID void
#define CONST const
#define BOOLEAN int
#define UINTN size_t
#define INTN long
#define UINT32 uint32_t
#define CHAR8 char
#define EP_IN 0x81
#define AsciiStrLen strlen
#define CopyMem memcpy
static struct {uint64_t ReturnTest;} stage;
static int mKernelRunning, published, transferred, fail, pending;
static const char *Command;
#define AsciiStrCmp strcmp
static unsigned UsbControl(void) { return pending; }
static void PublishControl(unsigned value) { assert(transferred && !fail); published=value; }
static char mTx[512];
static long Transfer(unsigned endpoint, void *buffer, size_t size) {
    assert(endpoint==EP_IN && buffer==mTx && size>4);
    assert(!published);
    transferred=1;
    return fail ? -1 : (long)size;
}
'''
cases = r'''
static void run(void) {
    for (unsigned attempt=0;attempt<1;++attempt) {
'''
ending = r'''
    }
}
int main(void) {
    Command="reboot-bootloader"; mKernelRunning=1;
    stage.ReturnTest=UINT64_C(0x80000005)<<32;
    run();
    assert(transferred && published==2);
    assert(stage.ReturnTest>>32==UINT32_C(0x80000005));
    assert(!strcmp(mTx,"OKAYbootloader reset requested"));
    stage.ReturnTest=UINT64_C(0x80000007)<<32;
    fail=1; published=transferred=0;
    run();
    assert(transferred && !published && (uint32_t)stage.ReturnTest==0);
    fail=0; mKernelRunning=0; transferred=0;
    run();
    assert(transferred && !published && (uint32_t)stage.ReturnTest==0);
    assert(!strncmp(mTx,"FAIL",4));
    mKernelRunning=1; Command="oem interrupt"; fail=0; published=transferred=0;
    run(); assert(transferred && published==1);
    fail=1; published=transferred=0; run(); assert(transferred && !published);
    fail=0; pending=1; transferred=0; run(); assert(transferred && !published);
    assert(!memcmp(mTx,"FAILcontrol request pending",strlen("FAILcontrol request pending")));
}
'''
with tempfile.TemporaryDirectory(prefix='root-usb-reboot-') as temporary:
    directory = Path(temporary)
    program = directory / 'reboot.c'
    program.write_text(preamble + reply + cases + command + ending)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', str(program),
                    '-o', str(directory / 'reboot')], check=True)
    subprocess.run([str(directory / 'reboot')], check=True)
print('RootStage reboot: acknowledgement completion, failed reply, phase gate and heartbeat preservation passed')
