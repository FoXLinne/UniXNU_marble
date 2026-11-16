"""Exercise Wi-Fi burst coalescing and both wait-entry race windows."""
from pathlib import Path
import subprocess,tempfile
repo=Path(__file__).resolve().parents[2]
s=(repo/'package/MarbleWifiRuntime.cpp').read_text()
def body(name):
 a=s.index('void '+name+'(');b=s.index('{',a);depth=1;i=b+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]
code=r'''
#include <cstdint>
#include <cassert>
using event_t=volatile uint32_t*;
static volatile uint32_t gWake;
static volatile uint64_t native_marble_wifi_wake_stats[2];
static unsigned wakeCalls,armCalls,blocks,clearCalls,inject;
static bool armed,awakened;
constexpr int THREAD_UNINT=0,NSEC_PER_MSEC=1000000,THREAD_AWAKENED=1,THREAD_CONTINUE_NULL=0;
void wakeWorker(bool force=false);
void thread_wakeup(event_t){wakeCalls++;awakened=true;}
int current_thread(){return 0;}
void clear_wait(int,int){clearCalls++;armed=false;}
void assert_wait_timeout(event_t,int,unsigned,int){armCalls++;armed=true;awakened=false;if(inject==1)wakeWorker();}
void thread_block(int){if(inject==2)wakeWorker();assert(armed);if(!awakened)blocks++;armed=false;}
'''+body('wakeWorker')+'\n'+body('wait')+r'''
void reset(){gWake=0;wakeCalls=armCalls=blocks=clearCalls=inject=0;armed=awakened=false;native_marble_wifi_wake_stats[0]=native_marble_wifi_wake_stats[1]=0;}
int main(){
 reset();for(int i=0;i<100;i++)wakeWorker();assert(wakeCalls==1&&native_marble_wifi_wake_stats[1]==99);wait(1000);assert(!armCalls&&!gWake);
 reset();inject=1;wait(1000);assert(wakeCalls==1&&clearCalls==1&&!blocks&&!gWake);
 reset();inject=2;wait(1000);assert(wakeCalls==1&&!blocks&&gWake);wait(1000);assert(!gWake);
 reset();wait(1000);assert(blocks==1&&!wakeCalls);
 reset();wakeWorker();armed=true;awakened=false;wakeWorker(true);assert(wakeCalls==2&&awakened&&native_marble_wifi_wake_stats[0]==2);wakeWorker();assert(wakeCalls==2&&native_marble_wifi_wake_stats[1]==1);
}
'''
with tempfile.TemporaryDirectory() as t:
 p=Path(t)/'wake.cpp';p.write_text(code);b=Path(t)/'wake'
 subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(b)],check=True)
 subprocess.run([str(b)],check=True)
print('Wi-Fi wake bursts coalesced; wake-before-arm and wake-before-block retained')
