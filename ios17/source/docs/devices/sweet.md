# Redmi Note 10 Pro (sweet)

A community-maintained port for SM7150. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-sweet](https://github.com/CrackerCat/UniXNU-sweet) | [CrackerCat](https://github.com/CrackerCat) |
| [UniXNU-sweet](https://github.com/zeroninevibedslop/UniXNU-sweet) | [zeroninevibedslop](https://github.com/zeroninevibedslop) |

## State

The CrackerCat source snapshot records kernel/platform builds and QEMU CPU/timer checks. Device firmware and root integration for that tree remain pending.

The separate zeroninevibedslop port targets iOS 16.7.16 / Darwin 22. Its maintainer reports UFS root mounting, `launchd`, SpringBoard/backboardd surface swaps and USB CDC-ECM telemetry. Adreno 618 power and GMU/HFI startup are recorded; GPU command submission and Metal acceleration remain in progress. Follow each repository's own OS inputs and build path.

## Sources

- [Source baseline: CrackerCat/UniXNU-sweet](https://github.com/CrackerCat/UniXNU-sweet/blob/d3dd6fdf9e889f08b2564d92f7fef5b5c913263e/README.md)
- [iOS 16 port: zeroninevibedslop/UniXNU-sweet](https://github.com/zeroninevibedslop/UniXNU-sweet/blob/843c8ed5398388cf1116b095fb6ef30dbed6b2ad/README.md)

## Build

Host checks:

```sh
python3 scripts/check-host.py
python3 scripts/make-sweet-adt.py --output out/sweet.adt
```

Device repository integration:

```sh
python3 ../UniXNU/unixnu defconfig sweet_defconfig -D . --out out/driver
python3 ../UniXNU/unixnu status -D . --out out/driver
```

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
