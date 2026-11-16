#!/usr/bin/env python3
"""Run the production Darwin synchronization functions against a mock kernel boundary."""
from pathlib import Path
import subprocess

repo = Path(__file__).resolve().parents[2]
out = repo.parents[1] / 'outputs/UniXNU_marble/ios27_24B119/host-tests/gpu-sync'
out.mkdir(parents=True, exist_ok=True)
source = (repo / 'ios27/sources/Mesa/src/freedreno/vulkan/tu_knl_darwin.cc').read_text()


def function(name):
    start = source.index('\n' + name + '(')
    start = source.rfind('\nstatic ', 0, start) + 1
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


head = r'''
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <unistd.h>
using VkResult=int;
const int VK_SUCCESS=0,VK_TIMEOUT=2,VK_ERROR_DEVICE_LOST=-4,VK_ERROR_OUT_OF_DEVICE_MEMORY=-2;
using kern_return_t=unsigned;
const unsigned KERN_SUCCESS=0,kIOReturnTimeout=1,kIOReturnNoMemory=2,kIOReturnBusy=3;
struct vk_device {bool lost;};
struct tu_device {vk_device vk;pthread_mutex_t submit_mutex;pthread_cond_t timeline_cond;};
struct vk_sync {};
enum darwin_sync_state {DARWIN_SYNC_UNSIGNALED,DARWIN_SYNC_SIGNALED,DARWIN_SYNC_FENCE};
struct vk_darwin_sync {vk_sync vk;darwin_sync_state state;uint32_t fence;};
struct vk_sync_wait {vk_sync *sync;};
enum vk_sync_wait_flags {VK_SYNC_WAIT_PENDING=1,VK_SYNC_WAIT_ANY=2};
#define container_of(p,type,member) ((type*)((char*)(p)-offsetof(type,member)))
static uint64_t os_time_get_nano(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static VkResult mock_result=VK_SUCCESS;static unsigned polls;
static VkResult dw_wait_fence_locked_free(tu_device*,uint32_t,uint64_t){++polls;return mock_result;}
static VkResult vk_device_set_lost(vk_device *d,const char*,const char*,unsigned){d->lost=true;return VK_ERROR_DEVICE_LOST;}
static VkResult vk_error(tu_device*,VkResult value){return value;}
static unsigned darwin_device_error_state;
static char darwin_device_error_text[192];
'''
head += r'''

struct tu_queue {tu_device *device;};
struct NativeAdrenoAbiSubmitBuffer {uint64_t value;};
struct NativeAdrenoAbiSubmit {unsigned buffer_count,queue;NativeAdrenoAbiSubmitBuffer buffers[8];};
struct NativeAdrenoAbiSubmitReply {uint64_t fence;};
struct tu_darwin_submit_rec {uint32_t call_us,busy,space_wait_us;};
static struct {pthread_mutex_t lock;unsigned inflight_count;uint32_t inflight[1];} dw;
static unsigned submit_status=KERN_SUCCESS;
static unsigned submit_calls;
static constexpr unsigned NativeAdrenoMethodSubmit=1;
static kern_return_t dw_call(unsigned,const void*,size_t,void *out,size_t) {++submit_calls;((NativeAdrenoAbiSubmitReply*)out)->fence=55;return submit_status;}
static uint64_t tu_trace_clock_ns(){return os_time_get_nano();}
#define MIN2(a,b) ((a)<(b)?(a):(b))
'''
test = r'''
int main(){
 tu_device device{};pthread_mutex_init(&device.submit_mutex,nullptr);pthread_cond_init(&device.timeline_cond,nullptr);
 vk_darwin_sync signal{{},DARWIN_SYNC_UNSIGNALED,0};
 assert(vk_darwin_sync_wait(&device.vk,&signal.vk,0,VK_SYNC_WAIT_PENDING,0)==VK_TIMEOUT);
 signal.state=DARWIN_SYNC_FENCE;signal.fence=9;polls=0;
 assert(vk_darwin_sync_wait(&device.vk,&signal.vk,0,VK_SYNC_WAIT_PENDING,UINT64_MAX)==VK_SUCCESS&&polls==0);
 signal.state=DARWIN_SYNC_UNSIGNALED;signal.fence=0;
 std::thread producer([&]{usleep(20000);pthread_mutex_lock(&device.submit_mutex);signal.fence=11;signal.state=DARWIN_SYNC_FENCE;pthread_cond_broadcast(&device.timeline_cond);pthread_mutex_unlock(&device.submit_mutex);});
 uint64_t start=os_time_get_nano();
 assert(vk_darwin_sync_wait(&device.vk,&signal.vk,0,VK_SYNC_WAIT_PENDING,UINT64_MAX)==VK_SUCCESS);
 assert(os_time_get_nano()-start>=10000000&&polls==0);producer.join();
 vk_sync_wait waits[]={{&signal.vk}};mock_result=VK_ERROR_DEVICE_LOST;polls=0;
 assert(vk_darwin_sync_wait_many(&device.vk,1,waits,VK_SYNC_WAIT_ANY,os_time_get_nano()+10000000)==VK_ERROR_DEVICE_LOST&&polls==1);
 mock_result=VK_ERROR_OUT_OF_DEVICE_MEMORY;polls=0;
 assert(vk_darwin_sync_wait_many(&device.vk,1,waits,VK_SYNC_WAIT_ANY,os_time_get_nano()+10000000)==VK_ERROR_OUT_OF_DEVICE_MEMORY&&polls==1);
 mock_result=VK_SUCCESS;polls=0;
 assert(vk_darwin_sync_wait_many(&device.vk,1,waits,VK_SYNC_WAIT_ANY,0)==VK_SUCCESS&&polls==1);
 signal.state=DARWIN_SYNC_UNSIGNALED;
 assert(vk_darwin_sync_wait_many(&device.vk,1,waits,VK_SYNC_WAIT_PENDING,0)==VK_TIMEOUT);
 assert(vk_darwin_sync_wait_many(&device.vk,0,nullptr,VK_SYNC_WAIT_ANY,0)==VK_SUCCESS);
 signal.state=DARWIN_SYNC_SIGNALED;polls=0;
 assert(vk_darwin_sync_wait_many(&device.vk,1,waits,VK_SYNC_WAIT_PENDING,0)==VK_SUCCESS&&polls==0);
 assert(dw_status_to_vk(&device,kIOReturnNoMemory,"allocation")==VK_ERROR_OUT_OF_DEVICE_MEMORY&&!device.vk.lost);
 assert(darwin_device_error_state==0);
 assert(dw_status_to_vk(&device,33,"failure")==VK_ERROR_DEVICE_LOST&&device.vk.lost);
 assert(strstr(darwin_device_error_text,"failure status=0x21"));
 dw_record_device_error("later failure",44,77);
 assert(strstr(darwin_device_error_text,"failure status=0x21"));

 tu_queue queue{&device};tu_darwin_submit_rec rec{};uint32_t fence=0;
 pthread_mutex_init(&dw.lock,nullptr);dw.inflight_count=1;dw.inflight[0]=42;
 device.vk.lost=false;darwin_device_error_state=0;darwin_device_error_text[0]=0;
 submit_status=kIOReturnNoMemory;submit_calls=0;
 assert(darwin_submit_chunk(&queue,nullptr,0,&fence,&rec)==VK_ERROR_OUT_OF_DEVICE_MEMORY&&!device.vk.lost&&submit_calls==1);
 assert(darwin_device_error_state==0);
 submit_status=33;
 assert(darwin_submit_chunk(&queue,nullptr,0,&fence,&rec)==VK_ERROR_DEVICE_LOST&&device.vk.lost);
 assert(strstr(darwin_device_error_text,"submit status=0x21"));
 device.vk.lost=false;submit_status=kIOReturnBusy;mock_result=VK_ERROR_OUT_OF_DEVICE_MEMORY;
 assert(darwin_submit_chunk(&queue,nullptr,0,&fence,&rec)==VK_ERROR_OUT_OF_DEVICE_MEMORY&&!device.vk.lost);
 submit_status=KERN_SUCCESS;
 assert(darwin_submit_chunk(&queue,nullptr,0,&fence,&rec)==VK_SUCCESS&&fence==55);
 pthread_mutex_destroy(&dw.lock);
 pthread_cond_destroy(&device.timeline_cond);pthread_mutex_destroy(&device.submit_mutex);
 puts("PASS: pending timeout, pending publication, blocking pending wait, wait-any error propagation, single poll, empty wait, allocation error class, fatal-error retention and production submit error propagation");
}
'''
functions = '\n'.join(function(n) for n in ['dw_record_device_error','dw_status_to_vk','darwin_sync_wait','vk_darwin_sync_wait','vk_darwin_sync_wait_many','darwin_submit_chunk'])
(out/'test.cpp').write_text(head+functions+test)
subprocess.run(['clang++','-std=c++17','-pthread','-fsanitize=address,undefined',str(out/'test.cpp'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True,timeout=10)
