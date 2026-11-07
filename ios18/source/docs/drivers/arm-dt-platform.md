# ArmDTPlatform

Start with the [adaptation procedure](/porting/platform) for board inputs, source changes, build integration and verification order.

ARM device-tree platform expert. Publishes CPU nubs and every `arm-io` child from the ADT.

| Repository | Maintainer | Scope |
| --- | --- | --- |
| [ArmDTPlatform](https://github.com/UniXNU/ArmDTPlatform) | [UniXNU](https://github.com/UniXNU) | ARM device-tree platform expert. Publishes CPU nubs and every `arm-io` child from the ADT. |

## State

Generic driver. Requires `/cpus`, `/arm-io`, `/defaults`, `/options` and the NVRAM proxy.

## Build

The selected kernel recipe builds `build/armdt-platform/libArmDTPlatform.a` through `component:ArmDTPlatform`. Enable its personality and supply the required ADT nodes before checking provider publication.
