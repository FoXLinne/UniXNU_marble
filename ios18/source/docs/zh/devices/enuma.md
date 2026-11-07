# 小米平板 5 Pro 5G（enuma）

搭载 SM8250-AC 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-enuma](https://github.com/wh1stl3/UniXNU-enuma) | [wh1stl3](https://github.com/wh1stl3) |
| [UniXNU-enuma](https://github.com/Tlrenhb/UniXNU-enuma) | [Tlrenhb](https://github.com/Tlrenhb) |
| [uefi-enuma](https://github.com/Tlrenhb/uefi-enuma) | [Tlrenhb](https://github.com/Tlrenhb) |
| [uefi-enuma](https://github.com/wh1stl3/uefi-enuma) | [wh1stl3](https://github.com/wh1stl3) |

## 当前状态

源码可组装预链接、ADT、FAT 和 boot-v3 负载输入。实机诊断通过校准后的时序观察记录了 EL1 中限定范围的帧缓冲读取。可见 GOP 输出和 UniXNU 菜单待完成。

当前像素候选通过离线构建和资格检查，设备显示验证、内核启动、存储和用户态仍需分别提供结果。

## 来源

- [状态: wh1stl3/UniXNU-enuma](https://github.com/wh1stl3/UniXNU-enuma/blob/3be6fbff66613f7cdf610ed0371e76f69b383792/docs/STATUS.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
