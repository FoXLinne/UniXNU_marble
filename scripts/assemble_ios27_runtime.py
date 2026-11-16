#!/usr/bin/env python3
"""Assemble the signed 24B119 GPU, Wi-Fi, AppSync and audio runtime payload."""
import argparse
import importlib.util
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

from ios27_migration import DEVICE, DEFAULT_OUTPUT, digest, save, record_stage


def unpack_addon(path):
    blob = Path(path).read_bytes()
    if len(blob) < 24 or blob[:8] != b'NGPUADD2' or struct.unpack_from('<Q', blob, 16)[0] != len(blob):
        raise ValueError('GPU addon has an invalid header or byte count')
    count, = struct.unpack_from('<I', blob, 8)
    offset, files = 24, {}
    for _ in range(count):
        length, mode, raw_size, packed_size = struct.unpack_from('<IIQQ', blob, offset)
        offset += 24
        if not 1 <= length <= 160 or offset + length > len(blob):
            raise ValueError('GPU addon path header is invalid')
        encoded = blob[offset:offset + length]
        if encoded[-1] != 0:
            raise ValueError('GPU addon path requires its terminator')
        name = encoded[:-1].decode()
        if not name or Path(name).is_absolute() or '..' in Path(name).parts or name in files:
            raise ValueError('GPU addon path is invalid or duplicated: ' + name)
        offset += (length + 7) & ~7
        if offset + packed_size > len(blob):
            raise ValueError('GPU addon compressed data is truncated')
        raw = zlib.decompress(blob[offset:offset + packed_size])
        if len(raw) != raw_size:
            raise ValueError('GPU addon raw data length differs: ' + name)
        files[name] = (mode, raw)
        offset += (packed_size + 7) & ~7
    if offset != len(blob):
        raise ValueError('GPU addon has trailing data')
    return files


def pack_addon(files, path):
    body = bytearray()
    for name, (mode, data) in sorted(files.items()):
        encoded = name.encode() + b'\0'
        if not 1 <= len(encoded) <= 160 or Path(name).is_absolute() or '..' in Path(name).parts:
            raise ValueError('Runtime addon path violates the loader contract: ' + name)
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed))
        body += encoded + bytes(-len(encoded) % 8) + packed + bytes(-len(packed) % 8)
    Path(path).write_bytes(b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, len(body) + 24) + body)


def add_directory(files, source, prefix):
    if not source.is_dir():
        raise FileNotFoundError(source)
    added = 0
    for path in sorted(source.rglob('*')):
        if path.is_symlink():
            raise ValueError('Runtime overlay requires explicit file contents: ' + str(path))
        if path.is_file():
            name = prefix + '/' + path.relative_to(source).as_posix()
            files[name] = (path.stat().st_mode & 0o777, path.read_bytes())
            added += 1
    if not added:
        raise ValueError('Runtime overlay has no files: ' + str(source))
    return added


def collect_hashes(value, found):
    if isinstance(value, dict):
        for key, item in value.items():
            if (key == 'cdhash' or key.endswith('_cdhash')) and isinstance(item, str) and re.fullmatch('[0-9a-f]{40}', item):
                found.add(bytes.fromhex(item))
            elif key == 'cdhashes' and isinstance(item, list):
                for text in item:
                    if isinstance(text, str) and re.fullmatch('[0-9a-f]{40}', text):
                        found.add(bytes.fromhex(text))
            else:
                collect_hashes(item, found)
    elif isinstance(value, list):
        for item in value:
            collect_hashes(item, found)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--wifi-firmware', type=Path, required=True)
    parser.add_argument('--debug-agent', type=Path, help='Signed 24B119 USB kernel-log service')
    args = parser.parse_args()
    root, runtime, output = args.root.resolve(), args.runtime.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = json.loads((runtime / 'runtime.json').read_text())
    if report.get('build') != '24B119':
        raise ValueError('Runtime source requires build 24B119')
    steps = report['steps']
    for name in ('wifi', 'launchd', 'haptics', 'appsync', 'phone_ui'):
        if steps.get(name, {}).get('status') != 'built':
            raise ValueError('Complete the runtime build step: ' + name)
    if steps.get('fixups', {}).get('count', 0) < 19:
        raise ValueError('Runtime requires the qualified target text-fixup set')
    for name, expected in report['input_hashes'].items():
        folder = DEVICE / ('package/ios27-runtime' if name.endswith('.c') else 'profiles/runtime/ios27-24B119')
        if digest(folder / name) != expected:
            raise ValueError('Runtime source changed after its build: ' + name)
    for relative, identity in report['trust_manifest'].items():
        path = runtime / relative
        if digest(path) != identity['sha256']:
            raise ValueError('Signed runtime binary changed: ' + relative)
    launchd = steps['launchd']
    cache, signature = Path(launchd['cache']), Path(launchd['signature'])
    if (digest(cache) != launchd['output_sha256']
            or digest(signature) != launchd['detached_signature_sha256']):
        raise ValueError('Target launchd cache or its signature changed after its build')
    turnip = root / 'tree/build/adreno-metal-plugin27/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib'
    if steps['phone_ui']['turnip_sha256'] != digest(turnip):
        raise ValueError('Phone UI runtime must contain this migration\'s 27 GPU build')
    files = unpack_addon(Path(steps['phone_ui']['addon']))
    build_meta = 'addons/NativeAdrenoMetal.bundle/gpu-build.json'
    if build_meta not in files:
        raise ValueError('GPU addon requires its source build identity')
    expected_meta = json.loads((turnip.parent / 'gpu-build.json').read_text())
    if json.loads(files[build_meta][1]) != expected_meta:
        raise ValueError('Phone UI plugin source identity differs from this GPU build')
    overlays = {}
    for relative, prefix in [('wifi-runtime/bin', 'bin'), ('wifi-runtime/etc/wifi', 'etc/wifi'),
                             ('wifi-runtime/daemons', 'daemons'),
                             ('haptics/hal', 'haptics-hal'), ('haptics/tunings', 'haptics-tunings'),
                             ('appsync/lib', 'appsync/lib'), ('appsync/libexec', 'appsync/libexec'),
                             ('appsync/helper', 'appsync/helper'), ('appsync/coreservices', 'appsync/coreservices')]:
        overlays[prefix] = add_directory(files, runtime / relative, prefix)
    overlays['xpc'] = add_directory(files, cache.parent, 'xpc')
    debug_hashes, debug_record = [], None
    if args.debug_agent:
        agent = args.debug_agent.resolve()
        debug_record = json.loads((agent / 'report.json').read_text())
        if (debug_record.get('build') != '24B119'
                or digest(agent / 'bin/marble-debug') != debug_record['binary_sha256']):
            raise ValueError('USB diagnostics executable differs from its signed build')
        overlays['debug-bin'] = add_directory(files, agent / 'bin', 'bin')
        overlays['debug-daemons'] = add_directory(files, agent / 'daemons', 'daemons')
        script = DEVICE / 'ios27/source/scripts/build-boot-runtime.py'
        spec = importlib.util.spec_from_file_location('marble_debug_boot_runtime', script)
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        signed_cache = output / 'xpc/launchd.plist'
        signed_cache.parent.mkdir(exist_ok=True)
        merged = builder.merge_launchd_cache(cache, agent / 'daemons', signed_cache)
        signature = builder.sign_launchd_cache(signed_cache)
        overlays['debug-xpc'] = add_directory(files, signed_cache.parent, 'xpc')
        debug_hashes = [debug_record['cdhash'], signature['cdhash']]
        debug_record.update(launchd_cache=merged, launchd_signature=signature)
    firmware = json.loads((DEVICE / 'profiles/wifi-wcn6855-hw21.json').read_text())
    prefix = 'firmware/ath11k/WCN6855/hw2.1'
    for item in firmware['files']:
        path = args.wifi_firmware / item['name']
        if digest(path) != item['sha256']:
            raise ValueError('WCN6855 firmware differs from its pinned input: ' + item['name'])
        files[prefix + '/' + item['name']] = (0o644, path.read_bytes())
    addon = output / 'runtime.gpuaddon'
    pack_addon(files, addon)
    # Original modules retain their own bytes. This extra owner module names
    # the actual signed payload executables and target launchd-cache signature.
    hashes = set()
    collect_hashes(report, hashes)
    gpu = json.loads((root / 'gpu/userspace.json').read_text())
    collect_hashes(gpu, hashes)
    hashes.update(bytes.fromhex(value) for value in debug_hashes)
    if not hashes:
        raise ValueError('Runtime requires signed executable CDHashes')
    packed_hashes = sorted(hashes)
    owner = output / 'owner.trustcache'
    owner.write_bytes(struct.pack('<I16sI', 2, hashlib.sha256(b''.join(packed_hashes)).digest()[:16], len(packed_hashes)) +
                      b''.join(value + bytes([2, 0, 0, 0]) for value in packed_hashes))
    save(output / 'report.json', {'target': 'ios27', 'build': '24B119', 'addon': str(addon),
                                 'addon_sha256': digest(addon), 'owner_trust_cache': str(owner),
                                 'trust_cache_sha256': digest(owner), 'cdhash_count': len(hashes),
                                 'files': {name: {'sha256': hashlib.sha256(data).hexdigest(), 'mode': mode}
                                           for name, (mode, data) in files.items()},
                                 'overlays': overlays, 'runtime_report_sha256': digest(runtime / 'runtime.json'),
                                 'usb_debug_agent': debug_record,
                                 'device_result': 'pending'})
    record_stage('runtime_payload', 'assembled', report=str(output / 'report.json'), addon_sha256=digest(addon),
                 trust_cache_sha256=digest(owner))
    print(output / 'report.json')


if __name__ == '__main__':
    main()
