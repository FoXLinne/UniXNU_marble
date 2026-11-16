#!/usr/bin/env python3
"""Install 24B119 system windows inside the existing unixnu partition and boot a candidate."""
import argparse
from datetime import datetime, timezone, timedelta
import hashlib
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess

from ios27_migration import DEVICE, digest, save, record_stage


class Android:
    def __init__(self, serial):
        self.command = ['adb', '-s', serial]

    def root(self, command, binary=False):
        output = subprocess.check_output([*self.command, 'exec-out', 'su -c ' + shlex.quote(command)])
        return output if binary else output.decode().strip()

    def read(self, offset, count):
        if offset % 4096 or count % 4096 or count <= 0:
            raise ValueError('Partition reads require whole 4K blocks')
        return self.root(f'dd if=/dev/block/by-name/unixnu bs=4096 skip={offset // 4096} '
                         f'count={count // 4096} status=none', binary=True)


def inspect_installation(android, candidate, images):
    if android.root('getprop ro.product.device') != 'marble':
        raise ValueError('Connected Android device must identify as marble')
    capacity = int(android.root('blockdev --getsize64 /dev/block/by-name/unixnu'))
    profile = json.loads((DEVICE / 'profiles/marble-storage-24B119.json').read_text())
    logical = profile['data_logical_bytes']
    data_first = capacity - profile['userdata_tail_reserve_bytes'] - logical
    if data_first < 0 or data_first % 4096:
        raise ValueError('Existing Data window geometry is invalid')
    old = android.read(0, 4096)
    if len(old) != 4096 or old[:8] != b'SHNGROOT' or hashlib.sha256(old[:4064]).digest() != old[4064:]:
        raise ValueError('Existing system manifest requires its SHNGROOT digest')
    magic, version, count, old_total = struct.unpack_from('<8sIIQ', old)
    if version != 1 or count != 3:
        raise ValueError('Existing installation requires three system windows')
    # The installed kernel mailbox binds Data UUID and geometry to this board.
    at = 24 + 2 * 128
    if old[at:at + 11] != b'KernelSeed\0':
        raise ValueError('Existing system manifest lacks the KernelSeed identity')
    old_offset, old_size = struct.unpack_from('<QQ', old, at + 32)
    if old_size < 1 << 20 or old_offset < 4096 or old_offset + old_size > data_first:
        raise ValueError('Existing KernelSeed overlaps Data or has invalid length')
    mailbox = android.read(old_offset + old_size - (1 << 20), 4096)
    uuid = mailbox[64:101].split(b'\0')[0].decode()
    if (mailbox[:8] != b'ROOTLOG1' or struct.unpack_from('<Q', mailbox, 56)[0] != logical
            or uuid.upper() != profile['data_volume_uuid'].upper()):
        raise ValueError('Existing Data installation identity differs from the target profile')
    if candidate['data_uuid'].upper() != uuid.upper() or candidate['data_logical_bytes'] != logical:
        raise ValueError('Candidate Data identity differs from the existing installation')
    end = 4096
    for image in images:
        source = Path(image['source'])
        first, length = image['offset'], image['raw_bytes']
        if (first < end or first % 4096 or length <= 0 or length % 4096
                or first + length + (4 << 20) > data_first):
            raise ValueError('Target system window violates the preserved Data boundary: ' + image['name'])
        if source.stat().st_size != length or digest(source) != image['sha256']:
            raise ValueError('Target system window differs from its build: ' + image['name'])
        end = first + length
    data_header = android.read(data_first, 4096)
    if len(data_header) != 4096 or data_header[32:36] != b'NXSB':
        raise ValueError('Preserved Data requires its APFS container superblock')
    report = {'capacity': capacity, 'data_offset': data_first, 'data_logical_bytes': logical,
              'data_uuid': uuid, 'data_superblock_sha256': hashlib.sha256(data_header).hexdigest(),
              'current_manifest_sha256': hashlib.sha256(old).hexdigest(), 'system_bytes': end,
              'device_slot': android.root('getprop ro.boot.slot_suffix')}
    return report, old, data_header


def install(android, folder, candidate, images):
    report, previous, data_header = inspect_installation(android, candidate, images)
    folder.mkdir(parents=True, exist_ok=True)
    before = folder / 'root_manifest.before.bin'
    if before.exists() and before.read_bytes() != previous:
        raise ValueError('Install record belongs to a different installed system manifest')
    before.write_bytes(previous)
    (folder / 'Data-superblock.before.bin').write_bytes(data_header)
    save(folder / 'preflight.json', report)
    assets = Path(candidate['image']).parent
    header = bytearray((assets / 'root_manifest.bin').read_bytes())
    if len(header) != 4096 or hashlib.sha256(header[:4064]).digest() != header[4064:]:
        raise ValueError('Candidate system manifest has an invalid digest')
    # Preserve the board's persisted RTC calibration inside the manifest.
    signature, value, inverse = struct.unpack_from('<3Q', previous, 1024)
    if signature == 0x314c41435452424d and inverse == (~value & ((1 << 64) - 1)):
        header[1024:1048] = previous[1024:1048]
        header[4064:] = hashlib.sha256(header[:4064]).digest()
    manifest = folder / 'root_manifest.target.bin'
    manifest.write_bytes(header)
    done_file = folder / 'system-writes.json'
    done = json.loads(done_file.read_text()) if done_file.exists() else {}
    for image in images:
        offset, size = image['offset'], image['raw_bytes']
        if done.get(image['name']) == image['sha256']:
            actual = android.root(f'dd if=/dev/block/by-name/unixnu bs=4194304 skip={offset} '
                                  f'count={size} iflag=skip_bytes,count_bytes status=none | sha256sum').split()[0]
            if actual == image['sha256']:
                continue
        remote = '/data/local/tmp/unixnu-ios27-' + image['sha256'][:16] + '.bin'
        subprocess.run([*android.command, 'push', str(image['source']), remote], check=True)
        try:
            actual = android.root('sha256sum ' + shlex.quote(remote)).split()[0]
            if actual != image['sha256']:
                raise ValueError('Android staging file differs: ' + image['name'])
            android.root(f'dd if={shlex.quote(remote)} of=/dev/block/by-name/unixnu bs=4194304 '
                         f'seek={offset} count={size} iflag=count_bytes oflag=seek_bytes conv=notrunc,fsync')
            actual = android.root(f'dd if=/dev/block/by-name/unixnu bs=4194304 skip={offset} '
                                  f'count={size} iflag=skip_bytes,count_bytes status=none | sha256sum').split()[0]
            if actual != image['sha256']:
                raise ValueError('Installed system window differs: ' + image['name'])
            done[image['name']] = image['sha256']
            save(done_file, done)
        finally:
            android.root('rm -f ' + shlex.quote(remote))
    remote = '/data/local/tmp/unixnu-ios27-manifest-' + digest(manifest)[:16] + '.bin'
    subprocess.run([*android.command, 'push', str(manifest), remote], check=True)
    try:
        android.root('dd if=' + shlex.quote(remote) + ' of=/dev/block/by-name/unixnu bs=4096 count=1 conv=notrunc,fsync')
    finally:
        android.root('rm -f ' + shlex.quote(remote))
    if android.read(0, 4096) != header or android.read(report['data_offset'], 4096) != data_header:
        raise ValueError('Installed manifest or preserved Data superblock differs')
    report.update(target='24B119', candidate_sha256=digest(assets / 'candidate.json'),
                  installed_manifest_sha256=digest(manifest), installed_windows=done,
                  timestamp=datetime.now(timezone(timedelta(hours=8))).isoformat(), status='systems-installed')
    save(folder / 'installation.json', report)
    record_stage('systems_installation', 'written-and-read', report=str(folder / 'installation.json'),
                 data_uuid=report['data_uuid'], data_superblock_sha256=report['data_superblock_sha256'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('inspect', 'systems', 'boot'))
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--record', type=Path, required=True)
    args = parser.parse_args()
    candidate = json.loads(args.candidate.read_text())
    if candidate.get('target') != 'ios-27.2-24B119':
        raise ValueError('Select the recorded iOS 27.2 / 24B119 candidate')
    image = Path(candidate['image'])
    if digest(image) != candidate['image_sha256']:
        raise ValueError('Temporary boot image differs from its candidate record')
    if candidate['enabled_cpu_affinities'] != list(range(0, 0x800, 0x100)):
        raise ValueError('Candidate requires eight enabled CPU affinities')
    images = json.loads((image.parent / 'storage_images.json').read_text())
    if args.action == 'boot':
        devices = subprocess.check_output(['fastboot', 'devices'], text=True)
        if args.serial not in [line.split()[0] for line in devices.splitlines() if line.split()]:
            raise ValueError('Select a connected fastboot device')
        product = subprocess.run(['fastboot', '-s', args.serial, 'getvar', 'product'], capture_output=True, text=True, check=True)
        if not re.search(r'product:\s*marble\b', product.stdout + product.stderr):
            raise ValueError('Fastboot device must identify as marble')
        installed = json.loads((args.record / 'installation.json').read_text())
        if installed.get('candidate_sha256') != digest(args.candidate):
            raise ValueError('Install the system windows recorded by this candidate before boot')
        record_stage('device_boot', 'candidate-selected', candidate=str(args.candidate),
                     image=str(image), image_sha256=digest(image), serial=args.serial)
        subprocess.run(['fastboot', '-s', args.serial, 'boot', str(image)], check=True)
        record_stage('device_boot', 'boot-command-sent', candidate=str(args.candidate), serial=args.serial)
        return
    android = Android(args.serial)
    if args.action == 'inspect':
        report, _, _ = inspect_installation(android, candidate, images)
        save(args.record / 'preflight.json', report)
        print(json.dumps(report, indent=2))
    else:
        install(android, args.record, candidate, images)


if __name__ == '__main__':
    main()
