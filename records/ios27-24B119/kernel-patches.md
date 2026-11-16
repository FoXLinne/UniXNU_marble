# Marble kernel patches — iOS 27.2 (24B119), XNU 12377.121.6

Base: `xnu-12377.121.6` @ `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`.
Authoritative upstream: `work/upstream/UniXNU` @ `6b24c06`, `work/upstream/UniXNU-sheng` @ `abfc15a`.

## Application order (required)

1. `ios27/source/profiles/arch/arm64e/patches/xnu/series` — 69 patches (parent pipeline)
2. SM8550 common — `sheng-platform-power.patch`, `native-smp-secondary.patch` — must precede the OS series: `zzz-boot-progress.patch` anchors on the `sheng_pmgr_run` hunk
3. `ios27/source/profiles/os/ipados-27.0.1-24A446/patches/xnu/series` — 4 patches
4. `work/upstream/UniXNU-sheng/patches/xnu-12377/series` — 16 patches
5. `patches/xnu-12377/series` — 10 marble patches (this port)

## Marble series

| Patch | Files | Provenance / hooks |
| --- | --- | --- |
| `native-rtc.patch` | `iokit/Kernel/IOPlatformExpert.cpp` | verbatim port; calls `native_marble_rtc_{get,set}` (package/MarbleRtc.cpp) |
| `fractional-mhz-microtime.patch` | `osfmk/arm/rtclock.c` | verbatim port; scales the abstime remainder for the 19.2 MHz timer |
| `early-video-console.patch` | `pexpert/arm/pe_init.c` | verbatim port; `early_console`/`-v` select the text console |
| `release-msgbuf-size.patch` | `bsd/kern/subr_log.c` | identical hunk; `msgbuf=` startup outside `CONFIG_XNUPOST` |
| `gpu-trust-cache.patch` | `bsd/kern/kern_trustcache.c` | identical hunk; `ARM64_BOARD_CONFIG_BCM2837` allows engineering caches for the payload-appended GPU trust module |
| `metal-cache-access.patch` | `security/mac_vfs.c` | applied clean; Metal cache vnode-boundary authorization |
| `metal-cache-times.patch` | `security/mac_vfs.c` | applied clean; cumulative after access |
| `haptics-device-access.patch` | `security/mac_vfs.c` | applied clean; `native_marble_haptics_vnode` weak hook |
| `phone-ui-startup.patch` | `bsd/kern/kern_exec.c`, `security/mac_base.c` | applied clean; `native_marble_ui_dyld_policy` weak hook |
| `marble-platform-hooks.patch` | `bsd_init.c`, `kern_exec.c`, `kern_exit.c`, `ubc_subr.c`, `bind_vfsops.c`, `debug.c`, `machine_routines.c`, `pe_bootargs.c` | marble hunks extracted from `arm64e.patch`: `native_marble_pmgr_start`, `native_root_block_authorized` (only when `XNU_OWNER_ROOT_AUDIT` unset), `native_marble_root_start`/`display_start`, `native_marble_prepare_systemos`, `native_marble_cellular_text_fixup` (+ `CS_TEXT_FIXUP_BYTES` 16→32 for the 24-byte entries), `native_marble_bindfs_allowed`, `native_marble_ui_exit`, `native_resident_record_panic`/`native_resident_panic_return` (under `XNU_ARM_DT_PLATFORM`), `native_root_block_boot_args`, `_enable_virtual_timer` physical-timer guard under `XNU_ARMCPU` |

## Inherited replacements (not ported)

| iOS 18 patch | 12377 replacement |
| --- | --- |
| `arm64e.patch` bulk | upstream arch series: PAC/TCR/pmap, 4K user ABI + hash cache, text fixups, union fs, work intervals, sandbox grants, fileset-unmapped, trustcache layout, `MASTER.arm64.bcm2837`, CoreEntitlements, exports |
| `native-union-statfs-fsid.patch` | upstream `native-union-statfs-fsid.patch` |
| `native-pac-el0.patch` | upstream `native-pac-arm64-user-jop.patch` (superset) |
| `appsync-user-trust.patch` | sheng `sheng-appsync-user-trust.patch` (identical platform-binary logic + runtime registration; accepts any `.app` under `/var`) |
| `sheng-amp.patch` | sheng `zzyf-sheng-amp.patch` + sched_clutch `sheng_edge` policy |
| `sheng-owner-dataprotect.patch` | upstream `NATIVE_DATAPROTECT` emulation in `kpi_vfs.c` (`owner-dataprotect=1`) |
| `wifi-association-event-cluster.patch` | sheng `sheng-wifi-association-event.patch` (cluster mbuf only for `KEV_IEEE80211_CLASS`) |
| `architectural-timer-entry.patch` | obsolete — BCM2837 timer-routing block removed from `start.s` |
| `native-trust-cache-runtime.patch` | obsolete — `trust_cache_runtime_init()` now unconditional |
| `non-apple-pmap-field.patch` | inherited — `pmap.h` already has `disable_jop`/`reserved13` |
| `published-freezer-condition.patch` | inherited — fixed upstream with XR exclusion |
| `disabled-deferred-reclaim.patch` | obsolete — call site gone in `vm_user.c` |
| `offline-pgz-alignment-probe` / `pgz-aligned-validation` | not ported — 4K-transition diagnostics superseded by upstream `native-4k-*`; re-add if zone misalignment recurs |

## Verification (source only; no device operations)

Fresh baseline worktree at `build/ios27-patch-port/xnu-12377/xnu-12377-verify` (`e9e8a6c` = base + all upstream series):

```
while read p; do git apply --check $M/$p && git apply $M/$p; done < patches/xnu-12377/series
# OK #1 native-rtc ... OK #10 marble-platform-hooks — 10/10
diff -r -q xnu-12377-verify <authored tree>   # identical
```

Upstream series all apply clean in the order above (arch 69/69, sm8550 2/2, os 4/4, sheng 16/16).

Fixes made during verification: `metal-cache-times`/`haptics-device-access` regenerated as incremental diffs; `phone-ui-startup` gained its `security/mac_base.c` hunk.

## Unresolved gaps

- `package/MarbleUiPolicy.c` text-fixup entries name the iOS 18 (22H384) shared cache; they no-op on iOS 27 until updated for the 24B119 cache (match-checked, so safe).
- RTC/audio/haptics hooks depend on `package/` drivers owned by the parent agent.
- No compile or hardware run in this scope — `git apply` verification only; hardware qualification pending.
