#!/usr/bin/env python3
"""Install the iOS 18 system windows and writable Data on marble UFS."""
import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path
import shlex
import struct
import subprocess
import zlib


def mark_slot_b_successful(root, push, backup):
    """Publish the boot-success bit used by Qualcomm's GPT boot HAL."""
    root('bootctl set-active-boot-slot 1')
    header = root('dd if=/dev/block/sde bs=4096 skip=1 count=1 status=none',
                  stdout=subprocess.PIPE).stdout
    entry_lba, count, entry_size = struct.unpack_from('<QII', header, 72)
    blocks = (count * entry_size + 4095) // 4096
    primary = bytearray(root(f'dd if=/dev/block/sde bs=4096 count={entry_lba + blocks} status=none',
                             stdout=subprocess.PIPE).stdout)
    backup_lba = struct.unpack_from('<Q', header, 32)[0] - blocks
    secondary = bytearray(root(f'dd if=/dev/block/sde bs=4096 skip={backup_lba} count={blocks + 1} status=none',
                               stdout=subprocess.PIPE).stdout)
    (backup / 'slot-metadata.primary.before.bin').write_bytes(primary)
    (backup / 'slot-metadata.secondary.before.bin').write_bytes(secondary)
    entries = bytearray(primary[entry_lba * 4096:entry_lba * 4096 + count * entry_size])
    slot = next(i for i in range(count)
                if entries[i * entry_size + 56:i * entry_size + 128].decode('utf-16le').rstrip('\0') == 'boot_b')
    at = slot * entry_size + 48
    struct.pack_into('<Q', entries, at, struct.unpack_from('<Q', entries, at)[0] | (1 << 54))
    primary[entry_lba * 4096:entry_lba * 4096 + len(entries)] = entries
    secondary[:len(entries)] = entries
    for data, offset in ((primary, 4096), (secondary, blocks * 4096)):
        struct.pack_into('<I', data, offset + 88, zlib.crc32(entries))
        struct.pack_into('<I', data, offset + 16, 0)
        size = struct.unpack_from('<I', data, offset + 12)[0]
        struct.pack_into('<I', data, offset + 16, zlib.crc32(data[offset:offset + size]))
    for name, data, offset in (('slot-metadata.secondary.bin', secondary, backup_lba),
                                ('slot-metadata.primary.bin', primary, 0)):
        path = backup / name
        path.write_bytes(data)
        remote = push(path)
        root(f'dd if={shlex.quote(remote)} of=/dev/block/sde bs=4096 seek={offset} conv=notrunc,fsync')
    root('bootctl is-slot-marked-successful 1')



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['systems', 'data', 'boot', 'update'])
    parser.add_argument('--images', required=True, type=Path)
    parser.add_argument('--assets', required=True, type=Path)
    parser.add_argument('--data', type=Path)
    parser.add_argument('--serial', default='556fdfe')
    parser.add_argument('--backup', type=Path, help='Directory for the installed boot image and storage metadata')
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
    adb = ['adb', '-s', args.serial]
    target = '/dev/block/by-name/unixnu'

    def root(command, **options):
        return subprocess.run([*adb, 'exec-out', 'su -c ' + shlex.quote(command)], check=True, **options)

    def push(path):
        remote = '/data/local/tmp/' + args.assets.name + '-' + path.name
        subprocess.run([*adb, 'push', str(path), remote], check=True)
        return remote

    capacity = int(root('blockdev --getsize64 ' + target, stdout=subprocess.PIPE).stdout)
    images = json.loads(args.images.read_text())
    if args.action == 'update':
        backup = args.backup or args.assets / 'ufs-install'
        backup.mkdir(parents=True, exist_ok=True)
        old_header = root(f'dd if={target} bs=4096 count=1 status=none', stdout=subprocess.PIPE).stdout
        if old_header[:8] != b'SHNGROOT' or hashlib.sha256(old_header[:4064]).digest() != old_header[4064:]:
            raise RuntimeError('Installed root manifest is invalid')
        (backup / 'root_manifest.before.bin').write_bytes(old_header)
        for index, image in enumerate(images):
            if image['name'] == 'KernelSeed':
                continue
            at = 24 + index * 128
            if (old_header[at:at + 32].split(b'\0')[0].decode() != image['name'] or
                    struct.unpack_from('<QQ', old_header, at + 32) != (image['offset'], image['raw_bytes']) or
                    old_header[at + 56:at + 88].hex() != image['sha256']):
                raise RuntimeError('Installed system windows differ from the selected boot candidate')
        kernel = next(image for image in images if image['name'] == 'KernelSeed')
        source = Path(kernel['source'])
        with source.open('rb') as stream:
            stream.seek(kernel['raw_bytes'] - 1048576)
            mailbox = stream.read(128)
        logical = struct.unpack_from('<Q', mailbox, 56)[0]
        data_first = capacity - 1048576 - logical
        if (mailbox[:8] != b'ROOTLOG1' or logical == 0 or logical % 4096 or
                kernel['offset'] % 4096 or kernel['raw_bytes'] % 4096 or
                kernel['offset'] + kernel['raw_bytes'] + 4 * 1048576 > data_first or
                source.stat().st_size != kernel['raw_bytes'] or
                hashlib.sha256(source.read_bytes()).hexdigest() != kernel['sha256']):
            raise RuntimeError('Kernel window differs from its manifest or overlaps Data')
        image = args.assets / 'marble_persistent_boot.img'
        boot_capacity = int(root('blockdev --getsize64 /dev/block/by-name/boot_b', stdout=subprocess.PIPE).stdout)
        if image.stat().st_size > boot_capacity:
            raise RuntimeError('Persistent boot image exceeds boot_b')
        saved_boot = backup / 'boot_b.before.img'
        if not saved_boot.exists():
            with saved_boot.open('wb') as stream:
                root('dd if=/dev/block/by-name/boot_b bs=4194304 status=none', stdout=stream)
        header = bytearray((args.assets / 'root_manifest.bin').read_bytes())
        # The RTC offset is persistent board state stored inside SHNGROOT.
        calibration = struct.unpack_from('<3Q', old_header, 1024)
        if calibration[0] == 0x314c41435452424d and calibration[2] == (~calibration[1] & ((1 << 64) - 1)):
            header[1024:1048] = old_header[1024:1048]
            header[4064:] = hashlib.sha256(header[:4064]).digest()
            (args.assets / 'root_manifest.bin').write_bytes(header)
        at = 24 + images.index(kernel) * 128
        installed_kernel = (struct.unpack_from('<QQ', old_header, at + 32) ==
                            (kernel['offset'], kernel['raw_bytes']) and
                            old_header[at + 56:at + 88].hex() == kernel['sha256'])
        if not installed_kernel:
            remote = push(source)
            root(f'dd if={shlex.quote(remote)} of={target} bs=4096 seek={kernel["offset"] // 4096} conv=notrunc,fsync')
            digest = root(f'dd if={target} bs=4096 skip={kernel["offset"] // 4096} count={kernel["raw_bytes"] // 4096} status=none | sha256sum',
                          stdout=subprocess.PIPE).stdout.decode().split()[0]
            if digest != kernel['sha256']:
                raise RuntimeError('UFS kernel readback differs from the candidate')
        if header != old_header:
            remote = push(args.assets / 'root_manifest.bin')
            root(f'dd if={shlex.quote(remote)} of={target} bs=4096 count=1 conv=notrunc,fsync')
        remote = push(image)
        root(f'dd if={shlex.quote(remote)} of=/dev/block/by-name/boot_b bs=4194304 conv=notrunc,fsync')
        digest = root(f'dd if=/dev/block/by-name/boot_b bs=4194304 count={image.stat().st_size} iflag=count_bytes status=none | sha256sum',
                      stdout=subprocess.PIPE).stdout.decode().split()[0]
        if digest != hashlib.sha256(image.read_bytes()).hexdigest():
            raise RuntimeError('boot_b readback differs from the persistent boot image')
        mark_slot_b_successful(root, push, backup)
        candidate = json.loads((args.assets / 'persistent-boot.json').read_text())
        report = dict(kernel_sha256=candidate['kernel_sha256'],
                      kernel_window_sha256=kernel['sha256'], boot_image_sha256=digest,
                      data_logical_bytes=logical, data_offset=data_first,
                      data_uuid=mailbox[64:101].split(b'\0')[0].decode(),
                      boot_slot='b', slot_marked_successful=True,
                      rtc_calibration=bytes(header[1024:1048]).hex())
        (backup / 'installation.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
        return
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
