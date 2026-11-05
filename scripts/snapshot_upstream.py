#!/usr/bin/env python3
"""Record upstream revisions and local integration patches."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent
SOURCES = {
    'UniXNU': (WORK / 'UniXNU', None),
    'UniXNUBuild': (WORK / 'upstream/UniXNU', []),
    'ArmDTPlatform': (WORK / 'UniXNU/drivers/ArmDTPlatform', None),
    'XnuPrelink': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/XnuPrelink', None),
    'NativeRamBlock18': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/NativeRamBlock', None),
    'IOHIDFamily18': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/IOHIDFamily', None),
    'ShengHidInput': (WORK / 'upstream/ShengHidInput', None),
    'GopFramebuffer18': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/GopFramebuffer', None),
    'SoftwareAES': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/SoftwareAES', None),
    'AppleKeyStoreState18': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/AppleKeyStoreState', None),
    'DarwinBoot': (WORK / 'UniXNU/drivers/DarwinBoot', None),
    'ArmCpu': (WORK / 'UniXNU/drivers/ArmCpu', None),
    'NativeAdreno': (WORK / 'UniXNU/drivers/NativeAdreno', None),
    'NativeMetal': (WORK / 'UniXNU/drivers/NativeMetal', None),
    'UniXNU18': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/gpu/UniXNU', []),
    'MetalTranslator18': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/gpu/UniXNU/.cache/gpu-reference/metal2vulkan', []),
    'Adreno74018': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/gpu/Adreno740', []),
    'AdrenoMetal18': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/gpu/AdrenoMetal', []),
    'ArmSmmu4K18': (WORK.parent / 'outputs/UniXNU_marble/ios18_arm64e/gpu/ArmSmmu4K', []),
    'NativeMobileDisplay': (WORK / 'UniXNU/drivers/NativeMobileDisplay', None),
    'NativeUfs': (WORK / 'UniXNU/drivers/NativeUfs', None),
    'NativeRamBlock': (WORK / 'UniXNU/drivers/NativeRamBlock', None),
    'NativeGeniSpi': (WORK / 'UniXNU/drivers/NativeGeniSpi', None),
    'NativeKeyStoreEvents': (WORK / 'UniXNU/drivers/NativeKeyStoreEvents', None),
    'Mu_Basecore': (WORK / 'marble_hackios/Mu-Silicium/Mu_Basecore', []),
    'MarbleBinaries': (WORK / 'marble_hackios/Mu-Silicium/Binaries', []),
    'ArmSmmu4K': (WORK / 'UniXNU/kits/ArmSmmu4K', None),
    'NativePrelink': (WORK / 'UniXNU/kits/NativePrelink', None),
    'Mu_Silicium': (WORK / 'marble_hackios/Mu-Silicium', [
        'Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.c',
        'Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.h',
    ]),
}


def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args])


def main():
    output = ROOT / 'upstream_patches'
    output.mkdir(exist_ok=True)
    revisions = {}
    for name, (repo, extra) in SOURCES.items():
        revisions[name] = {
            'commit': git(repo, 'rev-parse', 'HEAD').decode().strip(),
            'remote': git(repo, 'remote', 'get-url', 'origin').decode().strip(),
        }
        # Keep firmware and generated binary inputs in their source checkout.
        # A binary deletion patch also contains the original input bytes.
        changes = git(repo, 'diff', '--numstat', '-z', '--no-renames',
                      '--ignore-submodules=all', 'HEAD').decode().split('\0')
        paths = [row.split('\t', 2)[2] for row in filter(None, changes)
                 if row.split('\t', 2)[0] != '-']
        if name == 'UniXNU18':
            paths = [path for path in paths if path.startswith('scripts/') or path in (
                'docs/drivers/adreno-metal.md', 'docs/zh/drivers/adreno-metal.md',
                'docs/drivers/adreno740.md', 'docs/zh/drivers/adreno740.md')]
        patch = git(repo, 'diff', '--no-renames', '--ignore-submodules=all',
                    'HEAD', '--', *paths) if paths else b''
        if extra is None:
            extra = git(repo, 'ls-files', '--others', '--exclude-standard', '-z').decode().split('\0')
        for name_in_repo in filter(None, extra):
            if '__pycache__' in Path(name_in_repo).parts or Path(name_in_repo).suffix == '.pyc':
                continue
            result = subprocess.run(['git', 'diff', '--no-index', '--binary', '--',
                                     '/dev/null', name_in_repo], cwd=repo, capture_output=True)
            if result.returncode not in (0, 1):
                raise RuntimeError(result.stderr.decode())
            patch += result.stdout
        (output / f'{name}.patch').write_bytes(patch)
    (ROOT / 'upstream_revisions.json').write_text(json.dumps(revisions, indent=2) + '\n')


if __name__ == '__main__':
    main()
