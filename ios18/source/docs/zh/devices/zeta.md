# Microsoft Surface Duo 2（zeta）

搭载 SM8350 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-zeta](https://github.com/none24smu/UniXNU-zeta) | [none24smu](https://github.com/none24smu) |
| [uefi-zeta](https://github.com/none24smu/uefi-zeta) | [none24smu](https://github.com/none24smu) |

## 当前状态

移植已通过原生 UFS 挂载 APFS 根卷。离线 arm64e 到 arm64 重写使 iOS 26.5.2 的 `launchd` 和 `dyld` 路径能够在 EL0 执行 PID 1，并进入共享缓存代码。

执行在未应用地址偏移的指针处发生异常，分页集成仍在开发，图形界面待继续适配。

## 来源

- [README: none24smu/UniXNU-zeta](https://github.com/none24smu/UniXNU-zeta/blob/825d75028a3aefc2035164591ba9074837e39979/README.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
