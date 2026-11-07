# Motorola Moto Z2 Force (nash)

A community-maintained port for MSM8998. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-nash](https://github.com/fcxuhaq3/UniXNU-nash) | [fcxuhaq3](https://github.com/fcxuhaq3) |

## State

The board file selects Moto Z2 Force, MSM8998 and `nash_defconfig`. Device patches cover early ARM initialization and the board includes kernel, ADT and packaging inputs. Target-specific hardware boot evidence remains to be published.

## Sources

- [Board configuration: fcxuhaq3/UniXNU-nash](https://github.com/fcxuhaq3/UniXNU-nash/blob/df2147ca9806d64467ce1199b10fc21f4bc2d6f1/board.toml)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
