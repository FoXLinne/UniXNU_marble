"""Check the production first-loss recorder across owner changes and restart."""
from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/gpu-loss-record';out.mkdir(parents=True,exist_ok=True)
def extract(s,signature):
 start=s.index(signature);end=s.index('{',start)+1;depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
a=(repo/'ios27/sources/Adreno740/NativeAdrenoPublish.cpp').read_text();a=extract(a,'static bool nativeAdrenoLossMarker(')
b=(repo/'ios27/sources/Mesa/src/freedreno/vulkan/tu_knl_darwin.cc').read_text();b=extract(b,'extern "C" void\ntu_darwin_record_device_loss(')
head='''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
char native_adreno_loss_text[256];volatile uint32_t native_adreno_loss_record[4];
static unsigned darwin_device_error_state;static char darwin_device_error_text[192];
'''
body='''int main(){
 tu_darwin_record_device_loss("/Mesa/vk_sync_timeline.c",455,"Timeline values must strictly increase");
 assert(darwin_device_error_state==2&&strstr(darwin_device_error_text,"vk_sync_timeline.c:455 Timeline"));
 tu_darwin_record_device_loss("later.c",1,"later error");assert(strstr(darwin_device_error_text,"Timeline"));
 assert(!nativeAdrenoLossMarker(0xc000,68));
 assert(nativeAdrenoLossMarker(0x7ffd,68));
 nativeAdrenoLossMarker(0x7200|'!',999);assert(native_adreno_loss_record[2]==0);
 for(const char *p=darwin_device_error_text;*p;++p)nativeAdrenoLossMarker(0x7200|*p,68);
 nativeAdrenoLossMarker(0x7ffc,999);assert(native_adreno_loss_record[0]==1);
 nativeAdrenoLossMarker(0x7ffc,68);assert(native_adreno_loss_record[0]==2);
 assert(!strcmp(native_adreno_loss_text,darwin_device_error_text));
 nativeAdrenoLossMarker(0x7ffd,461);nativeAdrenoLossMarker(0x7200|'X',461);nativeAdrenoLossMarker(0x7ffc,461);
 assert(native_adreno_loss_record[1]==68&&native_adreno_loss_record[3]==2&&!strcmp(native_adreno_loss_text,darwin_device_error_text));
 memset((void*)native_adreno_loss_record,0,sizeof native_adreno_loss_record);nativeAdrenoLossMarker(0x7ffd,1);
 for(int i=0;i<1024;++i)nativeAdrenoLossMarker(0x7200|'a',1);
 nativeAdrenoLossMarker(0x7ffc,1);assert(strlen(native_adreno_loss_text)==255&&native_adreno_loss_record[2]==255);
 puts("PASS: Vulkan source reason retained; first kernel loss survives renderer restart; foreign pid ignored; bounds and commit state pass");
}
'''
(out/'test.cpp').write_text(head+a+'\n'+b+'\n'+body)
subprocess.run(['xcrun','clang++','-std=c++17','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',str(out/'test.cpp'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
