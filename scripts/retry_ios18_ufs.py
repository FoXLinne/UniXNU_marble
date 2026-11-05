#!/usr/bin/env python3
"""Load a validated UFS kernel window and retry within the resident firmware."""
import argparse
import hashlib
import json
from pathlib import Path

from build_ios18_arm64e import validate_prelink
from root_control import RootUsb, CHUNK


def retry(assets):
    checkpoint = json.loads((assets / 'checkpoint.json').read_text())
    baseline = validate_prelink(Path(checkpoint['source_kernel']).parent)
    kernel = (assets / 'kernel.macho').read_bytes()
    if (checkpoint['native_sha256'] != baseline['inputs']['native_sha256'] or
            checkpoint['source_sha256'] != baseline['candidate_sha256'] or
            checkpoint['kernel_sha256'] != hashlib.sha256(kernel).hexdigest()):
        raise ValueError('UFS replacement differs from the compiled kernel checkpoint')
    entries = json.loads((assets / 'storage_images.json').read_text())
    entry = next(item for item in entries if item['name'] == 'KernelSeed')
    data = Path(entry['source']).read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != entry['raw_bytes'] or digest != entry['sha256'] or not data.startswith(kernel):
        raise ValueError('UFS kernel window differs from the packed manifest')
    channel = RootUsb(timeout_ms=15000)
    try:
        previous = int(channel.command('getvar:returns'), 16)
        if not previous:
            raise RuntimeError('Resident kernel return is required before replacement')
        channel.command(f'oem begin:2:{len(data):016x}:{digest[:32]}')
        for offset in range(0, len(data), CHUNK):
            channel.write(data[offset:offset + CHUNK])
        received, size = (int(value, 16) for value in channel.command('getvar:received').split(':'))
        if received != len(data) or size != len(data):
            raise IOError('UFS kernel window transfer is incomplete')
        report = dict(previous_returns=previous, expected_returns=previous+1, bytes=received, sha256=digest)
        (assets / 'resident-transfer.json').write_text(json.dumps(report, indent=2) + '\n')
        channel.command('oem retry')
        print(json.dumps(report), flush=True)
    finally:
        channel.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', required=True, type=Path)
    args = parser.parse_args()
    retry(args.assets.resolve())
