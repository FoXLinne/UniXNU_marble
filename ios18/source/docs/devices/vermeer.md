# Redmi K70 (vermeer)

A community-maintained port for SM8550. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-vermeer](https://github.com/mansar1337/UniXNU-vermeer) | [mansar1337](https://github.com/mansar1337) |
| [UniXNU-vermeer](https://github.com/yyx2025/UniXNU-vermeer) | [yyx2025](https://github.com/yyx2025) |
| [k70-unixnu-build](https://github.com/wuan0039/k70-unixnu-build) | [wuan0039](https://github.com/wuan0039) |
| [k70-unixnu-build-20261022](https://github.com/xyyyds0106/k70-unixnu-build-20261022) | [xyyyds0106](https://github.com/xyyyds0106) |

## State

The xyyyds0106 repository records a passing platform-kernel CI build and a separately verified UEFI menu on hardware. XNU execution on the phone remains pending.

The mansar1337 repository provides a source-kernel build baseline. The yyx2025 device tree and wuan0039 CI repository provide additional source references.

## Sources

- [README: xyyyds0106/k70-unixnu-build-20261022](https://github.com/xyyyds0106/k70-unixnu-build-20261022/blob/8a0aa65b425593985be5b9b05e5bd72620cebb36/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
