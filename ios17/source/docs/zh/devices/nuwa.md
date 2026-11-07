# 小米 13 Pro（nuwa）

搭载 SM8550 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-nuwa](https://github.com/wh1stl3/UniXNU-nuwa) | [wh1stl3](https://github.com/wh1stl3) |
| [uefi-nuwa](https://github.com/wh1stl3/uefi-nuwa) | [wh1stl3](https://github.com/wh1stl3) |
| [nuwa-dev](https://github.com/Alex-Enix/nuwa-dev) | [Alex-Enix](https://github.com/Alex-Enix) |

## 当前状态

源码内核可构建，临时实机启动已记录 `AppleARMPlatform: IODTNVRAM`。设备约 235 秒后重启，原因未查明。用户态启动仍待验证。

板级文件仍设有 `example = true`，使用完整构建流程前需要补齐并验证平台输入。

## 来源

- [README: wh1stl3/UniXNU-nuwa](https://github.com/wh1stl3/UniXNU-nuwa/blob/54b3674b8ad95ebf00d64e6cbf227391a56f180c/README.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
