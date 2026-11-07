# macOS on sheng

A community-maintained port for SM8550. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [macOS-ForSheng](https://github.com/Jelly10086/macOS-ForSheng) | [Jelly10086](https://github.com/Jelly10086) |

## State

The paired macOS 26.5 / 25F71 kernel and recovery userland run the original arm64e `launchd` in QEMU and on sheng hardware. Hardware records cover BaseSystem and ISC mounts, temporary Data and template copying. Logs persist through `macoslog` and are read back after reboot.

WindowServer repeatedly exits during HID and display initialization. Graphical Recovery remains pending. Follow the current status record for the paired inputs and hardware configuration.

## Sources

- [Status: Jelly10086/macOS-ForSheng](https://github.com/Jelly10086/macOS-ForSheng/blob/4ec9620f127fe0daa7e9e9498ee7b0d374fd032f/docs/status.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
