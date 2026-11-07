"""Stage this device repository's sources into generated build directories."""
import json
import os
from pathlib import Path
import shutil
import tempfile

DEVICE = Path(__file__).resolve().parents[1]


def copy_source_file(source, destination):
    source, destination = Path(source), Path(destination)
    if destination.is_file() and not destination.is_symlink():
        if (source.stat().st_size == destination.stat().st_size
                and source.read_bytes() == destination.read_bytes()):
            return str(destination)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
        temporary = Path(stream.name)
    try:
        shutil.copy2(source, temporary)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return str(destination)


def copy_sources(source, destination, excluded=()):
    source, destination = Path(source), Path(destination)
    if source.resolve() == destination.resolve():
        return destination
    if not source.is_dir():
        raise FileNotFoundError(source)
    for entry in source.rglob('*'):
        if entry.is_symlink():
            target = destination / entry.relative_to(source)
            if target.is_symlink() or target.is_file():
                target.unlink()
    shutil.copytree(source, destination, dirs_exist_ok=True, symlinks=True,
                    copy_function=copy_source_file,
                    ignore=shutil.ignore_patterns('.git', '__pycache__', *excluded))
    return destination


def stage_ios17(destination):
    destination = copy_sources(DEVICE / 'ios17/source', destination)
    for name, relative in (('Mesa', '.cache/mesa-work'),
                           ('metal2vulkan', '.cache/gpu-reference/metal2vulkan')):
        copy_sources(DEVICE / 'ios17/sources' / name, destination / relative)
    return destination


def stage_ios18(name, destination):
    source = DEVICE / 'ios18/sources' / name
    return copy_sources(source, destination)


def stage_ios18_build(destination):
    return copy_sources(DEVICE / 'ios18/source', destination, ('drivers', 'kits'))


def build_source_revision():
    origins = json.loads((DEVICE / 'source_origins.json').read_text())
    return next(row['upstream_commit'] for row in origins['sources']
                if row['name'] == 'ios18-build')
