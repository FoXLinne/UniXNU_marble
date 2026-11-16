#!/usr/bin/env python3
"""Collect one marble boot's USB logs, display and panic evidence with source identity."""
import argparse
from bisect import bisect_right
from contextlib import contextmanager
from datetime import datetime, timezone, timedelta
import fcntl
import hashlib
import http.client
import json
import plistlib
import re
from pathlib import Path
import shutil
import signal
import struct
import socket
import subprocess
import sys
import tempfile
import threading
import time

from capture_ios18_resident import RetainedKernel, capture_processes, capture_init, png_chunk
from root_control import RootUsb

DEVICE = Path(__file__).resolve().parents[1]
WORKSPACE = DEVICE.parents[1]
DEFAULT_ROOT = WORKSPACE / 'outputs/UniXNU_marble/ios27_24B119'


class RetainedKernel27(RetainedKernel):
    def __init__(self, channel):
        super().__init__(channel)
        self.ranges = [(self.base, self.size)]
        if 'retained-ranges' in channel.command('getvar:features').split(','):
            channel.command('getvar:retained-ranges')
            raw = channel.read_back()
            magic, version, count = struct.unpack_from('<8sII', raw)
            if magic != b'ROOTMEM1' or version != 1 or not 1 <= count <= 256 or len(raw) != 16 + count * 16:
                raise ValueError('Retained DRAM table header or length is invalid')
            self.ranges = [struct.unpack_from('<QQ', raw, 16 + 16 * i) for i in range(count)]

    def physical(self, address, count):
        if count < 1 or not any(address >= base and count <= size and address - base <= size - count
                                for base, size in self.ranges):
            raise ValueError(f'Read outside declared retained DRAM: {address:#x}+{count:#x}')
        self.channel.command(f'oem memory-read:{address:016x}:{count:08x}')
        data = self.channel.read_back()
        if len(data) != count:
            raise IOError(f'Short retained DRAM read: {len(data)} of {count}')
        return data


def file_hash(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def symbols_for(path, tools):
    sys.path.insert(0, str(tools))
    from kc_image import Image
    collection = Image(path.read_bytes())
    offset = collection.entries['com.apple.kernel'][1] if collection.entries else 0
    kernel = Image(collection.data, offset)
    symbols = {name: address for address, names in kernel.symbols().items() for name in names}
    for identifier, (_, offset) in collection.entries.items():
        if identifier == 'com.apple.kernel':
            continue
        member = Image(collection.data, offset)
        for address in member.function_starts():
            symbols[f'{identifier}+{address - member.base:#x}'] = address
    return kernel, symbols


def panic_frames(text, symbols):
    starts = sorted(set(symbols.values()))
    names = {}
    for name, address in symbols.items():
        names.setdefault(address, name)
    result = []
    for label, value in re.findall(r'\b(LR|BT|PC)=(?:0x)?([0-9a-fA-F]{8,16})', text):
        address = int(value, 16)
        # Kernel PAC bits can survive in the saved LR. Kernel text occupies
        # the canonical 38-bit address range used by this build.
        canonical = (address & ((1 << 38) - 1)) | (((1 << 64) - 1) ^ ((1 << 38) - 1))
        # LR and backtrace entries identify the instruction following a call.
        # A noreturn call can be the last instruction before the next function.
        lookup = canonical - 4 if label in ('LR', 'BT') else canonical
        index = bisect_right(starts, lookup) - 1
        row = {'kind': label, 'raw_address': hex(address), 'address': hex(canonical)}
        if index >= 0:
            base = starts[index]
            row.update(symbol=names[base], offset=canonical - base)
        result.append(row)
    return result


def capture_native_log(port, destination):
    result = {}
    for route, name in (('/status', 'kernel-status.json'), ('/kernel.log', 'kernel.log')):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        try:
            connection.request('GET', route)
            response = connection.getresponse()
            data = response.read((16 << 20) + 1)
            if len(data) > 16 << 20:
                raise ValueError('USB debug response exceeds the diagnostic bound')
            result[route] = response.status
            if response.status == 200:
                (destination / name).write_bytes(data)
                if route == '/status':
                    result['status'] = json.loads(data)
                    result['eight_cpus_observed'] = result['status'].get('cpus') == 8
                    result['observed_memsize_bytes'] = result['status'].get('memsize')
        finally:
            connection.close()
    return result


def export_ring(memory, symbols, destination):
    pointer = int.from_bytes(memory.read(symbols['_msgbufp'], 8), 'little')
    _, size, cursor, _, address = struct.unpack('<4IQ', memory.read(pointer, 24))
    if not 0 < size <= 16 << 20 or cursor >= size:
        raise ValueError('Kernel message-ring size or cursor is invalid')
    data = memory.read(address, size)
    (destination / 'kernel.log').write_bytes((data[cursor:] + data[:cursor]).replace(b'\0', b''))


def screenshot(memory, symbols, destination):
    required = ('_marble_display_geometry', '_marble_display_pixels')
    if any(name not in symbols for name in required):
        return
    width, height, stride = struct.unpack('<3I', memory.read(symbols[required[0]], 12))
    address = int.from_bytes(memory.read(symbols[required[1]], 8), 'little')
    if not (address and 0 < width <= 4096 and 0 < height <= 4096 and width * 4 <= stride <= width * 8):
        return
    raw = memory.read(address, stride * height)
    rows = []
    for y in range(height):
        row = raw[y * stride:y * stride + width * 4]
        rgb = bytearray(width * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        rows.append(b'\0' + rgb)
    import zlib
    png = b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += png_chunk(b'IDAT', zlib.compress(b''.join(rows))) + png_chunk(b'IEND', b'')
    (destination / 'screen.png').write_bytes(png)


def capture_retained(kernel, tools, layout, destination):
    """Panic return preserves MMU registers through the firmware handoff contract."""
    image, symbols = symbols_for(kernel, tools)
    channel = RootUsb(timeout_ms=5000)
    report = {}
    try:
        for key in ('version', 'phase', 'returns', 'kernel-memory', 'handoff-translation'):
            report[key] = channel.command('getvar:' + key)
        if report['phase'] != 'firmware':
            # Live native Apple USB uses usbmuxd. A retained-memory read waits
            # for the firmware handoff, whose register contract is versioned.
            report['capture_status'] = 'waiting-for-retained-firmware'
            return report
        memory = RetainedKernel27(channel)
        report['retained_dram_ranges'] = [{'base': hex(base), 'bytes': size} for base, size in memory.ranges]
        for name, capacity in (('_native_resident_panic_message', 512),
                               ('_native_root_block_panic_text', 4096),
                               ('_boot_progress_panic_text', 256)):
            if name in symbols:
                text = memory.read(symbols[name], capacity).split(b'\0')[0].decode(errors='replace')
                if text:
                    report.setdefault('panic', {})[name] = text
                    frames = panic_frames(text, symbols)
                    if frames:
                        report.setdefault('panic_frames', {})[name] = frames
        if '_msgbufp' in symbols:
            export_ring(memory, symbols, destination)
        screenshot(memory, symbols, destination)
        report['globals'] = {}
        for name, size in (('_real_ncpus', 4), ('_armcpu_smp_running', 4), ('_marble_display_frames', 4),
                           ('_marble_ufs_reads', 8), ('_marble_ufs_writes', 8), ('_marble_usb_heartbeat', 4)):
            if name in symbols:
                report['globals'][name] = int.from_bytes(memory.read(symbols[name], size), 'little')
        if layout and layout.is_file():
            offsets = json.loads(layout.read_text())
            processes = capture_processes(memory, symbols, offsets)
            (destination / 'processes.json').write_text(json.dumps(processes, indent=2) + '\n')
            report['init'] = capture_init(memory, symbols, offsets, hex, destination)
        report['kernel_base'] = hex(image.base)
        report['capture_status'] = 'retained-memory-captured'
        return report
    finally:
        channel.close()


@contextmanager
def usb_reader_lock():
    path = Path(tempfile.gettempdir()) / 'marble-rootstage-observer.lock'
    with path.open('a') as stream:
        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=DEFAULT_ROOT)
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--layout', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--udid', help='Apple USB device identity when several phones are connected')
    parser.add_argument('--duration', type=float, default=600)
    parser.add_argument('--interval', type=float, default=60)
    parser.add_argument('--once', action='store_true')
    args = parser.parse_args()
    if args.interval < 30 or args.duration <= 0:
        parser.error('interval must be at least 30 seconds; duration must be positive')
    root, output = args.root.resolve(), args.output.resolve()
    kernel = (args.kernel or root / 'kernel.fileset.macho').resolve()
    if not kernel.is_file():
        parser.error('select the fileset kernel of the boot being captured')
    output.mkdir(parents=True, exist_ok=True)
    tools = DEVICE / 'ios27/sources/XnuPrelink'
    identity = {'target': 'ios-27.2-24B119', 'kernel': str(kernel), 'kernel_sha256': file_hash(kernel),
                'started_at': datetime.now(timezone(timedelta(hours=8))).isoformat()}
    for name in ('kernel.fileset.json', 'kernel.layout.json', 'candidate.json'):
        path = root / name
        if path.is_file():
            shutil.copyfile(path, output / name)
            identity[name] = file_hash(path)
    (output / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    stop = threading.Event()
    for signum in (signal.SIGINT, signal.SIGTERM):
        signal.signal(signum, lambda *_: stop.set())
    syslog, log_file = None, None
    proxy, proxy_log, proxy_device, proxy_port = None, None, None, None
    began, sequence = time.monotonic(), 0
    # One process owns the RootStage transactions; usbmuxd owns Apple USB.
    with usb_reader_lock():
        try:
            while not stop.is_set():
                destination = output / f'sample-{sequence:04d}'
                destination.mkdir(exist_ok=True)
                report = {'elapsed_seconds': time.monotonic() - began}
                try:
                    ids = subprocess.run(['idevice_id', '-l'], capture_output=True, text=True, timeout=10) if shutil.which('idevice_id') else None
                except subprocess.TimeoutExpired:
                    ids = None
                    report['apple_usb_status'] = 'device-list-timeout'
                connected = ids.stdout.splitlines() if ids and ids.returncode == 0 else []
                report['apple_usb_devices'] = connected
                if connected:
                    device = args.udid or (connected[0] if len(connected) == 1 else None)
                    if device:
                        if (proxy is None or proxy.poll() is not None or proxy_device != device) and shutil.which('iproxy'):
                            if proxy and proxy.poll() is None:
                                proxy.terminate()
                                proxy.wait(timeout=10)
                            if proxy_log:
                                proxy_log.close()
                            with socket.socket() as available:
                                available.bind(('127.0.0.1', 0))
                                proxy_port = available.getsockname()[1]
                            proxy_log = (output / 'iproxy.log').open('ab')
                            proxy = subprocess.Popen(['iproxy', '-u', device, f'{proxy_port}:23456'],
                                                     stdout=proxy_log, stderr=subprocess.STDOUT)
                            proxy_device = device
                        try:
                            info = subprocess.run(['ideviceinfo', '-u', device, '-x'], capture_output=True, timeout=30)
                            (destination / 'device-info.plist').write_bytes(info.stdout)
                            (destination / 'device-info.stderr').write_bytes(info.stderr)
                            report['device_info_returncode'] = info.returncode
                            if info.returncode == 0:
                                properties = plistlib.loads(info.stdout)
                                report['device_build'] = properties.get('BuildVersion')
                                report['target_build_matches'] = properties.get('BuildVersion') == '24B119'
                        except (subprocess.TimeoutExpired, plistlib.InvalidFileException) as error:
                            report['device_info_status'] = f'{type(error).__name__}: {error}'
                        if proxy and proxy.poll() is None:
                            try:
                                report['native_kernel'] = capture_native_log(proxy_port, destination)
                            except (OSError, ValueError, http.client.HTTPException) as error:
                                report['native_kernel_status'] = f'{type(error).__name__}: {error}'
                        if syslog and syslog.poll() is not None:
                            log_file.close()
                            syslog, log_file = None, None
                        if syslog is None and shutil.which('idevicesyslog'):
                            log_file = (output / 'syslog.log').open('ab')
                            syslog = subprocess.Popen(['idevicesyslog', '-u', device], stdout=log_file,
                                                      stderr=subprocess.STDOUT)
                        if shutil.which('idevicescreenshot'):
                            try:
                                screen = subprocess.run(['idevicescreenshot', '-u', device, str(destination / 'screen.png')],
                                                        capture_output=True, timeout=30)
                                report['screenshot_returncode'] = screen.returncode
                            except subprocess.TimeoutExpired:
                                report['screenshot_status'] = 'timeout'
                try:
                    report['retained'] = capture_retained(kernel, tools, args.layout or root / 'kernel.layout.json', destination)
                except Exception as error:
                    report['retained_status'] = f'{type(error).__name__}: {error}'
                if syslog:
                    report['syslog_returncode'] = syslog.poll()
                (destination / 'state.json').write_text(json.dumps(report, indent=2) + '\n')
                print(json.dumps(report), flush=True)
                sequence += 1
                if args.once or time.monotonic() - began >= args.duration:
                    break
                stop.wait(min(args.interval, max(0, args.duration - (time.monotonic() - began))))
        finally:
            if syslog and syslog.poll() is None:
                syslog.send_signal(signal.SIGINT)
                try:
                    syslog.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    syslog.terminate()
                    syslog.wait(timeout=10)
            if log_file:
                log_file.close()
            if proxy and proxy.poll() is None:
                proxy.terminate()
                proxy.wait(timeout=10)
            if proxy_log:
                proxy_log.close()


if __name__ == '__main__':
    main()
