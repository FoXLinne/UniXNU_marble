from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];out=repo.parents[1]/'outputs/UniXNU_marble/ios18-port/power-v62-ios18/host-tests/charge-source';out.mkdir(parents=True,exist_ok=True)
s=r'''#include "MarbleBattmgr.h"
#include "MarbleChargeSourcePolicy.h"
#include <assert.h>
static uint32_t get32(const uint8_t*p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static MarbleBattmgr make(int limit,int max){MarbleBattmgr b;marble_battmgr_init(&b);b.valid=(1u<<BmUsbOnline)|(1u<<BmUsbAdapter)|(1u<<BmUsbLimit)|(1u<<BmUsbMax);b.value[BmUsbOnline]=1;b.value[BmUsbAdapter]=1;b.value[BmUsbLimit]=limit;b.value[BmUsbMax]=max;b.valid|=1u<<BmUsbTypecCompliant;b.value[BmUsbTypecCompliant]=1;return b;}
static unsigned request(MarbleBattmgr*b){uint8_t wire[MARBLE_BATTMGR_REQUEST_BYTES];unsigned n=marble_battmgr_next(b,wire);if(!n)return 0;assert(get32(wire+8)==MARBLE_BATTMGR_OP_USB_SET);assert(get32(wire+16)==5);return get32(wire+20);}
int main(void){
 const unsigned connected=1u<<19;MarbleChargeSourcePolicy p;
 for(unsigned mode=0;mode<8;++mode)for(unsigned direction=0;direction<2;++direction)for(unsigned online=0;online<2;++online){
  p=marble_charge_source_policy(1,mode<<16|direction<<20|online<<19,500000);
  if(!online||direction||mode==0||mode==3||mode>5)assert(p.firmware_owned&&p.budget_ua==0);
 }
 p=marble_charge_source_policy(1,connected|4u<<16,500000);assert(p.budget_ua==1500000&&!p.firmware_owned);
 p=marble_charge_source_policy(1,connected|5u<<16,500000);assert(p.budget_ua==3000000&&!p.firmware_owned);
 MarbleBattmgr b=make(500000,3000000);marble_battmgr_source_budget(&b,1,0,3000000);assert(request(&b)==1500000);
 b=make(500000,1500000);marble_battmgr_source_budget(&b,1,0,3000000);assert(request(&b)==1500000);
 b=make(100000,500000);marble_battmgr_source_budget(&b,1,0,1500000);assert(request(&b)==1500000);
 b=make(500000,3000000);b.valid&=~(1u<<BmUsbMax);marble_battmgr_source_budget(&b,1,0,3000000);assert(request(&b)==1500000);
 b=make(500000,3000000);marble_battmgr_source_budget(&b,1,0,3000000);b.value[BmUsbMax]=1500000;assert(request(&b)==1500000);
 b=make(500000,3000000);marble_battmgr_source_budget(&b,1,0,3000000);marble_battmgr_source_budget(&b,1,1,0);assert(request(&b)==0);
 b=make(3000000,3000000);marble_battmgr_source_budget(&b,1,0,500000);assert(request(&b)==500000);
 b=make(500000,3000000);marble_battmgr_source_budget(&b,1,0,3000000);b.value[BmUsbOnline]=0;assert(request(&b)==0);
 b=make(500000,3000000);marble_battmgr_source_budget(&b,1,0,3000000);assert(b.limit_tries==0);assert(request(&b)==1500000);assert(b.limit_tries==1);marble_battmgr_source_budget(&b,1,0,3000000);assert(b.limit_tries==1);
 b=make(500000,500000);b.value[BmUsbTypecCompliant]=0;marble_battmgr_source_budget(&b,1,0,3000000);assert(request(&b)==0);
 b=make(500000,500000);b.valid&=~(1u<<BmUsbTypecCompliant);marble_battmgr_source_budget(&b,1,0,3000000);assert(request(&b)==0);
 b=make(1500000,500000);b.valid|=1u<<BmUsbVoltage;b.value[BmUsbVoltage]=4400000;marble_battmgr_source_budget(&b,1,0,3000000);assert(request(&b)==500000);assert(b.source_voltage_fault);
 b.limit_in_flight=0;b.value[BmUsbLimit]=500000;b.value[BmUsbVoltage]=5100000;assert(request(&b)==0);assert(b.source_voltage_fault);
 b.value[BmUsbOnline]=0;b.limit_due=1;assert(request(&b)==0);assert(!b.source_voltage_fault);
}
''';(out/'test.c').write_text(s)
subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(repo/'package'),str(out/'test.c'),str(repo/'package/MarbleBattmgr.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
print('PASS: actual USB_SET requests honor explicit Type-C advertisements with initial1.5A requests, retain legacy class bounds, preserve PD ownership, revoke stale queues, lower reduced budgets and retain retry bounds')
