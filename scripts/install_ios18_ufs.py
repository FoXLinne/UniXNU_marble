#!/usr/bin/env python3
"""Install the iOS 18 system windows and writable Data on marble UFS."""
import argparse
import json
import re
import shutil
from pathlib import Path
import shlex
import struct
import subprocess



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['systems', 'data', 'boot'])
    parser.add_argument('--images', required=True, type=Path)
    parser.add_argument('--assets', required=True, type=Path)
    parser.add_argument('--data', type=Path)
    parser.add_argument('--serial', default='556fdfe')
    args = parser.parse_args()
    if args.action == 'boot':
        fastboot = ['fastboot', '-s', args.serial]
        result = subprocess.run([*fastboot, 'getvar', 'partition-size:unixnu'],
                                capture_output=True, text=True, check=True)
        match = re.search(r'partition-size:unixnu:\s*(0x[0-9a-fA-F]+)', result.stdout + result.stderr)
        if not match:
            raise RuntimeError('Fastboot did not report the UFS partition size')
        capacity = int(match[1], 16)
        entries = json.loads((args.assets / 'storage_images.json').read_text())
        kernel = next(image for image in entries if image['name'] == 'KernelSeed')
        source = Path(kernel['source'])
        offset, length = kernel['offset'], kernel['raw_bytes']
        header = (args.assets / 'root_manifest.bin').read_bytes()
        if len(header) != 4096 or offset < 4096 or offset % 4096 or length % 4096 or source.stat().st_size != length:
            raise RuntimeError('Kernel window has invalid sparse geometry')
        with source.open('rb') as stream:
            stream.seek(length - 1048576)
            mailbox = stream.read(128)
        if mailbox[:8] != b'ROOTLOG1':
            raise RuntimeError('Kernel mailbox header is invalid')
        logical = struct.unpack_from('<Q', mailbox, 56)[0]
        if not logical or logical % 4096 or offset + length + 4 * 1048576 > capacity - 1048576 - logical:
            raise RuntimeError('Kernel window overlaps persistent Data')
        args.assets.mkdir(parents=True, exist_ok=True)
        sparse = args.assets / 'kernel-update.sparse'
        with sparse.open('wb') as stream:
            stream.write(struct.pack('<I4H4I', 0xed26ff3a, 1, 0, 28, 12, 4096,
                                     (offset + length) // 4096, 3, 0))
            stream.write(struct.pack('<2H2I', 0xcac1, 0, 1, 12 + len(header)))
            stream.write(header)
            stream.write(struct.pack('<2H2I', 0xcac3, 0, offset // 4096 - 1, 12))
            stream.write(struct.pack('<2H2I', 0xcac1, 0, length // 4096, 12 + length))
            with source.open('rb') as kernel_stream:
                shutil.copyfileobj(kernel_stream, stream)
        subprocess.run([*fastboot, 'flash', 'unixnu', str(sparse)], check=True)
        subprocess.run([*fastboot, 'flash', 'boot_b', str(args.assets / 'marble_persistent_boot.img')], check=True)
        subprocess.run([*fastboot, 'set_active', 'b'], check=True)
        subprocess.run([*fastboot, 'reboot'], check=True)
        return
    adb = ['/opt/homebrew/bin/adb', '-s', args.serial]
    target = '/dev/block/by-name/unixnu'

    def root(command, **options):
        return subprocess.run([*adb, 'exec-out', 'su -c ' + shlex.quote(command)], check=True, **options)

    def push(path):
        remote = '/data/local/tmp/' + path.name
        subprocess.run([*adb, 'push', str(path), remote], check=True)
        return remote

    capacity = int(root('blockdev --getsize64 ' + target, stdout=subprocess.PIPE).stdout)
    images = json.loads(args.images.read_text())
    cursor = 1048576
    for image in images:
        image['offset'] = cursor
        cursor = (cursor + image['raw_bytes'] + 1048575) & -1048576
    if cursor >= capacity:
        raise RuntimeError('System windows exceed the UFS partition')
    args.assets.mkdir(parents=True, exist_ok=True)
    if args.action == 'systems':
        progress = args.assets / 'system-windows.json'
        installed = json.loads(progress.read_text()) if progress.exists() else []
        for image in images:
            if image in installed:
                continue
            source = push(Path(image['source']))
            root(f'dd if={shlex.quote(source)} of={target} bs=4194304 seek={image["offset"]} oflag=seek_bytes conv=notrunc,fsync')
            installed.append(image)
            progress.write_text(json.dumps(installed, indent=2) + '\n')
            print(f'{image["name"]}: {image["raw_bytes"]} bytes persisted', flush=True)
    elif args.action == 'data':
        if args.data is None:
            parser.error('data requires --data')
        with args.data.open('rb') as stream:
            magic, version, page, logical, count = struct.unpack('<8sIIQQ', stream.read(32))
            if (magic, version, page) != (b'DBRDISK1', 1, 4096) or logical % 4096:
                raise RuntimeError('Invalid packed Data image')
            extents = [struct.unpack('<QQ', stream.read(16)) for _ in range(count)]
        first = capacity - 1048576 - logical
        if first < cursor + 4 * 1048576 or first % 4096:
            raise RuntimeError('Data overlaps the UFS system windows')
        remote = push(args.data)
        command = f'dd if=/dev/zero of={target} bs=4194304 seek={first} oflag=seek_bytes count={logical} iflag=count_bytes conv=notrunc,fsync'
        root(command)
        commands = ['set -e']
        offset = 32 + count * 16
        for at, length in extents:
            if (at | length) % 4096 or at + length > logical:
                raise RuntimeError('Packed Data extent exceeds its UFS window')
            commands.append(f'dd if={shlex.quote(remote)} of={target} bs=4194304 skip={offset} seek={first + at} count={length} iflag=skip_bytes,count_bytes oflag=seek_bytes conv=notrunc status=none')
            offset += length
        commands.append('sync')
        script = args.assets / 'write-data.sh'
        script.write_text('\n'.join(commands) + '\n')
        root('sh ' + shlex.quote(push(script)))
        (args.assets / 'data-window.json').write_text(json.dumps(dict(offset=first, logical_bytes=logical), indent=2) + '\n')
        print(f'Data: {logical} bytes on UFS', flush=True)



if __name__ == '__main__':
    main()
