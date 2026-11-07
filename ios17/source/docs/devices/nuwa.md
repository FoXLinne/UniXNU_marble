# Xiaomi 13 Pro (nuwa)

A community-maintained port for SM8550. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-nuwa](https://github.com/wh1stl3/UniXNU-nuwa) | [wh1stl3](https://github.com/wh1stl3) |
| [uefi-nuwa](https://github.com/wh1stl3/uefi-nuwa) | [wh1stl3](https://github.com/wh1stl3) |
| [nuwa-dev](https://github.com/Alex-Enix/nuwa-dev) | [Alex-Enix](https://github.com/Alex-Enix) |

## State

The source kernel builds, and a temporary hardware boot records `AppleARMPlatform: IODTNVRAM`. The device reboots after about 235 seconds; the cause remains undetermined. Userland startup requires further evidence.

The board still carries `example = true`. Complete and qualify its platform inputs before using the full build workflow.

## Sources

- [README: wh1stl3/UniXNU-nuwa](https://github.com/wh1stl3/UniXNU-nuwa/blob/54b3674b8ad95ebf00d64e6cbf227391a56f180c/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
