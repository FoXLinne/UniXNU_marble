# 小米 10（umi）

umi 是搭载 SM8250 的小米 10。本移植由 [reAAAq](https://github.com/reAAAq)
独立维护，UniXNU 组织不负责维护。已测试的 12 GiB / 256 GB 手机进入了
iPadOS 17.7.11 Hello／设置向导，第 80 版已由机主确认 XNU 下 ST FTS 触摸可用。

| 仓库 | 内容 |
| --- | --- |
| [UniXNU-umi](https://github.com/reAAAq/UniXNU-umi) | 板级配置、设备驱动、XNU 补丁、诊断和主机工具 |
| [uefi-umi](https://github.com/reAAAq/uefi-umi) | 从 uefi-sheng 派生的 Mu-Silicium UEFI 移植 |

## 状态

以下是维护者截至 2026-10-23 第 80 轮真机测试报告的状态。
[验证摘要](https://github.com/reAAAq/UniXNU-umi/blob/main/docs/validation.json)
记录了部分测量结果和镜像哈希，原始设备转储保留在本地。

| 项目 | 状态 |
| --- | --- |
| UEFI 菜单和 DarwinBoot | 继承显存、实体菜单键和 RAM FAT 负载可用 |
| XNU、GIC 和定时器 | 真机运行，已记录 8 核在线及各核 timer／IPI 活动 |
| Native UFS 和根卷 | OS、SystemOS 已挂载，launchd 及系统服务运行 |
| 可写 Data | 在测试分区布局下跨重启持久化 |
| 显示和用户态 | CPU framebuffer 显示苹果 Logo 及 Hello／设置向导 |
| 触摸 | XNU 下可用；第 80 版机主已确认真实 ST FTS 触摸有效 |
| 完成设置／进入桌面 | 未验证 |
| GPU 加速 | 未实现 |
| Wi-Fi、蓝牙、蜂窝、音频、相机和传感器 | 未实现或未完成验证 |
| XNU USB、电池管理、休眠唤醒 | 未验证；Recovery ADB 不等于 XNU USB 支持 |

## 板级和系统输入

| 项目 | 值 |
| --- | --- |
| Board／kit | `umi` / `umi` |
| SoC | `SM8250` |
| 系统输入 | `iPad7,1`、iPadOS `17.7.11`、构建 `21H461`、`j120ap` |
| 系统输入 chip／board ID | `0x8011` / `12` |
| 源码 XNU | `10063.141.1`，Darwin `23.6.0` |
| 原始二进制内核 | `10063.142.1.703.8`，与源码版本不同 |
| 实测 userdata | 起始 LBA `0x2c0000`，结束 LBA `0x3b51ffa`，4096 字节块 |
| 固件配置／DSC | `umi` / `Platforms/Xiaomi/umiPkg/umi.dsc` |
| 固件卷 | `Build/umiPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` |
| Android boot 镜像 | v1 头部，4096 字节页 |
| 实测根卷负载 | 124 MiB FAT，`0x130000000` owned-memory 窗口 |
| CPU affinity／PSCI | `0,0x100,0x200,0x300,0x400,0x500,0x600,0x700` / `smc` |

分区范围和内存窗口来自一台手机的测量，不能作为所有小米 10 的通用安装默认值。
该测试安装替换了 Android userdata，同时保留 Recovery 用于采集 RAM 日志。

## 源码与构建

设备仓库的 `source-lock.json` 固定了已验证的框架和固件版本。
默认 `board.toml`／`umi_defconfig` 选择最小源码内核，完整 Hello 配置由设备的
bring-up 工具组装。不能把基线参数当作最终根卷配置，也不能直接用最新上游
`main` 替换固定框架版本。

```sh
git clone https://github.com/reAAAq/UniXNU-umi
git clone https://github.com/reAAAq/uefi-umi
git clone https://github.com/UniXNU/UniXNU
git clone https://github.com/UniXNU/UniXNU-sheng
```

按 `source-lock.json` 检出各依赖版本，再参照设备仓库的
[工作区与构建说明](https://github.com/reAAAq/UniXNU-umi/blob/main/docs/BUILD.md)。
完整镜像复现仍需本地 ABI／来源记录、实测手机 DTB、已验证的固件导入、匹配的
Apple 组件及 packed Data 种子。这些输入不随仓库分发。当前发布开发源码，
不提供一键安装程序或可下载的启动镜像。

触摸使用原生 GPI DMA I²C 上的 ST FTM5。第 80 版在 FTS 启动前
[启用原生配置中的 PM8150B GPIO5 常开电源](https://github.com/reAAAq/UniXNU-umi/commit/138d6cb)，
包含身份、所有权检查及读回校验，随后机主确认触摸有效。
该控制器不使用 sheng 的 Novatek／SPI 固件和矩阵处理算法。

第 80 版的坐标／HID 计数尚未采集，当前触摸状态来自机主的真机交互确认。
多点触控、休眠唤醒、UEFI 菜单触摸及完成设置仍未验证。

## 维护

维护者负责仓库和发行决策，本页记录社区移植的设备状态。
