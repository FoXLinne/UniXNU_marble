#!/usr/bin/env python3
"""Check EP0 resource cancellation and rearming across native USB cable events."""
from pathlib import Path
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
source = (device / 'package/RuntimeUsb.cpp').read_text()
handler = source[source.index('void handleEvent(uint32_t event)'):source.index('bool initialize()')]
program = r'''
#include <cassert>
#include <cstdint>
#include "RuntimeUsbRegisters.h"
static unsigned events,configuration=1,controlState=1,controlTicks=499,enumerationTicks=999;
static unsigned connectCount,disconnectCount;
static unsigned resource[2]={7,8},statusEndpoint,setups,configs;
static bool resourceOwned[2]={true,true},linkConnected=true,active=true,threeStage;
static void end(unsigned ep) {resourceOwned[ep]=false;resource[ep]=0;}
static bool command(unsigned,unsigned) {return true;}
static uint32_t read(unsigned) {return 0;}
static void write(unsigned,uint32_t) {}
static bool startConfiguration() {++configs;return true;}
static bool configure(unsigned,bool) {return true;}
static void receiveSetup() {++setups;resourceOwned[0]=true;controlState=0;}
static void handleSetup() {}
static bool transfer(unsigned,unsigned,unsigned,uint32_t) {return true;}
static void stall() {}
static constexpr unsigned controlOffset=8192;
#define IOLog(...) ((void)0)
'''
cases = r'''
int main() {
 handleEvent(1 | DWC3_DEVICE_EVENT_DISCONNECT<<8);
 assert(!linkConnected&&!configuration&&!controlState&&!controlTicks&&!enumerationTicks);
 assert(!resourceOwned[0]&&!resourceOwned[1]&&!resource[0]&&!resource[1]&&active);
 handleEvent(1 | DWC3_DEVICE_EVENT_CONNECT_DONE<<8);
 assert(linkConnected&&configs==1&&setups==1&&resourceOwned[0]&&active);
 controlState=3;controlTicks=400;enumerationTicks=700;
 handleEvent(1 | DWC3_DEVICE_EVENT_DISCONNECT<<8);
 handleEvent(1 | DWC3_DEVICE_EVENT_RESET<<8);
 handleEvent(1 | DWC3_DEVICE_EVENT_CONNECT_DONE<<8);
 assert(linkConnected&&configs==2&&setups==2&&!controlState&&!controlTicks&&!enumerationTicks);
 assert(!configuration&&resourceOwned[0]&&!resourceOwned[1]&&active);
}
'''
with tempfile.TemporaryDirectory(prefix='runtime-usb-reconnect-') as temporary:
    path = Path(temporary)
    (path / 'test.cpp').write_text(program + handler + cases)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(device / 'package'),
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('RuntimeUSB: unplug cancels both resources and deadlines; reconnect rearms EP0 and preserves the worker')
