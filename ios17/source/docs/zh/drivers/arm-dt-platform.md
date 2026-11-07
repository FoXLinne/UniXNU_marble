# ArmDTPlatform

先按[适配流程](/zh/porting/platform)准备板级输入、修改源码、接入构建并逐步验证。

ARM 设备树平台专家。从 ADT 发布 CPU nub 和每个 `arm-io` 子节点。

| 仓库 | 维护者 | 范围 |
| --- | --- | --- |
| [ArmDTPlatform](https://github.com/UniXNU/ArmDTPlatform) | [UniXNU](https://github.com/UniXNU) | ARM 设备树平台专家。从 ADT 发布 CPU nub 和每个 `arm-io` 子节点。 |

## 状态

通用驱动。需要 `/cpus`、`/arm-io`、`/defaults`、`/options` 和 NVRAM proxy。

## 构建

选定内核配方通过 `component:ArmDTPlatform` 构建 `build/armdt-platform/libArmDTPlatform.a`。启用其 personality 并提供必需 ADT 节点，再检查 provider 发布。
