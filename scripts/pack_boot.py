#!/usr/bin/env python3
"""Pack the marble UEFI, UniXNU FAT volume and optional XNU kernel."""
import argparse
import gzip
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib

DEVICE = Path(__file__).resolve().parents[1]
STACK = DEVICE.parent / 'UniXNU'
PLATFORM = DEVICE.parent / 'marble_hackios'
BIN = STACK / 'out/marble/bootstrap/Build/MarbleBoot/RELEASE_CLANGPDB/AARCH64'
OUT = STACK / 'out/marble/boot'

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel', type=Path)
parser.add_argument('--unique-chip-id')
parser.add_argument('--root-stage', action='store_true')
parser.add_argument('--root-images', type=Path)
parser.add_argument('--trust-cache', type=Path)
parser.add_argument('--iphone-identity', action='store_true', help='Write the phone identity into the payload ADT instead of the iPad one')
parser.add_argument('--gpu-addon', type=Path, help='GPU addon file handed to the kernel as kernel.macho.gpuaddon')
args = parser.parse_args()
OUT.mkdir(parents=True, exist_ok=True)
if (OUT / 'stage').exists():
    shutil.rmtree(OUT / 'stage')
stage = OUT / 'stage/EFI/UniXNU'
stage.mkdir(parents=True, exist_ok=True)
payloads = stage / 'Payloads'
payloads.mkdir(exist_ok=True)
shutil.copy2(BIN / 'UniXNU.efi', stage / 'UniXNU.efi')
shutil.copy2(BIN / 'DarwinBoot.efi', payloads / 'DarwinBoot.efi')
if args.kernel:
    if not args.unique_chip_id:
        parser.error('--kernel requires --unique-chip-id')
    shutil.copy2(args.kernel, payloads / 'kernel.macho')
    board = tomllib.loads((DEVICE / 'board.toml').read_text())
    adt_command = [sys.executable, DEVICE / 'scripts/make-marble-adt.py',
                    '--output', payloads / 'kernel.macho.adt', '--dram-size', '0x400000000',
                    '--owned-memory-size', board['payload']['adt']['owned_memory_size'],
                    '--cpu-regs', board['payload']['adt']['cpu_regs'],
                    '--unique-chip-id', args.unique_chip_id, '--chip-id', '207', '--iphone-identity' if args.iphone_identity else '--ipad-identity']
    if args.root_stage:
        if args.root_images is None:
            parser.error('--root-stage requires --root-images')
        images = json.loads(args.root_images.read_text())
        os_image = next(image for image in images if image['name'] == 'OS')
        adt_command += ['--root-disks', '--root-match-volume-uuid', os_image['volumes'][0]['uuid']]
    if args.trust_cache:
        shutil.copy2(args.trust_cache, payloads / 'kernel.macho.trustcache')
        adt_command.append('--trust-cache')
    if args.gpu_addon:
        shutil.copy2(args.gpu_addon, payloads / 'kernel.macho.gpuaddon')
        adt_command.append('--gpu-addon')
    subprocess.run(adt_command, check=True)
    kernel_args = ' '.join(board['payload']['kernel_args'])
    entry = f'type = "xnu"\npath = "Payloads/kernel.macho"\nargs = "{kernel_args}"\n'
else:
    shutil.copy2(BIN / 'HelloPayload.efi', payloads / 'HelloPayload.efi')
    entry = 'type = "efi"\npath = "Payloads/HelloPayload.efi"\nargs = "pause"\n'
timeout = 0 if args.root_stage else 3
(stage / 'unixnu.toml').write_text(f'version = 1\n[boot]\ntimeout = {timeout}\ndefault = "marble"\n[kits]\nautoload = false\n[[driver]]\npath = "Payloads/DarwinBoot.efi"\n[[entry]]\nid = "marble"\ntitle = "UniXNU"\n' + entry)
size = sum(p.stat().st_size for p in stage.rglob('*') if p.is_file())
volume = OUT / 'volume.img'
volume_bytes = ((size + 8 * 1024**2 - 1) // (8 * 1024**2)) * 8 * 1024**2
with volume.open('wb') as file:
    file.truncate(volume_bytes)
subprocess.run(['mformat', '-i', volume, '-v', 'MARBLEESP', '::'], check=True)
subprocess.run(['mcopy', '-i', volume, '-s', '-Q', str(stage.parent), '::/'], check=True)
container = OUT / 'bootstrap.bin'
body = bytearray()
blobs = [('loader.efi', BIN / 'MarbleBootstrap.efi')]
if args.root_stage:
    marker = OUT / 'root.stage'
    marker.write_bytes(b'1')
    blobs.append(('root.stage', marker))
blobs.append(('volume.img', volume))
for name, path in blobs:
    data = path.read_bytes()
    body.extend(struct.pack('<16sII', name.encode(), len(data), 0))
    body.extend(data)
    body.extend(bytes(-len(data) % 16))
body.extend(bytes(24))
container.write_bytes(struct.pack('<II', 0x50554E58, len(body)) + body)
if container.stat().st_size > 0x4000000:
    raise SystemExit('Payload exceeds the 64 MiB marble BootShim reserved window')
firmware = PLATFORM / 'Mu-Silicium'
fd = firmware / 'Build/marblePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd'
shim_object = OUT / 'BootShim.elf'
shim_binary = OUT / 'BootShim.bin'
subprocess.run([PLATFORM / 'bin/clang', '--target=aarch64-linux-gnu', '-c',
                firmware / 'BootShim/BootShim.S', '-o', shim_object,
                '-DREQUIRES_KERNEL_HEADER=1', '-DFD_BASE=0xa7100000', '-DSHIM_BASE=0xa7000000',
                f'-DFD_SIZE={fd.stat().st_size:#x}', '-DPAYLOAD_BASE=0xa8000000'], check=True)
subprocess.run([PLATFORM / 'bin/llvm-objcopy', '-O', 'binary', shim_object, shim_binary], check=True)
data = bytearray(shim_binary.read_bytes() + fd.read_bytes() + container.read_bytes())
# The Android loader reserves memory using the ARM64 Image header, including payloads.
struct.pack_into('<Q', data, 16, (len(data) + 0x1fffff) & ~0x1fffff)
kernel = OUT / 'firmware.bin'
kernel.write_bytes(gzip.compress(data, 6) + (PLATFORM / 'marble_live.dtb').read_bytes())
output = OUT / ('marble_kernel_boot.img' if args.kernel else 'marble_menu_boot.img')
subprocess.run([sys.executable, firmware / 'Resources/Scripts/mkbootimg.py', '--kernel', kernel,
                '--ramdisk', firmware / 'Resources/ramdisk', '--header_version', '4', '-o', output], check=True)
print(output)
