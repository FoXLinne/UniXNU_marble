# 小米 10S（thyme）

搭载 SM8250-AC 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-thyme](https://github.com/tobyfox1640/UniXNU-thyme) | [tobyfox1640](https://github.com/tobyfox1640) |
| [UniXNU-thyme](https://github.com/Coryyi/UniXNU-thyme) | [Coryyi](https://github.com/Coryyi) |

## 当前状态

Coryyi 板级文件标识为小米 10S，并记录 SM8250 系列的实测板级输入，仓库包含内核配置和早期跟踪代码。tobyfox1640 仓库提供另一份板级、defconfig 和 ADT 实现。目标设备的启动和用户态结果待发布。

## 来源

- [板级配置: tobyfox1640/UniXNU-thyme](https://github.com/tobyfox1640/UniXNU-thyme/blob/8efba539ce54b34e576e71799498d287c6eba894/board.toml)
- [板级配置: Coryyi/UniXNU-thyme](https://github.com/Coryyi/UniXNU-thyme/blob/8728dc4666759998a7873966bd220702af638fec/board.toml)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
