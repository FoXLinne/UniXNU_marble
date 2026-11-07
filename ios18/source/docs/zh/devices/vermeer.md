# Redmi K70（vermeer）

搭载 SM8550 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-vermeer](https://github.com/mansar1337/UniXNU-vermeer) | [mansar1337](https://github.com/mansar1337) |
| [UniXNU-vermeer](https://github.com/yyx2025/UniXNU-vermeer) | [yyx2025](https://github.com/yyx2025) |
| [k70-unixnu-build](https://github.com/wuan0039/k70-unixnu-build) | [wuan0039](https://github.com/wuan0039) |
| [k70-unixnu-build-20261022](https://github.com/xyyyds0106/k70-unixnu-build-20261022) | [xyyyds0106](https://github.com/xyyyds0106) |

## 当前状态

xyyyds0106 仓库记录了通过的平台内核 CI 构建，以及单独验证的实机 UEFI 菜单。手机上的 XNU 执行待验证。

mansar1337 仓库提供源码内核构建基线，yyx2025 设备树和 wuan0039 CI 仓库提供其他源码参考。

## 来源

- [README: xyyyds0106/k70-unixnu-build-20261022](https://github.com/xyyyds0106/k70-unixnu-build-20261022/blob/8a0aa65b425593985be5b9b05e5bd72620cebb36/README.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
