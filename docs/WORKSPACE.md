# 工作区维护与归档

## 目录归属

设备仓库为 `work/UniXNU-marble/`，远端为 `FoXLinne/UniXNU_marble`，维护分支为 `main`。工作区根目录承担输入、工具、上游对照和输出的容器职责。

| 目录 | 保存内容 |
| --- | --- |
| `work/UniXNU-marble/` | 源码、profiles、补丁、文档、阶段摘要和来源清单 |
| `work/upstream/` | 上游仓库及来源提交，用于对照与整合 |
| `tools/marble-build/` | Python、主机构建工具与依赖 |
| `outputs/UniXNU_marble/` | 本地内核、镜像、系统输入、Data、截图、日志与构建产物 |
| `cache/persistent_boot/` | 历史启动与 Android 分区备份、GPT、设备证据及工具 |

`work/UniXNU-docs/` 为本地参考副本；本轮设备文档在设备仓库 `docs/`、`records/` 中整理。

## 状态归档

原 CURRENT.json 全量保存于 [历史归档](../records/archive/current-2026-11-11.json)。当前入口保留设备终态、最后构建、功能范围与来源关系。专题中的旧“进行中”和“待采集”描述属于形成时状态；2026-11-11 终态为 Android 启动完成、设备操作停止。

原始诊断保存于本地 outputs；音频关键计数和状态转折点导出到 `records/audio-2026-11-11/captures.json`，并记录原文件 SHA-256。上游整合报告导出到 `records/upstream-sync-2026-11-11.json`，冲突文件内容保留在本地 `build/upstream/conflicts/`。

## 清理策略

`scripts/clean_workspace.py` 生成计划，`--apply` 消费同一计划，核对目标路径、大小和修改时间后执行。清单见 [workspace-cleanup-2026-11-11.json](../records/workspace-cleanup-2026-11-11.json)。本轮处理 840 个文件，逻辑大小 49.40 GiB；清理时测得可用空间增加 49.40 GiB。

清理对象为：同时具有 `temporary-boot.json` 和最终启动镜像的候选目录中的 `KernelSeed.bin`、`firmware.bin`、`volume.img`、`runtime.gpuaddon`、`wifi-runtime.gpuaddon`、`haptics-runtime.gpuaddon`，以及源码目录的 Python 字节码和 Finder 元数据。

保留内容包括最终启动镜像、用于符号解析的内核、构建与来源清单、最终 addon、诊断日志和截图、Data 与 seed 元数据、系统盘、IPSW、固件输入、Android 启动备份和 GPT。指定历史基线及关键失败样本的打包目录整体保留。

```sh
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/clean_workspace.py \
  --workspace "$PWD" --report /tmp/marble-cleanup-plan.json
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/clean_workspace.py \
  --workspace "$PWD" --report /tmp/marble-cleanup-plan.json --apply
```

清理后的历史候选可使用保留的最终启动镜像。重建中间件时，按对应 manifest 选择原内核、来源摘要、runtime、固件和存储输入，在独立输出目录打包。旧中间件路径作为历史记录保留，清理清单说明其移除原因。

## 仓库上传范围

提交保存设备源码实际工作树、上游快照改动、来源摘要、当前状态、文档与专题证据。部分上游冲突的待整合提交单独记录。发布状态维持实验阶段，音频失败候选保持失败标记。

系统盘、Data、IPSW、固件输入二进制和构建中间件位于仓库外或忽略目录。设备恢复后，本轮整理期间的检查均在主机执行。
