# Agent instructions

This repository owns marble board configuration, peripherals, firmware
selection, partition layout, power policy, and packaging.

Search UniXNU's `components.toml` and read existing implementations before
adding a capability.

Use the README for firmware and payload inputs. Run existing checks
relevant to the change.

## Marble 记录

- `CURRENT.json` 记录各分支选用的基线和当前候选。切换源码、内核、插件、启动参数或镜像时，更新对应的版本、路径、摘要和真机结果。首次启动前标明待启动，启动后填写实际结果。
- iOS 18 分支用 `docs/IOS18-PORT.md` 维护阶段状态。每项记录实现来源、最近一轮真机结果、已知故障和下一项工作。一次结果只保留一份摘要，原始证据用相对工作区路径引用。
- 优先复用现有检查和设备命令。新增用例或探针用于定位具体故障或覆盖现有检查缺失的可复现问题。采集限于能决定下一步的日志、计数或故障现场。截图用于核对用户态界面，结合内核日志选取时机。
- 临时产物放在工作区 `outputs/UniXNU_marble/`。保留当前候选和能复现故障的样本。清理本轮生成的重复转储、临时探针和中间产物。提交携带源码、实际构建所需配置和简要阶段结果。

## 上游来源

查阅 [UniXNU 中文移植文档](https://unixnu.com/zh/porting/) 的相关章节，用 `scripts/update_upstream.sh --fetch-only` 更新上游分支引用。按组件及其在目标系统上的实际表现选择实现，记录构建采用的提交和本地补丁。
