# OnePlus Ace 5 (giulia)

A community-maintained port for SM8650. Current results are based on the maintainer records at the revisions linked below.

| Repository | Maintainer |
| --- | --- |
| [UniXNU-OnePlus-Ace5](https://github.com/tycykp/UniXNU-OnePlus-Ace5) | [tycykp](https://github.com/tycykp) |

## State

The iOS 16 / Darwin 22 port reaches Hello and Setup with full 1264x2780 display coverage and aligned touch. CPU0, native UFS, HFSX root, `launchd` and USB CDC logs work. Physical power and volume keys are verified in the minimal key build.

Wi-Fi reaches PCIe, BHI/BHIE, AMSS and QMI resource requests; the device run stops at QMP open before firmware-ready. GPU preparation and audio runtime code are present. GPU execution, radio networking and audio playback require device validation.

## Sources

- [README: tycykp/UniXNU-OnePlus-Ace5](https://github.com/tycykp/UniXNU-OnePlus-Ace5/blob/aaa100d19f6fb14e5897a7e496edf7838d4d4e9f/README.md)

## Use this port

1. Read the repository build instructions and pinned records. Prepare its firmware, system images and board inputs.
2. Build with that repository's commands. Use [Build and boot](/building) for common configuration.
3. Follow the [bring-up order](/porting/bring-up) and save results through [log capture](/porting/log-capture).
4. Update the [porting record](/porting/records) with the source revisions and last confirmed stage.
