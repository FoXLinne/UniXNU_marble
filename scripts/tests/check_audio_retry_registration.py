"""Exercise the actual per-service reservation policy and concurrent callers."""
from pathlib import Path
import subprocess,tempfile
repo=Path(__file__).resolve().parents[2]
s=(repo/'package/ios27-runtime/userspace/MarbleAudioRetry27.c').read_text()
body=s[s.index('static int service_slot'):s.index('static xpc_connection_t marble_audio_create_service')]
head='''#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define XPC_CONNECTION_MACH_SERVICE_LISTENER 1
volatile uint64_t marble_audio_retry_stats[2][4];
static pthread_mutex_t retry_lock=PTHREAD_MUTEX_INITIALIZER;
'''
tail='''
static void *reserve(void *out){*(uint64_t *)out=reserve_attempt(0,1000);return NULL;}
static int compare(const void *a,const void *b){uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);}
int main(void){
 assert(service_slot("com.apple.BTAudioHALPluginAccessories",0)==-1);
 assert(service_slot("com.apple.BTAudioHALPluginAccessories",1)==0);
 assert(service_slot("com.apple.AudioAccessoryCommunication.xpc",1)==1);
 assert(service_slot("com.apple.audio.hapticd",1)==-1);
 assert(reserve_attempt(0,1000)==1000);
 assert(reserve_attempt(1,1000)==1000);
 pthread_t threads[8];uint64_t values[8];
 for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,reserve,&values[i]));
 for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
 qsort(values,8,sizeof(uint64_t),compare);
 for(unsigned i=0;i<8;i++)assert(values[i]==1000+250000000ULL*(i+1));
 assert(reserve_attempt(0,3000000000ULL)==3000000000ULL);
 assert(marble_audio_retry_stats[0][0]==10 && marble_audio_retry_stats[0][1]==8);
 puts("PASS: actual two-name listener policy, immediate first/later independent registration and serialized concurrent retry spacing; build qualification");
}
'''
with tempfile.TemporaryDirectory(prefix='marble-audio-retry-') as d:
 p=Path(d);c=p/'retry.c';c.write_text(head+body+tail)
 subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(p/'retry')],check=True)
 subprocess.run([str(p/'retry')],check=True)
