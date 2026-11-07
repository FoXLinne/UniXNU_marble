# AirNir

先按[适配流程](/zh/porting/gpu)准备板级输入、修改源码、接入构建并逐步验证。

离线 AIR 到 NIR 到 IR3 编译器 kit。在没有 GPU 提交的情况下验证 shader 路径。

| 仓库 | 维护者 | 范围 |
| --- | --- | --- |
| [AirNir](https://github.com/UniXNU/AirNir) | [UniXNU](https://github.com/UniXNU) | 离线 AIR 到 NIR 到 IR3 编译器 kit。在没有 GPU 提交的情况下验证 shader 路径。 |

## 状态

离线工具。运行时 Metal 路径使用 metal2vulkan 和 Turnip。

## 构建

离线编译器构建和输入格式见 kit README。设备图形集成按 [GPU 适配](/zh/porting/gpu)和 [Adreno740](/zh/drivers/adreno740)中的运行时编译步骤操作。
