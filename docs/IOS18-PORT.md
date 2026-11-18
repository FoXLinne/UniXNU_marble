# marble iOS 18.7.11 移植

系统版本为 22H384。分支起点为 `7a0d68ff`，iOS 27 实现参照 `780d3b2d`。候选与产物摘要见 [CURRENT.json](../CURRENT.json)。本页记录每个阶段的来源和实际结果。

| 阶段 | 实现来源与结果 |
| --- | --- |
| USB、内核日志和复位 | R1 在 MPIDR `0x100` 保持 RootStage，panic 后读出 XNU 日志并经固件命令返回 Fastboot。R2 在 DarwinBoot 末段启动驻留核心时触发固件异常。R3 保留 22H384 的 USB 交接时机和 watchdog，通过 USB 读取 IOKit panic 并回到 Fastboot。R4 在用户态可读内核日志和控制触控。R5 经 Mac 发起的单设备 USB 总线复位恢复连接，耗时 9.313 秒。物理拔插和服务停滞恢复待覆盖。 |
| UFS 和 Data | `unixnu` 扩到 256 GiB。Android 重启后 USB ADB 可用，Fastboot 报告 bootloader 已解锁。64 GiB Data 用 22H384 种子重建，APFS 超级块读回一致。22H384 的 OS、SystemOS、KernelSeed 和 UFS 清单已写入并读回。 |
| 用户态和系统 App | R4 启动 7 个 XNU 核心，恢复完成后跳过设置向导，可解锁并打开设置。R5 清理一份旧 LaunchServices 数据库后，照片可启动。Safari 和时钟图标显示等待状态。 |
| Wi-Fi | 引入 iOS 27 v47 的 supplicant 定向重连、调度唤醒合并、监管域处理和扫描缓存策略。R4 网卡注册成功，设置显示 Wi-Fi 未连接。R5 增加 22H384 wifid 使用的信号阈值和监管信道查询。自动连接、切换和 DHCP 待真机覆盖。 |
| 电源、CPU 休眠、自动熄屏 | iOS 27 v62 有三次自动锁屏、七个 XNU 核心进入和离开休眠的记录。R5 通过 22H384 的 idle 回调更新 EPSS 频率投票，屏幕关闭时限制活跃核的最高频率。整机休眠和唤醒待移植。 |
| 蓝牙 | iOS 27 v82 可双向发现，经典蓝牙连接失败。以同板 QCA 固件和 GENI 传输作起点，针对 22H384 适配 Bluetooth 栈。 |
| 扬声器和马达 | R5 启动镜像包含低音量自检。三条播放路径使用峰值约 −54 dBFS、时长 250 ms 的测试音，扬声器音量只读。播放、唤醒恢复和系统触觉请求待真机覆盖。 |
| Metal | 保留 22H384 的 Metal 接口、扫描输出和纹理约束。补齐 Adreno 内存分配实现和 Pool 构建输入，GPU 内核模块已编译。库加载、资源寿命和真机交互待处理。 |

新内核通过 Fastboot 临时启动。本轮 USB 证据记录设备枚举、内核日志和复位结果。用户态到达桌面后按需截图。

`scripts/capture_ios18_resident.py --log-only --kernel KERNEL --output DIR` 通过 RootStage 读内核消息缓冲尾部和 panic 文本，输出一份 `kernel.log`。`scripts/root_control.py reboot-bootloader` 由驻留固件复位到 Fastboot。
