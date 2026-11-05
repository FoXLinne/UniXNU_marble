#!/usr/bin/env python3
"""Pack the iOS 18 UFS kernel window and persistent boot image."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

from build_ios18_arm64e import DEVICE, ADSP_FIRMWARE, read_adt, write_adt
from build_ios18_probe import patch_kernel
from build_pac_uefi import build_applications, pack_boot_image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--assets', required=True, type=Path)
    parser.add_argument('--images', required=True, type=Path)
    parser.add_argument('--data-seed', required=True, type=Path)
    args = parser.parse_args()
    root, out = args.root.resolve(), args.assets.resolve()
    out.mkdir(parents=True, exist_ok=True)
    platform = DEVICE.parent / 'marble_hackios'
    binaries = build_applications(out, platform, DEVICE.parent / 'UniXNU')
    kernel = out / 'kernel.macho'
    shutil.copyfile(root / 'kernel.fileset.macho', kernel)
    patch_kernel(root, kernel, out, platform, 'early')
    metadata = json.loads(args.data_seed.with_name('seed.json').read_text())
    data = kernel.read_bytes()
    data += bytes(-len(data) % 4096)
    offset = len(data)
    firmware = zlib.compress(ADSP_FIRMWARE.read_bytes(), 9)
    data += firmware
    data += bytes(-len(data) % 4096)
    mailbox = bytearray(1048576)
    struct.pack_into('<8sII', mailbox, 0, b'ROOTLOG1', 1, 0)
    struct.pack_into('<Q', mailbox, 56, metadata['logical_bytes'])
    mailbox[64:101] = metadata['volume_uuid'].upper().encode() + b'\0'
    struct.pack_into('<QQQ', mailbox, 104, offset, len(firmware), ADSP_FIRMWARE.stat().st_size)
    boot_args = ('offline-legacy-phase=4 offline-legacy-register=1 offline-legacy-gop=1 '
                 'offline-legacy-pm=1 offline-legacy-transfer=1 offline-legacy-retire=1 '
                 'offline-legacy-cpu-present=1 offline-legacy-validate=1 sheng-vsync=60 '
                 'offline-key-events=1 sheng-aks=1 rootstage-runtime-usb=3 marble-ufs=1')
    mailbox[128:128 + len(boot_args)] = boot_args.encode()
    data += mailbox
    window = out / 'KernelSeed.bin'
    window.write_bytes(data)
    images = json.loads(args.images.read_text())
    images.append(dict(name='KernelSeed', source=str(window), format='raw', raw_bytes=len(data),
                       sha256=hashlib.sha256(data).hexdigest()))
    header = bytearray(4096)
    struct.pack_into('<8sII', header, 0, b'SHNGROOT', 1, len(images))
    cursor = 1048576
    for index, image in enumerate(images):
        image['offset'] = cursor
        at = 24 + index * 128
        header[at:at + len(image['name'])] = image['name'].encode()
        struct.pack_into('<QQII', header, at + 32, cursor, image['raw_bytes'], 512, 1)
        header[at + 56:at + 88] = bytes.fromhex(image['sha256'])
        cursor = (cursor + image['raw_bytes'] + 1048575) & -1048576
    struct.pack_into('<Q', header, 16, cursor)
    header[4064:] = hashlib.sha256(header[:4064]).digest()
    (out / 'root_manifest.bin').write_bytes(header)
    (out / 'storage_images.json').write_text(json.dumps(images, indent=2) + '\n')
    stage = out / 'stage/EFI/UniXNU'
    payloads = stage / 'Payloads'
    payloads.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binaries / 'UniXNU.efi', stage / 'UniXNU.efi')
    shutil.copyfile(binaries / 'DarwinBoot.efi', payloads / 'DarwinBoot.efi')
    for name in ['kernel.macho.adt', 'kernel.macho.trustcache']:
        shutil.copyfile(root / 'boot/stage/EFI/UniXNU/Payloads' / name, payloads / name)
    adt = payloads / 'kernel.macho.adt'
    tree, _ = read_adt(adt.read_bytes())
    chosen = next(node for node in tree[1] if node[0].get('name') == b'chosen\0')
    chosen[0]['darwinboot-memory-size'] = struct.pack('<Q', 0x200000000)
    memory = next(node for node in chosen[1] if node[0].get('name') == b'memory-map\0')
    memory[0]['RootDisks'] = bytes(16)
    adt.write_bytes(write_adt(tree))
    shutil.copyfile(root / 'boot/stage/EFI/UniXNU/unixnu.toml', stage / 'unixnu.toml')
    volume = out / 'volume.img'
    with volume.open('wb') as stream:
        stream.truncate(4 * 1048576)
    subprocess.run(['mformat', '-i', volume, '-v', 'MARBLEESP', '::'], check=True)
    subprocess.run(['mcopy', '-i', volume, '-s', '-Q', stage.parent, '::/'], check=True)
    blobs = [('loader.efi', (binaries / 'MarbleBootstrap.efi').read_bytes()),
             ('root.usb', b'1'),
             ('root.storage', b'1'), ('volume.img', volume.read_bytes())]
    body = b''.join(struct.pack('<16sII', name.encode(), len(blob), 0) + blob + bytes(-len(blob) % 16)
                    for name, blob in blobs) + bytes(24)
    boot = pack_boot_image(out, platform, struct.pack('<II', 0x50554e58, len(body)) + body,
                           'marble_persistent_boot.img')
    print(boot)


if __name__ == '__main__':
    main()
