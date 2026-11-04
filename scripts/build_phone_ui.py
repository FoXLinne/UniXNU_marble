#!/usr/bin/env python3
"""Attach native battery percentage display to the phone runtime addon."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    return subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--addon', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--metal-bundle', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    blob = args.addon.read_bytes()
    if blob[:8] != b'NGPUADD2':
        parser.error('expected NGPUADD2 addon')
    entries, at = [], 24
    prefix = 'addons/NativeAdrenoMetal.bundle/'
    bundle = args.output / 'NativeAdrenoMetal.bundle'
    for _ in range(struct.unpack_from('<I', blob, 8)[0]):
        length, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        name = blob[at:at+length-1].decode()
        at += (length + 7) & ~7
        data = zlib.decompress(blob[at:at+packed])
        at += (packed + 7) & ~7
        if len(data) != size or '..' in Path(name).parts:
            raise ValueError('invalid addon entry')
        if name.startswith(prefix):
            target = bundle / name[len(prefix):]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.chmod(mode)
        else:
            entries.append((name, mode, data))
    if at != len(blob):
        raise ValueError('trailing addon bytes')
    if args.metal_bundle:
        shutil.rmtree(bundle)
        shutil.copytree(args.metal_bundle, bundle)
    turnip = bundle / 'libvulkan_freedreno.dylib'
    turnip_hash = hashlib.sha256(turnip.read_bytes()).hexdigest()
    library = bundle / 'MarblePhoneUI.dylib'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    run('xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk,
        '-O2', '-Wall', '-Wextra', '-Werror', '-fno-objc-arc', '-dynamiclib',
        ROOT / 'package/MarblePhoneUI.m', '-lobjc', '-framework', 'CoreFoundation',
        '-Wl,-reexport_framework,Foundation', '-install_name', '@loader_path/MarblePhoneUI.dylib',
        '-compatibility_version', '300.0.0', '-current_version', '300.0.0', '-o', library)
    executable = bundle / 'NativeAdrenoMetal'
    run('xcrun', 'install_name_tool', '-change',
        '/System/Library/Frameworks/Foundation.framework/Foundation',
        '@loader_path/MarblePhoneUI.dylib', executable)
    hashes = []
    for target, identifier in ((library, 'org.unixnu.marble.phone-ui'),
                               (bundle, 'org.blu-sharky.NativeAdrenoMetal')):
        run('codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', identifier, target)
        run('codesign', '--verify', '--strict', target)
        detail = subprocess.run(['codesign', '--display', '--verbose=4', str(target)],
                                check=True, capture_output=True, text=True).stderr
        hashes.append(re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)[1])
    if hashlib.sha256(turnip.read_bytes()).hexdigest() != turnip_hash:
        raise RuntimeError('Turnip changed during UIKit packaging')
    for path in sorted(bundle.rglob('*')):
        if path.is_file():
            entries.append((prefix + path.relative_to(bundle).as_posix(),
                            0o755 if path.name in ('NativeAdrenoMetal', 'MarblePhoneUI.dylib', 'libvulkan_freedreno.dylib') else 0o644,
                            path.read_bytes()))
    body = bytearray()
    for name, mode, data in entries:
        encoded, packed = name.encode() + b'\0', zlib.compress(data, 9)
        if len(encoded) > 160:
            raise ValueError('addon path exceeds loader limit')
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed))
        body += encoded + bytes(-len(encoded) % 8) + packed + bytes(-len(packed) % 8)
    addon = args.output / 'phone-ui.gpuaddon'
    addon.write_bytes(b'NGPUADD2' + struct.pack('<IIQ', len(entries), 0, len(body) + 24) + body)
    report = dict(cdhashes=hashes, addon=str(addon), turnip_sha256=turnip_hash,
                  metal_bundle=str(args.metal_bundle) if args.metal_bundle else str(args.addon))
    (args.output / 'phone-ui.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
