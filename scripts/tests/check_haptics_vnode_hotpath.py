"""Exercise the actual vnode hook, including its immutable-mode fast path."""
from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2]
out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/haptics-vnode'
out.mkdir(parents=True,exist_ok=True)
s=(repo/'package/MarbleMetal.c').read_text()
start=s.index('boolean_t native_marble_haptics_vnode(vfs_context_t context')
body=s[start:s.index('\n#ifdef MARBLE_IOS27',start)]
stubs=r'''
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#define MAXPATHLEN 1024
#define FALSE 0
#define VDIR 1
#define VCHR 2
typedef int boolean_t;
struct proc { const char *p_name; }; typedef struct proc *proc_t;
typedef proc_t vfs_context_t;
struct vnode { const char *path; int type; int error; }; typedef struct vnode *vnode_t;
struct componentname { unsigned cn_namelen; const char *cn_nameptr; };
static unsigned mode, parses, paths;
static proc_t vfs_context_proc(vfs_context_t p){return p;}
static int PE_parse_boot_argn(const char *s,void *p,int n){assert(!strcmp(s,"marble-haptics")&&n==4);__atomic_add_fetch(&parses,1,__ATOMIC_RELAXED);memcpy(p,&mode,4);return 1;}
static int vn_getpath(vnode_t v,char *p,int *n){__atomic_add_fetch(&paths,1,__ATOMIC_RELAXED);assert(*n>(int)strlen(v->path));strcpy(p,v->path);return v->error;}
static int vnode_vtype(vnode_t v){return v->type;}
'''
tests=r'''
static struct proc audio={"audiomxd"}, launch={"launchd"};
static struct vnode dir={"/dev",VDIR,0}, haptic={"/dev/marble-haptics",VCHR,0};
static void *race(void *p){(void)p; for(int i=0;i<10000;i++)assert(native_marble_haptics_vnode(&audio,&haptic,0)==(mode>=2));return 0;}
int main(int argc,char **argv){assert(argc==2);mode=(unsigned)strtoul(argv[1],0,0);
 for(int i=0;i<100000;i++)assert(!native_marble_haptics_vnode(&launch,&haptic,0));
 assert(!native_marble_haptics_vnode(0,&haptic,0));assert(parses==0&&paths==0);
 pthread_t t[8]; for(int i=0;i<8;i++)assert(!pthread_create(t+i,0,race,0));for(int i=0;i<8;i++)pthread_join(t[i],0);
 unsigned cold=parses;assert(cold>=1&&cold<=8);
 struct componentname c={14,"marble-haptics"};c.cn_namelen=(unsigned)strlen(c.cn_nameptr);
 assert(native_marble_haptics_vnode(&audio,&dir,&c)==(mode>=2));
 c.cn_nameptr="marble-speaker";c.cn_namelen=(unsigned)strlen(c.cn_nameptr);assert(native_marble_haptics_vnode(&audio,&dir,&c)==(mode>=2));
 c.cn_nameptr="marble-haptics-other";c.cn_namelen=(unsigned)strlen(c.cn_nameptr);assert(!native_marble_haptics_vnode(&audio,&dir,&c));
 struct vnode other={"/private/dev/marble-haptics",VCHR,0};assert(!native_marble_haptics_vnode(&audio,&other,0));
 other=haptic;other.type=VDIR;assert(!native_marble_haptics_vnode(&audio,&other,0));
 other=haptic;other.error=1;assert(!native_marble_haptics_vnode(&audio,&other,0));
 assert(parses==cold);printf("PASS mode=%u: unrelated lookups parse0; concurrent audio lookups retain mode and device matching\n",mode);
}
'''
(out/'test.c').write_text(stubs+body+tests)
subprocess.run(['xcrun','clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True)
for mode in ['0','1','2','3','4294967295']:subprocess.run([str(out/'test'),mode],check=True)
