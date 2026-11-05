#!/usr/bin/env python3
"""Stage the supplied iPadOS 17 flat wallpaper for the marble phone layout."""
import argparse
from pathlib import Path
import plistlib
import shutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('os_mount', type=Path)
    parser.add_argument('animation', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('output exists')
    source = args.os_mount / 'Library/Wallpaper/Collections/iPadOS_17~ipad.wallpaperCollection'
    collection = args.output / 'Phone.wallpaperCollection'
    collection.mkdir(parents=True)
    for name in ('Info.plist', 'WallpaperCollection.plist', 'Localizable.loctable'):
        shutil.copyfile(source / name, collection / name)
    order = plistlib.loads((args.os_mount / 'Library/Wallpaper/Collections/Collections~ipad.plist').read_bytes())
    identifier = plistlib.loads((collection / 'WallpaperCollection.plist').read_bytes())['identifier']
    order['order'] = [identifier]
    (args.output / 'Collections~iphone.plist').write_bytes(plistlib.dumps(order, fmt=plistlib.FMT_BINARY))
    template = next(source.glob('Wallpapers/*Flat-1024*/Wallpaper.plist'))
    for screen in ('360w-800h@3x~iphone', '375w-812h@3x~iphone'):
        destination = collection / 'Wallpapers' / f'7570.Phone-{screen}.wallpaper'
        destination.mkdir(parents=True)
        metadata = plistlib.loads(template.read_bytes())
        metadata['logicalScreenClass'] = screen
        metadata['productTypes'] = ['iPhone10,3', 'iPhone10,6']
        metadata['assets']['lockAndHome']['default']['backgroundAnimationFileName'] = 'background.ca'
        (destination / 'Wallpaper.plist').write_bytes(plistlib.dumps(metadata, fmt=plistlib.FMT_BINARY))
        shutil.copytree(args.animation, destination / 'background.ca')
    print(args.output)


if __name__ == '__main__':
    main()
