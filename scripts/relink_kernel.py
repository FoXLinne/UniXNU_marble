#!/usr/bin/env python3
"""Relink the saved XNU core archive with the current platform driver archives."""
import argparse
from pathlib import Path
import shlex
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    tree = args.tree.resolve()
    output = args.output.resolve()
    base = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    name = 'mach.development.bcm2837'
    saved = base / (name + '.link')
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    flags = shlex.split((saved / (name + '.linkarguments')).read_text())
    # The saved KC recipe merges this data segment into executable text. Preserve it for the standalone kernel.
    flags.remove('-Wl,-rename_segment,__LASTDATA_CONST,__LAST')
    command = ['xcrun', 'clang', '-isysroot', sdk, *flags,
               '-Wl,-alias_list,' + str(saved / (name + '.alias.exp')),
               '-Wl,-sectcreate,__LINKINFO,__symbolsets,' + str(base / 'symbolsets.plist'),
               '-Wl,-segprot,__LINKINFO,r--,r--',
               '-Wl,-force_load,' + str(saved / (name + '.a'))]
    command += shlex.split((saved / (name + '.linkarchives')).read_text())
    command += ['-o', str(output)]
    subprocess.run(command, cwd=tree, check=True)
    print(output)


if __name__ == '__main__':
    main()
