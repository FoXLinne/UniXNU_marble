#!/usr/bin/env python3
"""Pack a 24B119 marble candidate with exact kernel/runtime/system provenance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib
import zlib

from build_ios18_arm64e import read_adt, write_adt
from build_pac_uefi import build_applications, pack_boot_image
from ios27_migration import DEVICE, WORKSPACE, DEFAULT_OUTPUT, digest, record_stage, save
from marble_display_geometry import display_geometry, display_layout


def original_apple_adt(root, profile):
    sys.path.insert(0, str(DEVICE / 'ios27/source/scripts'))
    from unixnu.ipsw import im4p_payload
    name = next(name for name in profile['ipsw']['sha256'] if name.startswith('DeviceTree.'))
    path = root / 'ipsw/24B119' / name
    if digest(path) != profile['ipsw']['sha256'][name]:
        raise ValueError('Target Apple device tree differs from its pinned digest')
    data = im4p_payload(path.read_bytes())
    if data.startswith((b'bvx2', b'bvxn', b'bvx-')):
        packed, decoded = root / 'apple.adt.lzfse', root / 'apple.adt'
        packed.write_bytes(data)
        subprocess.run(['/usr/bin/compression_tool', '-decode', '-a', 'lzfse', '-i', str(packed),
                        '-o', str(decoded)], check=True)
        data = decoded.read_bytes()
    tree, end = read_adt(data)
    if end != len(data) or tree[0]['model'].rstrip(b'\0').decode() != profile['ipsw']['product']:
        raise ValueError('Apple device tree identity differs from iPhone12,1')
    return tree


def kernel_window(kernel, dsp, metadata, arguments, destination):
    """Use the board's versioned KernelSeed DSP and diagnostic-mailbox contract."""
    data = bytearray(kernel.read_bytes())
    data += bytes(-len(data) % 4096)
    offset = len(data)
    firmware = zlib.compress(dsp.read_bytes(), 9)
    data += firmware
    data += bytes(-len(data) % 4096)
    mailbox = bytearray(1 << 20)
    struct.pack_into('<8sII', mailbox, 0, b'ROOTLOG1', 1, 0)
    struct.pack_into('<Q', mailbox, 56, metadata['logical_bytes'])
    mailbox[64:101] = metadata['volume_uuid'].upper().encode() + b'\0'
    struct.pack_into('<QQQ', mailbox, 104, offset, len(firmware), dsp.stat().st_size)
    encoded = arguments.encode()
    if len(encoded) >= 1000:
        raise ValueError('Boot arguments exceed the loader command-line capacity')
    mailbox[128:128 + len(encoded)] = encoded
    data += mailbox
    destination.write_bytes(data)
    return destination


def storage_manifest(images, destination):
    if not 1 <= len(images) <= 4:
        raise ValueError('Root manifest requires one to four system/kernel windows')
    header = bytearray(4096)
    struct.pack_into('<8sII', header, 0, b'SHNGROOT', 1, len(images))
    cursor = 1 << 20
    for index, image in enumerate(images):
        name = image['name'].encode()
        if not name or len(name) > 31 or image['raw_bytes'] % 4096:
            raise ValueError('Root window requires a short name and 4K-aligned disk length')
        image['offset'] = cursor
        at = 24 + index * 128
        header[at:at + len(name)] = name
        struct.pack_into('<QQII', header, at + 32, cursor, image['raw_bytes'], 512, 1)
        header[at + 56:at + 88] = bytes.fromhex(image['sha256'])
        cursor = (cursor + image['raw_bytes'] + (1 << 20) - 1) & -(1 << 20)
    struct.pack_into('<Q', header, 16, cursor)
    header[4064:] = hashlib.sha256(header[:4064]).digest()
    (destination / 'root_manifest.bin').write_bytes(header)
    save(destination / 'storage_images.json', images)
    return cursor


def seed_metadata(seed):
    metadata = json.loads(seed.with_name('seed.json').read_text())
    profile = json.loads((DEVICE / 'profiles/marble-storage-24B119.json').read_text())
    if metadata.get('source_build') not in ('22H384', '24B119'):
        raise ValueError('Select the recorded 22H384 installation or its 24B119 Data identity')
    if (metadata['volume_uuid'].upper() != profile['data_volume_uuid'].upper()
            or metadata['logical_bytes'] != profile['data_logical_bytes']):
        raise ValueError('Data seed identity or geometry differs from this installation')
    if digest(seed) != metadata['sha256']:
        raise ValueError('Data seed differs from its recorded SHA-256')
    with seed.open('rb') as stream:
        if stream.read(8) != b'DBRDISK1':
            raise ValueError('Data seed requires the packed DBRDISK1 contract')
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, help='Fileset kernel or its debug-screen copy')
    parser.add_argument('--runtime', type=Path, required=True, help='24B119 addon and owner trust-cache build directory')
    parser.add_argument('--addon', type=Path, required=True)
    parser.add_argument('--owner-trust-cache', type=Path, required=True)
    parser.add_argument('--data-seed', type=Path, required=True)
    parser.add_argument('--dsp', type=Path, required=True)
    parser.add_argument('--binaries', type=Path, help='Built MarbleBoot AARCH64 directory')
    parser.add_argument('--unique-chip-id', default=os.environ.get('UNIXNU_UNIQUE_CHIP_ID'))
    parser.add_argument('--identity-from-adt', type=Path, help='Preserved installation ADT supplying the platform seed')
    parser.add_argument('--hold-rootstage', action='store_true', help='Hold once for firmware-side debugging before XNU')
    parser.add_argument('--resident-usb', action='store_true', help='Reserve affinity 0x100 for the firmware USB diagnostic worker')
    args = parser.parse_args()
    root, out = args.root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    board = tomllib.loads((DEVICE / 'board.toml').read_text())
    profile = tomllib.loads((DEVICE / 'profiles/os/ios-27.2-24B119/profile.toml').read_text())
    kernel = (args.kernel or root / 'kernel.fileset.macho').resolve()
    prelink = json.loads((root / 'kernel.fileset.json').read_text())
    if (not prelink.get('mapping_complete') or not prelink.get('candidate_written')
            or prelink.get('missing') or prelink.get('stubbed') or prelink.get('allow_missing')):
        raise ValueError('Candidate requires the complete 24B119 prelink closure')
    if prelink.get('inputs', {}).get('kernelcache_sha256') != profile['kernel']['prelink']['kernelcache_sha256']:
        raise ValueError('Candidate original kernel collection differs from 24B119')
    native = root / 'kernel.native.macho'
    if prelink.get('inputs', {}).get('native_sha256') != digest(native):
        raise ValueError('Candidate native kernel differs from this board build')
    source_kernel = root / 'kernel.fileset.macho'
    if prelink.get('candidate_sha256') != digest(source_kernel):
        raise ValueError('Fileset kernel differs from its prelink report')
    if kernel != source_kernel.resolve():
        debug = json.loads(kernel.with_suffix('.debug.json').read_text())
        if debug['source_sha256'] != digest(source_kernel) or debug['debug_sha256'] != digest(kernel):
            raise ValueError('Debug image provenance differs from this fileset kernel')
    runtime = json.loads((args.runtime / 'report.json').read_text())
    if runtime.get('build') != '24B119':
        raise ValueError('Runtime requires the 24B119 target')
    # Runtime builders record signed addon and owner module identities explicitly.
    addon, owner = args.addon.resolve(), args.owner_trust_cache.resolve()
    if runtime.get('addon_sha256') != digest(addon) or runtime.get('trust_cache_sha256') != digest(owner):
        raise ValueError('Runtime addon or owner trust module differs from its build report')
    if not 24 <= addon.stat().st_size <= 128 << 20:
        raise ValueError('Runtime addon exceeds the selected loader capacity')
    metadata = seed_metadata(args.data_seed.resolve())
    arguments = ' '.join(board['payload']['kernel_args'])
    if args.resident_usb:
        arguments = arguments.replace('rootstage-runtime-usb=0', 'rootstage-runtime-usb=3')
    if 'cpus=8' not in arguments.split():
        raise ValueError('The target boot policy requires all eight cores')
    window = kernel_window(kernel, args.dsp.resolve(), metadata, arguments, out / 'KernelSeed.bin')
    source_images = json.loads((DEVICE / 'records/ios27-24B119/system-images.json').read_text())
    images = []
    for name, component in [('OS', 'OS'), ('SystemOS', 'Cryptex1,SystemOS')]:
        item = source_images['images'][component]
        path = Path(item['path'])
        if digest(path) != item['sha256']:
            raise ValueError('System disk differs from its 24B119 source record: ' + name)
        images.append(dict(name=name, source=str(path), format='raw', raw_bytes=path.stat().st_size,
                           sha256=item['sha256']))
    images.append(dict(name='KernelSeed', source=str(window), format='raw', raw_bytes=window.stat().st_size,
                       sha256=digest(window)))
    total = storage_manifest(images, out)
    platform = DEVICE / 'firmware'
    binaries = args.binaries.resolve() if args.binaries else build_applications(out / 'firmware', platform, root / 'tree')
    stage = out / 'stage/EFI/UniXNU'
    payloads = stage / 'Payloads'
    payloads.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binaries / 'UniXNU.efi', stage / 'UniXNU.efi')
    shutil.copyfile(binaries / 'DarwinBoot.efi', payloads / 'DarwinBoot.efi')
    (payloads / 'kernel.macho').unlink(missing_ok=True)
    shutil.copyfile(addon, payloads / 'kernel.macho.gpuaddon')
    caches = [root / 'ipsw/24B119' / name for name in profile['ipsw']['sha256'] if name.endswith('.trustcache')]
    for cache in caches:
        if digest(cache) != profile['ipsw']['sha256'][cache.name]:
            raise ValueError('Original target trust cache differs: ' + cache.name)
    subprocess.run([sys.executable, str(DEVICE / 'ios27/source/scripts/bundle-ipsw-trust.py'),
                    str(payloads / 'kernel.macho.trustcache'), *map(str, caches), str(owner)], check=True)
    identity = args.unique_chip_id
    if args.identity_from_adt:
        tree, _ = read_adt(args.identity_from_adt.read_bytes())
        chosen = next(node[0] for node in tree[1] if node[0]['name'] == b'chosen\0')
        identity = f'{int.from_bytes(chosen["unique-chip-id"], "little"):x}'
    if not identity:
        raise ValueError('Select the preserved platform identity with --identity-from-adt or --unique-chip-id')
    sys.path.insert(0, str(DEVICE / 'ios27/source/scripts'))
    from unixnu.devicetree import generate
    from unixnu.profiles import merge
    hardware = out / 'marble-hardware.dtb'
    spec = board['device_tree']
    roots = {'device': DEVICE, 'soc': DEVICE / 'socs' / board['board']['soc_source']}
    def hardware_source(value):
        prefix, separator, relative = value.partition(':')
        return roots[prefix] / relative if separator else DEVICE / value
    command = ['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(hardware)]
    for include in spec.get('include_dirs', []):
        command += ['-i', str(hardware_source(include))]
    command.append(str(hardware_source(spec['source'])))
    with (out / 'device-tree.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    adt = generate(hardware, merge(profile, board), unique_chip_id=identity,
                   data=False, trust=True, addon=True)
    tree, _ = read_adt(adt)
    apple = original_apple_adt(root, profile)
    apple_product = next(node[0] for node in apple[1] if node[0]['name'] == b'product\0')
    product = next(node[0] for node in tree[1] if node[0]['name'] == b'product\0')
    for key in ('product-name', 'product-description', 'artwork-device-idiom', 'artwork-device-subtype',
                'artwork-scale-factor', 'artwork-display-gamut', 'compatible-device-fallback', 'thin-bezel'):
        if key in apple_product:
            product[key] = apple_product[key]
    identity_profile = profile['ui_identity']
    tree[0]['model'] = identity_profile['product'].encode() + b'\0'
    tree[0]['target-type'] = identity_profile['target_type'].encode() + b'\0'
    tree[0]['ui-hardware-model'] = identity_profile['hardware_model'].encode() + b'\0'
    product.update({'product-name': identity_profile['name'].encode() + b'\0',
                    'product-description': identity_profile['name'].encode() + b'\0',
                    'model': identity_profile['product'].encode() + b'\0',
                    'artwork-device-subtype': struct.pack('<I', identity_profile['artwork_device_subtype']),
                    'compatible-device-fallback': identity_profile['compatible_device_fallback'].encode() + b'\0',
                    'large-format-phone': struct.pack('<I', int(identity_profile['large_format_phone'])),
                    'island-notch-location': struct.pack('<I', identity_profile['island_notch_location']),
                    'chrome-identifier': identity_profile['chrome_identifier'].encode() + b'\0',
                    'oled-display': struct.pack('<I', int(identity_profile['oled_display'])),
                    'udid-version': struct.pack('<I', identity_profile['udid_version'])})
    chosen = next(node for node in tree[1] if node[0]['name'] == b'chosen\0')
    affinities = list(range(0, 0x800, 0x100))
    if args.resident_usb:
        cpus = next(node for node in tree[1] if node[0]['name'] == b'cpus\0')
        cpus[1][:] = [node for node in cpus[1]
                      if int.from_bytes(node[0].get('reg', b''), 'little') != 0x100]
        affinities.remove(0x100)
    apple_chosen = next(node[0] for node in apple[1] if node[0]['name'] == b'chosen\0')
    for key in ('crypto-hash-method', 'amfi-only-platform-code'):
        if key not in apple_chosen:
            raise ValueError('Target Apple device tree requires ' + key)
        chosen[0][key] = apple_chosen[key]
    geometry, layout = display_geometry(), display_layout()
    chosen[0]['display-scale'] = struct.pack('<I', geometry['scale'])
    chosen[0]['dot-pitch'] = struct.pack('<I', geometry['pixels_per_inch'])
    chosen[0]['display-physical-size-mm'] = struct.pack('<II', geometry['width_mm'], geometry['height_mm'])
    product['artwork-scale-factor'] = struct.pack('<I', geometry['scale'])
    product['display-corner-radius'] = struct.pack('<II', round(layout['corner_radius_pixels'] /
                                                layout['native_points_scale'] * 10000), 10000)
    # Capability publishing follows the board hardware; both interfaces are
    # instantiated by the selected WCN6855 and actuator drivers.
    haptics = ({'name': b'haptics\0', 'supports-3rd-party-haptics': struct.pack('<I', 1),
                'closed-loop': struct.pack('<I', 1)}, [])
    product_node = next(node for node in tree[1] if node[0]['name'] == b'product\0')
    product_node[1].append(haptics)
    hashed = 0xcbf29ce484222325
    seed = int(identity, 16)
    for shift in range(0, 64, 8):
        hashed = ((hashed ^ ((seed >> shift) & 255)) * 0x100000001b3) & 0xffffffffffffffff
    mac = bytearray(hashed.to_bytes(8, 'little')[:6])
    mac[0] = (mac[0] & 0xfe) | 2
    chosen[0]['mac-address-wifi0'] = bytes(mac)
    armio = next(node for node in tree[1] if node[0]['name'] == b'arm-io\0')
    intc=next(node for node in armio[1] if node[0]['name']==b'intc\0')
    intc[0]['firmware-awake']=bytes(4)
    armio[1].append(({'name': b'wlan\0', 'compatible': b'qcom,wcn6855\0', 'status': b'okay\0',
                      'local-mac-address': bytes(mac)}, []))
    memory = next(node[0] for node in chosen[1] if node[0]['name'] == b'memory-map\0')
    memory['RootDisks'] = bytes(16)
    # Persistent Data is published by MarbleRootDisk using the mailbox UUID
    # and geometry. This candidate uses the existing UFS volume.
    memory.pop('RAMDisk', None)
    memory.pop('PackedRAMDisk', None)
    (payloads / 'kernel.macho.adt').write_bytes(write_adt(tree))
    text = ('version = 1\n[boot]\ntimeout = 0\ndefault = "marble"\n[kits]\nautoload = false\n'
            '[[driver]]\npath = "Payloads/DarwinBoot.efi"\n[[entry]]\nid = "marble"\n'
            'title = "marble iOS 27.2"\ntype = "xnu"\npath = "Payloads/kernel.macho"\n'
            'args = ' + json.dumps(arguments) + '\n')
    (stage / 'unixnu.toml').write_text(text)
    volume = out / 'volume.img'
    volume_bytes = (sum(p.stat().st_size for p in stage.rglob('*') if p.is_file()) + (4 << 20) + (1 << 20) - 1) & -(1 << 20)
    with volume.open('wb') as stream:
        stream.truncate(volume_bytes)
    subprocess.run(['mformat', '-i', str(volume), '-v', 'MARBLEESP', '::'], check=True)
    subprocess.run(['mcopy', '-i', str(volume), '-s', '-Q', str(stage.parent), '::/'], check=True)
    encoded_arguments = arguments.encode() + b'\0'
    overlay = (struct.pack('<8sQQ32s8x', b'KPATCH01', kernel.stat().st_size,
                           len(encoded_arguments), bytes.fromhex(digest(window)))
               + encoded_arguments + kernel.read_bytes())
    (out / 'kernel.patch').write_bytes(overlay)
    sys.path.insert(0, str(DEVICE / 'ios27/sources/XnuPrelink'))
    from kc_image import Image
    collection = Image(kernel.read_bytes())
    member = Image(collection.data, collection.entries['com.apple.kernel'][1])
    symbols = {name: address for address, names in member.symbols().items() for name in names}
    trace = symbols.get('_native_marble_boot_events')
    trust_bytes = (payloads / 'kernel.macho.trustcache').stat().st_size
    prefix = (trust_bytes + 0x1fffff) & ~0x1fffff
    trace_offset = ((collection.base & 0x1fffff) + prefix + trace - collection.base) if trace else None
    blobs = [('loader.efi', (binaries / 'MarbleBootstrap.efi').read_bytes()),
             ('kernel.patch', overlay),
             ('root.gicawake', b'1'),
             ('root.usb', b'1'),
             ('root.storage', b'1'), ('volume.img', volume.read_bytes())]
    if trace_offset is not None:
        blobs.insert(1, ('root.trace', struct.pack('<Q', trace_offset)))
    if not args.resident_usb:
        blobs.insert(2, ('root.nativeusb', b'1'))
    if args.hold_rootstage:
        blobs.insert(1, ('root.stage', b'1'))
    body = b''.join(struct.pack('<16sII', name.encode(), len(data), 0) + data + bytes(-len(data) % 16)
                    for name, data in blobs) + bytes(24)
    boot = pack_boot_image(out, platform, struct.pack('<II', 0x50554e58, len(body)) + body, 'marble_ios27_temporary_boot.img')
    if boot.stat().st_size > board['package']['boot_partition_bytes']:
        raise ValueError('Candidate boot image exceeds the board boot-image capacity')
    report = {'target': 'ios-27.2-24B119', 'image': str(boot), 'image_sha256': digest(boot),
              'kernel': str(kernel), 'kernel_sha256': digest(kernel), 'boot_args': arguments,
              'usb_owner': 'firmware-resident' if args.resident_usb else 'kernel',
              'enabled_cpu_affinities': affinities,
              'resident_usb_affinity': 0x100 if args.resident_usb else None,
              'hold_rootstage': args.hold_rootstage,
              'system_source': 'ufs', 'kernel_source': 'boot-image', 'storage_bytes': total,
              'kernel_seed_source': 'installed-ufs-window-with-ram-overlay',
              'kernel_overlay_sha256': digest(out / 'kernel.patch'),
              'boot_log_offset': trace_offset,
              'ram_seed_sha256': digest(window),
              'data_logical_bytes': metadata['logical_bytes'], 'data_uuid': metadata['volume_uuid'],
              'data_seed_sha256': metadata['sha256'], 'addon_sha256': digest(addon),
              'data_source': 'existing-ufs', 'data_reference_build': metadata['source_build'],
              'data_reference_purpose': 'preserved installation UUID and geometry; target OS performs on-device migration',
              'owner_trust_cache_sha256': digest(owner), 'dsp_sha256': digest(args.dsp),
              'adt_sha256': digest(payloads / 'kernel.macho.adt'),
              'hardware_dtb': str(hardware), 'hardware_dtb_sha256': digest(hardware),
              'hardware_sources': {str(path): digest(path) for path in
                                   [hardware_source(spec['source']), roots['soc'] / 'dts/sm7475.dtsi']}, 'runtime_report': str(args.runtime / 'report.json'),
              'system_product': profile['ipsw']['product'], 'ui_identity': identity_profile,
              'source_baseline': '7a0d68ff', 'device_result': 'pending'}
    save(out / 'candidate.json', report)
    record_stage('image', 'built', candidate=str(out / 'candidate.json'), image=str(boot), image_sha256=digest(boot))
    print(boot)


if __name__ == '__main__':
    main()
