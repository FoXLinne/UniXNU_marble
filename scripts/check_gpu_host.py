#!/usr/bin/env python3
"""Check GPU allocation, mapping and submission with host IOKit fixtures."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2] / 'UniXNU'
OUT = ROOT / 'build/marble_gpu_host'
FLAGS = ['-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined']
CASES = {
    'memory': ['NativeAdrenoMemory'],
    'smmu_memory': ['NativeAdrenoMemory', 'NativeAdrenoSmmuMemory'],
    'vm': ['NativeAdrenoMemory', 'NativeAdrenoSmmuMemory', 'NativeAdrenoVm'],
    'submit': ['NativeAdrenoMemory', 'NativeAdrenoSmmuMemory', 'NativeAdrenoVm',
               'NativeAdrenoRing', 'NativeAdrenoSubmit'],
    'userclient': ['NativeAdrenoMemory', 'NativeAdrenoSmmuMemory', 'NativeAdrenoVm',
                   'NativeAdrenoRing', 'NativeAdrenoSubmit', 'NativeAdrenoUserClient',
                   'NativeAdrenoTimer'],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tests', nargs='*', help='Case names; defaults to all cases')
    args = parser.parse_args()
    selected = args.tests or list(CASES)
    unknown = set(selected) - CASES.keys()
    if unknown:
        parser.error('Unknown cases: ' + ', '.join(sorted(unknown)))
    OUT.mkdir(parents=True, exist_ok=True)
    include = OUT / 'include'
    for name in ['IOKit/IOTypes.h', 'IOKit/IOReturn.h', 'IOKit/IOBufferMemoryDescriptor.h',
                 'IOKit/IOLib.h', 'kern/task.h']:
        path = include / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "XnuMemoryFixture.h"\n')
    driver = ROOT / 'drivers/NativeAdreno'
    smmu = ROOT / 'kits/ArmSmmu4K'
    objects = []
    if any(name != 'memory' for name in selected):
        for stem in ['ArmSmmu4KContext', 'ArmSmmu4KProgram', 'ArmSmmu4KLive', 'ArmSmmu4KStreams']:
            obj = OUT / (stem + '.o')
            subprocess.run(['xcrun', 'clang', '-std=c11', *FLAGS, '-c',
                            smmu / (stem + '.c'), '-o', obj], check=True, timeout=90)
            objects.append(obj)
    pm4 = OUT / 'AdrenoPM4.o'
    if any(name in selected for name in ('submit', 'userclient')):
        subprocess.run(['xcrun', 'clang', '-std=c11', *FLAGS, '-c',
                        ROOT / 'kits/AdrenoPM4/AdrenoPM4.c', '-o', pm4], check=True, timeout=90)
    includes = [value for path in [include, driver / 'tests', driver, smmu, ROOT / 'kits/AdrenoPM4']
                for value in ['-I', str(path)]]
    for name in selected:
        binary = OUT / ('test_' + name)
        linked = objects if name != 'memory' else []
        if name in ('submit', 'userclient'):
            linked = [*linked, pm4]
        command = ['xcrun', 'clang++', '-std=c++23', *FLAGS, *includes,
                   *[driver / (stem + '.cpp') for stem in CASES[name]],
                   driver / 'tests' / ('test_' + name + '.cpp'), *linked, '-o', binary]
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        (OUT / (name + '_build.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            print(result.stdout + result.stderr, end='')
            raise SystemExit(result.returncode)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=60)
        (OUT / (name + '.log')).write_text(result.stdout + result.stderr)
        print(name + ': ' + result.stdout + result.stderr, end='', flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
