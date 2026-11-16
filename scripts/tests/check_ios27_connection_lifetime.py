from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];r=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119';src=(repo/'ios27/sources/Mesa/src/freedreno/vulkan/tu_knl_darwin.cc').read_text();out=r/'host-tests/connection-lifetime';out.mkdir(parents=True,exist_ok=True)
def function(name):
 end=src.index('\n{',src.index('\n'+name+'('));start=src.rfind('\nstatic ',0,end);i=end+2;depth=1
 while depth:
  depth+=(src[i]=='{')-(src[i]=='}');i+=1
 return src[start+1:i]
structs=src[src.index('struct darwin_va_hole {'):src.index('struct darwin_retire_state {')]
va=src[src.index('static int\ndarwin_va_cmp_address'):src.index('/* Hands the CPU lease')]
head=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <vector>
#include <mutex>
#include <set>
#include "util/rb_tree.h"
#include "util/macros.h"
using VkResult = int;
const int VK_SUCCESS=0, VK_ERROR_OUT_OF_DEVICE_MEMORY=-2, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS=-3;
enum tu_bo_alloc_flags { TU_BO_ALLOC_NONE=0, TU_BO_ALLOC_REPLAYABLE=1 };
const uint64_t os_page_size=4096;
struct tu_instance {};
struct tu_physical_device { uint64_t va_start,va_size; };
struct tu_device { tu_physical_device *physical_device; };
static struct {unsigned conn;bool open;unsigned users;pthread_mutex_t lock;unsigned inflight_count,last_fence;} dw={.lock=PTHREAD_MUTEX_INITIALIZER};
static pthread_mutex_t dw_lifecycle_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned opens,closes;static bool fail_open,primary_mapping;
static VkResult dw_open_connection_unlocked(tu_instance*){if(fail_open)return -1;dw.open=true;dw.conn=++opens;dw.last_fence=0;return 0;}
static int IOServiceClose(unsigned c){assert(c);++closes;primary_mapping=false;return 0;}
#define mesa_loge(...) do {fprintf(stderr,__VA_ARGS__);abort();}while(0)
static void darwin_reset_iova(void);
'''
test=r'''
int main(){
 tu_physical_device p{0x100000000ULL,0x40000000};tu_device a{&p},b{&p};uint64_t first=0,second=0;
 assert(dw_open_connection(nullptr)==0);primary_mapping=true;
 assert(darwin_allocate_iova(&a,8<<20,4096,0,TU_BO_ALLOC_NONE,&first)==0);
 dw.last_fence=711;dw.inflight_count=3;
 // Enumeration must borrow without reopening, and return only its lease.
 assert(dw_open_connection(nullptr)==0);assert(opens==1&&dw.users==2&&dw.last_fence==711);
 dw_close_connection();assert(primary_mapping&&dw.users==1&&closes==0&&dw.inflight_count==3);
 // A second logical device uses a disjoint range. Its failure cleanup keeps peer pages.
 assert(dw_open_connection(nullptr)==0);
 assert(darwin_allocate_iova(&b,8<<20,4096,0,TU_BO_ALLOC_NONE,&second)==0);
 assert(second>=first+(8<<20)||first>=second+(8<<20));
 darwin_free_iova(&b,second,8<<20);dw_close_connection();
 assert(primary_mapping&&closes==0&&dw.last_fence==711&&darwin_shared_va.used==(8<<20));
 // A replay address owned by the peer stays reserved.
 uint64_t collision=0;assert(darwin_allocate_iova(&b,4096,4096,first,TU_BO_ALLOC_REPLAYABLE,&collision)==VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
 std::mutex seen_lock;std::set<uint64_t> seen;std::vector<std::thread> threads;
 for(unsigned i=0;i<24;++i)threads.emplace_back([&]{for(unsigned j=0;j<100;++j){
  assert(dw_open_connection(nullptr)==0);uint64_t address;
  assert(darwin_allocate_iova(&b,65536,65536,0,TU_BO_ALLOC_NONE,&address)==0);
  {std::lock_guard<std::mutex> g(seen_lock);assert(address>=first+(8<<20));assert(seen.insert(address).second);}
  {std::lock_guard<std::mutex> g(seen_lock);assert(seen.erase(address)==1);}
  darwin_free_iova(&b,address,65536);dw_close_connection();
 }});
 for(auto &t:threads)t.join();assert(seen.empty());assert(primary_mapping&&dw.users==1&&opens==1&&closes==0);
 darwin_free_iova(&a,first,8<<20);assert(darwin_shared_va.used==0&&darwin_shared_va.holes==1);
 dw_close_connection();assert(closes==1&&!dw.open&&!dw.users&&!darwin_shared_va.seeded&&!darwin_shared_va.address.root);
 dw_close_connection();assert(closes==1);
 fail_open=true;assert(dw_open_connection(nullptr)==-1&&!dw.users);fail_open=false;
 assert(dw_open_connection(nullptr)==0&&opens==2);dw_close_connection();assert(closes==2);
 puts("PASS: probe ownership, failed second device, disjoint shared IOVA, replay collision, 2400 concurrent lifetimes, final close and failed open");
}
'''
(out/'test.cpp').write_text(head+structs+function('dw_open_connection')+'\n'+function('dw_close_connection')+'\n'+va+test)
mesa=repo/'ios27/sources/Mesa/src';common=['-I'+str(mesa),'-I'+str(mesa/'util'),'-DHAVE_STRUCT_TIMESPEC','-DUTIL_ARCH_LITTLE_ENDIAN=1','-DUTIL_ARCH_BIG_ENDIAN=0']
subprocess.run(['clang',*common,'-c',str(mesa/'util/rb_tree.c'),'-o',str(out/'rb_tree.o')],check=True)
subprocess.run(['clang++','-std=c++17','-pthread','-fsanitize=address,undefined',*common,str(out/'test.cpp'),str(out/'rb_tree.o'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
