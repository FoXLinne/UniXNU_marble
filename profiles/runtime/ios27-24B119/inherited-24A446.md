# Inherited 24A446 probes requiring 24B119 matching

Hardcoded iPadOS 27 (24A446) assumptions found in the inherited sheng source
tree. None may flow silently into the 24B119 build. Status column:

- **worked around**: `scripts/build_ios27_runtime.py` produces the artifact
  itself with 24B119-qualified inputs and never invokes the pinned path.
- **parent must match**: the file lives outside this task's editable scope;
  the parent or the ios27/sources worker must update it before using it.
- **cosmetic only**: label/metadata, no functional input.

| File | Probe | Status |
|---|---|---|
| `ios27/source/scripts/build-boot-runtime.py:345` | `if (target, build) != ('ios27','24A446'): raise 'AppSync requires iPadOS 27 build 24A446'` | worked around: the runtime builder constructs the AppSync payload with the 24B119 pins in `target.json`; do not pass an appsync request through this builder until the guard is widened |
| `ios27/source/scripts/unixnu/runtime.py:83` | same `(target,build)!=('ios27','24A446')` AppSync gate | parent must match, or drive AppSync through `build_ios27_runtime.py` |
| `ios27/source/scripts/appsync/build.py:27` | `PINS` sha256 of 24A446 installd / backboardd / MobileInstallationHelperService / SpringBoard | worked around: 24B119 pins re-derived from the OS root into `target.json#appsync.pins_24B119`; parent may alternatively re-pin PINS with those values |
| `ios27/source/scripts/appsync/build.py:93` | `UniXNUAppSync.runtime` marker written as `24A446` | worked around: runtime builder writes `24B119` |
| `ios27/source/scripts/appsync/build.py` report | `build: '24A446'` in report.json | cosmetic once pins are fixed; runtime builder emits `24B119` |
| `scripts/build_phone_ui.py:26` | `--target` limited to `ios17`/`ios18` | worked around: phone-UI packaging is implemented in `build_ios27_runtime.py` with `arm64e-apple-ios27.0` and the D84 screen category |
| `scripts/build_marble_haptics.py` | reads `Library/Audio/Tunings/N841` (absent on 24B119), `MinimumOSVersion 18.0`, clang target `arm64e-apple-ios18.0`, report `build: '22H384'` | worked around for tunings (N104 source per `target.json`); parent may update the HAL compile target/min-OS for a native ios27 variant |
| `scripts/build_marble_wifi_userspace.py:43,53` | loads `ios18/source/scripts/build-boot-runtime.py`, report `build: '22H384'` | functional for launchd-cache merge when pointed at a 24B119 `--os-root`; runtime builder drives it with `ios27/source/scripts` helpers instead and emits a 24B119 report |
| `package/MarbleUiPolicy.c` | all 22H384 fixup offsets/bytes + 22H384 cache identity | replaced by `package/ios27-runtime/MarbleUiPolicy27.c` (per-site 24B119 offsets/bytes from `fixups.json`) |
| `package/MarbleSystemOS.c` `captureAbortReason` | 22H384 all-image UUID `52fd..e7`, five `__crash_info` annotation VAs, libobjc message slot `0x1eb068488` | replaced by `package/ios27-runtime/MarbleCrashInfo27.c` under `MARBLE_IOS27_GPU` with the values in `target.json#crash_info` |
| `profiles/appsync-22H384.json`, `profiles/gpu-22H384.json` | 22H384 pins/profile values | superseded by this directory; do not reference for 24B119 |

## Kernel-side contract consumed by these inputs

`patches/xnu-12377/marble-platform-hooks.patch` iterates
`native_marble_cellular_text_fixup(i)` until NULL and parses
`file,offset,original,replacement` (hex byte strings, equal length, <=32
bytes after `native-text-fixup.patch`). The 24B119 table keeps this exact
format, so the kernel patch needs no change for the new entries.

## Driver-build selection

Compile `package/ios27-runtime/MarbleUiPolicy27.c` instead of
`package/MarbleUiPolicy.c`, and `package/ios27-runtime/MarbleCrashInfo27.c`
for the crash-info/`native_marble_ui_exit` symbols (define
`MARBLE_IOS27_GPU`). `MarbleSystemOS.c` must not also export those same
symbols in the ios27 object set; the rest of that file (storage/mount
policy) is outside this task's scope.
