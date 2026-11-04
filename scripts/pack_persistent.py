#!/usr/bin/env python3
"""Pack the marble storage boot image and its kernel/Data seed window."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

ROOT=Path(__file__).resolve().parents[1]
STACK=ROOT.parent/'UniXNU'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets',required=True,type=Path)
    parser.add_argument('--base',required=True,type=Path)
    parser.add_argument('--data-seed',required=True,type=Path)
    parser.add_argument('--images',required=True,type=Path)
    args=parser.parse_args()
    out=args.assets.resolve(); base=args.base.resolve()
    seed=args.data_seed.read_bytes()
    meta=json.loads(args.data_seed.with_name('seed.json').read_text())
    if hashlib.sha256(seed).hexdigest()!=meta['sha256']:
        raise RuntimeError('Data seed differs from seed identity')
    data=(out/'kernel.macho').read_bytes()
    kernel_window=(len(data)+1048575)//1048576*1048576
    previous=out/'KernelSeed.bin'
    window_bytes=0
    if previous.exists():
        old=previous.read_bytes();mailbox_at=len(old)-1048576
        if old[mailbox_at:mailbox_at+8]!=b'ROOTLOG1':
            raise RuntimeError('Existing kernel window has an invalid mailbox')
        kernel_window=struct.unpack_from('<Q',old,mailbox_at+40)[0]
        window_bytes=len(old)
        if len(data)>kernel_window:
            raise RuntimeError('Kernel exceeds the installed window')
    data+=bytes(kernel_window-len(data))
    packed=zlib.compress(seed,6);offset=len(data);data+=packed;data+=bytes(-len(data)%1048576)
    if window_bytes:
        if len(data)>window_bytes-1048576:
            raise RuntimeError('Data seed exceeds the installed window')
        data+=bytes(window_bytes-1048576-len(data))
    boot_args=(base/'boot_arguments.txt').read_text().strip().split()
    boot_args=[arg for arg in boot_args if not arg.startswith('rootstage-panic-process=')]
    boot_args=' '.join('rootstage-runtime-usb=1' if arg=='rootstage-runtime-usb=2' else arg for arg in boot_args)+' marble-ufs=1'
    mailbox=struct.pack('<8sII',b'ROOTLOG1',1,0)+bytes(16)
    mailbox+=struct.pack('<I4xQQQ',0,offset,len(packed),len(seed))
    mailbox+=str(uuid.UUID(meta['volume_uuid'])).upper().encode()+b'\0'+bytes(27)
    mailbox+=boot_args.encode()+bytes(1048576-128-len(boot_args.encode()))
    data+=mailbox;(out/'KernelSeed.bin').write_bytes(data)
    images=json.loads(args.images.read_text())
    images.append(dict(name='KernelSeed',source=str(out/'KernelSeed.bin'),format='raw',raw_bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),
                       data_seed={key:meta[key] for key in ('logical_bytes','logical_sha256','volume_uuid')}))
    header=bytearray(4096);header[:8]=b'SHNGROOT';struct.pack_into('<II',header,8,1,len(images));cursor=1048576
    for index,image in enumerate(images):
        image['offset']=cursor;at=24+128*index
        header[at:at+len(image['name'])]=image['name'].encode()
        struct.pack_into('<QQII',header,at+32,cursor,image['raw_bytes'],512,1)
        header[at+56:at+88]=bytes.fromhex(image['sha256'])
        cursor=(cursor+image['raw_bytes']+1048575)//1048576*1048576
    struct.pack_into('<Q',header,16,cursor);header[4064:]=hashlib.sha256(header[:4064]).digest()
    (out/'root_manifest.bin').write_bytes(header)
    (out/'storage_images.json').write_text(json.dumps(images,indent=2)+'\n')
    stage=out/'stage/EFI/UniXNU';payload=stage/'Payloads';payload.mkdir(parents=True,exist_ok=True)
    binary=STACK/'out/marble/bootstrap/Build/MarbleBoot/RELEASE_CLANGPDB/AARCH64'
    for name,destination in [('UniXNU.efi',stage),('DarwinBoot.efi',payload)]:
        shutil.copyfile(binary/name,destination/name)
    for name in ['kernel.macho.adt','kernel.macho.trustcache','kernel.macho.gpuaddon']:
        shutil.copyfile(base/'stage/EFI/UniXNU/Payloads'/name,payload/name)
    shutil.copyfile(base/'stage/EFI/UniXNU/unixnu.toml',stage/'unixnu.toml')
    volume=out/'volume.img'
    with volume.open('wb') as stream:stream.truncate(16*1024*1024)
    subprocess.run(['mformat','-i',volume,'-v','MARBLEESP','::'],check=True)
    subprocess.run(['mcopy','-i',volume,'-s','-Q',stage.parent,'::/'],check=True)
    blobs=[('loader.efi',(binary/'MarbleBootstrap.efi').read_bytes()),('root.storage',b'1'),('volume.img',volume.read_bytes())]
    for image_name,firmware_name,entries in [
        ('marble_persistent_boot.img','firmware.bin',blobs),
        ('marble_usb_handoff.img','firmware_usb_handoff.bin',[blobs[0],('root.stage',b'1'),*blobs[1:]])
    ]:
        body=b''.join(struct.pack('<16sII',name.encode(),len(blob),0)+blob+bytes(-len(blob)%16) for name,blob in entries)+bytes(24)
        raw=bytearray((base/'firmware_prefix.bin').read_bytes()+struct.pack('<II',0x50554e58,len(body))+body)
        struct.pack_into('<Q',raw,16,(len(raw)+0x1fffff)&~0x1fffff)
        firmware=out/firmware_name;firmware.write_bytes(gzip.compress(raw,6)+(base/'platform.dtb').read_bytes())
        subprocess.run([sys.executable,str(ROOT.parent/'marble_hackios/Mu-Silicium/Resources/Scripts/mkbootimg.py'),'--kernel',firmware,
                        '--ramdisk',base/'ramdisk','--header_version','4','-o',out/image_name],check=True)
        print(out/image_name)

if __name__=='__main__':main()
