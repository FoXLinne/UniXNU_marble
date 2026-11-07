#!/usr/bin/env python3
"""Pack the iOS 18 UFS kernel window and persistent boot image."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib
import zlib

from build_ios18_arm64e import DEVICE, ADSP_FIRMWARE, PROFILE as OS_PROFILE, read_adt, write_adt, validate_prelink
from build_pac_uefi import build_applications, pack_boot_image

USB_AFFINITY = int(re.search(r'^#define MARBLE_USB_AFFINITY (0x[0-9a-fA-F]+)$',
    (DEVICE / 'package/MarblePkg/MarbleCpuOwnership.h').read_text(), re.M).group(1), 16)
XNU_AFFINITIES = [value for value in range(0, 0x800, 0x100) if value != USB_AFFINITY]


def persistent_data_metadata(seed, profile_path):
    metadata = json.loads(seed.with_name('seed.json').read_text())
    profile = json.loads(profile_path.read_text())
    if metadata['logical_bytes'] != profile['data_logical_bytes']:
        raise ValueError('Persistent Data size differs from the board storage profile')
    if metadata['volume_uuid'].upper() != profile['data_volume_uuid'].upper():
        raise ValueError('Persistent Data UUID differs from the board storage profile')
    if metadata.get('source_build') != profile['build']:
        raise ValueError('Data metadata OS build differs from the board storage profile')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--assets', required=True, type=Path)
    parser.add_argument('--images', required=True, type=Path)
    parser.add_argument('--data-seed', required=True, type=Path)
    parser.add_argument('--storage-profile', type=Path, default=DEVICE / 'profiles/marble-storage-22H384.json',
                        help='Persistent Data geometry and identity for this board')
    parser.add_argument('--temporary', action='store_true',
                        help='Embed the kernel and retain the RootStage USB service')
    parser.add_argument('--vsync', type=int, default=120,
                        help='Display presentation cadence')
    parser.add_argument('--gpu-addon', type=Path, help='22H384 NGPUADD2 bundle; defaults to ROOT/gpu/metal.gpuaddon')
    parser.add_argument('--display-scale', type=int, choices=(2, 3), default=3,
                        help='Pixels per UI point on the 1080x2400 panel')
    parser.add_argument('--cpu-regs', default=','.join(hex(value) for value in XNU_AFFINITIES),
                        help='XNU CPU affinities excluding the resident USB service owner')
    parser.add_argument('--disabled-jobs', default='com.apple.CommCenter,com.apple.locationd',
                        help='Board services excluded until their hardware backend is available')
    parser.add_argument('--kernel-arg', action='append', default=[], help='Additional NAME=VALUE runtime setting')
    parser.add_argument('--skip-setup', action='store_true', help='Persist Setup completion before launchd starts')
    args = parser.parse_args()
    identity = tomllib.loads(OS_PROFILE.read_text())['ui_identity']
    cpu_regs = [int(value, 0) for value in args.cpu_regs.split(',')]
    if not cpu_regs or cpu_regs[0] != 0 or len(set(cpu_regs)) != len(cpu_regs) or any(
            value not in XNU_AFFINITIES for value in cpu_regs):
        parser.error('--cpu-regs must select distinct XNU-owned affinities, starting with 0')
    metadata = persistent_data_metadata(args.data_seed.resolve(), args.storage_profile.resolve())
    root, out = args.root.resolve(), args.assets.resolve()
    out.mkdir(parents=True, exist_ok=True)
    platform = DEVICE.parent / 'marble_hackios'
    binaries = build_applications(out, platform, DEVICE.parent / 'UniXNU')
    kernel = out / 'kernel.macho'
    shutil.copyfile(root / 'kernel.fileset.macho', kernel)
    baseline = validate_prelink(root)
    if hashlib.sha256(kernel.read_bytes()).hexdigest() != baseline['candidate_sha256']:
        raise ValueError('Kernel differs from the current prelink report')
    addon = (args.gpu_addon or root / 'gpu/metal.gpuaddon').resolve()
    gpu = json.loads((root / 'gpu/userspace.json').read_text())
    if hashlib.sha256(addon.read_bytes()).hexdigest() != gpu['addon_sha256']:
        raise ValueError('GPU addon differs from its signed userspace build')
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
                 'offline-legacy-cpu-present=1 offline-legacy-validate=1 '
                 f'sheng-vsync={args.vsync} '
                 'offline-key-events=1 sheng-aks=1 '
                 'rootstage-runtime-usb=3 marble-ufs=1 owner-staged-apps=1 '
                 'marble-adreno=1 sheng-fb-gpu=1 sheng-gpu-addon=1 sheng-fb-bgra=1 '
                 'sheng-gpu-preempt=2 sheng-gpu-ttbr=2 '
                 'sheng-early-retire=0 marble-display-flip=1')
    for value in args.kernel_arg:
        if not value or any(ch.isspace() for ch in value) or '\0' in value:
            parser.error('--kernel-arg requires one NAME=VALUE token')
        boot_args += ' ' + value
    cpu_args = f'cpus={len(cpu_regs)} sheng-smp={int(len(cpu_regs) > 1)} armcpu-self-ipi=1'
    boot_args += ' ' + cpu_args + f' marble-display-scale={args.display_scale}'
    boot_args += f' marble-ui-identity={identity["generation"]}'
    if args.disabled_jobs:
        boot_args += ' sheng-disable-jobs=' + args.disabled_jobs
    if args.skip_setup:
        boot_args += ' marble-skip-setup=1'
    mailbox[128:128 + len(boot_args)] = boot_args.encode()
    data += mailbox
    window = out / 'KernelSeed.bin'
    window.write_bytes(data)
    images = [image for image in json.loads(args.images.read_text()) if image['name'] != 'KernelSeed']
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
    shutil.copyfile(addon, payloads / 'kernel.macho.gpuaddon')
    # Preserve each original trust module and append the owner-built GPU module.
    original_trust = (payloads / 'kernel.macho.trustcache').read_bytes()
    count, = struct.unpack_from('<I', original_trust)
    offsets = struct.unpack_from(f'<{count}I', original_trust, 4)
    modules = [original_trust[start:end] for start, end in zip(offsets, (*offsets[1:], len(original_trust)))]
    owner = root / 'gpu/owner.trustcache'
    owner_bytes = owner.read_bytes()
    if hashlib.sha256(owner_bytes).hexdigest() != gpu['trust_cache_sha256']:
        raise ValueError('GPU owner trust module differs from the userspace build')
    if owner_bytes not in modules:
        modules.append(owner_bytes)
    cursor = 4 + 4 * len(modules)
    offsets = []
    for module in modules:
        offsets.append(cursor)
        cursor += len(module)
    (payloads / 'kernel.macho.trustcache').write_bytes(
        struct.pack(f'<{1 + len(modules)}I', len(modules), *offsets) + b''.join(modules))
    adt = payloads / 'kernel.macho.adt'
    tree, _ = read_adt(adt.read_bytes())
    cpus = next(node for node in tree[1] if node[0].get('name') == b'cpus\0')
    template = cpus[1][0][0]
    cpus[1][:] = [({**template, 'name': f'cpu{index}\0'.encode(),
                    'reg': struct.pack('<I', affinity),
                    'state': b'running\0' if index == 0 else b'waiting\0'}, [])
                  for index, affinity in enumerate(cpu_regs)]
    if not any(node[0].get('name') == b'psci\0' for node in tree[1]):
        tree[1].append(({'name': b'psci\0', 'compatible': b'arm,psci-1.0\0', 'method': b'smc\0'}, []))
    chosen = next(node for node in tree[1] if node[0].get('name') == b'chosen\0')
    # Keep display geometry and artwork density in agreement on the phone panel.
    chosen[0]['display-scale'] = struct.pack('<I', args.display_scale)
    product = next(node for node in tree[1] if node[0].get('name') == b'product\0')
    product[0]['artwork-scale-factor'] = struct.pack('<I', args.display_scale)
    tree[0]['model'] = identity['product'].encode() + b'\0'
    tree[0]['target-type'] = identity['target_type'].encode() + b'\0'
    for key in ('product-name', 'product-description'):
        product[0][key] = identity['name'].encode() + b'\0'
    product[0]['model'] = identity['product'].encode() + b'\0'
    product[0]['artwork-device-subtype'] = struct.pack('<I', identity['artwork_device_subtype'])
    product[0]['compatible-device-fallback'] = identity['compatible_device_fallback'].encode() + b'\0'
    chosen[0]['darwinboot-memory-size'] = struct.pack('<Q', 0x200000000)
    memory = next(node for node in chosen[1] if node[0].get('name') == b'memory-map\0')
    memory[0]['RootDisks'] = bytes(16)
    memory[0]['GpuAddon'] = bytes(16)
    adt.write_bytes(write_adt(tree))
    config = (root / 'boot/stage/EFI/UniXNU/unixnu.toml').read_text()
    config = config.replace('args = "', 'args = "rootstage-runtime-usb=3 ' + cpu_args + ' ', 1)
    (stage / 'unixnu.toml').write_text(config)
    volume = out / 'volume.img'
    volume_size = sum(path.stat().st_size for path in stage.rglob('*') if path.is_file()) + 4 * 1048576
    volume_size = (volume_size + 1048575) & -1048576
    with volume.open('wb') as stream:
        stream.truncate(volume_size)
    subprocess.run(['mformat', '-i', volume, '-v', 'MARBLEESP', '::'], check=True)
    subprocess.run(['mcopy', '-i', volume, '-s', '-Q', stage.parent, '::/'], check=True)
    blobs = [('loader.efi', (binaries / 'MarbleBootstrap.efi').read_bytes())]
    if args.temporary:
        blobs.append(('kernel.seed', data))
    blobs.append(('root.usb', b'1'))
    blobs += [('root.storage', b'1'), ('volume.img', volume.read_bytes())]
    body = b''.join(struct.pack('<16sII', name.encode(), len(blob), 0) + blob + bytes(-len(blob) % 16)
                    for name, blob in blobs) + bytes(24)
    boot = pack_boot_image(out, platform, struct.pack('<II', 0x50554e58, len(body)) + body,
                           'marble_temporary_boot.img' if args.temporary else 'marble_persistent_boot.img')
    if args.temporary:
        (out / 'temporary-boot.json').write_text(json.dumps(dict(
            image=str(boot), kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(),
            native_sha256=baseline['inputs']['native_sha256'],
            kernel_source='boot-image', system_source='ufs', exit_boot_services=False,
            resident_usb=True, runtime_usb_mode=3, enabled_cpu_affinities=cpu_regs,
            firmware_usb_affinity=USB_AFFINITY, display_scale=args.display_scale, ui_identity=identity,
            data_logical_bytes=metadata['logical_bytes'], data_uuid=metadata['volume_uuid'],
            storage_profile_sha256=hashlib.sha256(args.storage_profile.read_bytes()).hexdigest(),
            logical_size=[1080 // args.display_scale, 2400 // args.display_scale], boot_args=boot_args,
            gpu_addon_sha256=gpu['addon_sha256'], gpu_sources=gpu['sources'],
            owner_trust_cache_sha256=gpu['trust_cache_sha256']), indent=2) + '\n')
    print(boot)


if __name__ == '__main__':
    main()
