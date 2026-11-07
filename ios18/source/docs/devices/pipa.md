# Xiaomi Pad 6 (pipa)

A community-maintained port for SM8250. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [uefi-pipa](https://github.com/xVanTuring/uefi-pipa) | [xVanTuring](https://github.com/xVanTuring) |
| [UniXNU-pipa](https://github.com/xVanTuring/UniXNU-pipa) | [xVanTuring](https://github.com/xVanTuring) |

## State

The tablet runs UEFI, the UniXNU menu, DarwinBoot and XNU. The current run stops at `Failed to mount root device @IOKitBSDInit.cpp:984`. Storage-driver integration is the next step. Board memory and userdata geometry are recorded in the device repository.

## Sources

- [README: xVanTuring/UniXNU-pipa](https://github.com/xVanTuring/UniXNU-pipa/blob/c28476ea5d3c66fb634152ea7cd0f5ddc7a63233/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
