"""Compile the actual Apple physical-memory query with a denied page-size query."""
from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2]
out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/apple-memory-query';out.mkdir(parents=True,exist_ok=True)
s=(repo/'ios27/sources/Mesa/src/util/os_misc.c').read_text();start=s.index('bool\nos_get_total_physical_memory(');end=s.index('\n}',start)+2;body=s[start:end]
head='''#include <cassert>
#include <cstdint>
#include <cstddef>
#define HAVE_SYSCONF 1
#define HAVE_SC_PHYS_PAGES 1
#define DETECT_OS_APPLE 1
#define CTL_HW 6
#define HW_MEMSIZE 24
#define _SC_PHYS_PAGES 200
#define _SC_PAGESIZE 29
static int page_queries,byte_queries;static bool deny_bytes;
static long sysconf(int name){++page_queries;return name==_SC_PAGESIZE?-1:1000000;}
static int sysctl(int *mib,unsigned n,void *out,size_t *size,void *,size_t){assert(n==2&&mib[0]==CTL_HW&&mib[1]==HW_MEMSIZE);++byte_queries;if(deny_bytes)return -1;assert(*size==8);*(uint64_t*)out=6ULL<<30;return 0;}
'''
test='''
int main(){uint64_t size=0;assert(os_get_total_physical_memory(&size)&&size==(6ULL<<30)&&byte_queries==1&&!page_queries);deny_bytes=true;size=77;assert(!os_get_total_physical_memory(&size)&&size==77&&!page_queries);}
'''
(out/'query.cpp').write_text(head+body+test)
subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined',str(out/'query.cpp'),'-o',str(out/'query')],check=True);subprocess.run([str(out/'query')],check=True)
print('PASS: Apple uses byte-valued HW_MEMSIZE with page-size access denied; denied memory query propagates failure')
