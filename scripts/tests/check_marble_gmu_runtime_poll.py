from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/gmu-runtime-poll';out.mkdir(parents=True,exist_ok=True)
s=(repo/'package/MarbleGmu.cpp').read_text();method=s[s.index('    int exchange('):s.index('    int performance()')]
fixture=r'''#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstdarg>
static uint64_t now_us,ack_at;static unsigned spins,sleeps,firmware_error;
static void IODelay(unsigned n){now_us+=n;++spins;}
static void IOSleep(unsigned){now_us+=10000;++sleeps;}
static void IOLog(const char*,...){}
struct Hfi{uint32_t shared[4096]{};uint32_t skipped_header=0,skipped_ret=0,skipped_code=0;};
static constexpr int AdrenoHfiEmpty=1;
static int AdrenoHfiSend(Hfi*,unsigned,const uint32_t*,size_t,uint32_t*h){*h=1;return 0;}
static int AdrenoHfiPoll(Hfi*,uint32_t*,unsigned,size_t*n,uint32_t*e){if(now_us<ack_at)return AdrenoHfiEmpty;*n=1;*e=firmware_error;return 0;}
struct MockGmu{struct {uint32_t read(unsigned){return 0;}} smmu;struct {unsigned context_offset=0;} programmed;Hfi hfi;void write(unsigned,uint32_t){}uint32_t read(unsigned){return 0;}
'''+method+r'''};
int main(){MockGmu g;const uint32_t vote[]={1,7,11};uint32_t error=0;
 for(unsigned ack:{0u,10u,50u,990u}){
  now_us=spins=sleeps=firmware_error=0;ack_at=ack;
  assert(g.exchange(30,vote,3,&error,true)==0);assert(error==0);assert(now_us<=1000);assert(sleeps==0);
 }
 now_us=spins=sleeps=0;ack_at=50;assert(g.exchange(30,vote,3,&error,false)==0);assert(sleeps==1&&now_us==10000);
 now_us=spins=sleeps=0;ack_at=50000;assert(g.exchange(30,vote,3,&error,true)==0);assert(spins==100&&sleeps>0);
 now_us=spins=sleeps=0;ack_at=UINT64_MAX;assert(g.exchange(30,vote,3,&error,true)!=0);assert(spins==100&&sleeps==100);
 now_us=spins=sleeps=0;ack_at=0;firmware_error=13;assert(g.exchange(30,vote,3,&error,true)==0);assert(error==13);
}
'''
fixture=fixture.replace('#include <cstdarg>','#include <cstdarg>\n#include <initializer_list>')
(out/'test.cpp').write_text(fixture)
subprocess.run(['xcrun','clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(out/'test.cpp'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
print('PASS: actual exchange method handles0..990us acknowledgements without sleeping, bounded1ms spins, slow reply fallback, finite timeout and firmware errors')
