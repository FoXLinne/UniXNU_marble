#!/usr/bin/env python3
"""Reproduce the evidence in records/ios27-24B119/csm-final-review.json.

Disassembles the three remaining CSM/image4-v2 targets in the original
24B119 kernelcache, prints whole-function sha256 (the exact digest
fileset_match.anchor_reviewed verifies), and compares the flag-computation
function against the native research-mode pair.
"""
import bisect
import hashlib
import json
import sys

sys.path.insert(0, 'ios27/sources/XnuPrelink')
sys.path.insert(0, 'build/ios27-prelink/toolkit')
from kc_image import Image
import capstone

KC = '/Volumes/Extra/https-github-com-oosexgang-hackios-xnu/outputs/UniXNU_marble/ios27_24B119/ipsw/24B119/kernelcache.decompressed'
NATIVE = '/Volumes/Extra/https-github-com-oosexgang-hackios-xnu/outputs/UniXNU_marble/ios27_24B119/kernel.native.macho'
TARGETS = [0xfffffff00a7381b8, 0xfffffff00a7381f8, 0xfffffff00a738238,
           0xfffffff00a7382a0, 0xfffffff00a7382e8]
NATIVE_FUNS = ['_research_mode_state', '_extended_research_mode_state',
               '_developer_mode_state', '_update_csm_device_state']


def disasm(img, starts, address, md):
    i = bisect.bisect_right(starts, address) - 1
    end = starts[i + 1] if i + 1 < len(starts) else address + 0x100
    return end - address, list(md.disasm(img.read(address, end - address), address))


def main():
    raw = open(KC, 'rb').read()
    top = Image(raw)
    kern = Image(raw, top.entries['com.apple.kernel'][1])
    starts = sorted(kern.function_starts())
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for a in TARGETS:
        size, insns = disasm(kern, starts, a, md)
        digest = hashlib.sha256(kern.read(a, size)).hexdigest()
        print(f'== {a:#x} ({size}b) sha256 {digest}')
        for ins in insns:
            print(f'   {ins.address - 0x10000000:x}: {ins.mnemonic} {ins.op_str}')
    native = Image(open(NATIVE, 'rb').read())
    nstarts = sorted(native.function_starts())
    syms = {}
    import subprocess, re
    for line in subprocess.run(['nm', NATIVE], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            syms.setdefault(parts[2], int(parts[0], 16))
    for name in NATIVE_FUNS:
        a = syms.get(name)
        if a is None:
            print(f'{name}: ABSENT')
            continue
        size, insns = disasm(native, nstarts, a, md)
        print(f'== native {name} @{a:#x} ({size}b)')
        for ins in insns:
            print(f'   {ins.address:x}: {ins.mnemonic} {ins.op_str}')


if __name__ == '__main__':
    main()
