#!/usr/bin/env python3
"""Install the marble UFS images while retaining the Android filesystem."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import struct
import subprocess
import zlib


def data_store_boundary(partition_bytes, entries):
    kernel = next(image for image in entries if image['name'] == 'KernelSeed')
    identity = kernel['data_seed']
    logical = identity['logical_bytes']
    if not logical or logical % 4096 or partition_bytes % 4096:
        raise RuntimeError('Invalid persistent Data geometry')
    pages = logical // 4096
    bitmap_blocks = (pages + 32767) // 32768
    first = partition_bytes // 4096 - 256 - (1 + bitmap_blocks + pages)
    if first < 0 or any(image['offset'] + image['raw_bytes'] + 4 * 1024**2 > first * 4096 for image in entries):
        raise RuntimeError('Images overlap the persistent Data window')
    return first, pages, identity


def validate_data_store(header, pages, identity):
    if len(header) != 4096:
        raise RuntimeError('Short persistent Data header')
    if header[:8] != b'SHNGDATA':
        return
    if (struct.unpack_from('<II', header, 8) != (1, 64) or
            zlib.crc32(header[:60]) != struct.unpack_from('<I', header, 60)[0]):
        raise RuntimeError('Persistent Data header is damaged')
    if (struct.unpack_from('<Q', header, 16)[0] != pages or
            header[24:56].hex() != identity['logical_sha256']):
        raise RuntimeError('Installed Data belongs to a different seed')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=('partition','images','test','boot'))
    parser.add_argument('--serial',default='556fdfe')
    parser.add_argument('--cache',required=True,type=Path)
    parser.add_argument('--assets',required=True,type=Path)
    args=parser.parse_args()
    cache=args.cache.resolve(); assets=args.assets.resolve()
    plan=json.loads((cache/'storage_plan.json').read_text())
    adb=['adb','-s',args.serial]

    def root(command,**kwargs):
        return subprocess.run([*adb,'exec-out','su -c '+shlex.quote(command)],check=True,**kwargs)

    def read(command):
        return root(command,stdout=subprocess.PIPE,timeout=180).stdout

    def push(path):
        target='/data/local/tmp/'+path.name
        subprocess.run([*adb,'push',str(path),target],check=True)
        return target

    if args.action=='partition':
        current=read('dd if=/dev/block/sda bs=4096 count=4 2>/dev/null')
        original=(cache/'sda_gpt_primary.bin').read_bytes()
        changed=(cache/'sda_gpt_primary_new.bin').read_bytes()
        if current==changed:
            print('UniXNU partition present'); return
        if current!=original:
            raise RuntimeError('LUN 0 GPT differs from the saved Android layout')
        superblock=read('dd if=/dev/block/mapper/userdata bs=4096 count=1 2>/dev/null')
        blocks=struct.unpack_from('<Q',superblock,1060)[0]
        if blocks==plan['f2fs_old_blocks']:
            helper=push(cache/'f2fs_resize')
            root('chmod 755 '+helper+' && '+helper+' '+str(plan['f2fs_request_blocks']))
            superblock=read('dd if=/dev/block/mapper/userdata bs=4096 count=1 2>/dev/null')
            blocks=struct.unpack_from('<Q',superblock,1060)[0]
        if blocks!=plan['f2fs_new_blocks']:
            raise RuntimeError('F2FS size differs from the reserved UniXNU boundary')
        backup=push(cache/'sda_gpt_backup_new.bin'); primary=push(cache/'sda_gpt_primary_new.bin')
        root(f'dd if={backup} of=/dev/block/sda bs=4096 seek={plan["gpt_backup_lba"]} conv=notrunc && sync && '
             f'dd if={primary} of=/dev/block/sda bs=4096 conv=notrunc && sync')
        if read('dd if=/dev/block/sda bs=4096 count=4 2>/dev/null')!=changed:
            raise RuntimeError('GPT write differs from the installation layout')
        subprocess.run([*adb,'reboot'],check=True)
        return

    if args.action=='images':
        from root_control import image_stream
        size=int(read('blockdev --getsize64 /dev/block/by-name/unixnu'))
        if size!=plan['unixnu_bytes']:
            raise RuntimeError('UniXNU partition size differs from installation layout')
        entries=json.loads((assets/'storage_images.json').read_text())
        first,pages,identity=data_store_boundary(size,entries)
        data_header=read(f'dd if=/dev/block/by-name/unixnu bs=4096 skip={first} count=1 2>/dev/null')
        validate_data_store(data_header,pages,identity)
        old_header=read('dd if=/dev/block/by-name/unixnu bs=4096 count=1 2>/dev/null')
        installed={}
        checkpoint_file=cache/'install_checkpoint.json'
        checkpoint={'serial':args.serial,'unixnu_first':plan['unixnu_first'],'verified':{}}
        if checkpoint_file.exists():
            saved=json.loads(checkpoint_file.read_text())
            if saved['serial']==args.serial and saved['unixnu_first']==plan['unixnu_first']:
                checkpoint=saved
                for name,image in saved['verified'].items():
                    installed[name]=((image['offset'],image['raw_bytes']),image['sha256'])
        if (old_header[:8]==b'SHNGROOT' and len(old_header)==4096 and
            hashlib.sha256(old_header[:4064]).digest()==old_header[4064:]):
            count=struct.unpack_from('<I',old_header,12)[0]
            if 0<count<=4:
                for i in range(count):
                    entry=old_header[24+i*128:24+(i+1)*128]
                    installed[entry[:32].split(b'\0')[0].decode('ascii')]=(struct.unpack_from('<QQ',entry,32),entry[56:88].hex())
        def device_digest(offset,total):
            result=read(f'dd if=/dev/block/by-name/unixnu bs=1048576 skip={offset//1048576} count={(total+1048575)//1048576} '
                        f'2>/dev/null | head -c {total} | sha256sum').decode().split()
            if not result: raise RuntimeError('Device checksum response ended before completion')
            return result[0]

        for image in entries:
            offset=image['offset']; total=image['raw_bytes']
            if installed.get(image['name'])==((offset,total),image['sha256']):
                print(image['name'],'retained',flush=True); continue
            if offset%1048576 or offset+total>size:
                raise RuntimeError('Image exceeds UniXNU partition')
            if checkpoint['verified'] and device_digest(offset,total)==image['sha256']:
                checkpoint['verified'][image['name']]={key:image[key] for key in ('offset','raw_bytes','sha256')}
                checkpoint_file.write_text(json.dumps(checkpoint,indent=2)+'\n')
                print(image['name'],'retained after checksum',flush=True); continue
            command=f'dd of=/dev/block/by-name/unixnu bs=4096 seek={offset//4096} conv=notrunc'
            process=subprocess.Popen([*adb,'exec-in','su -c '+shlex.quote(command)],stdin=subprocess.PIPE)
            digest=hashlib.sha256(); remaining=total
            try:
                with image_stream(image) as stream:
                    while remaining:
                        block=stream.read(min(4*1024*1024,remaining))
                        if not block: raise RuntimeError('System image ended before its recorded length')
                        process.stdin.write(block); digest.update(block); remaining-=len(block)
                process.stdin.close()
                if process.wait()!=0: raise RuntimeError('UFS image write failed')
            except BaseException:
                process.terminate(); process.wait(); raise
            if digest.hexdigest()!=image['sha256']:
                raise RuntimeError('Source image differs from recorded identity')
            # Device hashes the persisted bytes, without transferring the image back to the host.
            result=device_digest(offset,total)
            if result!=image['sha256']:
                raise RuntimeError('Persisted image differs from source: '+image['name'])
            checkpoint['verified'][image['name']]={key:image[key] for key in ('offset','raw_bytes','sha256')}
            checkpoint_file.write_text(json.dumps(checkpoint,indent=2)+'\n')
            print(image['name'],total,'bytes persisted',flush=True)
        header=push(assets/'root_manifest.bin')
        root(f'dd if={header} of=/dev/block/by-name/unixnu bs=4096 count=1 conv=notrunc && sync')
        return

    image=assets/'marble_persistent_boot.img'
    if args.action=='test':
        subprocess.run([*adb,'reboot','bootloader'],check=True)
        subprocess.run(['fastboot','-s',args.serial,'boot',str(image)],check=True)
    else:
        target=push(image)
        root(f'dd if={target} of=/dev/block/by-name/boot_b bs=1048576 conv=notrunc && sync')
        root('bootctl set-active-boot-slot 1')
        # Qualcomm's boot HAL stores the successful flag in boot_b's GPT attributes.
        header=read('dd if=/dev/block/sde bs=4096 skip=1 count=1 2>/dev/null')
        entry_lba,count,entry_size=struct.unpack_from('<QII',header,72)
        blocks=(count*entry_size+4095)//4096
        primary=bytearray(read(f'dd if=/dev/block/sde bs=4096 count={entry_lba+blocks} 2>/dev/null'))
        backup_lba=struct.unpack_from('<Q',header,32)[0]-blocks
        backup=bytearray(read(f'dd if=/dev/block/sde bs=4096 skip={backup_lba} count={blocks+1} 2>/dev/null'))
        entries=bytearray(primary[entry_lba*4096:entry_lba*4096+count*entry_size])
        slot=next(i for i in range(count) if entries[i*entry_size+56:i*entry_size+128].decode('utf-16le').rstrip('\0')=='boot_b')
        at=slot*entry_size+48
        attributes=struct.unpack_from('<Q',entries,at)[0]|(1<<54)
        struct.pack_into('<Q',entries,at,attributes)
        primary[entry_lba*4096:entry_lba*4096+len(entries)]=entries;backup[:len(entries)]=entries
        for data,offset in [(primary,4096),(backup,blocks*4096)]:
            struct.pack_into('<I',data,offset+88,zlib.crc32(entries));struct.pack_into('<I',data,offset+16,0)
            size=struct.unpack_from('<I',data,offset+12)[0]
            struct.pack_into('<I',data,offset+16,zlib.crc32(data[offset:offset+size]))
        for name,data,offset in [('sde_slot_backup.bin',backup,backup_lba),('sde_slot_primary.bin',primary,0)]:
            path=cache/name;path.write_bytes(data);target=push(path)
            root(f'dd if={target} of=/dev/block/sde bs=4096 seek={offset} conv=notrunc && sync')
        root('bootctl is-slot-marked-successful 1')
        subprocess.run([*adb,'reboot'],check=True)


if __name__=='__main__':
    main()
