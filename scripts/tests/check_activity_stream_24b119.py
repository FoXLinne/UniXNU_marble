"""Qualify the decoded 24B119 wire prefix against a packed fixture."""
from pathlib import Path
import subprocess
import tempfile
repo = Path(__file__).resolve().parents[2]
source = r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ActivityStreamSPI.h"
int main(void) {
    unsigned char wire[0xec] = {0};
    uint32_t type=0x400, uid=501;
    const char *path="/Applications/Alook.app/Alook";
    memcpy(wire,&type,4);memcpy(wire+0x10,&uid,4);
    memcpy(wire+0x1c,&path,8);
    os_activity_stream_entry_t entry=(void *)wire;
    assert(entry->type==0x400 && entry->uid==501);
    assert(!strcmp(entry->proc_imagepath,path));
    assert((unsigned char *)entry->log_message==wire+0x34);
    uint64_t timestamp=123456789, actual=0;
    memcpy(wire+0x3c,&timestamp,8);
    memcpy(&actual,entry->log_message+8,8);assert(actual==timestamp);
    puts("PASS: 24B119 uid, unaligned process path, message base and timestamp fixture; build qualification");
}
'''
with tempfile.TemporaryDirectory(prefix='marble-activity-abi-') as temporary:
    root=Path(temporary);c=root/'fixture.c';c.write_text(source);binary=root/'fixture'
    subprocess.run(['xcrun','clang','-std=c11','-fblocks','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-I',str(repo/'package/debug27'),str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
