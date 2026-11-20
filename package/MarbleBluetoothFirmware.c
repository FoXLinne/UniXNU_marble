/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include "MarbleBluetoothFirmware.h"
extern volatile int marble_addon_install_result;
static bool firmware_name(const char *name) {
    if(!name || strncmp(name,"qca/",4))return false;
    name+=4;size_t n=strlen(name);if(n<8 || n>40)return false;
    if(strncmp(name,"hpbtfw",6) && strncmp(name,"hpnv",4) &&
       strncmp(name,"wcnhpbtfw",9) && strncmp(name,"wcnhpnv",7))return false;
    for(size_t i=0;i<n;i++)if(!((name[i]>='a' && name[i]<='z') ||
        (name[i]>='0' && name[i]<='9') || name[i]=='.' || name[i]=='_'))return false;
    return true;
}
int marble_bt_firmware_read(const char *name,uint8_t **data,size_t *size) {
    if(!data || !size)return EINVAL;
    *data=NULL;*size=0;
    if(!firmware_name(name))return ENOENT;
    if (__atomic_load_n(&marble_addon_install_result, __ATOMIC_ACQUIRE) != 1) return EAGAIN;
    char path[160];snprintf(path,sizeof(path),"%s/%s",MARBLE_BT_FIRMWARE_ROOT,name+4);
    vfs_context_t ctx=vfs_context_kernel();vnode_t vp=NULL;
    int error=vnode_open(path,FREAD,0,0,&vp,ctx);uint8_t *buffer=NULL;size_t length=0;
    if(!error) {
        struct vnode_attr va;VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);
        error=vnode_getattr(vp,&va,ctx);
        if(!error && (vnode_vtype(vp)!=VREG || !va.va_data_size || va.va_data_size>4u*1024*1024))error=EINVAL;
        if(!error) {
            length=(size_t)va.va_data_size;buffer=IOMallocData(length);
            if(!buffer)error=ENOMEM;
            else {
                int resid=0;
                error=vn_rdwr(UIO_READ,vp,(caddr_t)buffer,(int)length,0,UIO_SYSSPACE,IO_NOAUTH,
                    vfs_context_ucred(ctx),&resid,vfs_context_proc(ctx));
                if(!error && resid)error=EIO;
            }
        }
    }
    if(vp)vnode_close(vp,FREAD,ctx);
    if(error){if(buffer)IOFreeData(buffer,length);return error;}
    *data=buffer;*size=length;return 0;
}
void marble_bt_firmware_release(uint8_t *data,size_t size) {if(data)IOFreeData(data,size);}
