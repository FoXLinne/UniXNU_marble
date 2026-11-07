# 一加 Ace 2 / 11R（udon）

SM8475 社区设备移植。

- [takasugl/UniXNU-udon](https://github.com/takasugl/UniXNU-udon)
- [takasugl/uefi-udon](https://github.com/takasugl/uefi-udon)

## 当前状态

PHK110 已通过 UEFI、DarwinBoot 和原生 UFS 进入 iPadOS 17.7.11 用户态，OS/SystemOS 和共享缓存已加载，并提供 32 MiB 可写 APFS 内存 Data 种子。一次设备运行记录了 117 个进程和 176 秒执行，随后按诊断配置重启。

可见阶段结束于 ReportCrash，信号退出仍在排查。完整界面、持久化日志获取、GPU、音频、Wi-Fi 和挂起待完成。

## 使用此移植

1. 阅读下方固定版本的说明和记录，准备对应的固件、系统输入与构建工具。
2. 克隆设备源码，并按其 README 准备匹配的主仓库和固件源码：

```sh
git clone --branch main https://github.com/takasugl/UniXNU-udon.git
cd UniXNU-udon
git checkout ad550a97f612836181dd36c6c08b1d7e90fb7b14
git submodule update --init --recursive
```

3. 在该版本说明指定的工作目录执行构建，核对其记录的输出和未完成输入。
4. 按[启动验证顺序](/zh/porting/bring-up)逐项确认，通过[日志抓取](/zh/porting/log-capture)保存结果，并填写[适配记录](/zh/porting/records)。板级结构和组件选择见[移植步骤](/zh/porting/steps)。

## 来源

- [README: takasugl/UniXNU-udon](https://github.com/takasugl/UniXNU-udon/blob/ad550a97f612836181dd36c6c08b1d7e90fb7b14/README.md)
- [构建指南: takasugl/UniXNU-udon](https://github.com/takasugl/UniXNU-udon/blob/ad550a97f612836181dd36c6c08b1d7e90fb7b14/docs/REPRODUCE.md)
