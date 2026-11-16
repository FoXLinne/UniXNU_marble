"""Exercise actual Darwin BO retirement and submission ownership routines."""
from pathlib import Path
import subprocess
repo = Path(__file__).resolve().parents[2]
out = repo.parents[1] / 'outputs/UniXNU_marble/ios27_24B119/host-tests/bo-lifetime'
out.mkdir(parents=True, exist_ok=True)
source = (repo/'ios27/sources/Mesa/src/freedreno/vulkan/tu_knl_darwin.cc').read_text()
def function(text, name):
    end = text.index('\n{', text.index('\n'+name+'('))
    start = text.rfind('\nstatic ', 0, end)
    return text[start+1:text.index('\n}', end)+2]
head = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <thread>
using VkResult=int;
constexpr int VK_SUCCESS=0;
struct tu_bo {uint32_t gem_handle;int refcnt;uint64_t darwin_handle,iova,size;void *map;uint32_t darwin_token;uint64_t darwin_alloc_size;bool darwin_read_only;};
struct darwin_retired_bo {uint64_t handle,iova,size;void *mapping;uint32_t token,fence;bool cacheable,read_only;};
struct darwin_retire_state {pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;unsigned count=0,capacity=64,head=0;int failure=0;darwin_retired_bo *entries;uint64_t enqueued=0;};
struct tu_device {int dma_bo_lock;void *darwin_retire;struct {int alloc;}vk;};
static struct {pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;uint32_t last_fence=0;}dw;
static darwin_retire_state retire;
static tu_bo *reuse_target;
static bool checkout_on_unlock;
static void tracked_unlock(pthread_mutex_t *lock) {
 pthread_mutex_unlock(lock);
 if (lock==&retire.lock && checkout_on_unlock) {
  checkout_on_unlock=false;
  std::thread peer([]{pthread_mutex_lock(&retire.lock);*reuse_target={23,1,23,0x200000,4096};pthread_mutex_unlock(&retire.lock);});
  peer.join();
 }
}
#define pthread_mutex_unlock tracked_unlock
#define u_rwlock_rdlock(x) ((void)0)
#define u_rwlock_rdunlock(x) ((void)0)
#define tu_debug_bos_del(...) ((void)0)
#define tu_dump_bo_del(...) ((void)0)
#define TU_RMV(...) ((void)0)
static bool p_atomic_dec_zero(int *p){return --*p==0;}
static void darwin_reap_locked(tu_device *,darwin_retire_state *){}
static int vk_device_set_lost(void *,const char *){return -1;}
static void *vk_zalloc(void *,size_t n,size_t,int){return calloc(1,n);}
constexpr int VK_SYSTEM_ALLOCATION_SCOPE_DEVICE=0;
static void vk_free(void *,void *p){free(p);}
struct util_dynarray {unsigned char *data;size_t size;};
static void *grow(util_dynarray *a,size_t n){size_t old=a->size;a->size+=n;a->data=(unsigned char*)realloc(a->data,a->size);return a->data+old;}
#define util_dynarray_grow(a,T,n) grow(a,sizeof(T)*(n))
#define util_dynarray_append(a,v) (*(decltype(v)*)grow(a,sizeof(v))=(v))
#define util_dynarray_foreach(a,T,p) for(T *p=(T*)(a)->data;p<(T*)((a)->data+(a)->size);++p)
static void util_dynarray_fini(util_dynarray *a){free(a->data);}
struct NativeAdrenoAbiSubmitBuffer {uint64_t handle,iova,bytes;};
struct tu_cs_entry {const tu_bo *bo;uint32_t size,offset;};
static tu_bo *tu_bo_get_ref(tu_bo *bo){++bo->refcnt;return bo;}
'''
struct_start=source.index('struct tu_darwin_queue_submit {')
struct_end=source.index('\n};',struct_start)+3
routines=function(source,'darwin_bo_finish')+'\nstatic void tu_bo_finish(tu_device *d,tu_bo *b){darwin_bo_finish(d,b);}\n'+source[struct_start:struct_end]+'\n'+'\n'.join(function(source,n) for n in ['darwin_submit_create','darwin_submit_finish','darwin_submit_add_entries'])
test=r'''
int main(){
 retire.entries=(darwin_retired_bo*)calloc(64,sizeof(darwin_retired_bo));tu_device dev{};dev.darwin_retire=&retire;
 tu_bo bo{7,1,7,0x100000,4096};reuse_target=&bo;checkout_on_unlock=true;
 darwin_bo_finish(&dev,&bo);
 if(bo.darwin_handle!=23){puts("OLD_SPARSE_SLOT_ERASED_REUSED_BO");return 17;}
 assert(bo.refcnt==1&&bo.gem_handle==23);
 retire.count=0;retire.enqueued=0;bo={17,1,17,0x300000,4096};
 auto *submit=darwin_submit_create(&dev);tu_cs_entry entries[]={{&bo,16,0},{&bo,16,16}};
 darwin_submit_add_entries(&dev,submit,entries,2);assert(bo.refcnt==3);
 tu_bo_finish(&dev,&bo);assert(bo.refcnt==2&&bo.darwin_handle==17&&retire.enqueued==0);
 dw.last_fence=77;darwin_submit_finish(&dev,submit);
 assert(!bo.darwin_handle&&retire.count==1&&retire.entries[0].handle==17&&retire.entries[0].fence==77);
 // A submission discarded before dispatch releases references against the existing fence.
 bo={19,1,19,0x400000,4096};submit=darwin_submit_create(&dev);entries[0].bo=&bo;
 darwin_submit_add_entries(&dev,submit,entries,1);darwin_submit_finish(&dev,submit);
 assert(bo.refcnt==1&&bo.darwin_handle==19);tu_bo_finish(&dev,&bo);
 free(retire.entries);puts("PASS: sparse-slot reuse ownership, temporary command BO retention, final fence publication, discarded submission cleanup");
}
'''
path=out/'lifetime.cpp';path.write_text(head+routines+test)
cmd=['clang++','-std=c++17','-pthread','-fsanitize=address,undefined',str(path),'-o',str(out/'lifetime')]
subprocess.run(cmd,check=True);subprocess.run([str(out/'lifetime')],check=True)
old=(out.parents[1]/'gpu-bo-lifetime-before.cc').read_text()
old_body=function(old,'darwin_bo_finish')
legacy=head+old_body+r'''
int main(){retire.entries=(darwin_retired_bo*)calloc(64,sizeof(darwin_retired_bo));tu_device dev{};dev.darwin_retire=&retire;tu_bo bo{7,1,7,0x100000,4096};reuse_target=&bo;checkout_on_unlock=true;darwin_bo_finish(&dev,&bo);free(retire.entries);return bo.darwin_handle==23?0:17;}
'''
path=out/'before.cpp';path.write_text(legacy)
subprocess.run(['clang++','-std=c++17','-pthread','-fsanitize=address,undefined',str(path),'-o',str(out/'before')],check=True)
assert subprocess.run([str(out/'before')]).returncode==17
print('Previous source reproduces erased reused BO under the same forced checkout schedule')
