# marble 硬件 Wi-Fi 接入调研

日期：2026-11-08。目标系统：iOS 18.7.11 / 22H384。

## 结论

marble 设备树声明 QCA6490，使用 PCIe0。建议路线为：SM7475 板级电源与 PCIe 驱动 → ath11k 候选后端 → UniXNU 的 cfg80211/mac80211、认证与网络接口集成。

首个硬件里程碑是读取端点 PCI ID、subsystem ID 和 SoC 版本。Linux ath11k 为 `17cb:1103` 提供 WCN6855 路径，并按硬件版本选择 hw2.0 / hw2.1；QCA6490 的实际端点信息用于决定这条路径及固件变体。设备树还带有 `qcom,same-dt-multi-dev`，因此本报告保留实际端点识别作为后端选择条件。[1][2]

UniXNU 的 Ath12kWifi 提供可复用的 OS 与协议栈基础。其生产清单选择 ath12k；`src/nw_wifi.c` 调用 `ath12k_wifi7_pci_init()`，诊断逻辑访问 `ath12k_base`、CE、DP、HTT 内部结构。接入工作需要增加 ath11k 后端、补齐 Linux 兼容层差异，并拆分这些芯片相关调用。[3]

本次完成源码调研、板级参数提取和参考源码归档。硬件结果待 PCIe 枚举、固件启动、扫描与联网阶段记录。

## 现有实现与适用范围

| 部分 | 可采用基础 | marble 接入工作 |
| --- | --- | --- |
| Linux 兼容层 | Ath12kWifi 的内存、工作队列、NAPI、skb、MHI、QRTR、QMI 支持 | 编译 ath11k 所需接口，处理版本与结构差异 |
| 无线协议栈 | cfg80211、mac80211、扫描和 station 生命周期 | ath11k 注册、收发、密钥、监管域及速率查询 |
| 板级接口 | `lx_platform`、`nw_board`、`nw_wifi_events` | SM7475 电源、PCIe、BAR、配置空间、MSI、DMA、固件回调 |
| iOS 网络接入 | sheng 的 WifiNet.cpp、en0、Apple80211 事件 | 22H384 的 ioctl、关联请求、完成事件、信道与身份布局 |
| 认证 | wpa_supplicant 2.12 与 `org.unixnu.wifi.control` | arm64e 构建、签名、trust cache、launchd 服务与 OS 凭据流 |
| 固件加载 | WifiFirmware.c/h 的 VFS 加载与打包方式 | QCA6490/WCN6855 镜像、board data、校准与监管数据 |

上游 README 记录 sheng / 24A446 的开关、扫描、WPA2、DHCP、网络流量及关联结果设备测试；超时、取消、信号显示、Data 固件启动及性能项目列为待测。marble / 22H384 按自身硬件与系统版本验收。[3][4]

上游 Ath12kWifi 的 source-lock 固定 Linux v7.0.14 和 wpa_supplicant 2.12。实现阶段从该 Linux 来源选取 ath11k，可减少共用协议栈的接口差异。本报告下载的 Linux master 文件用于识别硬件分支、接口和固件路径；文件摘要保存在证据目录。[3]

## 板级输入

来源：`firmware/inputs/marble_live.dts`。[1]

| 项目 | 提取值 |
| --- | --- |
| 无线节点 | `qcom,cnss-qca6490@b0000000`，`qcom,wlan-rc-num = 0` |
| WLAN_EN / BT_EN / SW_CTRL | GPIO 80 / 81 / 82 |
| WLAN_EN active 配置 | GPIO 功能、输出高、上拉、16 mA |
| PCIe PERST / WAKE | GPIO 94 / 96 |
| XO GPIO | GPIO 204 |
| PCIe PARF | `0x01c00000`，长度 `0x3000` |
| PCIe PHY | `0x01c06000`，长度 `0x2000` |
| DBI / ELBI / iATU | `0x60000000` / `0x60000f20` / `0x60001000` |
| 配置空间窗口 | `0x60100000`，长度 `0x100000` |
| Memory BAR aperture | `0x60300000`，长度 `0x03d00000` |
| SMMU RID 映射 | RID `0x000` → SID `0x1c00`；RID `0x100` → SID `0x1c01` |
| Android CNSS IOVA 池 | `0xa0000000`，长度 `0x10000000` |
| MSI 映射 | RID `0x000` / `0x100` → ITS device ID `0x5980` / `0x5981` |
| GIC ITS | `0x17140000`，长度 `0x20000` |
| 备用 PCIe MSI 节点 | `0x17110040`，节点状态 `disabled` |
| PHY 配置 | `qcom,pcie-phy-ver = 0x63`，设备树含 PHY 初始化表 |

MSI 的 DT 输入指向 GIC ITS。实现阶段核对 UniXNU 的 GIC/LPI 支持和 sheng 采用的 MSI 路由；若选取 PCIe 控制器 MSI 模式，补齐路由配置与中断观测。

无线供电来自六组 RPMh 资源：

| 用途 | RPMh resource | WLAN 节点请求电压范围 |
| --- | --- | --- |
| AON | `smpe2` | 1.012–1.040 V |
| DIG | `smpb11` | 0.966–1.170 V |
| IO | `smpb10` | 1.800 V |
| RFA1 | `smpc1` | 1.900–2.024 V |
| RFA2 | `smpb12` | 1.350–2.040 V |
| 天线开关 | `ldoe7` | 2.800 V |

这些数值记录设备树约束；运行投票结合板级时序、固件需求与资源仲裁选择。PCIe PHY 另有电源及 CX/MX 依赖。参考 sheng 的阶段划分，使用 SM7475 的 GCC、GDSC、PHY、复位和互连参数实现板级启动。[1][4]

小米 `marble-s-oss` 的 CNSS2 在 QCA6490 初始枚举阶段选择 PCIe Gen2，并保留后续调速。该行为可作为链路启动依据。[5]

## 源码中的接入缺口

1. **ath11k 后端。** Ath12kWifi 的 `tools/sources.txt` 选择 ath12k 文件。新增 ath11k 的 PCI、MHI、QMI、HTC/WMI、CE、HAL、DP、MAC 及相关构建输入；补充所需 shim。共享 C ABI 保留供板级和 OS 层调用，生产初始化及诊断按后端分派。[3]
2. **marble 电源与 PCIe。** 新增板级实现，提供 rail、XO、WLAN_EN、PERST、PHY、LTSSM、BAR 与配置空间访问。参考 sheng WifiProbe 的状态机，常量从 SM7475 板级输入生成或定义。[1][4]
3. **DMA 所有权。** `ios18/sources/ShengUfs/NativeUfs.cpp` 的辅助 Wi-Fi 路径受 `sheng-wifi >= 2` 控制，使用 stream `0x1401`、IOVA `0x10000000` 和 128 MiB 缓冲。marble DT 给出端点 SID `0x1c01`。需要为 marble 建立 DMA 窗口、页表、缓存同步及生命周期，和 UFS、GPU 分配协调；内存容量由固件与环形队列占用实测确定。[1][6]
4. **固件与 board data。** 小米 CNSS2 为 QCA6490 添加 `qca6490/` 路径前缀，涉及 `amss.bin`、`amss20.bin`、`m3.bin`；Linux ath11k 的 WCN6855 路径分 hw2.0 / hw2.1。需核对固件格式、版本与驱动协议，再选择加载文件。设备的 board ID、subsystem ID、BDF、监管与校准数据需要形成配套清单。[2][5]
5. **22H384 用户空间接口。** sheng WifiNet 同时处理 592 与 656 字节关联请求，24A446 的完成事件为 540 字节。marble 的布局从 22H384 相关框架核对。覆盖扫描列表、关联结果、错误密码、断连、信号、保存网络和开关状态。[4]
6. **构建与打包。** marble 的 `feeds.toml`、`source_origins.json` 与 iOS 18 构建入口需接入驱动、板级对象、认证服务、固件、launchd 缓存和信任数据。当前检索的板级编译对象列表覆盖显示、存储、输入、DSP 等组件；Wi-Fi 集成属于新增工作。[7]

## 固件输入清单

实际开发前收集以下输入，并记录来源路径、长度和 SHA-256：

- 原机 Wi-Fi 固件目录与版本字符串，包含 QCA6490 的 amss 和 m3 候选文件。
- board data、regdb、校准文件和原机 MAC 身份来源；文件名称按原机清单记录。
- PCI vendor/device/subsystem IDs、SoC major/minor/sub-version，以及 QMI 返回的 chip/board 信息。
- Linux ath11k 对应版本的 firmware API、board-data 匹配键和需求；board-2.bin 的选择或生成依据。

原机 Android 固件与 Linux ath11k 固件分别保留来源；匹配阶段比较格式、硬件版本和 QMI/WMI 协议。原机校准内容及设备数据作为受保护输入，取证使用读取路径。

## 实施顺序与验收

| 阶段 | 产物 | 完成条件 |
| --- | --- | --- |
| A：身份与链路 | marble PCIe、电源与诊断实现 | 电源投票、PHY ready、LTSSM link-up、PCI ID 和 SoC 版本读出 |
| B：DMA 与中断 | 端点 SMMU 映射、MSI 路由及缓冲分配 | 地址转换结果匹配；端点 DMA 和实际 MSI 到达；UFS/GPU 工作状态维持 |
| C：固件启动 | ath11k 构建与固件清单 | MHI mission mode、QRTR/QMI 服务、固件版本、board-data 与 WMI ready |
| D：无线数据 | scan、station、密钥、收发 | 2.4/5 GHz 扫描，开放/WPA2 网络，EAPOL 与受保护数据帧收发 |
| E：iOS 集成 | en0、Apple80211、认证服务与打包 | 设置页选网、关联完成、DHCP、DNS、双向网络流量与保存网络重连 |
| F：使用体验 | 电源管理和恢复 | 开关、飞行模式、锁屏/唤醒、错误密码、超时、网络切换、掉线恢复；持续传输下的吞吐、延迟、CPU 与温度记录 |

每阶段记录源码版本、固件摘要、日志、计数器与真机结果；源码、内核、插件、启动参数或镜像变化时更新 CURRENT.json。开发起点遵循 CURRENT.json 的指定基线。[8]

本轮建议首先交付 A 阶段，同时推进 ath11k 在现有 Linux shim 下的编译缺口清单。PCI ID 和硬件版本确定后进入固件组合选择；固件与扫描通过后进入 iOS 设置页关联链路。

## 调研执行记录

- 阅读中文移植文档本地源码中的 network、platform 章节和 Ath12kWifi 组件页。网站请求及网络错误保存在证据目录的 references.json。
- 执行 `scripts/update_upstream.sh`；Ath12kWifi 与 UniXNU-sheng 的远端引用获取成功。全量更新在 Mesa 的 chromium.googlesource.com HEAD 查询处因连接失败退出。日志归档到证据目录。
- 参考版本：Ath12kWifi `28de7b91d7a880682b777e0e3564245a99dbc3f5`；UniXNU-sheng `689400177f93219a38bd7cf5ae2e9f5782adc594`。报告使用实际读取的源码快照。
- 下载 Linux ath11k 与小米 marble-s-oss CNSS2 源码，保存 Git blob ID 和 SHA-256。网络主机为 GitHub API。
- 本轮范围为研究记录与参考源码归档。设备启动、驱动构建和联网测试待实施阶段执行。

## 依据

证据目录：工作区 `outputs/UniXNU_marble/wifi-research-20261108/`。`evidence.json` 记录源码摘要与来源；`api-references*.json` 记录下载文件的 Git blob 和摘要。

1. 本设备 `firmware/inputs/marble_live.dts`：无线节点 12364 行起，PCIe0 19521 行起，GIC ITS 1340 行起；供电 phandle 对应 regulator 节点。
2. 证据目录 `linux-ath11k-pci.c`：32 行 WCN6855 ID，1028 行硬件版本分派；`linux-ath11k-core.c`：398 / 490 行固件目录；`linux-ath11k-mhi.c`：WCN6855 的 MHI 配置分支。
3. 证据目录 `ath12k-reference/`：README.md、include/nw_abi.h、tools/sources.txt、tools/source-lock.json、src/nw_wifi.c、src/nw_pci.c。上游仓库：UniXNU/Ath12kWifi。
4. 工作区 `work/upstream/UniXNU-sheng/package/WifiProbe.cpp`、WifiNet.cpp、WifiFirmware.c/h，及 `reports/ipados27-wifi-association.md`、`reports/ipados27-wifi-mainline-migration.md`。
5. 证据目录 `xiaomi-cnss2-pci.c`：44–52 行固件名称，5060 行附近固件路径选择，5903 行初始 Gen2 策略；`xiaomi-cnss2-main.c`：453–488 行 board-data 下载流程、3556 行 compatible。
6. 本设备 `ios18/sources/ShengUfs/NativeUfs.cpp`：1042 行起辅助 Wi-Fi SMMU 路径。
7. 本设备 `scripts/build_ios18_arm64e.py`：273 行起板级对象列表；`scripts/build_ios18_gpu.py`、feeds.toml、source_origins.json。
8. 本设备 CURRENT.json；工作区 `work/UniXNU-docs/docs/zh/porting/network.md`、platform.md；`work/upstream/UniXNU/components.toml` 的 Ath12kWifi 条目。
