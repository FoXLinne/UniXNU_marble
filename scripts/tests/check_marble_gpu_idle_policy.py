from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];out=repo.parents[1]/'outputs/UniXNU_marble/ios18-port/power-v62-ios18/host-tests/gpu-idle-policy';out.mkdir(parents=True,exist_ok=True)
source=r'''#include "MarbleGpuIdlePolicy.h"
#include <cassert>
int main(){
 for(unsigned failureMask=0;failureMask<256;++failureMask){
  MarbleGpuIdlePolicy p;unsigned hardware=7,calls=0,dispatch=0;
  auto vote=[&](unsigned level,unsigned bw){assert((level==1&&bw==1)||(level==7&&bw==11));hardware=level;return (failureMask&(1u<<(calls++&7)))?-5:0;};
  // Repeated idle notification must not resend an acknowledged vote.
  int low=p.change(true,vote);assert(p.needsRestore());
  unsigned after=calls;p.change(true,vote);assert(calls==after);
  if(low)assert(p.idleDisabled());else assert(p.lowConfirmed()&&hardware==1);
  // Firmware may apply either request even when its acknowledgement is lost.
  // A failed high vote blocks dispatch until a later high acknowledgement.
  for(unsigned n=0;n<16;++n){
   int result=p.change(false,vote);
   if(result){assert(p.needsRestore());assert(p.idleDisabled());}
   else {assert(!p.needsRestore());assert(!p.lowConfirmed());assert(hardware==7);++dispatch;}
  }
  const bool disabled=p.idleDisabled();after=calls;
  if(!p.needsRestore()){
   p.change(true,vote);
   if(disabled)assert(calls==after);
  }
  (void)dispatch;
 }
 MarbleGpuIdlePolicy p;unsigned calls=0;
 auto ok=[&](unsigned l,unsigned b){++calls;assert((l==1&&b==1)||(l==7&&b==11));return 0;};
 for(unsigned n=0;n<100000;++n){assert(p.change(true,ok)==0);assert(p.lowConfirmed());assert(p.change(false,ok)==0);assert(!p.needsRestore());}
 assert(calls==200000);
}
''';(out/'test.cpp').write_text(source)
subprocess.run(['xcrun','clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(repo/'package'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
print('PASS: 256 acknowledgement-failure sequences and100000 sleep/wake cycles; unacknowledged high votes block dispatch and failed idle votes disable further drops')
