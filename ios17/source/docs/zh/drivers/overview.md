# 开发驱动

适配硬件请按[驱动清单](/zh/porting/drivers)操作，其中按依赖顺序列出组件驱动和板级 package 服务。

UniXNU 有三层驱动，分别运行在不同环境，构建和加载方式不同。

| 层 | 运行环境 | 构建产物 | 加载方 |
| --- | --- | --- | --- |
| UEFI 驱动 | UEFI boot services | EDK II `.efi` | 固件 dispatch、UniXNU `[[driver]]` 或 kit |
| 内置 XNU 驱动 | XNU 内核 | 链接进内核的静态库 | 内核启动时 |
| kext 包 | XNU 内核，由 UEFI 暂存 | `.kext` 包 | 实现协议的 XNU 加载器 |

## 操作步骤 {#procedure}

1. 按硬件接口搜索[组件目录](/zh/reference/components)和[已有仓库](/zh/drivers/repositories)，阅读候选实现的范围、ABI、依赖和源码。
2. 根据能力需要的阶段[选择执行层次](#选择层次)：固件、内置 XNU 驱动，或匹配加载器的 kext 路径。
3. 按[匹配](#匹配)和[设备树规则](/zh/drivers/device-tree-matching)确定 provider 与资源，记录真实板级值。
4. 执行对应实现步骤：[UEFI](/zh/drivers/uefi-drivers)、[内置 XNU](/zh/drivers/xnu-drivers#procedure)或 [kext 暂存](/zh/drivers/kexts#procedure)，将组件和配方加入设备选择。
5. 构建受影响的产物，按子系统[适配检查](/zh/porting/drivers)验证，在 [porting.toml](/zh/porting/records)中分别记录源码、构建结果和设备行为。

## 选择层次

| 需求 | 层次 |
| --- | --- |
| XNU 启动前可用的显示、输入或存储 | UEFI 驱动 |
| XNU 内的平台专家、CPU、中断控制器、存储、显示或 GPU | 内置 XNU 驱动 |
| 由外部加载器在运行时校验并注入的包 | kext 包 |

UEFI 驱动可以为 UniXNU 菜单提供 framebuffer 和输入，也可以发布供其他 UEFI 驱动使用的协议。

内置 XNU 驱动可以使用完整内核 API，按实际 XNU 构建 ABI 编译，并强制链接进内核。

kext 包保持标准 bundle 布局。UniXNU 暂存 plist 和可执行文件并交给加载器，校验、依赖解析和注入由加载器负责。

## 匹配

UEFI 驱动通过 UEFI driver model 绑定，或作为应用运行。

XNU 驱动通过 `IOKitPersonalities` 绑定。personality 指定 provider 类、驱动类、可选名称匹配和 probe 分数。

| 键 | 含义 |
| --- | --- |
| `IOClass` | 驱动类 |
| `IOProviderClass` | Provider 类 |
| `IONameMatch` | Provider 名称或 `compatible` 值 |
| `IOProbeScore` | 分数越高越先 probe |
| `IOMatchCategory` | 允许第二个驱动匹配同一 provider |
| `IOResourceMatch` | 等待命名资源，例如 `IOBSD` |

内置 personality 位于 `iokit/KernelConfigTables.cpp`，kext personality 位于 bundle 的 `Info.plist`。

## 设备树

ADT 描述硬件。平台专家为每个 `arm-io` 子节点发布 nub。驱动从 provider 读取资源：

```cpp
OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
```

`compatible` 属性用于选择驱动，`IONameMatch` 与 provider 名称比较。

## 驱动目录

| 目录 | 内容 |
| --- | --- |
| `drivers/<Name>` | 内置 XNU 驱动源码和参考 `Info.plist` |
| `kits/<Name>` | 链接进驱动或内核的内核库 |
| `package/UniXNUPkg/Driver` | UEFI 驱动 |
| `package/UniXNUPkg/Test` | UEFI 测试驱动和负载 |

驱动和 kit 是独立仓库，以子模块形式引入。设备仓库可以覆盖其中任何目录。

## 下一步

- [驱动仓库](/zh/drivers/repositories)
- [UEFI 驱动](/zh/drivers/uefi-drivers)
- [XNU 驱动](/zh/drivers/xnu-drivers)
- [Adreno 740 驱动](/zh/drivers/adreno740)
- [kext 包](/zh/drivers/kexts)
- [设备树匹配](/zh/drivers/device-tree-matching)
