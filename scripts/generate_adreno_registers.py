#!/usr/bin/env python3
"""Generate marble's gen7_4_0 register data from Qualcomm KGSL."""
import argparse
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kgsl', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    definitions = dict(re.findall(r'^#define\s+(GEN7_\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\b',
                                  (args.kgsl / 'gen7_reg.h').read_text(), re.M))
    def value(expression):
        return sum(int(definitions.get(term.strip(), term.strip()), 0) for term in expression.split('+'))
    def array(source, name):
        match = re.search(r'\b' + name + r'\[\]\s*=\s*\{(.*?)\n\};', source, re.S)
        if not match:
            raise ValueError('Missing KGSL array: ' + name)
        return match[1]
    catalog = (args.kgsl / 'adreno-gpulist.h').read_text()
    gpu = (args.kgsl / 'adreno_gen7.c').read_text()
    hwcg = [[value(reg), value(data)] for reg, data in
            re.findall(r'\{\s*(GEN7_\w+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}', array(catalog, 'gen7_0_0_hwcg_regs'))]
    protect = []
    for reg, start, end, access in re.findall(r'\{\s*([^,{}]+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*([01])\s*\}',
                                            array(catalog, 'gen7_0_0_protected_regs')):
        lo, hi = value(start), value(end)
        protect.append([value(reg), lo | (hi - lo) << 18 | int(access) << 31])
    lists = {}
    for output, source in [('MarbleIfpc', 'gen7_0_0_ifpc_pwrup_reglist'), ('MarblePreempt', 'gen7_0_0_pwrup_reglist')]:
        lists[output] = [value(reg) for reg in array(gpu, source).split(',') if reg.strip()]
    required = sorted(set(re.findall(r'\bGEN7_\w+', (root / 'package/MarbleAdreno.cpp').read_text())))
    revision = subprocess.check_output(['git', '-C', args.kgsl, 'rev-parse', 'HEAD'], text=True).strip()
    lines = ['// SPDX-License-Identifier: GPL-2.0-only', '// Qualcomm KGSL ' + revision,
             '#pragma once', '#include <stdint.h>', 'enum : uint32_t {']
    lines += [f'    {name} = {value(name):#x},' for name in required]
    lines += ['};']
    for name, rows in [('MarbleHwcg', hwcg), ('MarbleProtect', protect)]:
        lines += [f'static constexpr uint32_t {name}[][2] = {{']
        lines += [f'    {{{reg:#x}, {data:#x}}},' for reg, data in rows]
        lines += ['};']
    for name, registers in lists.items():
        lines += [f'static constexpr uint32_t {name}[] = {{']
        lines += [f'    {reg:#x},' for reg in registers]
        lines += ['};']
    (root / 'package/MarbleAdrenoRegisters.h').write_text('\n'.join(lines) + '\n')
    print(f'hwcg={len(hwcg)} protect={len(protect)} ifpc={len(lists["MarbleIfpc"])} preemption={len(lists["MarblePreempt"])}')


if __name__ == '__main__':
    main()
