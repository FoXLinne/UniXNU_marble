# Redmi K30 5G（picasso）

搭载 SM7250 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-picasso](https://github.com/YinCQL/UniXNU-picasso) | [YinCQL](https://github.com/YinCQL) |

## 当前状态

UEFI 已启动菜单和 DarwinBoot，XNU 到达 BSD 根设备等待。内核加载了 AMFI、AppleImage4、pthread 和 MAC 框架，初始化网络核心并注册 APFS。

当前适配任务为 UFS 根卷挂载。源码包含 USB 控制台工作，完整用户态和 GPU 支持仍需后续设备结果。

## 来源

- [README: YinCQL/UniXNU-picasso](https://github.com/YinCQL/UniXNU-picasso/blob/11d7ea8e78976e84663ea4b19e656a5564e6c40e/README.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
