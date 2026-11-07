# Xiaomi Pad 5 Pro 5G (enuma)

A community-maintained port for SM8250-AC. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-enuma](https://github.com/wh1stl3/UniXNU-enuma) | [wh1stl3](https://github.com/wh1stl3) |
| [UniXNU-enuma](https://github.com/Tlrenhb/UniXNU-enuma) | [Tlrenhb](https://github.com/Tlrenhb) |
| [uefi-enuma](https://github.com/Tlrenhb/uefi-enuma) | [Tlrenhb](https://github.com/Tlrenhb) |
| [uefi-enuma](https://github.com/wh1stl3/uefi-enuma) | [wh1stl3](https://github.com/wh1stl3) |

## State

The source assembles prelink, ADT, FAT and boot-v3 payload inputs. Hardware diagnostics record bounded framebuffer reads in EL1 through calibrated timing observations. Visible GOP output and the UniXNU menu remain pending.

The current pixel candidate passes offline build and qualification checks. Device display validation, kernel startup, storage and userland require their own results.

## Sources

- [Status: wh1stl3/UniXNU-enuma](https://github.com/wh1stl3/UniXNU-enuma/blob/3be6fbff66613f7cdf610ed0371e76f69b383792/docs/STATUS.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
