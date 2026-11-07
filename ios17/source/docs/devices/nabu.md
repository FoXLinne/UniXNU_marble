# Xiaomi Pad 5 (nabu)

A community-maintained port for SM8150. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [unixnu-nabu](https://github.com/Lux12138/unixnu-nabu) | [Lux12138](https://github.com/Lux12138) |
| [unixnu-nabu](https://github.com/zzc775/unixnu-nabu) | [zzc775](https://github.com/zzc775) |
| [unixnu-nabu-build](https://github.com/3214791900-del/unixnu-nabu-build) | [3214791900-del](https://github.com/3214791900-del) |
| [uefi-nabu-build](https://github.com/3214791900-del/uefi-nabu-build) | [3214791900-del](https://github.com/3214791900-del) |
| [NativeDwc3](https://github.com/Lux12138/NativeDwc3) | [Lux12138](https://github.com/Lux12138) |

## State

XNU reaches `load_init_program()` and attempts `/sbin/launchd`. The recorded run fails with `errno 2` (`ENOENT`); root contents need correction. Source-kernel builds run locally on Linux.

The recorded memory layout belongs to a tablet upgraded to 12 GiB RAM. Measure DRAM and partition values for the target tablet before using its configuration.

## Sources

- [Status: Lux12138/unixnu-nabu](https://github.com/Lux12138/unixnu-nabu/blob/2eff53cdba9f45228e70c80102158fd0bc755236/STATUS.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
