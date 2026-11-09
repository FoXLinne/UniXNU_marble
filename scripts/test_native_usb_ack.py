#!/usr/bin/env python3
"""Check native return actions require a completed reply and cancel on detach."""
from pathlib import Path
import subprocess
import tempfile

device=Path(__file__).resolve().parents[1]
source=(device/'package/RuntimeUsb.cpp').read_text()
start=source.index('if(rootReplyTransfer&&rootAction)')
begin=source.index('{',start)
depth=1;end=begin+1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
ack=source[start:end]
protocol=(device/'package/MarbleKernelUsb.inc').read_text()
reset=protocol[protocol.index('static void rootSessionReset()'):protocol.index('static unsigned rootUpload(')]
program=r'''
#include <cassert>
#include <cstdint>
#define OSSafeReleaseNULL(p) ((p)=nullptr)
static void *rootExport;
static const uint8_t *rootExportPointer;
static unsigned rootExportBytes,rootAction,returnTicks,rebootTicks,rootContactX,rootContactY;
static bool rootReplyTransfer,rootContactDown;
static uint64_t returnDeadline,rebootDeadline;
static unsigned releases;
static const unsigned kMillisecondScale=1000000;
static void rootRespond(const char *,const char *) {}
static void native_hid_request_contact(unsigned,unsigned,bool down) {assert(!down);++releases;}
static void clock_interval_to_deadline(unsigned value,unsigned scale,uint64_t *out) {*out=uint64_t(value)*scale;}
'''
cases=r'''
int main() {
 rootAction=1;rootReplyTransfer=false;acknowledge();assert(rootAction==1&&!returnTicks);
 rootSessionReset();assert(!rootAction&&!returnTicks&&!rootReplyTransfer);
 rootAction=1;rootReplyTransfer=true;acknowledge();
 assert(returnTicks==1&&returnDeadline==500000000&&!rootAction&&!rootReplyTransfer);
 acknowledge();assert(returnTicks==1);
 rootSessionReset();rootAction=2;rootReplyTransfer=true;acknowledge();
 assert(rebootTicks==1&&rebootDeadline==500000000&&!rootAction);
 rootContactDown=true;rootSessionReset();assert(!rebootTicks&&!rootContactDown&&releases==1);
}
'''
with tempfile.TemporaryDirectory(prefix='native-usb-ack-') as temporary:
    path=Path(temporary);(path/'test.cpp').write_text(program+reset+'\nstatic void acknowledge(){'+ack+'}\n'+cases)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Native USB: reply completion gates return/reset; detach cancels pending actions and releases contact')
