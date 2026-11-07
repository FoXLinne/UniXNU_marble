# 摩托罗拉 Moto Z2 Force（nash）

搭载 MSM8998 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-nash](https://github.com/fcxuhaq3/UniXNU-nash) | [fcxuhaq3](https://github.com/fcxuhaq3) |

## 当前状态

板级文件选择 Moto Z2 Force、MSM8998 和 `nash_defconfig`。设备补丁覆盖早期 ARM 初始化，仓库包含内核、ADT 和打包输入。该设备的实机启动证据待发布。

## 来源

- [板级配置: fcxuhaq3/UniXNU-nash](https://github.com/fcxuhaq3/UniXNU-nash/blob/df2147ca9806d64467ce1199b10fc21f4bc2d6f1/board.toml)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
