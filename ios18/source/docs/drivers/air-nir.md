# AirNir

Start with the [adaptation procedure](/porting/gpu) for board inputs, source changes, build integration and verification order.

Offline AIR to NIR to IR3 compiler kit. Qualifies the shader path without a GPU submission.

| Repository | Maintainer | Scope |
| --- | --- | --- |
| [AirNir](https://github.com/UniXNU/AirNir) | [UniXNU](https://github.com/UniXNU) | Offline AIR to NIR to IR3 compiler kit. Qualifies the shader path without a GPU submission. |

## State

Offline tooling. The runtime Metal path uses metal2vulkan and Turnip.

## Build

Use the kit README for its offline compiler build and supported inputs. For device graphics integration, follow [GPU adaptation](/porting/gpu) and the runtime compiler steps on [Adreno740](/drivers/adreno740).
