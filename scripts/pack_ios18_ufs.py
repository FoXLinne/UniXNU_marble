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
from marble_display_geometry import display_geometry, display_layout

from build_ios18_arm64e import DEVICE, ADSP_FIRMWARE, PROFILE as OS_PROFILE, read_adt, write_adt, validate_prelink
from build_pac_uefi import build_applications, pack_boot_image

USB_AFFINITY = int(re.search(r'^#define MARBLE_USB_AFFINITY (0x[0-9a-fA-F]+)$',
    (DEVICE / 'package/MarblePkg/MarbleCpuOwnership.h').read_text(), re.M).group(1), 16)
XNU_AFFINITIES = [value for value in range(0, 0x800, 0x100) if value != USB_AFFINITY]


def merge_kernel_args(base, additions):
    tokens = base.split()
    for value in additions:
        if not value or any(ch.isspace() for ch in value) or '\0' in value:
            raise ValueError('--kernel-arg requires one NAME=VALUE token')
        name = value.split('=', 1)[0]
        if not name:
            raise ValueError('--kernel-arg requires a setting name')
        tokens = [token for token in tokens if token.split('=', 1)[0] != name]
        tokens.append(value)
    return ' '.join(tokens)


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


def append_appsync(addon, runtime, output):
    report = json.loads((runtime / 'report.json').read_text())
    if (report['target'], report['build']) != ('ios18', '22H384'):
        raise ValueError('AppSync runtime must target 22H384')
    for relative, record in report['files'].items():
        if hashlib.sha256((runtime / relative).read_bytes()).hexdigest() != record['sha256']:
            raise ValueError('AppSync executable differs from its build: ' + relative)
    blob = addon.read_bytes()
    if blob[:8] != b'NGPUADD2' or struct.unpack_from('<Q', blob, 16)[0] != len(blob):
        raise ValueError('AppSync requires an NGPUADD2 base addon')
    count, = struct.unpack_from('<I', blob, 8)
    body = bytearray(blob[24:])
    for prefix, directory in report['directories'].items():
        for path in sorted((runtime / directory).rglob('*')):
            if not path.is_file():
                continue
            name = prefix + '/' + path.relative_to(runtime / directory).as_posix()
            encoded = name.encode() + b'\0'
            if len(encoded) > 160 or '..' in Path(name).parts or name.startswith('/'):
                raise ValueError('Invalid AppSync addon path: ' + name)
            data = path.read_bytes()
            packed = zlib.compress(data, 9)
            body += struct.pack('<IIQQ', len(encoded), path.stat().st_mode & 0o777, len(data), len(packed))
            body += encoded + bytes(-len(encoded) % 8) + packed + bytes(-len(packed) % 8)
            count += 1
    output.write_bytes(b'NGPUADD2' + struct.pack('<IIQ', count, 0, len(body) + 24) + body)
    return output, report


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
    parser.add_argument('--hold-rootstage', action='store_true',
                        help='Wait for the RootStage continue command before entering XNU')
    parser.add_argument('--usb-owner', choices=('firmware','kernel'), default='kernel',
                        help='Controller owner while XNU runs; kernel ownership uses all eight CPUs')
    parser.add_argument('--vsync', type=int, default=120,
                        help='Display presentation cadence')
    parser.add_argument('--gpu-addon', type=Path, help='22H384 NGPUADD2 bundle; defaults to ROOT/gpu/metal.gpuaddon')
    parser.add_argument('--appsync-runtime', type=Path, help='Built 22H384 AppSync runtime directory')
    parser.add_argument('--wifi-runtime', type=Path, help='Signed iOS 18 supplicant and launchd cache')
    parser.add_argument('--wifi-firmware', type=Path, help='WCN6855 firmware matching profiles/wifi-wcn6855-hw21.json')
    parser.add_argument('--kernel', type=Path, help='New fileset kernel built from the current device sources')
    parser.add_argument('--firmware-binaries', type=Path, help='Built MarbleBoot AARCH64 applications for this candidate')
    parser.add_argument('--baseline', type=Path,
                        help='Running candidate directory containing temporary-boot.json and kernel.macho')
    parser.add_argument('--display-scale', type=int, choices=(2, 3),
                        help='Integer artwork scale; FrontBoard uses the panel-to-layout point scale')
    parser.add_argument('--cpu-regs', default=','.join(hex(value) for value in XNU_AFFINITIES),
                        help='XNU CPU affinities excluding the resident USB service owner')
    parser.add_argument('--disabled-jobs', default='com.apple.CommCenter,com.apple.locationd',
                        help='Board services excluded until their hardware backend is available')
    parser.add_argument('--kernel-arg', action='append', default=[], help='Additional NAME=VALUE runtime setting')
    parser.add_argument('--skip-setup', action='store_true', help='Persist Setup completion before launchd starts')
    args = parser.parse_args()
    candidate = (json.loads((args.baseline / 'temporary-boot.json').read_text())
                 if args.baseline else None)
    identity = tomllib.loads(OS_PROFILE.read_text())['ui_identity']
    if candidate:
        args.cpu_regs = ','.join(hex(value) for value in candidate['enabled_cpu_affinities'])
        if args.display_scale is None:
            args.display_scale = display_geometry()['scale']
        if args.usb_owner=='firmware' and candidate['firmware_usb_affinity'] != USB_AFFINITY:
            raise ValueError('Candidate CPU ownership differs from this firmware')
    native_usb=args.usb_owner=='kernel'
    allowed_affinities=list(range(0,0x800,0x100)) if native_usb else XNU_AFFINITIES
    if native_usb:args.cpu_regs=','.join(hex(value) for value in allowed_affinities)
    if args.display_scale is None:
        args.display_scale = display_geometry()['scale']
    cpu_regs = [int(value, 0) for value in args.cpu_regs.split(',')]
    if not cpu_regs or cpu_regs[0] != 0 or len(set(cpu_regs)) != len(cpu_regs) or any(
            value not in allowed_affinities for value in cpu_regs):
        parser.error('--cpu-regs must select distinct XNU-owned affinities, starting with 0')
    metadata = persistent_data_metadata(args.data_seed.resolve(), args.storage_profile.resolve())
    root, out = args.root.resolve(), args.assets.resolve()
    out.mkdir(parents=True, exist_ok=True)
    platform = DEVICE / 'firmware'
    binaries = (args.firmware_binaries.resolve() if args.firmware_binaries else
                args.baseline / 'Build/MarbleBoot/RELEASE_CLANGPDB/AARCH64' if candidate
                else build_applications(out, platform, DEVICE.parent / 'UniXNU'))
    kernel = out / 'kernel.macho'
    selected_kernel = args.kernel or (args.baseline / 'kernel.macho' if candidate else root / 'kernel.fileset.macho')
    shutil.copyfile(selected_kernel, kernel)
    baseline = ({'candidate_sha256': candidate['kernel_sha256'],
                 'inputs': {'native_sha256': candidate['native_sha256']}}
                if candidate and args.kernel is None else validate_prelink(root))
    if hashlib.sha256(kernel.read_bytes()).hexdigest() != baseline['candidate_sha256']:
        raise ValueError('Kernel differs from the current prelink report')
    addon = (args.gpu_addon or root / 'gpu/metal.gpuaddon').resolve()
    gpu = json.loads((root / 'gpu/userspace.json').read_text())
    if hashlib.sha256(addon.read_bytes()).hexdigest() != gpu['addon_sha256']:
        raise ValueError('GPU addon differs from its signed userspace build')
    appsync = None
    if args.appsync_runtime:
        addon, appsync = append_appsync(addon, args.appsync_runtime.resolve(), out / 'runtime.gpuaddon')
    wifi_runtime = None
    if args.wifi_runtime:
        wifi_runtime = json.loads((args.wifi_runtime / 'report.json').read_text())
        if (wifi_runtime['target'], wifi_runtime['build']) != ('ios18', '22H384'):
            raise ValueError('Wi-Fi runtime must target 22H384')
        for relative, digest in wifi_runtime['files'].items():
            if hashlib.sha256((args.wifi_runtime / relative).read_bytes()).hexdigest() != digest:
                raise ValueError('Wi-Fi runtime differs from build: ' + relative)
        directories = []
        for prefix, relative in wifi_runtime['directories'].items():
            directories += [prefix, args.wifi_runtime / relative]
        wifi_addon = out / 'wifi-runtime.gpuaddon'
        subprocess.run([sys.executable, DEVICE / 'ios18/source/scripts/addon-add-dir.py',
                        addon, wifi_addon, *directories], check=True)
        addon = wifi_addon
        args.kernel_arg.append('marble-wifi-service=1')
    wifi_firmware = None
    if args.wifi_firmware:
        wifi_firmware = json.loads((DEVICE / 'profiles/wifi-wcn6855-hw21.json').read_text())
        for entry in wifi_firmware['files']:
            path = args.wifi_firmware / entry['name']
            if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
                raise ValueError('Wi-Fi firmware differs from selected profile: ' + entry['name'])
        prefix = re.search(r'^#define MARBLE_WIFI_FIRMWARE_PREFIX "([^"]+)"',
                           (DEVICE / 'package/MarbleWifiFirmware.h').read_text(), re.M).group(1)
        wifi_addon = out / 'wifi.gpuaddon'
        subprocess.run([sys.executable, DEVICE / 'ios18/source/scripts/addon-add-dir.py',
                        addon, wifi_addon, prefix, args.wifi_firmware.resolve()], check=True)
        addon = wifi_addon
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
                 'sheng-early-retire=0 marble-display-flip=0')
    cpu_args = f'cpus={len(cpu_regs)} sheng-smp={int(len(cpu_regs) > 1)} armcpu-self-ipi=1'
    boot_args += ' ' + cpu_args + f' marble-display-scale={args.display_scale}'
    boot_args += f' marble-ui-identity={identity["generation"]}'
    if args.disabled_jobs:
        boot_args += ' sheng-disable-jobs=' + args.disabled_jobs
    if args.skip_setup:
        boot_args += ' marble-skip-setup=1'
    if candidate:
        boot_args = candidate['boot_args']
        if (candidate['data_uuid'].upper() != metadata['volume_uuid'].upper()
                or candidate['data_logical_bytes'] != metadata['logical_bytes']):
            raise ValueError('Candidate Data identity differs from the selected installation')
    try:
        if native_usb:
            args.kernel_arg+=['rootstage-runtime-usb=1','marble-native-services=1',f'cpus={len(cpu_regs)}']
        if appsync:
            args.kernel_arg.append('appsync=1')
        boot_args = merge_kernel_args(boot_args, ['sheng-aks-sls=3', *args.kernel_arg,
                                                      f'marble-ui-identity={identity["generation"]}',
                                                      f'marble-display-scale={args.display_scale}'])
    except ValueError as error:
        parser.error(str(error))
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
        template = args.baseline if candidate else root / 'boot'
        shutil.copyfile(template / 'stage/EFI/UniXNU/Payloads' / name, payloads / name)
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
    if appsync:
        hashes = sorted({bytes.fromhex(record['cdhash']) for record in appsync['files'].values()})
        module = struct.pack('<I16sI', 2, hashlib.sha256(b''.join(hashes)).digest()[:16], len(hashes))
        module += b''.join(value + bytes([2, 0, 0, 0]) for value in hashes)
        if module not in modules:
            modules.append(module)
    if wifi_runtime:
        hashes = sorted({bytes.fromhex(value) for value in wifi_runtime['cdhashes']})
        module = struct.pack('<I16sI', 2, hashlib.sha256(b''.join(hashes)).digest()[:16], len(hashes))
        module += b''.join(value + bytes([2, 0, 0, 0]) for value in hashes)
        modules.append(module)
    cursor = 4 + 4 * len(modules)
    offsets = []
    for module in modules:
        offsets.append(cursor)
        cursor += len(module)
    (payloads / 'kernel.macho.trustcache').write_bytes(
        struct.pack(f'<{1 + len(modules)}I', len(modules), *offsets) + b''.join(modules))
    adt = payloads / 'kernel.macho.adt'
    tree, _ = read_adt(adt.read_bytes())
    if wifi_runtime:
        armio = next(node for node in tree[1] if node[0].get('name') == b'arm-io\0')
        chosen = next(node for node in tree[1] if node[0].get('name') == b'chosen\0')
        seed = int.from_bytes(chosen[0]['unique-chip-id'][:8], 'little')
        hashed = 0xcbf29ce484222325
        for shift in range(0, 64, 8):
            hashed = ((hashed ^ ((seed >> shift) & 255)) * 0x100000001b3) & 0xffffffffffffffff
        mac = bytearray(hashed.to_bytes(8, 'little')[:6])
        mac[0] = (mac[0] & 0xfe) | 2
        chosen[0]['mac-address-wifi0'] = bytes(mac)
        wlan = next((node for node in armio[1] if node[0].get('name') == b'wlan\0'), None)
        if wlan is None:
            wlan = ({'name': b'wlan\0', 'compatible': b'qcom,wcn6855\0', 'status': b'okay\0'}, [])
            armio[1].append(wlan)
        wlan[0]['local-mac-address'] = bytes(mac)
    cpus = next(node for node in tree[1] if node[0].get('name') == b'cpus\0')
    template = cpus[1][0][0]
    cpus[1][:] = [({**template, 'name': f'cpu{index}\0'.encode(),
                    'reg': struct.pack('<I', affinity),
                    'cluster-type': struct.pack('<I', ord('E' if affinity < 0x400 else 'P')),
                    'compatible': (b'arm,cortex-a510\0' if affinity < 0x400 else
                                   b'arm,cortex-a710\0' if affinity < 0x700 else b'arm,cortex-x2\0'),
                    'state': b'running\0' if index == 0 else b'waiting\0'}, [])
                  for index, affinity in enumerate(cpu_regs)]
    if not any(node[0].get('name') == b'psci\0' for node in tree[1]):
        tree[1].append(({'name': b'psci\0', 'compatible': b'arm,psci-1.0\0', 'method': b'smc\0'}, []))
    chosen = next(node for node in tree[1] if node[0].get('name') == b'chosen\0')
    # Keep display geometry and artwork density in agreement on the phone panel.
    chosen[0]['display-scale'] = struct.pack('<I', args.display_scale)
    geometry = display_geometry()
    chosen[0]['dot-pitch'] = struct.pack('<I', geometry['pixels_per_inch'])
    chosen[0]['display-physical-size-mm'] = struct.pack('<II', geometry['width_mm'], geometry['height_mm'])
    product = next(node for node in tree[1] if node[0].get('name') == b'product\0')
    product[0]['artwork-scale-factor'] = struct.pack('<I', args.display_scale)
    tree[0]['model'] = identity['product'].encode() + b'\0'
    tree[0]['target-type'] = identity['target_type'].encode() + b'\0'
    tree[0]['ui-hardware-model'] = identity['hardware_model'].encode() + b'\0'
    for key in ('product-name', 'product-description'):
        product[0][key] = identity['name'].encode() + b'\0'
    product[0]['model'] = identity['product'].encode() + b'\0'
    product[0]['artwork-device-subtype'] = struct.pack('<I', identity['artwork_device_subtype'])
    product[0]['large-format-phone'] = struct.pack('<I', int(identity['large_format_phone']))
    product[0]['compatible-device-fallback'] = identity['compatible_device_fallback'].encode() + b'\0'
    layout = display_layout()
    corner = layout['corner_radius_pixels'] / layout['native_points_scale']
    product[0]['display-corner-radius'] = struct.pack('<II', round(corner * 10000), 10000)
    product[0]['island-notch-location'] = struct.pack('<I', identity['island_notch_location'])
    product[0]['chrome-identifier'] = identity['chrome_identifier'].encode() + b'\0'
    product[0]['oled-display'] = struct.pack('<I', int(identity['oled_display']))
    product[0]['udid-version'] = struct.pack('<I', identity['udid_version'])
    chosen[0]['darwinboot-memory-size'] = struct.pack('<Q', 0x200000000)
    memory = next(node for node in chosen[1] if node[0].get('name') == b'memory-map\0')
    memory[0]['RootDisks'] = bytes(16)
    memory[0]['GpuAddon'] = bytes(16)
    adt.write_bytes(write_adt(tree))
    template = args.baseline if candidate else root / 'boot'
    config = (template / 'stage/EFI/UniXNU/unixnu.toml').read_text()
    config = re.sub(r'(?m)^(\s*args\s*=\s*)"[^"\n]*"',
                    lambda match: match[1] + json.dumps(boot_args), config, count=1)
    (stage / 'unixnu.toml').write_text(config)
    volume = out / 'volume.img'
    volume_size = sum(path.stat().st_size for path in stage.rglob('*') if path.is_file()) + 4 * 1048576
    volume_size = (volume_size + 1048575) & -1048576
    with volume.open('wb') as stream:
        stream.truncate(volume_size)
    subprocess.run(['mformat', '-i', volume, '-v', 'MARBLEESP', '::'], check=True)
    subprocess.run(['mcopy', '-i', volume, '-s', '-Q', stage.parent, '::/'], check=True)
    blobs = [('loader.efi', (binaries / 'MarbleBootstrap.efi').read_bytes())]
    if args.hold_rootstage:
        blobs.append(('root.stage', b'1'))
    if args.temporary:
        blobs.append(('kernel.seed', data))
    blobs.append(('root.usb', b'1'))
    if native_usb:blobs.append(('root.nativeusb', b'1'))
    blobs += [('root.storage', b'1'), ('volume.img', volume.read_bytes())]
    body = b''.join(struct.pack('<16sII', name.encode(), len(blob), 0) + blob + bytes(-len(blob) % 16)
                    for name, blob in blobs) + bytes(24)
    boot = pack_boot_image(out, platform, struct.pack('<II', 0x50554e58, len(body)) + body,
                           'marble_temporary_boot.img' if args.temporary else 'marble_persistent_boot.img')
    report = out / ('temporary-boot.json' if args.temporary else 'persistent-boot.json')
    report.write_text(json.dumps(dict(
            image=str(boot), kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(),
            native_sha256=baseline['inputs']['native_sha256'],
            kernel_source='boot-image' if args.temporary else 'ufs',
            system_source='ufs', exit_boot_services=False,
            resident_usb=not native_usb, runtime_usb_mode=1 if native_usb else 3,
            usb_owner=args.usb_owner, enabled_cpu_affinities=cpu_regs,
            rootstage_hold=args.hold_rootstage,
            firmware_usb_affinity=None if native_usb else USB_AFFINITY, display_scale=args.display_scale, ui_identity=identity,
            data_logical_bytes=metadata['logical_bytes'], data_uuid=metadata['volume_uuid'],
            storage_profile_sha256=hashlib.sha256(args.storage_profile.read_bytes()).hexdigest(),
            logical_size=[layout['logical_width'], layout['logical_height']],
            native_size=layout['compositor_size'], artwork_scale=args.display_scale,
            display_layout_target=layout, boot_args=boot_args,
            gpu_addon_sha256=hashlib.sha256(addon.read_bytes()).hexdigest(), gpu_sources=gpu['sources'],
            owner_trust_cache_sha256=gpu['trust_cache_sha256'],
            appsync_runtime=str(args.appsync_runtime.resolve()) if appsync else None,
            wifi_firmware=wifi_firmware,
            wifi_runtime=str(args.wifi_runtime.resolve()) if wifi_runtime else None,
            appsync_build=appsync['build'] if appsync else None), indent=2) + '\n')
    if candidate:
        data = json.loads(report.read_text())
        data['baseline_candidate'] = args.baseline.name
        data['baseline_kernel_sha256'] = candidate['kernel_sha256']
        report.write_text(json.dumps(data, indent=2) + '\n')
    print(boot)


if __name__ == '__main__':
    main()
