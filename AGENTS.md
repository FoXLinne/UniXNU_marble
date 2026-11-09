# Agent instructions

This repository owns marble board configuration, peripherals, firmware
selection, partition layout, power policy, and packaging.

Search UniXNU's `components.toml` and read existing implementations before
adding a capability.

Use the README for firmware and payload inputs. Run existing checks
relevant to the change.

当前候选、必须采用的基线和真机结果见 `CURRENT.json`。每次更换源码、内核、插件、启动参数或镜像时，立即更新该文件的版本、路径、摘要和运行结果。

工作前阅读 UniXNU 中文移植文档 https://unixnu.com/zh/porting/ 的相关章节，并执行 scripts/update_upstream.sh 更新全部上游开发分支。提交标识用于记录实际来源，构建采用更新后的设备源码。
