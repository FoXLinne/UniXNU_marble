#!/usr/bin/env python3
"""Audit the actual Android boot image, UFS inputs and panic kernel together."""
import argparse
import hashlib
import hmac
import json
from pathlib import Path
import struct
import tomllib
import zlib

from ios27_migration import DEVICE, DEFAULT_OUTPUT, digest, save
from build_ios18_arm64e import read_adt


def require(value, message):
    if not value:
        raise ValueError(message)


def check(candidate):
    root = candidate.parent
    report = json.loads(candidate.read_text())
    board = tomllib.loads((DEVICE / 'board.toml').read_text())
    image = Path(report['image'])
    raw = image.read_bytes()
    require(digest(image) == report['image_sha256'], 'Boot image digest differs')
    require(len(raw) <= board['package']['boot_partition_bytes'], 'Boot partition capacity exceeded')
    require(raw[:8] == b'ANDROID!' and struct.unpack_from('<I', raw, 40)[0] == 4,
            'Expected Android boot header version 4')
    kernel_bytes = struct.unpack_from('<I', raw, 8)[0]
    inflater = zlib.decompressobj(16 + zlib.MAX_WBITS)
    firmware = inflater.decompress(raw[4096:4096 + kernel_bytes]) + inflater.flush()
    require(inflater.eof and inflater.unused_data[:4] == bytes.fromhex('d00dfeed'),
            'Firmware gzip member or appended device tree is invalid')
    marker = struct.pack('<I', 0x50554e58)
    payload = firmware.find(marker + struct.pack('<I', len(firmware)))
    # The payload length follows its magic; its end is the raw firmware end.
    at, matches = 0, []
    while True:
        at = firmware.find(marker, at)
        if at < 0:
            break
        if at + 8 <= len(firmware):
            length = struct.unpack_from('<I', firmware, at + 4)[0]
            if at + 8 + length == len(firmware):
                matches.append(at)
        at += 4
    require(len(matches) == 1, 'Expected one bounded UniXNU payload')
    at, blobs = matches[0] + 8, {}
    while at + 24 <= len(firmware):
        name, length, reserved = struct.unpack_from('<16sII', firmware, at)
        if name == bytes(16):
            break
        name = name.split(b'\0')[0].decode()
        require(name not in blobs and reserved == 0 and at + 24 + length <= len(firmware),
                'Invalid payload entry: ' + name)
        blobs[name] = firmware[at + 24:at + 24 + length]
        at += 24 + ((length + 15) & ~15)
    overlay = report['kernel_seed_source'] == 'installed-ufs-window-with-ram-overlay'
    expected = {'loader.efi', 'root.usb', 'root.storage', 'root.gicawake', 'volume.img'}
    resident = report.get('usb_owner') == 'firmware-resident'
    if not resident:
        expected.add('root.nativeusb')
    if report.get('hold_rootstage'):
        expected.add('root.stage')
    if overlay:
        expected.add('kernel.patch')
    if report.get('boot_log_offset') is not None:
        expected.add('root.trace')
        require(blobs.get('root.trace') == struct.pack('<Q',report['boot_log_offset']), 'Boot event offset differs')
    require(set(blobs) == expected, 'Boot payload selection differs from candidate')
    require(blobs['volume.img'] == (root / 'volume.img').read_bytes(), 'Embedded ESP differs')
    require(report['kernel_seed_source'] in ('installed-ufs-window', 'installed-ufs-window-with-ram-overlay'),
            'UFS seed selection differs')
    affinities = [i for i in range(0, 0x800, 0x100) if not resident or i != 0x100]
    require(report['enabled_cpu_affinities'] == affinities, 'CPU ownership differs')
    if resident:
        require(report.get('resident_usb_affinity') == 0x100, 'Resident firmware CPU ownership differs')
    arguments = report['boot_args'].split()
    require('cpus=8' in arguments, 'Eight-core boot argument required')
    require(not any(a.startswith('maxmem=') for a in arguments), 'Boot arguments cap physical memory')
    kernel = Path(report['kernel'])
    require(digest(kernel) == report['kernel_sha256'], 'Debug kernel digest differs')
    stage = root / 'stage/EFI/UniXNU/Payloads'
    if overlay:
        patch = blobs['kernel.patch']
        magic, kernel_bytes, argument_bytes, seed_hash = struct.unpack_from('<8sQQ32s', patch)
        require(magic == b'KPATCH01' and kernel_bytes == kernel.stat().st_size
                and 1 <= argument_bytes <= 1000 and len(patch) == 64 + argument_bytes + kernel_bytes,
                'RAM overlay header differs')
        require(patch[64:64 + argument_bytes] == report['boot_args'].encode() + b'\0'
                and patch[64 + argument_bytes:] == kernel.read_bytes(), 'RAM overlay kernel or arguments differ')
        require(hashlib.sha256(patch).hexdigest() == report['kernel_overlay_sha256']
                and seed_hash.hex() == report['ram_seed_sha256'], 'RAM overlay provenance differs')
        require(not (stage / 'kernel.macho').exists(), 'ESP duplicates the selected RAM overlay kernel')
    else:
        require(digest(stage / 'kernel.macho') == digest(kernel), 'ESP kernel differs')
    require(digest(stage / 'kernel.macho.gpuaddon') == report['addon_sha256'], 'ESP runtime differs')
    images = json.loads((root / 'storage_images.json').read_text())
    header = (root / 'root_manifest.bin').read_bytes()
    require(len(header) == 4096 and header[:8] == b'SHNGROOT'
            and hashlib.sha256(header[:4064]).digest() == header[4064:], 'UFS manifest digest differs')
    for index, entry in enumerate(images):
        source = Path(entry['source'])
        require(source.stat().st_size == entry['raw_bytes'] and digest(source) == entry['sha256'],
                'UFS window identity differs: ' + entry['name'])
        offset = 24 + index * 128
        require(struct.unpack_from('<QQ', header, offset + 32) == (entry['offset'], entry['raw_bytes']),
                'UFS manifest extent differs')
        require(header[offset + 56:offset + 88] == bytes.fromhex(entry['sha256']), 'UFS extent hash differs')
    seed = next(Path(e['source']) for e in images if e['name'] == 'KernelSeed')
    with seed.open('rb') as stream:
        require(stream.read(kernel.stat().st_size) == kernel.read_bytes(), 'KernelSeed kernel differs')
        stream.seek(-1048576, 2)
        mailbox = stream.read(4096)
    require(mailbox[:8] == b'ROOTLOG1'
            and mailbox[64:101].split(b'\0')[0].decode() == report['data_uuid']
            and struct.unpack_from('<Q', mailbox, 56)[0] == report['data_logical_bytes'],
            'Persistent Data identity differs')
    import sys
    sys.path.insert(0, str(DEVICE / 'ios27/sources/XnuPrelink'))
    from kc_image import Image
    collection = Image(kernel.read_bytes())
    member = Image(collection.data, collection.entries['com.apple.kernel'][1])
    from fileset_emit import prelink_info
    _, metadata = prelink_info(collection)
    platform = [entry for entry in metadata['_PrelinkInfoDictionary']
                if entry['CFBundleIdentifier'] == 'com.apple.driver.AppleARMPlatform']
    require(len(platform) == 1 and not platform[0].get('IOKitPersonalities'),
            'Board CPU matching requires native ArmCpu with AppleARMPlatform dependency classes')
    require(b"'IOClass'         = ArmCpu;" in collection.data
            and b"'IOClass'         = ArmDTPlatform;" in collection.data,
            'Native CPU/platform catalogue entries are absent')
    properties, _ = read_adt((root / 'stage/EFI/UniXNU/Payloads/kernel.macho.adt').read_bytes())
    defaults = next(node[0] for node in properties[1] if node[0]['name'] == b'defaults\0')
    vnode_budget = board['adt']['defaults']['u32']['kern.maxvnodes']
    require(defaults.get('kern.maxvnodes') == struct.pack('<I', vnode_budget)
            and vnode_budget >= 66560, 'Board vnode budget is absent or too small for migration')
    require(defaults.get('sleep-disabled') == struct.pack('<I', 1),
            'Resident USB board requires the root-domain sleep policy')
    buttons = next((node[0] for node in properties[1] if node[0]['name'] == b'buttons\0'), None)
    require(buttons is not None and buttons.get('home-button-type') == struct.pack('<I', 2)
            and buttons.get('button-names') == b'hold\0voldown\0volup\0',
            'Marble requires the phone navigation capability and its physical button set')
    chosen = next(node[0] for node in properties[1] if node[0]['name'] == b'chosen\0')
    armio = next(node for node in properties[1] if node[0]['name'] == b'arm-io\0')
    gic = next((node[0] for node in armio[1] if node[0]['name'] == b'intc\0'), None)
    require(gic is not None and gic.get('compatible') == b'arm,gic-v3\0', 'Board GIC node is absent')
    child_base, bus_base, bus_size = struct.unpack('<3Q', armio[0]['ranges'])
    dist, dist_size, redist, redist_size = struct.unpack('<4Q', gic['reg'])
    require(bus_base+dist-child_base == 0x17100000 and dist_size == 0x10000
            and bus_base+redist-child_base == 0x17180000 and redist_size == 0x200000
            and int.from_bytes(gic['redistributor-stride'],'little') == 0x40000,
            'Board GIC windows or redistributor stride differs from firmware')

    require(chosen.get('crypto-hash-method') == b'sha2-384\0'
            and chosen.get('amfi-only-platform-code') == bytes(4),
            '24B119 crypto and AMFI device-tree inputs differ')
    names = {n: a for a, ns in member.symbols().items() for n in ns}
    require(struct.unpack('<I', member.read(names['_native_ios27_ce_page_shift'], 4))[0] == 14,
            'AMFI query stack ABI differs')
    seal = json.loads(kernel.with_suffix(kernel.suffix + '.crypto-hmac.json').read_text())
    code = int(seal['code_file_offset'], 0)
    field = int(seal['hmac_file_offset'], 0)
    body = kernel.read_bytes()
    require(body[field:field + 32] == hmac.digest(b'\0', body[code:code + seal['code_bytes']], 'sha256'),
            'Instrumented source POST HMAC differs')
    prelink = json.loads((DEFAULT_OUTPUT / 'kernel.fileset.json').read_text())
    require(prelink['mapping_complete'] and prelink['candidate_written'] and not prelink['missing']
            and not prelink['stubbed'] and not prelink['allow_missing'] and not prelink['deferred_sites'],
            'Strict prelink closure incomplete')
    return {'target': report['target'], 'status': 'host-audited', 'candidate': str(candidate),
            'image_sha256': digest(image), 'image_bytes': len(raw),
            'boot_capacity_bytes': board['package']['boot_partition_bytes'],
            'payload_blobs': {n: len(v) for n, v in blobs.items()},
            'kernel_sha256': digest(kernel), 'closure_components': len(prelink['closure']),
            'missing_references': 0, 'stubbed_references': 0, 'post_hmac': 'verified',
            'cpu_affinities': report['enabled_cpu_affinities'], 'data_uuid': report['data_uuid'],
            'cpu_ownership': '7 XNU + 1 resident firmware' if resident else '8 XNU',
            'kernel_selection': 'RAM overlay' if overlay else 'installed UFS seed',
            'device_result': 'pending'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', required=True, type=Path)
    args = parser.parse_args()
    result = check(args.candidate.resolve())
    save(args.candidate.resolve().parent / 'host-audit.json', result)
    print(json.dumps(result, indent=2))
