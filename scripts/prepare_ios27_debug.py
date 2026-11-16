#!/usr/bin/env python3
"""Produce a symbol-qualified marble panic-screen image with retained USB evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib

DEVICE = Path(__file__).resolve().parents[1]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--uefi', type=Path, default=DEVICE / 'firmware/source')
    parser.add_argument('--prelink-report', type=Path, help='24B119 fileset provenance; defaults beside the source kernel')
    parser.add_argument('--shared-region-trace', action='store_true',
                        help='Enable existing shared-region diagnostic output in this copy')
    parser.add_argument('--shared-region-trace-level', type=int, choices=(1, 2, 3), default=3)
    args = parser.parse_args()
    kernel, output = args.kernel.resolve(), args.output.resolve()
    if kernel == output:
        parser.error('source and debug output require distinct paths')
    profile = tomllib.loads((DEVICE / 'profiles/os/ios-27.2-24B119/profile.toml').read_text())
    report = json.loads((args.prelink_report or kernel.with_suffix('.json')).read_text())
    original_cache = report.get('base_kernelcache_sha256') or report.get('inputs', {}).get('kernelcache_sha256')
    if original_cache != profile['kernel']['prelink']['kernelcache_sha256'] or report.get('candidate_sha256') != sha(kernel):
        raise ValueError('Select the fileset kernel recorded against the original 24B119 collection')
    if (not report.get('mapping_complete') or report.get('missing') or report.get('stubbed')
            or report.get('allow_missing') or not report.get('candidate_written')):
        raise ValueError('Select the complete board fileset for panic/USB instrumentation')
    sys.path.insert(0, str(DEVICE / 'ios27/sources/XnuPrelink'))
    from kc_image import Image
    collection = Image(kernel.read_bytes())
    offset = collection.entries['com.apple.kernel'][1] if collection.entries else 0
    image = Image(collection.data, offset)
    symbols = {name: address for address, names in image.symbols().items() for name in names}
    required = ('_start_first_cpu', '_panic', '_vinfo', '_native_resident_panic_return',
                '_native_resident_record_panic', '_native_root_block_panic_text')
    missing = [name for name in required if name not in symbols]
    if missing:
        raise SystemExit('Panic/USB integration requires kernel symbols: ' + ', '.join(missing))
    output.parent.mkdir(parents=True, exist_ok=True)
    original = sha(kernel)
    shutil.copyfile(kernel, output)
    if args.shared_region_trace:
        address = symbols['_shared_region_trace_level']
        location = image.offset(address)
        raw = bytearray(output.read_bytes())
        previous = struct.unpack_from('<I', raw, location)[0]
        if previous not in (0, 1, 2, 3):
            raise ValueError('Unexpected shared-region trace level')
        struct.pack_into('<I', raw, location, args.shared_region_trace_level)
        output.write_bytes(raw)
    env = dict(os.environ, UNIXNU_UEFI=str(args.uefi.resolve()), UNIXNU_XNU_ARCH='arm64e',
               UNIXNU_PANIC_BUILD=f'marble iOS 27.2 24B119 kernel {original[:16]}',
               PATH=os.pathsep.join(['/opt/homebrew/opt/llvm/bin', '/opt/homebrew/opt/lld/bin', os.environ['PATH']]))
    subprocess.run([sys.executable, str(DEVICE / 'scripts/patch-xnu-entry-probe.py'), str(output),
                    '--panic-screen', '--no-reset', '--panic-only'], env=env, check=True)
    # The source crypto POST hashes executable bytes; seal this instrumented copy.
    subprocess.run([sys.executable, str(DEVICE / 'ios27/source/scripts/seal-source-crypto-kernel.py'),
                    str(output)], env=env, check=True)
    record = {'target': 'ios-27.2-24B119', 'source_kernel': str(kernel), 'source_sha256': original,
              'debug_kernel': str(output), 'debug_sha256': sha(output), 'automatic_reset': False,
              'panic_display': ['build', 'kernel identity', 'formatted panic', 'LR', '20-frame backtrace'],
              'usb_panic_return': True, 'symbols': {name: hex(symbols[name]) for name in required},
              'shared_region_trace': args.shared_region_trace,
              'shared_region_trace_level': args.shared_region_trace_level if args.shared_region_trace else None,
              'patcher_sha256': sha(DEVICE / 'scripts/patch-xnu-entry-probe.py'),
              'screen_source_sha256': sha(DEVICE / 'scripts/xnu-panic-screen.c'), 'device_result': 'pending'}
    output.with_suffix('.debug.json').write_text(json.dumps(record, indent=2) + '\n')
    print(output)


if __name__ == '__main__':
    main()
