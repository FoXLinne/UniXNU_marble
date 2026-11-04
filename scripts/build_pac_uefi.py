#!/usr/bin/env python3
"""Build the marble PAC inspection firmware and temporary boot image."""
import argparse
import gzip
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib

DEVICE = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--platform', type=Path, default=DEVICE.parent / 'marble_hackios')
    parser.add_argument('--stack', type=Path, default=DEVICE.parent / 'UniXNU')
    args = parser.parse_args()
    out, platform, stack = args.output.resolve(), args.platform.resolve(), args.stack.resolve()
    out.mkdir(parents=True, exist_ok=True)
    firmware = platform / 'Mu-Silicium'
    edk = firmware / 'Mu_Basecore'
    tools, conf = edk / 'BaseTools', out / 'Conf'
    conf.mkdir(exist_ok=True)
    for name in ('target', 'tools_def', 'build_rule'):
        path = conf / (name + '.txt')
        if not path.exists():
            shutil.copyfile(tools / 'Conf' / (name + '.template'), path)
    packages = [edk, DEVICE / 'package', stack / 'package', stack / 'drivers/DarwinBoot',
                firmware, firmware / 'Common/Mu', firmware / 'Silicon/Silicium']
    env = dict(os.environ, WORKSPACE=str(out), EDK_TOOLS_PATH=str(tools), CONF_PATH=str(conf),
               PACKAGES_PATH=os.pathsep.join(map(str, packages)), PYTHON_COMMAND=sys.executable,
               CLANG_BIN=str(platform / 'bin') + '/',
               PATH=os.pathsep.join([str(tools / 'BinWrappers/PosixLike'),
                                     str(tools / 'Source/C/bin'), os.environ['PATH']]))
    with (out / 'build.log').open('w') as log:
        subprocess.run(['build', '-p', 'MarblePkg/MarbleBoot.dsc', '-a', 'AARCH64',
                        '-b', 'RELEASE', '-t', 'CLANGPDB', '-n', '8'],
                       cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    binaries = out / 'Build/MarbleBoot/RELEASE_CLANGPDB/AARCH64'
    volume = out / 'volume.img'
    with volume.open('wb') as stream:
        stream.truncate(1024 * 1024)
    subprocess.run(['mformat', '-i', volume, '-v', 'MARBLEPAC', '::'], check=True)
    stage = out / 'stage/EFI/UniXNU'
    stage.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binaries / 'UniXNU.efi', stage / 'UniXNU.efi')
    subprocess.run(['mcopy', '-i', volume, '-s', '-Q', stage.parent, '::/'], check=True)
    blobs = [('loader.efi', (binaries / 'MarbleBootstrap.efi').read_bytes()),
             ('root.stage', b'1'), ('volume.img', volume.read_bytes())]
    body = b''.join(struct.pack('<16sII', name.encode(), len(data), 0) + data + bytes(-len(data) % 16)
                    for name, data in blobs) + bytes(24)
    payload = struct.pack('<II', 0x50554e58, len(body)) + body
    spec = tomllib.loads((firmware / 'Resources/Configs/marble.toml').read_text())
    fd = firmware / 'Build/marblePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd'
    if fd.stat().st_size != spec['uefi_fd']['size']:
        raise RuntimeError('Firmware size differs from the marble board configuration')
    shim_object, shim = out / 'BootShim.elf', out / 'BootShim.bin'
    subprocess.run([platform / 'bin/clang', '--target=aarch64-linux-gnu', '-c',
                    firmware / 'BootShim/BootShim.S', '-o', shim_object,
                    '-DREQUIRES_KERNEL_HEADER=1', f'-DFD_BASE={spec["uefi_fd"]["base"]:#x}',
                    '-DSHIM_BASE=0xa7000000', f'-DFD_SIZE={fd.stat().st_size:#x}',
                    '-DPAYLOAD_BASE=0xa8000000'], check=True)
    subprocess.run([platform / 'bin/llvm-objcopy', '-O', 'binary', shim_object, shim], check=True)
    raw = bytearray(shim.read_bytes() + fd.read_bytes() + payload)
    struct.pack_into('<Q', raw, 16, (len(raw) + 0x1fffff) & ~0x1fffff)
    kernel = out / 'firmware.bin'
    kernel.write_bytes(gzip.compress(raw, 6) + (platform / 'marble_live.dtb').read_bytes())
    output = out / 'cpu_probe_boot.img'
    subprocess.run([sys.executable, firmware / 'Resources/Scripts/mkbootimg.py',
                    '--kernel', kernel, '--ramdisk', firmware / 'Resources/ramdisk',
                    '--header_version', str(spec['boot_image']['header_version']), '-o', output], check=True)
    print(output)


if __name__ == '__main__':
    main()
