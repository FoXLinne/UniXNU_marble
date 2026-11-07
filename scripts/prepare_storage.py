#!/usr/bin/env python3
"""Save Android boot/GPT backups and calculate the marble UFS allocation."""
import argparse
import json
from pathlib import Path
import shlex
import struct
import subprocess
import uuid
import zlib


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache',required=True,type=Path)
    parser.add_argument('--serial',default='556fdfe')
    parser.add_argument('--size-gib',type=int,default=32)
    args=parser.parse_args();cache=args.cache.resolve();cache.mkdir(parents=True,exist_ok=True)
    if args.size_gib <= 0:
        parser.error('--size-gib must be positive')
    linker=Path(__file__).resolve().parents[1]/'firmware/bin/ld.lld'
    subprocess.run(['clang','--target=aarch64-linux-gnu','-O2','-ffreestanding','-fno-stack-protector',
                    '-nostdlib','-static','-fuse-ld='+str(linker),str(Path(__file__).with_name('f2fs_resize.c')),
                    '-o',str(cache/'f2fs_resize')],check=True)
    def read(command):
        return subprocess.check_output(['adb','-s',args.serial,'exec-out','su -c '+shlex.quote(command)])
    primary=read('dd if=/dev/block/sda bs=4096 count=4 2>/dev/null')
    header=primary[4096:8192];entry_lba,count,entry_size=struct.unpack_from('<QII',header,72)
    if count!=64 or entry_size!=128 or entry_lba!=2:
        raise RuntimeError('Unexpected marble GPT geometry')
    entries=bytearray(primary[8192:16384]);names=[entries[i*128+56:(i+1)*128].decode('utf-16le').rstrip('\0') for i in range(count)]
    index=names.index('userdata');first,last=struct.unpack_from('<QQ',entries,index*128+32)
    userdata_old_last=last
    unixnu_index=names.index('unixnu') if 'unixnu' in names else None
    if unixnu_index is not None:
        old_first,old_last=struct.unpack_from('<QQ',entries,unixnu_index*128+32)
        if old_first!=last+1:
            raise RuntimeError('UniXNU must follow Android userdata')
        last=old_last
    if any(struct.unpack_from('<Q',entries,i*128+40)[0]>last
           for i,name in enumerate(names) if name):
        raise RuntimeError('UniXNU allocation must end at the last occupied partition')
    backup_lba=struct.unpack_from('<Q',header,32)[0]-2
    backup=read(f'dd if=/dev/block/sda bs=4096 skip={backup_lba} count=3 2>/dev/null')
    superblock=read('dd if=/dev/block/mapper/userdata bs=4096 count=1 2>/dev/null')
    if struct.unpack_from('<I',superblock,1024)[0]!=0xf2f52010:
        raise RuntimeError('Android userdata requires F2FS')
    old_blocks=struct.unpack_from('<Q',superblock,1060)[0]
    log_seg,segs_per_sec=struct.unpack_from('<II',superblock,1044)
    section=(1<<log_seg)*segs_per_sec
    boundary=(last+1-args.size_gib*1024**3//4096)//256*256
    limit=boundary-first
    request=(limit-old_blocks%section)//section*section
    actual=request+old_blocks%section
    if not 0<actual<old_blocks:
        raise RuntimeError('Requested allocation must shrink the current Android filesystem')
    new_first=(first+actual+255)//256*256
    for name,data in [('sda_gpt_primary.bin',primary),('sda_gpt_backup.bin',backup),('f2fs_superblock.bin',superblock)]:
        path=cache/name
        if path.exists() and path.read_bytes()!=data:
            raise RuntimeError('Existing backup differs: '+str(path))
        path.write_bytes(data)
    for name in ('boot_a','boot_b'):
        path=cache/(name+'.img')
        if path.exists():continue
        with path.open('wb') as stream:
            subprocess.run(['adb','-s',args.serial,'exec-out','su -c '+shlex.quote(f'dd if=/dev/block/by-name/{name} bs=1048576 2>/dev/null')],check=True,stdout=stream)
    free=unixnu_index if unixnu_index is not None else next(i for i in range(count) if entries[i*128:i*128+16]==bytes(16))
    struct.pack_into('<Q',entries,index*128+40,new_first-1)
    entry=bytearray(entries[free*128:(free+1)*128])
    if unixnu_index is None:
        entry[:16]=uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le;entry[16:32]=uuid.uuid4().bytes_le
    struct.pack_into('<QQ',entry,32,new_first,last);entry[56:68]='unixnu'.encode('utf-16le');entries[free*128:(free+1)*128]=entry
    primary=bytearray(primary);backup=bytearray(backup)
    for data,at in [(primary,4096),(backup,8192)]:
        struct.pack_into('<I',data,at+88,zlib.crc32(entries));struct.pack_into('<I',data,at+16,0)
        size=struct.unpack_from('<I',data,at+12)[0];struct.pack_into('<I',data,at+16,zlib.crc32(data[at:at+size]))
    primary[8192:16384]=entries;backup[:8192]=entries
    (cache/'sda_gpt_primary_new.bin').write_bytes(primary);(cache/'sda_gpt_backup_new.bin').write_bytes(backup)
    plan=dict(f2fs_old_blocks=old_blocks,f2fs_request_blocks=request,f2fs_new_blocks=actual,
              userdata_first=first,userdata_old_last=userdata_old_last,userdata_new_last=new_first-1,
              unixnu_first=new_first,unixnu_last=last,unixnu_bytes=(last-new_first+1)*4096,gpt_backup_lba=backup_lba)
    (cache/'storage_plan.json').write_text(json.dumps(plan,indent=2)+'\n')
    print(plan)

if __name__=='__main__':main()
