# 小米平板 6（pipa）

搭载 SM8250 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [uefi-pipa](https://github.com/xVanTuring/uefi-pipa) | [xVanTuring](https://github.com/xVanTuring) |
| [UniXNU-pipa](https://github.com/xVanTuring/UniXNU-pipa) | [xVanTuring](https://github.com/xVanTuring) |

## 当前状态

平板已运行 UEFI、UniXNU 菜单、DarwinBoot 和 XNU。当前运行停止于 `Failed to mount root device @IOKitBSDInit.cpp:984`，下一步是集成存储驱动。设备仓库记录了内存和 userdata 几何参数。

## 来源

- [README: xVanTuring/UniXNU-pipa](https://github.com/xVanTuring/UniXNU-pipa/blob/c28476ea5d3c66fb634152ea7cd0f5ddc7a63233/README.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
