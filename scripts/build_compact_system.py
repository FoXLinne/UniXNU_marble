#!/usr/bin/env python3
"""Build a compressed iOS 16 APFS root from extracted restore components."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import uuid


def link_tree(source, destination):
    shutil.copytree(source, destination, symlinks=True, copy_function=os.link)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', required=True, type=Path)
    parser.add_argument('--cache', required=True, type=Path)
    parser.add_argument('--builder', type=Path, default=Path(__file__).with_name('mkapfs.py'))
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    assets, output = args.assets.resolve(), args.output.resolve()
    version = plistlib.loads((assets / 'rootfs/System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if not version['ProductVersion'].startswith('16.'):
        parser.error('The extracted restore must contain iOS 16')
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='compact_', dir=output) as temporary:
        stage = Path(temporary)
        root = stage / 'root'
        link_tree(assets / 'rootfs', root)
        apps = root / 'private/var/staged_system_apps'
        if apps.is_dir():
            for app in apps.iterdir():
                shutil.move(app, root / 'Applications' / app.name)
            apps.rmdir()
        cryptexes = root / 'System/Cryptexes'
        cryptexes.mkdir(parents=True, exist_ok=True)
        for source, name in [('cryptex', 'OS'), ('cryptex-app', 'App')]:
            link_tree(assets / source, cryptexes / name)
        cache = cryptexes / 'OS/System/Library/Caches/com.apple.dyld'
        cache.mkdir(parents=True, exist_ok=True)
        for path in sorted(args.cache.glob('dyld_shared_cache_arm64*')):
            if path.name.endswith('.symbols'):
                continue
            target = cache / path.name
            target.unlink(missing_ok=True)
            os.link(path, target)
        # Use Apple's launchd and keep Metal discovery available to NativeMetal.
        (root / 'System/Library/Extensions').mkdir(parents=True, exist_ok=True)
        image = stage / 'OS.apfs'
        subprocess.run([sys.executable, args.builder.resolve(), '-z', '-o', image,
                        f'System={root},manifest={assets / "system.manifest"}'], check=True)
        digest = hashlib.file_digest(image.open('rb'), 'sha256').hexdigest()
        volume_uuid = str(uuid.UUID(bytes=hashlib.sha256(b'OS.apfsvolume 0').digest()[:16])).upper()
        final = output / image.name
        os.replace(image, final)
        report = [dict(name='OS', source=str(final), format='raw', raw_bytes=final.stat().st_size,
                       sha256=digest, volumes=[dict(name='System', uuid=volume_uuid)],
                       system_version=version, cryptex_layout='integrated',
                       builder_sha256=hashlib.sha256(args.builder.read_bytes()).hexdigest())]
        (output / 'root_images.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
