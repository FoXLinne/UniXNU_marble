# UniXNU marble

Redmi Note 12 Turbo / POCO F5（marble，SM7475，Adreno 725）的板级适配仓库。默认研究目标为 iOS 18.7.11 / 22H384；iPadOS 17.7.11 / 21H461 保留历史运行记录。

## 当前阶段：2026-11-11

手机恢复 Android 17，槽位 A，`sys.boot_completed=1`。用户要求停止设备操作。仓库保存实验源码、构建来源和故障记录。

| 功能 | 阶段结果 |
| --- | --- |
| 系统启动、UFS | iOS 候选可进入系统，UFS 接口迁移存在启动记录；Android 与 Data 身份保留 |
| 显示、GPU | 桌面和锁屏存在运行证据；渲染正确性、Safari、流畅度与稳定性保持待处理 |
| Wi-Fi | 扫描、WPA2 认证、DHCP 存在真机记录；连接提示及重连按候选验收 |
| USB | RootStage 和 Apple USB 均有运行记录；断线恢复及锁屏路径存在故障 |
| 电量、充电 | 电量、电流、充电与 Powerlog 数据存在记录；设置统计页面按阶段记录验收 |
| 扬声器 | 系统播放曾工作；锁屏后失声；末轮候选收到锁屏一次触发崩溃的反馈，验收失败 |
| 马达 | 内核自检震动；系统触觉待接通，CoreHaptics 返回 -4810 |

当前源码含实验性音频改动及部分上游整合。85 个上游仓库的同步来源与冲突清单保存在 [同步记录](records/upstream-sync-2026-11-11.json)。主机测试、构建通过和真机功能结果分别记录。

## 文档入口

| 文档 | 内容 |
| --- | --- |
| [当前状态](CURRENT.json) | 设备终态、构建摘要、功能状态、基线与历史入口 |
| [阶段说明](docs/STATUS.md) | 已取得的结果、故障、候选关系与待处理范围 |
| [构建与输入](docs/BUILDING.md) | 源码准备、固件与系统输入、构建入口、主机检查 |
| [板级集成说明](docs/BRINGUP.md) | Wi-Fi、USB、显示、身份、存储和历史命令参考 |
| [音频阶段记录](records/audio-2026-11-11/README.md) | 扬声器、系统触觉、GLINK 时序修复和末轮失败结果 |
| [工作区维护](docs/WORKSPACE.md) | 目录归属、清理范围、保留策略与本地证据 |
| [记录索引](records/README.md) | 主题入口及历史记录目录 |
| [GPU 修复目标](GPU_REPAIR_GOAL.md) | 功能范围与真机验收条件 |

## 仓库布局

| 路径 | 归属 |
| --- | --- |
| `package/`、`hal/` | 板级驱动、固件接口、系统音频插件和诊断代码 |
| `profiles/`、`patches/`、`configs/` | 输入约束、系统适配和构建配置 |
| `scripts/` | 源码准备、构建、打包、主机检查与维护脚本 |
| `ios17/`、`ios18/`、`firmware/source/` | 设备仓库管理的来源快照及本地适配 |
| `firmware/inputs/` | 硬件输入与来源信息 |
| `records/`、`docs/` | 阶段证据、来源、状态说明和操作参考 |
| `source_origins.json`、`source_snapshot.json` | 来源提交、待整合提交和本地源码摘要 |

工作区的 `outputs/UniXNU_marble/` 保存构建与诊断产物，`tools/marble-build/` 保存主机构建环境，`work/upstream/` 保存上游对照。系统盘、Data、IPSW、固件二进制及个人设备备份在本地保留。

## 源码状态

提交前执行 `scripts/snapshot_upstream.py`，来源摘要随源码提交保存。上游同步遇到冲突时，`source_origins.json` 的 `upstream_commit` 与 `pending_upstream_commit` 分别表示已采用来源和待整合来源。构建版本读取对应产物清单与摘要。
