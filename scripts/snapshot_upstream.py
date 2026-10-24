#!/usr/bin/env python3
"""Record upstream revisions and local integration patches."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT.parent
SOURCES = {
    'UniXNU': (WORK / 'UniXNU', None),
    'DarwinBoot': (WORK / 'UniXNU/drivers/DarwinBoot', None),
    'ArmCpu': (WORK / 'UniXNU/drivers/ArmCpu', None),
    'NativeAdreno': (WORK / 'UniXNU/drivers/NativeAdreno', None),
    'NativeMetal': (WORK / 'UniXNU/drivers/NativeMetal', None),
    'NativeMobileDisplay': (WORK / 'UniXNU/drivers/NativeMobileDisplay', None),
    'NativeRamBlock': (WORK / 'UniXNU/drivers/NativeRamBlock', None),
    'NativeGeniSpi': (WORK / 'UniXNU/drivers/NativeGeniSpi', None),
    'NativeKeyStoreEvents': (WORK / 'UniXNU/drivers/NativeKeyStoreEvents', None),
    'Mu_Basecore': (WORK / 'marble_hackios/Mu-Silicium/Mu_Basecore', []),
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
        patch = git(repo, 'diff', '--binary', '--ignore-submodules=all', 'HEAD')
        if extra is None:
            extra = git(repo, 'ls-files', '--others', '--exclude-standard', '-z').decode().split('\0')
        for name_in_repo in filter(None, extra):
            result = subprocess.run(['git', 'diff', '--no-index', '--binary', '--',
                                     '/dev/null', name_in_repo], cwd=repo, capture_output=True)
            if result.returncode not in (0, 1):
                raise RuntimeError(result.stderr.decode())
            patch += result.stdout
        (output / f'{name}.patch').write_bytes(patch)
    (ROOT / 'upstream_revisions.json').write_text(json.dumps(revisions, indent=2) + '\n')


if __name__ == '__main__':
    main()
