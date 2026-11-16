# UniXNU marble

Redmi Note 12 Turbo / POCO F5（marble，SM7475，Adreno 725）的板级适配仓库。`port/ios27.2-marble` 分支目标为 iOS 27.2 / 24B119；iOS 18.7.11 / 22H384 和 iPadOS 17.7.11 / 21H461 保留来源与历史运行记录。

## 当前阶段：2026-11-16

iOS 27.2 / 24B119 真机运行版本为 v49，采用临时 RAM 启动。当前内核、固件、图形插件、镜像摘要及运行记录见 [CURRENT.json](CURRENT.json)。源码和回归用例随仓库保存；系统盘、Data、固件二进制、原始日志与截图保留在本地工作区。

| 功能 | 真机结果 |
| --- | --- |
| 系统与图形 | v49 进入锁屏、桌面与应用；PiliPlus 推荐页、列表滚动和热门页切换完成检查 |
| Wi-Fi | v47 完成自动连接及三次开关重连，DHCP 用时 1.271、1.308、0.860 秒；v49 沿用该内核，联网日志接口响应 HTTP 200 |
| USB | v48 修复主机命令中断触发看门狗重启；端口禁用 60 秒后系统持续运行，v49 沿用该固件 |
| PiliPlus | v49 支持省略扩展头的旧版 metallib；三个实际着色器库的 65 个函数完成解码，应用启动、列表显示和交互完成真机检查 |
| 其他外设 | 分项结果、历史候选和剩余范围见 [阶段状态](docs/STATUS.md) 与 CURRENT.json |

功能摘要见 [2026-11-16 验收记录](records/ios27-24B119/device-acceptance-2026-11-16.json)。历史 iOS 18 音频、触觉及图形记录保留在专题文档中。

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
| `ios17/`、`ios18/`、`ios27/`、`firmware/source/` | 设备仓库管理的来源快照及本地适配 |
| `firmware/inputs/` | 硬件输入与来源信息 |
| `records/`、`docs/` | 阶段证据、来源、状态说明和操作参考 |
| `source_origins.json`、`source_snapshot.json` | 来源提交、待整合提交和本地源码摘要 |

工作区的 `outputs/UniXNU_marble/` 保存构建与诊断产物，`tools/marble-build/` 保存主机构建环境，`work/upstream/` 保存上游对照。系统盘、Data、IPSW、固件二进制及个人设备备份在本地保留。

## 源码状态

提交前执行 `scripts/snapshot_upstream.py`，来源摘要随源码提交保存。上游同步遇到冲突时，`source_origins.json` 的 `upstream_commit` 与 `pending_upstream_commit` 分别表示已采用来源和待整合来源。构建版本读取对应产物清单与摘要。
