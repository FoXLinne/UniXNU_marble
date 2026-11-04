#!/usr/bin/env python3
"""Capture the retained kernel before loading its replacement in the same firmware session."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

from build_ios18_arm64e import validate_prelink
from capture_ios18_resident import capture
from root_control import CHUNK, RootUsb


def retry(previous, replacement, output):
    checkpoint = json.loads(replacement.with_name('checkpoint.json').read_text())
    baseline = validate_prelink(Path(checkpoint['source_kernel']).parent)
    data = replacement.read_bytes()
    if (checkpoint['native_sha256'] != baseline['inputs']['native_sha256'] or
            checkpoint['source_sha256'] != baseline['candidate_sha256'] or
            checkpoint['kernel_sha256'] != hashlib.sha256(data).hexdigest()):
        raise RuntimeError('Replacement differs from the current compiled kernel checkpoint')
    channel = RootUsb()
    try:
        capture(channel, previous, output / 'before')
        if len(data) > 192 * CHUNK:
            raise ValueError('Replacement exceeds the firmware kernel slot')
        identity = hashlib.sha256(data).hexdigest()[:32]
        channel.command(f'oem begin:2:{len(data):016x}:{identity}')
        for offset in range(0, len(data), CHUNK):
            channel.write(data[offset:offset + CHUNK])
        received, size = (int(v, 16) for v in channel.command('getvar:received').split(':'))
        if received != len(data) or size != len(data):
            raise IOError('Replacement transfer is incomplete')
        channel.command('oem retry')
    finally:
        channel.close()
    time.sleep(5)
    fastboot = '/opt/homebrew/share/android-commandlinetools/platform-tools/fastboot'
    completed = subprocess.run([fastboot, '-s', 'marble', 'getvar', 'returns'],
                               capture_output=True, text=True, timeout=180)
    (output / 'usb-return.log').write_text(completed.stdout + completed.stderr)
    completed.check_returncode()
    channel = RootUsb()
    try:
        return capture(channel, replacement, output / 'after')
    finally:
        channel.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--previous-kernel', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(retry(args.previous_kernel.resolve(), args.kernel.resolve(), args.output.resolve()), indent=2))
