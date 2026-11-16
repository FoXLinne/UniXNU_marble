"""Exercise CPU wake/idle vote invariants over every physical-core ordering."""
from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/cpu-idle-policy';out.mkdir(parents=True,exist_ok=True)
s='''#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include "MarbleCpuIdlePolicy.h"
int main(){
 const std::array<unsigned,3> groups={0x0d,0x70,0x80};
 std::array<unsigned,7> cores={0,2,3,4,5,6,7};unsigned orders=0;
 do {
  MarbleCpuIdlePolicy policy;std::array<int,3> vote={1,1,1};unsigned awake=0xfd;
  auto step=[&](unsigned cpu,bool enter){
   auto result=policy.change(cpu,enter);
   if(cpu<8&&cpu!=1){if(enter)awake&=~(1u<<cpu);else awake|=1u<<cpu;}
   if(result.vote>=0)vote[result.domain]=result.vote;
   for(unsigned d=0;d<3;++d){assert(policy.active(d)==(awake&groups[d]));assert(vote[d]==((awake&groups[d])?1:0));}
  };
  step(1,true);step(1,false);step(8,true);step(~0u,false);
  for(auto c:cores)step(c,true);
  for(auto c:cores){step(c,false);step(c,false);}
  uint32_t random=orders+1;
  for(unsigned n=0;n<1000;++n){random=random*1664525u+1013904223u;step(cores[(random>>8)%7],random&1);}
  ++orders;
 }while(std::next_permutation(cores.begin(),cores.end()));
 assert(orders==5040);
}
''';(out/'test.cpp').write_text(s)
subprocess.run(['xcrun','clang++','-std=c++17','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-I',str(repo/'package'),str(out/'test.cpp'),'-o',str(out/'test')],check=True);subprocess.run([str(out/'test')],check=True)
print('PASS:5040 core orders and5million idle/wake transitions retain maximum vote whenever a domain has runnable CPU work; resident CPU ownership and invalid IDs checked')
