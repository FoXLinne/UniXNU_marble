# 一加 8T（kebab）

搭载 SM8250 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-kebab](https://github.com/GFander/UniXNU-kebab) | [GFander](https://github.com/GFander) |
| [UniXNU-kebab](https://github.com/stevedang67889/UniXNU-kebab) | [stevedang67889](https://github.com/stevedang67889) |
| [KebabRamDisk](https://github.com/stevedang67889/KebabRamDisk) | [stevedang67889](https://github.com/stevedang67889) |

## 当前状态

GFander 移植使用 16 KiB 页，目标为 iOS 16.7.16。实机日志确认 UFS 分区发布、HFS 根卷以及静态测试 init 在 EL0 执行，测试根卷中名为 `/sbin/launchd` 的文件是该自制程序。控制台输出、完整 Apple 用户态、其他 CPU 和 USB 集成仍在开发。

stevedang67889 的独立移植记录了可用的固件菜单、按键和屏幕日志，内核、ADT 和负载已构建，固件存储集成待完成。

## 来源

- [启动记录: GFander/UniXNU-kebab](https://github.com/GFander/UniXNU-kebab/blob/786f31707708dbde0c4ae9b95784f543113b0314/docs/BRING-UP-STATE.md)
- [README: stevedang67889/UniXNU-kebab](https://github.com/stevedang67889/UniXNU-kebab/blob/848b04fea469062b48e0eb3799d656bc111b8ce0/README.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
