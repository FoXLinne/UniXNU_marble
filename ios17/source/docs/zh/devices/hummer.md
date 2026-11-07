# 一加 Ace 5 Pro（hummer）

搭载 SM8750 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-hummer](https://github.com/2426120557/UniXNU-hummer) | [2426120557](https://github.com/2426120557) |

## 当前状态

设备已通过 UEFI、XNU、六个 CPU 和物理根卷到达 Hello。160 MiB 负载包含 OS/SystemOS 提取结果、UFS 策略和内存 Data。

持久化 Data、GPU 加速和物理触控待完成。源码包含 S3910 触控适配，仍需单独进行设备验证。

## 来源

- [README: 2426120557/UniXNU-hummer](https://github.com/2426120557/UniXNU-hummer/blob/3d3d8ccd7e9544aefa5fb14ba8baad977828b435/README.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
