#!/usr/bin/env python3
"""Build resident iOS 18 kernel checkpoints with USB exception reporting."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tomllib

from build_ios18_arm64e import DEVICE, PROFILE, read_adt, write_adt
from build_pac_uefi import build_applications, pack_boot_image


def segments(blob):
    result, at = {}, 32
    for _ in range(struct.unpack_from('<I', blob, 16)[0]):
        command, size = struct.unpack_from('<II', blob, at)
        if command == 0x19:
            name = blob[at + 8:at + 24].split(b'\0', 1)[0].decode()
            result[name] = struct.unpack_from('<4Q', blob, at + 24)
        at += size
    return result


def return_code(out, platform, virtual=False):
    source, obj, binary = (out / name for name in ('return.S', 'return.o', 'return.bin'))
    prefix = (' adr x16,1f\n ldr x17,[x20,#8]\n sub x16,x16,x17\n'
              ' ldr x17,[x20,#16]\n add x16,x16,x17\n br x16\n1:\n') if virtual else ''
    source.write_text('.text\n' + prefix + ' mrs x16,sctlr_el1\n bic x16,x16,#1\n'
                      ' bic x16,x16,#4\n bic x16,x16,#0x1000\n'
                      ' msr sctlr_el1,x16\n isb\n ldr x16,[x20,#0x808]\n br x16\n')
    subprocess.run([platform / 'bin/clang', '--target=aarch64-none-elf', '-c', source, '-o', obj], check=True)
    subprocess.run([platform / 'bin/llvm-objcopy', '-O', 'binary', '--only-section=.text', obj, binary], check=True)
    code = binary.read_bytes()
    if len(code) != (56 if virtual else 32):
        raise RuntimeError('Unexpected resident return stub size')
    return code


def exception_code(out, platform):
    source, obj, binary = (out / name for name in ('exception.S', 'exception.o', 'exception.bin'))
    source.write_text('.text\n adrp x20,0\n add x20,x20,#0\n ldr x15,[x20,#16]\n'
                      ' mrs x16,sctlr_el1\n tbz x16,#0,1f\n ldr x16,[x20,#32]\n'
                      ' mrs x17,tcr_el1\n bic x17,x17,#0x3f\n bic x17,x17,#0xc000\n'
                      ' bic x17,x17,#0x80\n mov x14,#25\n orr x17,x17,x14\n dsb sy\n'
                      ' msr tcr_el1,x17\n msr ttbr0_el1,x16\n isb\n tlbi vmalle1\n'
                      ' dsb sy\n isb\n1: br x15\n')
    subprocess.run([platform / 'bin/clang', '--target=aarch64-none-elf', '-c', source, '-o', obj], check=True)
    subprocess.run([platform / 'bin/llvm-objcopy', '-O', 'binary', '--only-section=.text', obj, binary], check=True)
    code = binary.read_bytes()
    if not 32 <= len(code) <= 128 or struct.unpack_from('<I', code)[0] != 0x90000014:
        raise RuntimeError('Unexpected exception return stub')
    return code


def place_exception(code, address, control):
    result = bytearray(code)
    pages = (control >> 12) - (address >> 12)
    if not -(1 << 20) <= pages < 1 << 20:
        raise RuntimeError('Exception probe control is out of ADRP range')
    struct.pack_into('<2I', result, 0,
                     0x90000014 | ((pages & 3) << 29) | (((pages >> 2) & 0x7ffff) << 5),
                     0x91000294 | ((control & 0xfff) << 10))
    return result


def exception_filter(out, platform):
    source, obj, binary = (out / name for name in ('filter.S', 'filter.o', 'filter.bin'))
    source.write_text('.text\n stp x16,x17,[sp,#-16]!\n mrs x16,esr_el1\n'
                      ' lsr x16,x16,#26\n cbz x16,1f\n cmp x16,#60\n b.eq 1f\n'
                      ' ldp x16,x17,[sp],#16\n b .\n1: ldp x16,x17,[sp],#16\n')
    subprocess.run([platform / 'bin/clang', '--target=aarch64-none-elf', '-c', source, '-o', obj], check=True)
    subprocess.run([platform / 'bin/llvm-objcopy', '-O', 'binary', '--only-section=.text', obj, binary], check=True)
    code = binary.read_bytes()
    if len(code) != 36 or struct.unpack_from('<I', code, 28)[0] != 0x14000000:
        raise RuntimeError('Unexpected synchronous exception filter')
    return code


def patch_kernel(root, kernel, out, platform, mode):
    data = bytearray(kernel.read_bytes())
    native = (root / 'kernel.native.macho').read_bytes()
    native_base, _, native_file, _ = segments(native)['__TEXT_EXEC']
    cache_base, _, cache_file, _ = segments(data)['__TEXT_EXEC']
    listing = subprocess.check_output(['/opt/homebrew/opt/llvm/bin/llvm-nm', '-n', '--defined-only',
                                      root / 'kernel.native.macho'], text=True)
    symbols = {parts[2]: int(parts[0], 16) for line in listing.splitlines()
               if len(parts := line.split()) == 3}
    offset = symbols['_LowExceptionVectorBase'] - native_base
    vector = cache_file + offset
    if native[native_file + offset:native_file + offset + 4096] != data[vector:vector + 4096]:
        raise RuntimeError('Native and fileset low exception vectors differ')
    control = cache_base + offset + 0x800
    if any(data[vector + 0x800:vector + 0x800 + 40]):
        raise RuntimeError('Expected unused low vector padding')
    data[vector + 0x800:vector + 0x800 + 16] = b'MARBLEPROBEARGS1'
    handler = exception_code(out, platform)
    for index in range(16):
        at = vector + index * 128
        if struct.unpack_from('<I', data, at)[0] != 0x14000000:
            raise RuntimeError('Expected an early exception self-loop')
        code = place_exception(handler, cache_base + offset + index * 128, control)
        data[at:at + len(code)] = code
    high = symbols['_ExceptionVectorsBase'] - native_base
    filter_code = exception_filter(out, platform)
    for index in (0, 4):
        at = cache_file + high + index * 128
        word = struct.unpack_from('<I', data, at)[0]
        if word & 0xfc000000 != 0x14000000:
            raise RuntimeError('Unexpected runtime exception vector entry')
        address = cache_base + high + index * 128
        delta = word & 0x3ffffff
        if delta & 0x2000000:
            delta -= 0x4000000
        target = address + 4 * delta
        code = bytearray(filter_code)
        struct.pack_into('<I', code, 28, 0x14000000 | (((target - address - 28) >> 2) & 0x3ffffff))
        code += place_exception(handler, address + len(code), control)
        if len(code) > 128:
            raise RuntimeError('Synchronous probe exceeds its vector slot')
        data[at:at + len(code)] = code
    report = {'vectors': 18, 'vector_base': hex(cache_base + offset), 'control': hex(control), 'mode': mode}
    if mode == 'arm-init':
        at = cache_file + symbols['_arm_init'] - native_base
        code = return_code(out, platform, virtual=True)
        data[at:at + len(code)] = code
        report['checkpoint'] = hex(cache_base + at - cache_file)
    elif mode != 'early':
        start = cache_file + symbols['_start_first_cpu'] - native_base
        pattern = bytes.fromhex('02000014000000149f3b03d5' if mode == 'tables' else '001018d5df3f03d5')
        positions = [at for at in range(start, start + 0x600, 4) if data[at:at + len(pattern)] == pattern]
        if len(positions) != 1:
            raise RuntimeError('Require one verified bootstrap checkpoint sequence')
        at = positions[0] + (4 if mode == 'tables' else 0)
        report['checkpoint'] = hex(cache_base + at - cache_file)
        if mode == 'fault':
            struct.pack_into('<I', data, at + 8, 0xd4202460)
        elif mode == 'post-mmu':
            code = return_code(out, platform)
            data[at + 8:at + 8 + len(code)] = code
        else:
            struct.pack_into('<2I', data, at, 0xf9440690, 0xd61f0200)
    kernel.write_bytes(data)
    (out / 'checkpoint.json').write_text(json.dumps(report, indent=2) + '\n')


def build(root, mode):
    out = root / ('probe-' + mode)
    out.mkdir(exist_ok=True)
    platform, stack = DEVICE.parent / 'marble_hackios', DEVICE.parent / 'UniXNU'
    binaries = build_applications(out, platform, stack)
    stage = out / 'stage/EFI/UniXNU'
    shutil.copytree(root / 'boot/stage/EFI/UniXNU', stage, dirs_exist_ok=True)
    profile = tomllib.loads(PROFILE.read_text())
    config = stage / 'unixnu.toml'
    arguments = json.dumps(' '.join(profile['payload']['kernel_args']))
    config.write_text(re.sub(r'^args = .*$', 'args = ' + arguments, config.read_text(), flags=re.MULTILINE))
    shutil.copyfile(binaries / 'DarwinBoot.efi', stage / 'Payloads/DarwinBoot.efi')
    shutil.copyfile(root / 'kernel.fileset.macho', stage / 'Payloads/kernel.macho')
    adt = stage / 'Payloads/kernel.macho.adt'
    tree, _ = read_adt(adt.read_bytes())
    chosen = next(node for node in tree[1] if node[0].get('name') == b'chosen\0')
    memory = next(node for node in chosen[1] if node[0].get('name') == b'memory-map\0')
    memory[0]['RootDisks'] = bytes(16)
    adt.write_bytes(write_adt(tree))
    if mode != 'selftest':
        patch_kernel(root, stage / 'Payloads/kernel.macho', out, platform, mode)
    volume = out / 'volume.img'
    with volume.open('wb') as stream:
        stream.truncate(70 * 1024**2)
    subprocess.run(['mformat', '-i', volume, '-v', 'MARBLEESP', '::'], check=True)
    subprocess.run(['mcopy', '-i', volume, '-s', '-Q', stage.parent, '::/'], check=True)
    application = 'MarbleResidentSelftest.efi' if mode == 'selftest' else 'MarbleKernelProbe.efi'
    blobs = [('loader.efi', (binaries / application).read_bytes()), ('volume.img', volume.read_bytes())]
    body = b''.join(struct.pack('<16sII', name.encode(), len(data), 0) + data + bytes(-len(data) % 16)
                    for name, data in blobs) + bytes(24)
    image = pack_boot_image(out, platform, struct.pack('<II', 0x50554e58, len(body)) + body,
                            'marble_ios18_' + mode + '.img')
    (out / 'probe.json').write_text(json.dumps({'mode': mode, 'image': str(image)}, indent=2) + '\n')
    return image


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkpoint', choices=['selftest', 'early', 'tables', 'pre-mmu', 'post-mmu', 'arm-init', 'fault'])
    parser.add_argument('--output', type=Path, default=DEVICE.parent.parent / 'outputs/UniXNU_marble/ios18_arm64e')
    arguments = parser.parse_args()
    print(build(arguments.output.resolve(), arguments.checkpoint))
