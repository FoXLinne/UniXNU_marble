from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2]
out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/submit-reject'
out.mkdir(parents=True,exist_ok=True)
source=r'''#include "NativeAdrenoSubmitReject.h"
#include <cassert>
#include <thread>
#include <vector>
extern "C" {volatile uint64_t native_adreno_submit_reject[16];}
int main(){
 nativeAdrenoSubmitReject(7,1,10,0,1,0);
 nativeAdrenoSubmitReject(7,1,10,0xe00002d5,1,0);
 assert(native_adreno_submit_reject[0]==0 && native_adreno_submit_reject[15]==0);
 std::vector<std::thread> threads;
 for(unsigned i=1;i<=8;++i)threads.emplace_back([i]{for(unsigned j=0;j<10000;++j)
  nativeAdrenoSubmitReject(i,2,123,0xe00002c2,9,10,3,i+100,i+200,i+300,i+400,i+500,i+600,1);});
 for(auto &t:threads)t.join();
 assert(__atomic_load_n(&native_adreno_submit_reject[0],__ATOMIC_ACQUIRE)==2);
 const uint64_t owner=native_adreno_submit_reject[1];assert(owner>=1&&owner<=8);
 assert(native_adreno_submit_reject[2]==2&&native_adreno_submit_reject[3]==123);
 assert(native_adreno_submit_reject[8]==owner+100 && native_adreno_submit_reject[9]==owner+200);
 assert(native_adreno_submit_reject[10]==owner+300 && native_adreno_submit_reject[11]==owner+400);
 assert(native_adreno_submit_reject[12]==owner+500 && native_adreno_submit_reject[13]==owner+600);
 assert(native_adreno_submit_reject[15]==80000);
 nativeAdrenoSubmitReject(99,1,999,0xe00002c2,0,0);
 assert(native_adreno_submit_reject[1]==owner && native_adreno_submit_reject[3]==123);
}
'''
(out/'test.cpp').write_text(source)
subprocess.run(['xcrun','clang++','-std=c++17','-pthread','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(repo/'ios27/sources/Adreno740'),str(out/'test.cpp'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
print('PASS: 80000 concurrent reports retain one coherent first error; successful/transient results skip capture; later clients preserve record')
