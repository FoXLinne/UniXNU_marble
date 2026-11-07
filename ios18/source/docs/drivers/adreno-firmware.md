# AdrenoFirmware

Start with the [adaptation procedure](/porting/gpu) for board inputs, source changes, build integration and verification order.

Adreno GMU firmware and HFI kit. Loads the GMU firmware and speaks the HFI protocol.

| Repository | Maintainer | Scope |
| --- | --- | --- |
| [AdrenoFirmware](https://github.com/UniXNU/AdrenoFirmware) | [UniXNU](https://github.com/UniXNU) | Adreno GMU firmware and HFI kit. Loads the GMU firmware and speaks the HFI protocol. |

## State

Used by Adreno740.

## Build

The board GPU archive recipe compiles the firmware/HFI kit and embeds the pinned local firmware images. Follow [GPU adaptation](/porting/gpu) for extraction paths, board votes and startup checkpoints.
