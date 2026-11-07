#!/usr/bin/env python3
"""Copy marble's iOS 17 or iOS 18 sources into a build directory."""
import argparse
from pathlib import Path

from marble_sources import DEVICE, stage_ios17, stage_ios18


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=('17', '18'), required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    outputs = DEVICE.parents[1] / 'outputs/UniXNU_marble'
    if args.os == '17':
        output = args.output or outputs / 'ios17/tree'
        stage_ios17(output)
    else:
        from build_ios18_gpu import source_tree
        output = args.output or outputs / 'ios18_arm64e'
        output.mkdir(parents=True, exist_ok=True)
        source_tree(output)
        for name, directory in (('XnuPrelink', 'XnuPrelink'),
                                ('RamBlock', 'NativeRamBlock'),
                                ('SoftwareAES', 'SoftwareAES'),
                                ('AppleKeyStoreState', 'AppleKeyStoreState')):
            stage_ios18(name, output / directory)
        stage_ios18('Mesa', output / 'gpu/Mesa')
    print(output)


if __name__ == '__main__':
    main()
