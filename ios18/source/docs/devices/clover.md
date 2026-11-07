# Xiaomi Mi Pad 4 LTE (clover)

A community-maintained port for SDM660. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-clover](https://github.com/qine233/UniXNU-clover) | [qine233](https://github.com/qine233) |

## State

An eMMC-backed mockfs root runs a static EL0 test init. Device records cover fork/exec, Mach calls and a kernel panic triggered from EL0.

The current source contains APFS publication, mount and file-readback probes. Full Apple userland and those APFS checkpoints need matching device records.

## Sources

- [README: qine233/UniXNU-clover](https://github.com/qine233/UniXNU-clover/blob/7dec18936cebae5c4a1bf9c1b7b4db48643ac788/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
