#!/usr/bin/env python3
"""Stage marble firmware sources and connect the local build tools."""
from pathlib import Path

from marble_sources import DEVICE, copy_sources


def link(path, target):
    if path.is_symlink():
        path.unlink()
    path.symlink_to(target.resolve(), target_is_directory=target.is_dir())


def prepare():
    workspace = DEVICE.parents[1]
    platform = DEVICE / 'firmware'
    tools = workspace / 'tools/marble-build'
    tree = workspace / 'outputs/UniXNU_marble/firmware/tree'
    copy_sources(platform / 'source', tree)
    for name in ('bin', 'venv'):
        link(platform / name, tools / name)
    link(platform / 'Mu-Silicium', tree)
    for name in ('marble_live.dtb', 'marble_live.dts'):
        link(platform / name, platform / 'inputs' / name)
    return platform


if __name__ == '__main__':
    print(prepare())
