#!/usr/bin/env python3
"""Transfer read-only system disks into the marble UEFI staging area."""
import argparse
from contextlib import contextmanager
import hashlib
import io
import json
import os
from pathlib import Path
import plistlib
import subprocess
import struct
import time
import zlib
import uuid

import usb.backend.libusb1
import usb.core
import usb.util

CHUNK = 1024**2
LOG_BYTES = 65536
LOG_MAGIC = b'ROOTLOG1'


class RootUsb:
    def __init__(self):
        library = Path('/opt/homebrew/lib/libusb-1.0.dylib')
        backend = usb.backend.libusb1.get_backend(find_library=lambda _: str(library))
        self.device = usb.core.find(idVendor=0x18d1, idProduct=0xd00d, backend=backend)
        if self.device is None or self.device.serial_number != 'marble':
            raise RuntimeError('Marble RootStage USB is unavailable')
        self.device.default_timeout = 120000

    def close(self):
        usb.util.dispose_resources(self.device)

    def reply(self):
        value = self.device.read(0x81, 64).tobytes()
        if value[:4] == b'FAIL':
            raise RuntimeError(value[4:].decode())
        return value

    def command(self, text):
        self.device.write(0x01, text.encode())
        value = self.reply()
        if value[:4] != b'OKAY':
            raise RuntimeError(f'Unexpected command response: {value!r}')
        return value[4:].decode()

    def write(self, data):
        self.device.write(0x01, f'download:{len(data):08x}'.encode())
        response = self.reply()
        if response != f'DATA{len(data):08x}'.encode():
            raise RuntimeError(f'Unexpected download response: {response!r}')
        count = self.device.write(0x01, data)
        if count != len(data) or self.reply() != b'OKAY':
            raise RuntimeError('Incomplete USB download')

    def read_back(self):
        self.device.write(0x01, b'upload')
        response = self.reply()
        if response[:4] != b'DATA':
            raise RuntimeError(f'Unexpected upload response: {response!r}')
        size = int(response[4:], 16)
        result = bytearray()
        while len(result) < size:
            result.extend(self.device.read(0x81, size - len(result)))
        if self.reply() != b'OKAY':
            raise RuntimeError('Incomplete USB upload')
        return bytes(result)


@contextmanager
def image_stream(image):
    if 'payload' in image:
        with io.BytesIO(image['payload']) as stream:
            yield stream
        return
    if image.get('format') == 'raw':
        with open(image['source'], 'rb', buffering=0) as stream:
            yield stream
        return
    attached = plistlib.loads(subprocess.check_output(
        ['hdiutil', 'attach', '-readonly', '-nomount', '-plist', image['source']]))
    disk = next(e['dev-entry'] for e in attached['system-entities'] if e.get('dev-entry'))
    try:
        info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', disk]))
        if info['TotalSize'] != image['raw_bytes']:
            raise ValueError(f'Image size differs from manifest: {image["name"]}')
        with open(disk.replace('/dev/disk', '/dev/rdisk'), 'rb', buffering=0) as stream:
            yield stream
    finally:
        subprocess.run(['hdiutil', 'detach', disk], check=True, capture_output=True)


def load(channel, images, kernel=None, observe_seconds=0, data_seed=None, boot_args=""):
    if channel.command('getvar:version') != 'RootStage2':
        raise RuntimeError('Load the RootStage2 boot image to use resident disks')
    if not 1 <= len(images) <= 2:
        raise ValueError('RootStage accepts one or two root disks')
    entries = list(enumerate(images))
    if kernel:
        data = kernel.read_bytes()
        source_digest = hashlib.sha256(data).hexdigest()
        # The raw Data seed and output mailbox follow the Mach-O segments.
        data += bytes(-len(data) % CHUNK)
        seed_offset, seed_bytes, seed_raw_bytes = 0, 0, 0
        seed_uuid = bytes(37)
        if data_seed:
            seed = data_seed.read_bytes()
            if len(seed) % 4096 or seed[32:36] != b'NXSB':
                raise ValueError('Data seed must be a raw APFS container')
            metadata = json.loads(data_seed.with_name('seed.json').read_text())
            if metadata['sha256'] != hashlib.sha256(seed).hexdigest():
                raise ValueError('Data seed checksum differs from seed.json')
            seed_uuid = str(uuid.UUID(metadata['volume_uuid'])).upper().encode() + b'\0'
            seed_raw_bytes = len(seed)
            seed = zlib.compress(seed, level=1)
            seed_offset, seed_bytes = len(data), len(seed)
            data += seed
            data += bytes(-len(data) % CHUNK)
        extra_args = boot_args.encode()
        if len(extra_args) >= 256 or b'\0' in extra_args:
            raise ValueError('Extra boot arguments exceed the command line limit')
        data += (struct.pack('<8sII', LOG_MAGIC, 1, 0) + os.urandom(16) +
                 struct.pack('<I4xQQQ', observe_seconds, seed_offset, seed_bytes, seed_raw_bytes) + seed_uuid + bytes(27) +
                 extra_args + bytes(LOG_BYTES - 128 - len(extra_args)))
        if len(data) > 64 * CHUNK:
            raise ValueError('RootStage kernel update exceeds the 64 MiB slot')
        digest = hashlib.sha256(data).hexdigest()
        entries.append((2, dict(name='kernel', source=str(kernel), format='raw',
                                raw_bytes=len(data), sha256=digest, payload=data,
                                source_sha256=source_digest)))
    reports = []
    for index, image in entries:
        began = time.monotonic()
        with image_stream(image) as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            if digest != image['sha256']:
                raise ValueError(f'Image checksum differs from manifest: {image["name"]}')
            if image.get('format') == 'raw':
                stream.seek(0, 2)
                if stream.tell() != image['raw_bytes']:
                    raise ValueError(f'Image size differs from manifest: {image["name"]}')
            channel.command(f'oem begin:{index}:{image["raw_bytes"]:016x}:{digest[:32]}')
            offset, total = [int(v, 16) for v in channel.command('getvar:received').split(':')]
            if total != image['raw_bytes'] or not 0 <= offset <= total:
                raise RuntimeError('Resident disk geometry differs from image')
            stream.seek(offset)
            sent = 0
            while offset + sent < total:
                data = stream.read(min(CHUNK, total - offset - sent))
                if not data:
                    raise RuntimeError(f'Short read: {image["name"]}')
                channel.write(data)
                sent += len(data)
                if sent % (128 * CHUNK) == 0 or offset + sent == total:
                    print(f'{image["name"]}: {offset + sent}/{total}', flush=True)
        report = dict(name=image['name'], bytes=total, sha256=digest,
                      reused_bytes=offset, transmitted_bytes=sent,
                      elapsed_seconds=time.monotonic() - began)
        if 'source_sha256' in image:
            report['source_sha256'] = image['source_sha256']
        reports.append(report)
    channel.command(f'oem disks:{len(images)}')
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['status', 'load', 'start', 'logs', 'continue', 'selftest', 'reboot'])
    parser.add_argument('--images', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--boot-args', default='')
    parser.add_argument('--data-seed', type=Path)
    parser.add_argument('--observe-seconds', type=int, default=0)
    args = parser.parse_args()
    channel = RootUsb()
    try:
        if args.command in ('load', 'start'):
            if args.images is None:
                parser.error('--images is required')
            images = json.loads(args.images.read_text())
            result = load(channel, images, args.kernel, args.observe_seconds, args.data_seed, args.boot_args)
            if args.command == 'start':
                channel.command('continue')
        elif args.command == 'status':
            result = {name: channel.command('getvar:' + name) for name in ('version', 'kernel-memory', 'received', 'returns', 'handoff-fault')}
        elif args.command == 'logs':
            if args.output is None:
                parser.error('logs requires --output')
            channel.command('oem select:2')
            data = channel.read_back()
            if len(data) != LOG_BYTES or data[:8] != LOG_MAGIC:
                raise RuntimeError('Resident kernel has no log mailbox')
            version, length = struct.unpack_from('<II', data, 8)
            if version != 1 or not 0 < length <= LOG_BYTES - 16:
                raise RuntimeError('Kernel log has not been captured')
            args.output.write_bytes(data[16:16 + length])
            result = dict(output=str(args.output), bytes=length)
        else:
            result = channel.command(args.command)
        if args.report:
            args.report.write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
    finally:
        channel.close()


if __name__ == '__main__':
    main()
