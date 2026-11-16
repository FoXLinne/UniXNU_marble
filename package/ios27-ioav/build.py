#!/usr/bin/env python3
"""Compile the ios27-ioav adapter against the 24B119 target compile records.

Reuses scripts/build_ios27_drivers.py compile_source()/compile_record() so
the adapter object is produced with exactly the same arm64e-apple-ios27.2
toolchain flags as every other driver-family translation unit. Emits an
object plus an archive the parent driver/link step can pick up.

Usage:
    python3 package/ios27-ioav/build.py [--tree TREE] [--out DIR]
"""

import argparse
import hashlib
import json
import subprocess
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORKSPACE = ROOT.parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

import build_ios27_drivers as drivers  # noqa: E402

PIN = '8cc8eccc429aa95397aef0ffe64b226e71b014d84190c1e3f84cf5c270a6e6bc'
TARGETS = {
    '__orig_ioav_timing_withData': ('com.apple.iokit.IOAVFamily', 0xfffffff0097ad798),
    '__orig_ioav_timing_setColorElements': ('com.apple.iokit.IOAVFamily', 0xfffffff0097aec88),
    '__orig_ioav_color_withCapabilities': ('com.apple.iokit.IOAVFamily', 0xfffffff0097acb18),
    '__orig_iomfb_hotPlug_notify': ('com.apple.iokit.IOMobileGraphicsFamily', 0xfffffff009a7a4d4),
}


def verify_stub_object(path):
    raw = path.read_bytes()
    info = drivers.macho_obj(raw)
    symbols = {row['name']: row for row in drivers.object_symbols(raw, info)}
    verified = {}
    for name, (_, address) in TARGETS.items():
        if name not in symbols or not symbols[name]['kind'] & 0xe:
            raise ValueError('Original function stub lacks its defined symbol: ' + name)
        va = symbols[name]['address']
        sections = [section for segment in info['segments'] for section in segment['sections']
                    if section['va'] <= va and va + 24 <= section['va'] + section['size']]
        if len(sections) != 1:
            raise ValueError('Original function stub requires one executable extent: ' + name)
        section = sections[0]
        offset = section['fileoff'] + va - section['va']
        words = struct.unpack_from('<6I', raw, offset)
        expected = (0xd503245f, 0xd2800010 | ((address & 0xffff) << 5),
                    *(0xf2800010 | (shift // 16 << 21) | ((address >> shift & 0xffff) << 5)
                      for shift in (16, 32, 48)), 0xd61f0200)
        if words != expected:
            raise ValueError('Compiled stub target differs from the original VA: ' + name)
        verified[name] = hex(address)
    return verified


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', default=str(drivers.DEFAULT_TREE))
    parser.add_argument('--out', default=str(ROOT / 'build/ios27-ioav/obj'))
    parser.add_argument('--kernelcache', type=Path, default=drivers.DEFAULT_KERNELCACHE)
    args = parser.parse_args()
    tree, out = Path(args.tree), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    collection = drivers.Collection(args.kernelcache)
    if collection.sha256 != PIN:
        raise ValueError('IOAV adapter requires the original 24B119 collection')
    originals = {}
    for name, (bundle, address) in TARGETS.items():
        member = collection.member(bundle)
        if member is None or address not in member.function_starts():
            raise ValueError('Original adapter function lacks its qualified module entry: ' + name)
        body = member.read(address, 32)
        if struct.unpack_from('<I', body)[0] != 0xd503245f:
            raise ValueError('Original adapter function lacks its BTI landing pad: ' + name)
        originals[name] = {'module': bundle, 'address': hex(address),
                           'prefix': body.hex(), 'prefix_sha256': hashlib.sha256(body).hexdigest()}

    include = tree / 'build/marble-boot/include'
    extra_includes = [include, ROOT / 'package', ROOT / 'package/Protocol',
                      tree / 'build/marble-boot', tree / 'kits/marble',
                      tree / 'kits/marble/audio']

    objects = []
    for source in sorted(HERE.glob('*.cpp')) + sorted(HERE.glob('*.c')):
        obj = out / f'{source.stem}.o'
        drivers.compile_source(
            drivers.compile_record(tree, source.suffix), source, obj,
            extra_includes + [source.parent])
        objects.append(obj)
        targets = verify_stub_object(obj)
        print(f'compiled {source.name} -> {obj}')

    archive = out / 'libios27-ioav.a'
    archive.unlink(missing_ok=True)
    subprocess.check_call(['xcrun', 'ar', 'rcs', str(archive)]
                          + [str(o) for o in objects])
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    print(f'archive {archive} sha256={digest}')

    report = {
        'tree': str(tree), 'objects': [str(o) for o in objects],
        'archive': str(archive), 'archive_sha256': digest,
        'target': '24B119', 'kernelcache_sha256': PIN, 'originals': originals,
        'compiled_stub_targets': targets,
        'compile_record': str(tree / drivers.OBJ_ROOT / drivers.CXX_RECORD)}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
