# 技术参考

从这里查找一次修改对应的文件、操作步骤和验证方法。完整流程见[移植设备](/zh/porting/steps)和[构建与启动](/zh/building)；板级参数与当前结果见[设备列表](/zh/devices/)。

## 1. 找到配置或实现

| 修改内容 | 文件或实现 | 操作步骤 |
| --- | --- | --- |
| 本地输入路径 | 主仓库 `unixnu.local.toml` | [配置构建输入](/zh/reference/configuration#local-build-inputs) |
| 设备、SoC 或组件选择 | 设备 `board.toml`、`feeds.toml`、`configs/` | [解析设备配置](/zh/porting/steps#_4-解析并检查构建配置) |
| 硬件资源 | SoC DTSI 和板级 DTS | [设备树](/zh/porting/device-tree#procedure) |
| 驱动行为 | 所选组件或设备 package | [适配硬件](/zh/porting/drivers)、[开发驱动](/zh/drivers/overview#procedure) |
| 内核行为 | 所属 profile 或组件的补丁序列 | [修改 XNU](/zh/xnu/source-and-patches#procedure) |
| XNU 启动参数 | `[payload] kernel_args` 或本地 `[build] kernel_args` | [应用启动参数](/zh/xnu/boot-arguments#apply-arguments) |
| 启动菜单或 kit | payload 的 `EFI/UniXNU/unixnu.toml`、kit 的 `Kit.toml` | [启动配置](/zh/reference/boot-configuration#procedure) |
| 设备支持记录 | 设备 `porting.toml` 与主仓库 `devices.toml` | [更新和发布记录](/zh/porting/records#procedure) |
| 文档 | 主仓库 `docs/` 中英文页面 | [编辑和发布文档](/zh/contributing/documentation#procedure) |

**完成结果：** 编辑前确定文件归属和对应操作步骤。

## 2. 查阅对应规范

| 内容 | 参考文档 |
| --- | --- |
| 构建工具 | [命令与参数](/zh/reference/cli)、[配置字段](/zh/reference/configuration)、[输出路径与报告](/zh/reference/output-layout) |
| 组件 | [组件目录与源码身份](/zh/reference/components)、[仓库和组件页面](/zh/drivers/repositories) |
| 驱动接口 | [UEFI 驱动](/zh/drivers/uefi-drivers)、[内置 XNU 驱动](/zh/drivers/xnu-drivers)、[kext 包](/zh/drivers/kexts)、[设备树匹配](/zh/drivers/device-tree-matching) |
| XNU | [源码与补丁归属](/zh/xnu/source-and-patches)、[补丁目录](/zh/xnu/patch-reference)、[4 KiB 页表](/zh/xnu/four-k-page-tables) |
| 启动与镜像格式 | [启动链](/zh/concepts/boot-chain)、[预链接](/zh/concepts/prelink)、[payload](/zh/concepts/payload)、[userdata 布局](/zh/concepts/userdata)、[启动配置](/zh/reference/boot-configuration) |
| 完整性 | [trust cache 与启动策略](/zh/concepts/security) |
| 项目结构 | [仓库归属](/zh/contributing/repository)、[术语](/zh/reference/glossary) |

字段表列出可用值和路径，操作顺序与构建命令见对应步骤链接。

## 3. 验证受影响的输出

在主仓库中选定目标设备，运行：

```sh
./unixnu status --json
```

核对设备、OS profile、配置、模块来源和输出目录，再执行对应验证：

| 改动 | 验证方法 | 预期结果 |
| --- | --- | --- |
| 内核、驱动或补丁 | [构建内核](/zh/guide/build-kernel)，再按[启动验证顺序](/zh/porting/bring-up)检查 | 内核与预链接成功，受影响的设备检查点单独记录 |
| 固件或 payload | [构建与启动](/zh/building) | 更新的 FD、payload 和启动镜像使用指定输入 |
| 启动参数 | [检查 payload 配置](/zh/xnu/boot-arguments#apply-arguments) | 打包后的 `args` 与预期命令行一致 |
| 公共 UEFI 行为 | [QEMU 操作步骤](/zh/reference/qemu#procedure) | 所选场景出现预期日志标记 |
| 适配记录或文档 | [记录检查](/zh/porting/records#procedure)、[站点构建](/zh/contributing/documentation#procedure) | 记录验证和文档构建通过 |

步骤失败时按[故障排查](/zh/reference/troubleshooting#procedure)处理，保存命令、输入版本和相关[日志](/zh/guide/debugging#procedure)。
