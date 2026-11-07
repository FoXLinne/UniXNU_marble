# 小米 Civi 5 Pro（luming）

搭载 SM8735 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [-UniXNU-luming](https://github.com/infinityEX-RSXQ/-UniXNU-luming) | [infinityEX-RSXQ](https://github.com/infinityEX-RSXQ) |

## 当前状态

仓库记录了实测板级输入，源码和 ADT 宿主机检查通过。内核、预链接和 UEFI 构建待完成。

## 来源

- [README: infinityEX-RSXQ/-UniXNU-luming](https://github.com/infinityEX-RSXQ/-UniXNU-luming/blob/0bb65d9cd4b5cc6a65eb2665d29a0ba1ca153a43/README.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
