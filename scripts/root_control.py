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
OUTPUT_BYTES = CHUNK
LOG_MAGIC = b'ROOTLOG1'


class RootUsb:
    def __init__(self):
        library = Path('/opt/homebrew/lib/libusb-1.0.dylib')
        backend = usb.backend.libusb1.get_backend(find_library=lambda _: str(library))
        self.device = usb.core.find(idVendor=0x18d1, idProduct=0xd00d, backend=backend)
        if self.device is None:
            raise RuntimeError('Marble RootStage USB is unavailable')
        self.device.default_timeout = 120000
        if self.command('getvar:version') != 'RootStage2':
            raise RuntimeError('Unexpected RootStage USB protocol')

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
        for entity in attached['system-entities']:
            candidate = entity.get('dev-entry')
            if not candidate:
                continue
            info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', candidate]))
            if info['TotalSize'] == image['raw_bytes']:
                disk = candidate
                break
        else:
            raise ValueError(f'Image size differs from manifest: {image["name"]}')
        with open(disk.replace('/dev/disk', '/dev/rdisk'), 'rb', buffering=0) as stream:
            yield stream
    finally:
        subprocess.run(['hdiutil', 'detach', disk], check=True, capture_output=True)


def load(channel, images, kernel=None, observe_seconds=0, data_seed=None, boot_args="", adsp_firmware=None):
    if channel.command('getvar:version') != 'RootStage2':
        raise RuntimeError('Load the RootStage2 boot image to use resident disks')
    transfer_bytes = int(channel.command('getvar:max-download-size'), 16)
    zero_fill = 'zero-fill' in channel.command('getvar:features').split(',')
    if not 1 <= len(images) <= 2:
        raise ValueError('RootStage accepts one or two root disks')
    entries = list(enumerate(images))
    if kernel:
        data = kernel.read_bytes()
        source_digest = hashlib.sha256(data).hexdigest()
        seed = firmware = b''
        seed_offset, seed_bytes, seed_raw_bytes = 0, 0, 0
        seed_uuid = bytes(37)
        if data_seed:
            seed = data_seed.read_bytes()
            if len(seed) < 32 or seed[:8] != b'DBRDISK1':
                raise ValueError('Data seed must use the DBRDISK1 packed format')
            metadata = json.loads(data_seed.with_name('seed.json').read_text())
            if metadata['sha256'] != hashlib.sha256(seed).hexdigest():
                raise ValueError('Data seed checksum differs from seed.json')
            seed_uuid = str(uuid.UUID(metadata['volume_uuid'])).upper().encode() + b'\0'
            seed_raw_bytes = len(seed)
            seed = zlib.compress(seed, level=9)
            seed_bytes = len(seed)
        firmware_offset = firmware_bytes = firmware_raw_bytes = firmware_segments = 0
        if adsp_firmware:
            firmware = adsp_firmware.read_bytes()
            firmware_raw_bytes = len(firmware)
            firmware = zlib.compress(firmware, level=9)
            firmware_bytes = len(firmware)
        data += bytes(-len(data) % 4096)
        seed_offset, seed_segments = (len(data), 1) if seed else (0, 0)
        data += seed
        data += bytes(-len(data) % 4096)
        firmware_offset, firmware_segments = (len(data), 1) if firmware else (0, 0)
        data += firmware
        data += bytes(-len(data) % 4096)
        extra_args = boot_args.encode()
        if len(extra_args) >= 1024 or b'\0' in extra_args:
            raise ValueError('Extra boot arguments exceed the command line limit')
        data += (struct.pack('<8sII', LOG_MAGIC, 1, 0) + os.urandom(16) +
                 struct.pack('<I4xQQQ', observe_seconds, seed_offset, seed_bytes, seed_raw_bytes) + seed_uuid + bytes(3) +
                 struct.pack('<QQQ', firmware_offset, firmware_bytes, firmware_raw_bytes) +
                 extra_args + bytes(OUTPUT_BYTES - 128 - len(extra_args)))
        digest = hashlib.sha256(data).hexdigest()
        entries.append((2, dict(name='kernel', source=str(kernel), format='raw',
                                raw_bytes=len(data), sha256=digest, payload=data,
                                source_sha256=source_digest, firmware_segments=firmware_segments,
                                data_segments=seed_segments)))
    reports = []
    for index, image in entries:
        chunk_bytes = CHUNK if index == 2 else transfer_bytes
        began = time.monotonic()
        digest = image['sha256']
        total = image['raw_bytes']
        channel.command(f'oem select:{index}')
        identity = channel.command('getvar:image-id')
        received, resident_bytes = [int(v, 16) for v in channel.command('getvar:received').split(':')]
        offset, sent, zero_bytes = 0, 0, 0
        if identity == digest[:32] and received == resident_bytes == total:
            offset = total
        else:
            # Source bytes are required only for a new or incomplete resident disk.
            with image_stream(image) as stream:
                if image.get('format') == 'raw':
                    stream.seek(0, 2)
                    if stream.tell() != total:
                        raise ValueError(f'Image size differs from manifest: {image["name"]}')
                channel.command(f'oem begin:{index}:{total:016x}:{digest[:32]}')
                offset, resident_bytes = [int(v, 16) for v in channel.command('getvar:received').split(':')]
                if resident_bytes != total or not 0 <= offset <= total:
                    raise RuntimeError('Resident disk geometry differs from image')
                stream.seek(offset)
                while offset + sent < total:
                    data = stream.read(min(chunk_bytes, total - offset - sent))
                    if not data:
                        raise RuntimeError(f'Short read: {image["name"]}')
                    if zero_fill and data.count(0) == len(data):
                        channel.command(f'oem zero:{len(data):08x}')
                        zero_bytes += len(data)
                    else:
                        channel.write(data)
                    sent += len(data)
                    if sent % (192 * CHUNK) == 0 or offset + sent == total:
                        print(f'{image["name"]}: {offset + sent}/{total}', flush=True)
        report = dict(name=image['name'], bytes=total, sha256=digest,
                      reused_bytes=offset, transmitted_bytes=sent - zero_bytes,
                      zero_filled_bytes=zero_bytes,
                      elapsed_seconds=time.monotonic() - began)
        if 'source_sha256' in image:
            report['source_sha256'] = image['source_sha256']
            report['firmware_segments'] = image['firmware_segments']
            report['data_segments'] = image['data_segments']
        reports.append(report)
    channel.command(f'oem disks:{len(images)}')
    return reports


def export_data(channel, output):
    channel.command('oem select:2')
    mailbox = channel.read_back()
    magic, version, page, logical, _, index_bytes, volume = struct.unpack_from('<8sIIQQQ37s',mailbox,768*1024)
    if magic != b'ROOTDAT1' or version != 1 or page != 4096 or index_bytes != logical // page * 8:
        raise RuntimeError('Resident kernel has no frozen Data snapshot')
    index = bytearray()
    for offset in range(0,index_bytes,CHUNK):
        count=min(CHUNK,index_bytes-offset)
        channel.command(f'oem data-map:{offset:08x}:{count:08x}')
        index.extend(channel.read_back())
    if len(index) != index_bytes:
        raise RuntimeError('Short Data page index')
    pages=struct.unpack('<'+'Q'*(index_bytes//8),index)
    sent=0
    with output.open('xb') as stream:
        stream.truncate(logical)
        start=0
        while start<len(pages):
            if not pages[start]:
                start+=1
                continue
            end=start+1
            while end<len(pages) and pages[end] and (end-start)*page<CHUNK:
                end+=1
            offset,count=start*page,(end-start)*page
            channel.command(f'oem data-read:{offset:016x}:{count:08x}')
            data=channel.read_back()
            if len(data)!=count:
                raise RuntimeError('Short Data snapshot read')
            stream.seek(offset);stream.write(data)
            sent+=count;start=end
    with output.open('rb') as stream:
        stream.seek(32)
        if stream.read(4)!=b'NXSB':
            raise RuntimeError('Data snapshot has no APFS container header')
    return dict(output=str(output),logical_bytes=logical,transmitted_bytes=sent,
                volume_uuid=volume.rstrip(b'\0').decode(),captured_at_panic=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['status', 'cpu', 'load', 'start', 'logs', 'screen', 'trace', 'data', 'continue', 'selftest', 'reboot'])
    parser.add_argument('--images', type=Path)
    parser.add_argument('--report', type=Path)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--boot-args', default='')
    parser.add_argument('--data-seed', type=Path)
    parser.add_argument('--observe-seconds', type=int, default=0)
    parser.add_argument('--keep-usb', action='store_true', help='Keep the loader PHY powered for native runtime USB takeover')
    args = parser.parse_args()
    channel = RootUsb()
    try:
        if args.command in ('load', 'start'):
            if args.images is None:
                parser.error('--images is required')
            images = json.loads(args.images.read_text())
            if args.keep_usb:
                channel.command('oem runtime-usb:1')
            result = load(channel, images, args.kernel, args.observe_seconds, args.data_seed, args.boot_args)
            if args.command == 'start':
                channel.command('continue')
        elif args.command == 'cpu':
            isar1, isar2, level = [int(value, 16) for value in channel.command('getvar:cpu-features').split(':')]
            keys, authenticated = [int(value, 16) for value in channel.command('getvar:pac-el1').split(':')]
            result = dict(scope='boot-cpu-el1', current_el=level >> 2,
                          isar1=hex(isar1), isar2=hex(isar2),
                          key_register_mask=keys, authentication_mask=authenticated,
                          passed=level == 4 and keys == authenticated == 31)
        elif args.command == 'status':
            result = {name: channel.command('getvar:' + name) for name in ('version', 'kernel-memory', 'received', 'returns', 'handoff-fault')}
        elif args.command == 'data':
            if args.output is None:
                parser.error('data requires --output')
            result=export_data(channel,args.output)
        elif args.command == 'trace':
            if args.output is None:
                parser.error('trace requires --output')
            channel.command('oem select:2')
            data = channel.read_back()
            offset = 800 * 1024
            magic, version, length = struct.unpack_from('<8sII', data, offset)
            if magic != b'ROOTTRC1' or version != 1 or not 0 < length <= OUTPUT_BYTES - offset - 16:
                raise RuntimeError('Process trace has not been captured')
            args.output.write_bytes(data[offset + 16:offset + 16 + length])
            result = dict(output=str(args.output), bytes=length)
        elif args.command == 'screen':
            if args.output is None:
                parser.error('screen requires --output')
            channel.command('oem select:2')
            data = channel.read_back()
            magic, width, height, stride, size, frames = struct.unpack_from('<8sIIIIQ', data, LOG_BYTES)
            if magic != b'ROOTRGB1' or size != stride * height:
                raise RuntimeError('Framebuffer snapshot has not been captured')
            pixels = data[LOG_BYTES + 32:LOG_BYTES + 32 + size]
            def chunk(kind, payload):
                return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
            scanlines = b''.join(b'\0' + pixels[y * stride:(y + 1) * stride] for y in range(height))
            png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                   chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b''))
            args.output.write_bytes(png)
            result = dict(output=str(args.output), width=width, height=height, completed_copies=frames)
        elif args.command == 'logs':
            if args.output is None:
                parser.error('logs requires --output')
            channel.command('oem select:2')
            data = channel.read_back()
            if len(data) < LOG_BYTES or data[:8] != LOG_MAGIC:
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
        if args.command == 'cpu' and not result['passed']:
            raise SystemExit(1)
    finally:
        channel.close()


if __name__ == '__main__':
    main()
