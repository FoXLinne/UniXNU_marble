"""Exercise the actual CoreFoundation capacity adapter and answer ownership."""
from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/storage-user-api';out.mkdir(parents=True,exist_ok=True)
source='''#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <string.h>
#include <sys/mount.h>
const CFStringRef kMGQDiskUsageTotalDiskCapacity=CFSTR("TotalDiskCapacity");
static CFTypeRef stub_answer;
CFTypeRef MGCopyAnswer(CFStringRef question,CFDictionaryRef options){(void)question;(void)options;return stub_answer?CFRetain(stub_answer):NULL;}
#define MARBLE_STORAGE_HOST_TEST 1
#include "SOURCE"
static int64_t total(CFTypeRef value){int64_t n=0;assert(CFNumberGetValue(CFDictionaryGetValue(value,kMGQDiskUsageTotalDiskCapacity),kCFNumberSInt64Type,&n));return n;}
int main(void){
 const int64_t gib=1024LL*1024*1024,nine=9*gib;
 CFMutableDictionaryRef fixture=CFDictionaryCreateMutable(kCFAllocatorDefault,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
 CFNumberRef number=CFNumberCreate(kCFAllocatorDefault,kCFNumberSInt64Type,&nine);
 CFDictionarySetValue(fixture,kMGQDiskUsageTotalDiskCapacity,number);CFRelease(number);
 CFDictionarySetValue(fixture,CFSTR("Unrelated"),CFSTR("preserved"));
 struct statfs v={0};strcpy(v.f_fstypename,"apfs");v.f_bsize=4096;v.f_blocks=64*gib/4096;v.f_bfree=63*gib/4096;v.f_bavail=62*gib/4096;
 CFTypeRef result=marble_storage_answer(CFRetain(fixture),&v);assert(result!=fixture&&total(result)==64*gib&&total(fixture)==nine);
 assert(CFEqual(CFDictionaryGetValue(result,CFSTR("Unrelated")),CFSTR("preserved")));assert(CFDictionaryGetCount(result)==2);CFRelease(result);
 strcpy(v.f_fstypename,"hfs");result=marble_storage_answer(CFRetain(fixture),&v);assert(result==fixture);CFRelease(result);
 strcpy(v.f_fstypename,"apfs");v.f_bavail=v.f_blocks+1;result=marble_storage_answer(CFRetain(fixture),&v);assert(result==fixture);CFRelease(result);
 v.f_bavail=62*gib/4096;v.f_bsize=1024;v.f_blocks*=4;v.f_bfree*=4;v.f_bavail*=4;
 result=marble_storage_answer(CFRetain(fixture),&v);assert(total(result)==64*gib);CFRelease(result);
 result=marble_storage_answer(CFRetain(fixture),NULL);assert(result==fixture);CFRelease(result);
 stub_answer=CFSTR("unrelated answer");result=marble_copy_storage_answer(CFSTR("Other"),NULL);assert(CFEqual(result,stub_answer));CFRelease(result);
 stub_answer=NULL;assert(marble_copy_storage_answer(CFSTR("Other"),NULL)==NULL);
 CFRelease(fixture);
 return 0;
}
'''.replace('SOURCE',str(repo/'package/ios27-runtime/userspace/MarbleStorageCapacity27.c'))
(out/'test.c').write_text(source)
subprocess.run(['xcrun','clang','-std=c11','-fsanitize=address,undefined','-Wall','-Wextra','-Werror','-framework','CoreFoundation',str(out/'test.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
print('PASS: MobileGestalt retained disk dictionary projects actual Data capacity; original dictionary and unrelated answers survive; invalid stats and block units checked')
