#!/usr/bin/env python3
"""Exercise the iOS 18 CPU-map transport while generic IOKit mappings return KERN_DENIED."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    tree, out = args.tree.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    peer = tree / 'build/turnip-darwin-host'
    library = tree / 'build/mesa-tu/src/freedreno/vulkan/libvulkan_freedreno.dylib'
    shim = peer / 'libhostiokit.dylib'
    binary = out / 'task_cpu_map'
    subprocess.run(['xcrun', 'clang++', '-std=c++23', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-I', tree / 'drivers/Adreno740',
                    Path(__file__).with_name('test_ios18_task_cpu_map.cpp'), '-o', binary], check=True)
    env = dict(os.environ, DYLD_INSERT_LIBRARIES=str(shim), HOSTSIM_TASK_CPU_MAP='1', HOSTSIM_DENY_MAP_MEMORY='1')
    tests = [('task-protocol', [binary])]
    tests += [('denied-map-' + mode, [peer / 'deferred_bo', library, mode])
              for mode in ('normal', 'busy', 'pressure')]
    results = []
    for name, command in tests:
        case_env = dict(env, TU_DARWIN_BO_CACHE="1" if name == "denied-map-normal" else "0")
        result = subprocess.run(command, env=case_env, capture_output=True, text=True, timeout=90)
        (out / (name + '.log')).write_text(result.stdout + result.stderr)
        passed = result.returncode == 0 and 'PASS' in result.stdout
        results.append(dict(test=name, passed=passed, exit_code=result.returncode))
        print(name + ': ' + ('PASS' if passed else 'FAIL'), flush=True)
        if not passed:
            print((result.stdout + result.stderr)[-3000:])
            break
    (out / 'task-map-tests.json').write_text(json.dumps(results, indent=2) + '\n')
    if len(results) != len(tests) or not all(item['passed'] for item in results):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
