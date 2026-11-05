#!/usr/bin/env python3
"""Add iOS 17 phone layouts and wallpaper to the iPadOS runtime addon."""
import argparse
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import tempfile
import zlib


def unpack_addon(path):
    blob = path.read_bytes()
    if blob[:8] != b'NGPUADD2':
        raise ValueError('expected NGPUADD2 addon')
    entries, at = {}, 24
    for _ in range(struct.unpack_from('<I', blob, 8)[0]):
        length, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        name = blob[at:at + length - 1].decode()
        at += (length + 7) & ~7
        data = zlib.decompress(blob[at:at + packed])
        at += (packed + 7) & ~7
        if len(data) != size:
            raise ValueError(name)
        entries[name] = (mode, data)
    if at != len(blob):
        raise ValueError('trailing addon bytes')
    return entries


def pack_addon(entries, path):
    body = bytearray()
    for name, (mode, data) in sorted(entries.items()):
        encoded, packed = name.encode() + b'\0', zlib.compress(data, 9)
        if len(encoded) > 160:
            raise ValueError('addon path exceeds loader limit: ' + name)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed))
        body += encoded + bytes(-len(encoded) % 8) + packed + bytes(-len(packed) % 8)
    size = len(body) + 24
    if size > 64 * 1024 * 1024:
        raise ValueError('addon exceeds DarwinBoot’s 64 MiB limit')
    path.write_bytes(b'NGPUADD2' + struct.pack('<IIQ', len(entries), 0, size) + body)


def add_wallpaper(entries, ios_root):
    collection = ios_root / 'Library/Wallpaper/Collections/iOS_17~iphone.wallpaperCollection'
    prefix = f'collections/{collection.name}'
    metadata = plistlib.loads((collection / 'WallpaperCollection.plist').read_bytes())
    entries['collections/Collections~iphone.plist'] = (
        0o644, plistlib.dumps({'order': [metadata['identifier']]}, fmt=plistlib.FMT_BINARY))
    for name in ('Info.plist', 'Localizable.loctable', 'WallpaperCollection.plist'):
        entries[f'{prefix}/{name}'] = (0o644, (collection / name).read_bytes())

    source = collection / 'Wallpapers/7565.iOS_17-375w-812h@3x~iphone.wallpaper'
    animation_names = {'backgroundAnimationFileName': 'Background.ca',
                       'floatingAnimationFileNameKey': 'Floating.ca'}
    native = plistlib.loads((source / 'Wallpaper.plist').read_bytes())
    native_assets = native['assets']['lockAndHome']['default']
    animation_files = {}
    with tempfile.TemporaryDirectory() as temp:
        converted = Path(temp) / 'layer.png'
        for key, short in animation_names.items():
            animation = source / native_assets[key]
            for path in animation.rglob('*'):
                if not path.is_file():
                    continue
                name = path.relative_to(animation)
                if path.suffix == '.HEIC':
                    # ImageIO on the port cannot decode these HEVC layers.
                    subprocess.run(['sips', '-Z', '1440', '-s', 'format', 'png',
                                    path, '--out', converted], check=True,
                                   stdout=subprocess.DEVNULL)
                    data = converted.read_bytes()
                    name = name.with_suffix('.png')
                else:
                    data = path.read_bytes()
                    if path.suffix in ('.caml', '.xml'):
                        data = data.replace(b'.HEIC', b'.png')
                animation_files[f'{short}/{name.as_posix()}'] = (0o644, data)

    for screen in ('375w-812h@3x~iphone', '360w-800h@3x~iphone'):
        target = f'{prefix}/Wallpapers/7565.iOS_17-{screen}.wallpaper'
        wallpaper = plistlib.loads((source / 'Wallpaper.plist').read_bytes())
        wallpaper['logicalScreenClass'] = screen
        assets = wallpaper['assets']['lockAndHome']['default']
        # Keep the animation paths within the addon's 160-byte entry limit.
        assets.update(animation_names)
        entries.update((f'{target}/{name}', entry)
                       for name, entry in animation_files.items())
        entries[f'{target}/Wallpaper.plist'] = (
            0o644, plistlib.dumps(wallpaper, fmt=plistlib.FMT_BINARY))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ios-root', required=True, type=Path)
    parser.add_argument('--addon', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    entries = {name: entry for name, entry in unpack_addon(args.addon).items()
               if not name.startswith(('collections/', 'wallpaper/'))}
    resources = {
        'phone-control-center/DefaultModuleSettings~iphone.plist':
            'System/Library/PrivateFrameworks/ControlCenterUI.framework/DefaultModuleSettings~iphone.plist',
        'phone-keyboard/KBLayouts_iPhone.dat':
            'System/Library/PrivateFrameworks/TextInputUI.framework/KBLayouts_iPhone.dat',
    }
    for target, source in resources.items():
        entries[target] = (0o644, (args.ios_root / source).read_bytes())
    settings = plistlib.loads(entries['phone-control-center/DefaultModuleSettings~iphone.plist'][1])
    for module in ('com.apple.control-center.ConnectivityModule',
                   'com.apple.mediaremote.controlcenter.nowplaying'):
        if settings[module]['size'] != {'width': 2, 'height': 2}:
            raise ValueError('unexpected native phone module settings: ' + module)
    add_wallpaper(entries, args.ios_root)
    pack_addon(entries, args.output)
    print(json.dumps(dict(resources=resources, addon=str(args.output)), indent=2))


if __name__ == '__main__':
    main()
