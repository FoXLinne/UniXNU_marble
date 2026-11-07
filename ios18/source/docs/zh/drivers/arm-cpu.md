# ArmCpu

先按[适配流程](/zh/porting/platform)准备板级输入、修改源码、接入构建并逐步验证。

ARM64 CPU、GIC 和定时器驱动。发布 CPU、中断控制器和架构定时器。

| 仓库 | 维护者 | 范围 |
| --- | --- | --- |
| [ArmCpu](https://github.com/UniXNU/ArmCpu) | [UniXNU](https://github.com/UniXNU) | ARM64 CPU、GIC 和定时器驱动。发布 CPU、中断控制器和架构定时器。 |

## 状态

组件目录指定 ARM GICv3、ARM 通用定时器及选定的 XNU ABI。

## 构建

选定内核配方通过 `component:ArmCpu` 构建 `build/armcpu/libArmCpu.a`。启用对应内核配置，按平台适配流程核对 GIC、定时器和 PSCI 资源。
