"""Exercise production capacity projection with independent System/Data volumes."""
from pathlib import Path
import subprocess
repo=Path(__file__).resolve().parents[2];out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/storage-capacity';out.mkdir(parents=True,exist_ok=True)
s=(repo/'package/ios27-runtime/MarbleStorageCapacity27.c').read_text();s='\n'.join(l for l in s.splitlines() if not l.startswith('#include'))
head='''#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
struct mount;struct vnode {unsigned v_flag;struct mount *v_mount;};
struct vfs_attr {uint64_t f_active,f_supported,f_bsize,f_blocks,f_bfree,f_bavail,f_bused,f_quota,f_reserved,f_files;};
struct mount {unsigned mnt_flag,mnt_kern_flag;struct vnode *mnt_vnodecovered;struct vfs_attr mnt_vfsstat;unsigned fsid,refs;};
typedef struct mount *mount_t;typedef struct vnode *vnode_t;typedef void *vfs_context_t;
#define NULLVP NULL
#define MNT_ROOTFS 1
#define MNT_UNION 2
#define VROOT 1
#define LK_NOWAIT 1
#define VFSATTR_f_bsize 1
#define VFSATTR_f_blocks 2
#define VFSATTR_f_bfree 4
#define VFSATTR_f_bavail 8
#define VFSATTR_f_bused 16
#define VFSATTR_f_quota 32
#define VFSATTR_f_reserved 64
#define VFSATTR_INIT(v) memset(v,0,sizeof *(v))
#define VFSATTR_IS_SUPPORTED(v,f) ((v)->f_supported & VFSATTR_ ## f)
#define VFSATTR_IS_ACTIVE(v,f) ((v)->f_active & VFSATTR_ ## f)
#define VFSATTR_RETURN(v,f,x) do{(v)->f=(x);(v)->f_supported|=VFSATTR_ ## f;}while(0)
static int enabled=1,busy_error,attr_error,reads;
static struct vfs_attr data_stats;
static int PE_parse_boot_argn(const char *s,void *p,size_t n){(void)s;assert(n==4);*(unsigned*)p=enabled;return 1;}
static void mount_ref(mount_t m,int n){(void)n;++m->refs;}
static void mount_drop(mount_t m,int n){(void)n;--m->refs;}
static void vfs_setmntsystemdata(mount_t m){m->mnt_kern_flag|=1;}
static int vfs_busy(mount_t m,int n){(void)m;(void)n;return busy_error;}
static void vfs_unbusy(mount_t m){(void)m;}
static int vfs_getattr(mount_t,struct vfs_attr*,vfs_context_t);
'''
tail='''
static int vfs_getattr(mount_t m,struct vfs_attr *a,vfs_context_t c){++reads;if(attr_error)return attr_error;*a=data_stats;native_marble_storage_getattr(m,a,c);return 0;}
int main(){
 const uint64_t gib=1024ULL*1024*1024;
 struct mount root={.mnt_flag=MNT_ROOTFS,.fsid=9},data={.fsid=64},other={.fsid=7};
 root.mnt_vfsstat=(struct vfs_attr){.f_bsize=4096,.f_blocks=9*gib/4096};
 data_stats=(struct vfs_attr){.f_active=127,.f_supported=127,.f_bsize=4096,.f_blocks=64*gib/4096,.f_bfree=63*gib/4096,.f_bavail=62*gib/4096,.f_bused=gib/4096,.f_quota=0,.f_reserved=0};
 struct vfs_attr original={.f_active=127,.f_supported=127,.f_bsize=4096,.f_blocks=9*gib/4096,.f_bfree=0,.f_bavail=0,.f_quota=123,.f_files=111},a=original;
 native_marble_storage_getattr(&root,&a,NULL);assert(a.f_blocks==original.f_blocks&&reads==0);
 native_marble_storage_register_data(&data);assert(data.refs==1&&data.mnt_kern_flag==1);
 native_marble_storage_register_data(&other);assert(!other.refs);
 native_marble_storage_getattr(&root,&a,NULL);
 assert(a.f_bsize*a.f_blocks==64*gib&&a.f_bsize*a.f_bavail==62*gib&&a.f_files==111&&a.f_quota==0);
 assert(root.fsid==9&&root.mnt_flag==MNT_ROOTFS&&data.fsid==64&&reads==1);
 struct vnode covered={.v_flag=VROOT,.v_mount=&root};struct mount overlay={.mnt_flag=MNT_UNION,.mnt_vnodecovered=&covered,.fsid=6};a=original;
 native_marble_storage_getattr(&overlay,&a,NULL);assert(a.f_blocks==data_stats.f_blocks&&overlay.fsid==6);
 a=original;native_marble_storage_getattr(&other,&a,NULL);assert(!memcmp(&a,&original,sizeof a));
 a=original;native_marble_storage_getattr(&data,&a,NULL);assert(!memcmp(&a,&original,sizeof a));
 busy_error=1;a=original;native_marble_storage_getattr(&root,&a,NULL);assert(!memcmp(&a,&original,sizeof a));busy_error=0;
 attr_error=1;a=original;native_marble_storage_getattr(&root,&a,NULL);assert(!memcmp(&a,&original,sizeof a));attr_error=0;
 data_stats.f_bavail=data_stats.f_blocks+1;a=original;native_marble_storage_getattr(&root,&a,NULL);assert(!memcmp(&a,&original,sizeof a));
 data_stats.f_bavail=62*gib/4096;data_stats.f_supported&=~(VFSATTR_f_quota|VFSATTR_f_reserved|VFSATTR_f_bused);a=original;
 native_marble_storage_getattr(&root,&a,NULL);assert(!(a.f_supported&VFSATTR_f_quota)&&a.f_bused==data_stats.f_blocks-data_stats.f_bfree);
 data_stats.f_bsize=1024;data_stats.f_blocks*=4;data_stats.f_bfree*=4;data_stats.f_bavail*=4;a=original;
 native_marble_storage_getattr(&root,&a,NULL);assert(a.f_bsize==1024&&a.f_bsize*a.f_blocks==64*gib&&a.f_bsize*a.f_bavail==62*gib);
 puts("PASS: root/union capacity uses Data64GiB and consistent free/used units; fsid/flags/other mounts unchanged; no recursion; mount/error/quota cases pass");
}
'''
(out/'test.c').write_text(head+s+tail)
subprocess.run(['xcrun','clang','-std=c11','-fsanitize=address,undefined','-Wall','-Wextra','-Werror',str(out/'test.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
