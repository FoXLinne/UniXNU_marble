#!/usr/bin/env python3
"""Build panel-sized wallpaper resources from the selected iOS restore volume."""
import hashlib
from pathlib import Path
import plistlib
import re
import subprocess

from prepare_phone_resources import pack_addon, unpack_addon


def prepare_wallpaper(addon, ios_root, identity, cache):
    source = ios_root / 'Library/Wallpaper'
    entries = {name: value for name, value in unpack_addon(addon).items()
               if not name.startswith('wallpaper/')}
    cache.mkdir(parents=True, exist_ok=True)
    resources = {}
    converted = 0
    native = 0
    screen = '360w-800h@3x~iphone'

    def add(path, destination, names=None):
        nonlocal converted
        names = names or {}
        if path.suffix.upper() == '.HEIC':
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            png = cache / (digest + '.png')
            if not png.exists():
                subprocess.run(['sips', '-s', 'format', 'png', path, '--out', png],
                               check=True, stdout=subprocess.DEVNULL)
            data = png.read_bytes()
            destination = str(Path(destination).with_suffix('.png'))
            converted += 1
        else:
            data = path.read_bytes()
            if path.suffix in ('.caml', '.xml'):
                data = data.replace(b'.HEIC', b'.png')
                for original, replacement in names.items():
                    data = data.replace(original.encode(), replacement.encode())
        resources['wallpaper/' + destination] = (0o644, data)

    collections = {}
    for collection in sorted((source / 'Collections').iterdir()):
        if not collection.is_dir():
            continue
        metadata = plistlib.loads((collection / 'WallpaperCollection.plist').read_bytes())
        collections[metadata['identifier']] = metadata
        prefix = 'Collections/' + collection.name
        for path in sorted(collection.rglob('*')):
            relative = path.relative_to(collection)
            if path.is_file() and relative.parts[0] != 'Wallpapers':
                add(path, f'{prefix}/{relative.as_posix()}')
        for wallpaper in sorted((collection / 'Wallpapers').iterdir()):
            if not wallpaper.is_dir():
                continue
            metadata = plistlib.loads((wallpaper / 'Wallpaper.plist').read_bytes())
            names = {}
            folder = wallpaper.name
            if metadata['logicalScreenClass'] != 'any':
                folder = re.sub(r'\d+w-\d+h@\dx~iphone', screen, folder)
                metadata['logicalScreenClass'] = screen
                native += 1
                for variants in metadata['assets'].values():
                    for properties in variants.values():
                        for key, value in properties.items():
                            if isinstance(value, str) and value.endswith('.ca'):
                                short = key.split('Animation', 1)[0].capitalize() + '.ca'
                                names[value] = short
                                properties[key] = short
            target = f'{prefix}/Wallpapers/{folder}'
            for path in sorted(wallpaper.rglob('*')):
                if not path.is_file() or path.name == 'Wallpaper.plist':
                    continue
                relative = path.relative_to(wallpaper)
                parts = list(relative.parts)
                parts[0] = names.get(parts[0], parts[0])
                add(path, f'{target}/{"/".join(parts)}', names)
            resources[f'wallpaper/{target}/Wallpaper.plist'] = (
                0o644, plistlib.dumps(metadata, fmt=plistlib.FMT_BINARY))

    defaults = plistlib.loads((source / 'DefaultWallpapers~iphone.plist').read_bytes())
    default = defaults['default']
    if default['collectionIdentifier'] not in collections:
        raise ValueError('Default wallpaper collection is absent from the restore volume')
    for model in (identity['product'], identity['compatible_device_fallback']):
        defaults[model] = {'default': dict(default)}
    resources['wallpaper/DefaultWallpapers~iphone.plist'] = (
        0o644, plistlib.dumps(defaults, fmt=plistlib.FMT_BINARY))
    order = plistlib.loads((source / 'Collections/Collections~iphone.plist').read_bytes())
    order['order'] = [item for item in order['order'] if item in collections]
    resources['wallpaper/Collections/Collections~iphone.plist'] = (
        0o644, plistlib.dumps(order, fmt=plistlib.FMT_BINARY))
    entries.update(resources)
    pack_addon(entries, addon)
    return dict(source=str(source), screen=screen, product=identity['product'],
                collections=order['order'], native_variants=native,
                converted_layers=converted, files=len(resources),
                resource_bytes=sum(len(data) for _, data in resources.values()),
                default=default)
