# marble iOS 27.2 (24B119) userland/runtime record

Bounded scope: userland/runtime migration of the 22H384/24A446-era runtime
pieces to iOS 27.2 build 24B119 for the marble board (SM7475 / Adreno 725,
spoofed UI identity D84). Editable paths only; no commits, branch or device
operations. Machine-readable detail: `records/ios27-24B119/runtime.json`;
per-step build output: `outputs/UniXNU_marble/ios27_24B119/runtime/runtime.json`.

## Inputs (read-only mounts)

| Input | Mount | Identity |
| --- | --- | --- |
| OS DMG `141-08279-215.dmg` | `/private/tmp/marble-24b119-os` (`/dev/disk12s1`) | build 24B119, iPhone12,1 / n104ap |
| SystemOS cryptex `141-10128-227.dmg` (decrypted from `.aea`) | `/private/tmp/marble-24b119-cryptex` (`/dev/disk14s1`) | dyld cache UUID `555BFDAA-D930-31AC-A915-D472C273C527` |
| GPU addon | `outputs/UniXNU_marble/audio-20261110/boot-lock-state0/wifi.gpuaddon` | sha256 `07e423c6…34f2` (matches `CURRENT.json` `gpu_addon_sha256`) |

## Builder

`scripts/build_ios27_runtime.py --os-root OS --cryptex-root CRYPTEX --output OUT [--gpu-addon ADDON]`
Optional: `--source-scripts` (default `ios27/source/scripts`),
`--components` (default `ios27/sources`), `--skip-{wifi,haptics,appsync}`.

## What was ported and how it was qualified

All binary-specific values were re-derived from the 24B119 dyld cache; no
22H384 or 24A446 address/offset is used unqualified. Inherited assumptions
are catalogued in `profiles/runtime/ios27-24B119/inherited-24A446.md`.

### Shared-cache fixups — `profiles/runtime/ios27-24B119/fixups.json`

19 entries, all byte-verified against the live cache by the builder
(`fixups: verified, 19/19`). Format:
`subcache,file_offset,original_hex,replacement_hex` with image, symbol, VA
and reason attached.

- **Powerlog (8, PowerlogCore .36)** — `PLTimeManager` timestamp math widened
  from 32- to 64-bit: `scvtf d?,w?` -> `scvtf d?,x?`,
  `add w21,w21,w27` -> `add x21,x21,x27`, plus the `bucketNSDate:`/
  `bucketTimeStampForDate:` conversion sites and two callers. Register
  choices and surrounding instructions are unchanged.
- **Telephony (7)** — five `TUCallServicesInterface` sites in
  TelephonyUtilities (.07) and two `SBTelephonyManager` sites in SpringBoard
  (.36). Original prologues kept; the call/count setup replaced by the
  zero-safe sequence used at 22H384, re-encoded for the 24B119 prologues
  (byte-identical encodings, new offsets).
- **LockedContentServices (1, .59)** — `_lock_updateKnownCaptureApplications:`
  nil-container path: skip absent optional container IDs while preserving the
  notification flow.
- **Audio host clock (3)** — the board runs a 19.2 MHz host clock; 24B119
  audio code carries 24 MHz defaults. Patched double constants:
  CoreAudio (.38) `AudioGetHostClockFrequency` literal `0x22c55e2f8`,
  AudioDSPGraph (.28) queue configuration `0x2074f02e8`,
  libEmbeddedSystemAUs (.36) PerformIO/thread-priority `0x226bd3fe8`.
  PHASE's 24 MHz constants were inspected and excluded — they sit in
  acoustics/codec tables, not host-clock consumers. Calendar time is
  unaffected.

### Crash info — `package/ios27-runtime/MarbleCrashInfo27.c`

Replaces the `__crash_info` address block of `package/MarbleSystemOS.c` for
24B119. Gates on cache UUID `555BFDAA-…-C273C527` before using the new
addresses: CF `___crashreporter_info__` `0x1eaf8ab50`, CF `__crash_info`
`0x1eaf8a188`, Foundation `0x1eaf990a8`, libc++abi `0x2da9270e8`,
libdispatch `0x1ec216a78`, libswiftCore `0x1eb00e958`, libsystem_c
`0x1eb1c5408`, libobjc `0x1eaf700c0` with the message slot at
`0x1eaf700c8` (`__crash_info` + 8 — same layout as 22H384).

### UI policy — `package/ios27-runtime/MarbleUiPolicy27.c`

24B119 port of `package/MarbleUiPolicy.c`: retains AMFI/platform-binary
checks, AppSync executable identification and the approved-executable DYLD
insertion path, carries the 19-entry fixup table and runtime selection
metadata. Driver build contract is recorded in
`patch-inputs/hook-sources.json`: select `MarbleUiPolicy27.c` instead of
`package/MarbleUiPolicy.c` and `MarbleCrashInfo27.c` instead of the
`MarbleSystemOS.c` crash-info block for the ios27 payload.

## Runtime pieces produced

| Piece | Result | Notes |
| --- | --- | --- |
| Wi-Fi | `wifi-runtime/bin/unixnu-wpa_supplicant` (signed) + `etc/wifi` + `daemons` | built from `ios27/sources/Ath12kWifi` after `tools/fetch-sources.sh` populated the source-locked `deps/` |
| launchd cache | `wifi-runtime/xpc/launchd.plist` + `.sig` | 732 OS jobs preserved, `org.unixnu.wifi.supplicant` added; detached signature cdhash `1afb9213…c7f4` |
| Haptics HAL | `haptics/hal/MarbleHaptics.driver` (signed, cdhash `d9c9f7b9…b539a3`) | compiled for `arm64e-apple-ios27.0`; 251 N104 tunings published as `N104` + `D84` (N841 does not exist on 24B119) |
| Phone UI / GPU addon | `phone-ui/phone-ui.gpuaddon` | `MarblePhoneUI.dylib` + `NativeAdrenoMetal.bundle` re-signed; screen category 11 (D84 spoof); turnip sha256 preserved |
| Wallpapers | merged into the same addon | 103 files; 24 HEIC layers converted; N104 event-timing shipped as `event-timing/D84.*`; `iPhone16,2`/`iPhone15,3` defaults registered |
| AppSync | `appsync/` payload | `UniXNUAppSync.dylib`, `libunixnu-ldid.dylib`, `unixnu-appinst`, patched `installd`/`backboardd`/helper/`SpringBoard` — all 24B119-pinned, entitlements + identifiers preserved, nested executables hash-verified |
| Trust manifest | `runtime.json` `trust_manifest` | 24 artifacts (cdhash + sha256) for the parent trust-append step |
| Patch inputs | `patch-inputs/` | `text-fixups.txt` (19 lines consumed by the kernel fixup format), `text-fixups.json`, `hook-sources.json` |

## Verification performed

- `python3 -m py_compile` / AST parse of `build_ios27_runtime.py` — clean.
- C hooks reviewed against the 22H384 originals; compiled paths exercised
  only by the driver build (not run here — driver builder is out of scope).
- 19/19 fixup sites read from the live read-only-mounted cache and matched
  byte-for-byte; cache UUID gate enforced.
- End-to-end builder run: all steps green (`validated`, `verified`,
  `built` x6) producing the artifacts above.
- AppSync entitlement/identifier equality verified per patched executable;
  nested system executables hash-matched to the OS root.
- launchd merge asserts the 732 original OS jobs are unchanged (platform
  `plutil binary1` serialization, detached signature produced).

Not performed: on-device validation (no device operations were requested),
driver-build selection of the new hooks (contract documented in
`patch-inputs/hook-sources.json`), Data/Android identity handling (owned by
the parent pack flow — no inputs were modified).
