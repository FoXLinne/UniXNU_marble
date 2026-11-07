# 一加平板 Pro（caihong）

搭载 SM8650 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-caihong](https://github.com/Axiang-dev/UniXNU-caihong) | [Axiang-dev](https://github.com/Axiang-dev) |

## 当前状态

APFS 恢复内存盘已作为 `md0` 挂载，`/sbin/launchd` 已执行，文本控制台可用。IOFramebuffer 集成、输入和完整系统根卷仍在开发。

## 来源

- [README: Axiang-dev/UniXNU-caihong](https://github.com/Axiang-dev/UniXNU-caihong/blob/b57b7656b09133979975d74679e1d37e2b6c5d37/README.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
