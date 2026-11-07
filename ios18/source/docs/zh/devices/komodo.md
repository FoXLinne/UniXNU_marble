# Google Pixel 9 Pro XL（komodo）

搭载 Tensor G4 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-komodo](https://github.com/Trijal08/UniXNU-komodo) | [Trijal08](https://github.com/Trijal08) |

## 当前状态

实机测试已到达固件菜单、测试负载，并使用预构建的 uke XNU 镜像运行入口探针，记录包含 MMU 启用后的重启探针。Tensor 平台原生支持和根卷集成待完成。

## 来源

- [板级配置: Trijal08/UniXNU-komodo](https://github.com/Trijal08/UniXNU-komodo/blob/5a4cfddef602f11c1e5a1836a089e83f1794504c/board.toml)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
