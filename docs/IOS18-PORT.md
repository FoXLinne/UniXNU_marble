# marble iOS 18.7.11 移植

系统版本为 22H384。分支起点为 `7a0d68ff`，iOS 27 实现参照 `780d3b2d`。候选与产物摘要见 [CURRENT.json](../CURRENT.json)。本页记录每个阶段的来源和实际结果。

| 阶段 | 实现来源与结果 |
| --- | --- |
| USB、内核日志和复位 | 保留 22H384 的 RootStage 交接。引入 iOS 27 固件独立复位、驻留 watchdog、固件日志和 panic 时保留驻留核心。固件构建及现有复位、watchdog 检查通过。待真机读取 XNU 日志，并覆盖断线重连、panic、USB 服务卡死后回到 Fastboot。 |
| UFS 和 Data | `unixnu` 扩到 256 GiB，Android 重启后 USB ADB 可用，Fastboot 报告 bootloader 已解锁。64 GiB Data 窗口清零后写入 22H384 种子。22H384 的 OS 和 SystemOS 与原候选摘要一致，正在写入 UFS。 |
| 用户态和系统 App | 使用 22H384 的 Setup 跳过逻辑。恢复完成后检查设置、照片、Safari 和其他系统 App 的启动及可用性。 |
| Wi-Fi | iOS 18 有扫描、WPA2 和 DHCP 基础。iOS 27 v47 的自动连接、开关重连和省电路径作为实现参考。真机检查自动连接、快速切换和获取网络地址。 |
| 电源、CPU 休眠、自动熄屏 | iOS 27 v62 有三次自动锁屏、七个 XNU 核心进入和离开休眠的记录。需核对 22H384 的电源控制、计时和唤醒接口，以及驻留 USB 的核心归属。 |
| 蓝牙 | iOS 27 v82 可双向发现，经典蓝牙连接失败。以同板 QCA 固件和 GENI 传输作起点，针对 22H384 适配 Bluetooth 栈。 |
| 扬声器和马达 | iOS 18 已有音频和马达驱动。比较 iOS 27 的输出路由、锁屏行为和系统触觉，分别检查播放、唤醒恢复和系统触觉请求。 |
| Metal | 保留 22H384 的 Metal 接口、扫描输出和纹理约束。补齐 Adreno 内存分配实现和 Pool 构建输入，GPU 内核模块已编译。库加载、资源寿命和真机交互待处理。 |

新内核通过 Fastboot 临时启动。本轮 USB 证据记录设备枚举、内核日志和复位结果。用户态到达桌面后按需截图。

`scripts/capture_ios18_resident.py --log-only --kernel KERNEL --output DIR` 通过 RootStage 读内核消息缓冲尾部和 panic 文本，输出一份 `kernel.log`。
