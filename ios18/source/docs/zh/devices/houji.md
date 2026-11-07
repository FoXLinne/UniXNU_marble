# 小米 14（houji）

SM8650 社区设备移植。

- [lonzzi/UniXNU-houji](https://github.com/lonzzi/UniXNU-houji)

## 当前状态

设备已通过原生 UFS 和 HFSX 根卷，在八个 CPU 上进入 iOS 16.7.16 桌面。实机记录确认完整 1200x2670 显示覆盖、准确触控和最近任务卡片，界面使用 CPU 合成。“关于本机”页面会使界面无响应，此时内核仍可保存日志。

Adreno 750 诊断已验证 GMU/HFI 启动、内存权限、分配应答、HWSCHED 启动及供电/微码准备。GPU 命令提交、fence 和 Metal 界面加速待完成，仓库提供 Recovery 日志获取与运行标识说明。

## 使用此移植

1. 阅读下方固定版本的说明和记录，准备对应的固件、系统输入与构建工具。
2. 克隆设备源码，并按其 README 准备匹配的主仓库和固件源码：

```sh
git clone --branch main https://github.com/lonzzi/UniXNU-houji.git
cd UniXNU-houji
git checkout d4fb1472c86210303e36f311c07411dd486a753f
git submodule update --init --recursive
```

3. 在该版本说明指定的工作目录执行构建，核对其记录的输出和未完成输入。
4. 按[启动验证顺序](/zh/porting/bring-up)逐项确认，通过[日志抓取](/zh/porting/log-capture)保存结果，并填写[适配记录](/zh/porting/records)。板级结构和组件选择见[移植步骤](/zh/porting/steps)。

## 来源

- [README: lonzzi/UniXNU-houji](https://github.com/lonzzi/UniXNU-houji/blob/d4fb1472c86210303e36f311c07411dd486a753f/README.md)
- [实机记录: lonzzi/UniXNU-houji](https://github.com/lonzzi/UniXNU-houji/blob/d4fb1472c86210303e36f311c07411dd486a753f/docs/bringup.md)
