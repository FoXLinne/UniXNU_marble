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
    destination = Path(destination)
    inventory = destination.parent / ('.' + destination.name + '.source-files.json')
    current = {str(path.relative_to(source)) for path in source.rglob('*')
               if path.is_file() or path.is_symlink()}
    if inventory.is_file():
        previous = set(json.loads(inventory.read_text()))
        for relative in previous - current:
            path = destination / relative
            if path.is_file() or path.is_symlink():
                path.unlink()
    result = copy_sources(source, destination)
    inventory.write_text(json.dumps(sorted(current)) + '\n')
    return result


def stage_ios18_build(destination):
    return copy_sources(DEVICE / 'ios18/source', destination, ('drivers', 'kits'))


def build_source_revision():
    origins = json.loads((DEVICE / 'source_origins.json').read_text())
    return next(row['upstream_commit'] for row in origins['sources']
                if row['name'] == 'ios18-build')


def stage_ios18_stack(destination):
    destination = stage_ios18_build(destination)
    drivers = {'ArmCpu': 'ArmCpu', 'ArmDTPlatform': 'ArmDTPlatform',
               'DarwinBoot': 'DarwinBoot', 'QcomGeniSpi': 'NativeGeniSpi',
               'ShengUfs': 'NativeUfs', 'Adreno740': 'Adreno740',
               'AdrenoMetal': 'AdrenoMetal', 'GopFramebuffer': 'GopFramebuffer',
               'RamBlock': 'RamBlock', 'SoftwareAES': 'SoftwareAES',
               'AppleKeyStoreState': 'AppleKeyStoreState'}
    for name, target in drivers.items():
        stage_ios18(name, destination / 'drivers' / target)
    for name in ('ArmSmmu4K', 'AdrenoPM4', 'AdrenoFirmware', 'XnuPrelink'):
        stage_ios18(name, destination / 'kits' / name)
    references = destination / 'refer'
    references.mkdir(exist_ok=True)
    for source in (DEVICE.parent / 'UniXNU/refer').iterdir():
        target = references / source.name
        if not target.exists():
            target.symlink_to(source.resolve(), target_is_directory=True)
    return destination


def source_revision(name):
    origins = json.loads((DEVICE / 'source_origins.json').read_text())
    return next(row['upstream_commit'] for row in origins['sources'] if row['name'] == name)
