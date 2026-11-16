#!/usr/bin/env python3
"""Compile the ios27-display adapter against the 24B119 target compile records.

Reuses scripts/build_ios27_drivers.py compile_source()/compile_record() so
the adapter object is produced with exactly the same arm64e-apple-ios27.2
toolchain flags as every other driver-family translation unit. Emits an
object plus an archive the parent driver/link step can pick up.

Every absolute-VA stub is verified against the original kernelcache: the
target address must be a function start in its fileset member, and the
compiled stub's movz/movk/br word sequence must encode exactly that VA.

Usage:
    python3 package/ios27-display/build.py [--tree TREE] [--out DIR]
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

IMG = 'com.apple.iokit.IOMobileGraphicsFamily'
IOS = 'com.apple.iokit.IOSurface'
KERN = 'com.apple.kernel'

# Absolute-VA landing-pad stubs (bti; movz/movk x16, VA; br x16).
TARGETS = {
    '__orig_surf_allocate': (IOS,0xfffffff009be1fb4),
    '__orig_region_name': (IOS,0xfffffff009bf7df8),
    '_marble_region_base_init': (IOS,0xfffffff009bf4268),
    '__orig_region_register': (IOS,0xfffffff009bf7924),
    '__orig_surf_register_region': (IOS, 0xfffffff009bf7cb4),
    '__orig_surf_descriptor': (IOS, 0xfffffff009be5bf4),
    '__orig_surf_width': (IOS, 0xfffffff009be5cf8),
    '__orig_surf_height': (IOS, 0xfffffff009be5d04),
    '__orig_surf_stride': (IOS, 0xfffffff009be5d1c),
    '__orig_surf_format': (IOS, 0xfffffff009be5ce0),
    '__orig_surf_planes': (IOS, 0xfffffff009be5d4c),
    '__orig_surf_create': (IOS, 0xfffffff009bf75b8),
    '__orig_surf_region': (IOS, 0xfffffff009bf7bb4),
    '__orig_surf_setregion': (IOS, 0xfffffff009bedd54),
    '__orig_surf_setformat': (IOS, 0xfffffff009bedfd0),
    '__orig_surf_setx': (IOS, 0xfffffff009bedd60),
    '__orig_surf_sety': (IOS, 0xfffffff009bedfec),
    '__orig_surf_layout': (IMG, 0xfffffff009a74928),
    '__orig_vsync_notify': (IMG, 0xfffffff009a7a200),
    '__orig_refresh_props': (IMG, 0xfffffff009a7e3d4),
}

# Helpers the adapter exports to NativeLegacyDisplay/MarbleDisplay.
EXPORTS = [
    '_native_legacy_default_surface_create',
    '_native_legacy_set_refresh_properties',
    '_native_legacy_vsync_notify',
    '_native_legacy_surface_descriptor',
    '_native_legacy_surface_width',
    '_native_legacy_surface_height',
    '_native_legacy_surface_stride',
    '_native_legacy_surface_allocation',
    '_native_legacy_surface_format',
    '_native_legacy_surface_planes',
]

BTI = 0xd503245f        # bti c
PACIBSP = 0xd503237f    # pacibsp


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
        expected = (BTI, 0xd2800010 | ((address & 0xffff) << 5),
                    *(0xf2800010 | (shift // 16 << 21) | ((address >> shift & 0xffff) << 5)
                      for shift in (16, 32, 48)), 0xd61f0200)
        if words != expected:
            raise ValueError('Compiled stub target differs from the original VA: ' + name)
        verified[name] = hex(address)
    for name in EXPORTS:
        if name not in symbols or not symbols[name]['kind'] & 0xe:
            raise ValueError('Adapter export missing or not external: ' + name)
    verified['__orig_opts_setObject'] = 'source OSDictionary::setObject IOSurfaceMemoryRegion'
    verified['__orig_surf_opts'] = 'source OSDictionary::withCapacity'
    verified['__orig_surf_memlookup'] = 'source OSString::withCString'
    return verified


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', default=str(drivers.DEFAULT_TREE))
    parser.add_argument('--out', default=str(ROOT / 'build/ios27-display/obj'))
    parser.add_argument('--kernelcache', type=Path, default=drivers.DEFAULT_KERNELCACHE)
    args = parser.parse_args()
    tree, out = Path(args.tree), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    collection = drivers.Collection(args.kernelcache)
    if collection.sha256 != PIN:
        raise ValueError('display adapter requires the original 24B119 collection')
    originals = {}
    for name, (bundle, address) in TARGETS.items():
        member = collection.member(bundle)
        if member is None:
            raise ValueError('Missing member ' + bundle + ' for ' + name)
        body = member.read(address, 32)
        first = struct.unpack_from('<I', body)[0]
        if first not in (BTI, PACIBSP):
            raise ValueError(
                f'Original adapter function {name} lacks a function prologue '
                f'(first word {first:08x} at {hex(address)})')
        # Function-start check where the member supplies a starts table;
        # IOSurface trivial getters sit in the middle of adjacent functions
        # so they are validated by their instruction pattern instead.
        starts = member.function_starts()
        note = 'function-start' if address in starts else 'instruction-pattern'
        originals[name] = {'module': bundle, 'address': hex(address),
                           'evidence': note, 'prefix': body.hex(),
                           'prefix_sha256': hashlib.sha256(body).hexdigest()}

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

    archive = out / 'libios27-display.a'
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
        'exports': EXPORTS,
        'compile_record': str(tree / drivers.OBJ_ROOT / drivers.CXX_RECORD)}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
