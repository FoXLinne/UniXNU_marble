# AdrenoPM4

Start with the [adaptation procedure](/porting/gpu) for board inputs, source changes, build integration and verification order.

Adreno PM4 packet encoding kit. Builds the command streams Adreno740 submits.

| Repository | Maintainer | Scope |
| --- | --- | --- |
| [AdrenoPM4](https://github.com/UniXNU/AdrenoPM4) | [UniXNU](https://github.com/UniXNU) | Adreno PM4 packet encoding kit. Builds the command streams Adreno740 submits. |

## State

Used by Adreno740.

## Build

The board GPU archive recipe compiles `AdrenoPM4.c` with the selected GPU driver. Follow [GPU adaptation](/porting/gpu) to match packet support and command submission before enabling the frontend.
