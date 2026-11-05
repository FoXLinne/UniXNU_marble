# iOS 18 显示功能与上游实现调研

日期：2026-11-06，Asia/Shanghai。设备：marble / SM7475。系统：iOS 18.7.11 / 22H384。
范围：远端版本核对、源码与硬件描述调研。驱动移植、构建、刷机及真机功能验收属于实现工作。

## 远端版本

本次执行 `git fetch origin`，通过 `git show origin/HEAD` 与 `git grep origin/HEAD` 读取远端源码。研究版本与构建基线分别记录；构建基线沿用 `upstream_revisions.json`。

| 仓库 | 远端 HEAD | 提交时间（UTC+8） |
| --- | --- | --- |
| UniXNU-sheng | `460179565a018c741b93e0b1efa07c55d4de497a` | 2026-11-05 01:21:53 |
| UniXNU | `7307c26ea5053a9b7a3e58b81a12b6f1d5a2b226` | 2026-11-06 01:14:43 |
| GopFramebuffer | `e47c1b8de169cd2bd0b885c7d85e433010e3cdf2` | 2026-11-03 15:11:00 |
| AdrenoMetal | `e82788dd367753802ec2e00476ab27addfb46834` | 2026-11-05 01:21:53 |
| ShengNovatekTouch | `69fdd6223ed2572aa8bd8351e77c53a238276130` | 2026-11-05 01:21:53 |

sheng 的触控子模块固定值为 `3b44b1b7603eaadae942ea44ddc51bdcc48ad060`；ShengNovatekTouch 的远端 HEAD 为上表值。NativeMobileDisplay 的远端查询返回 GopFramebuffer 的提交与文件布局。

## 功能结论

| 功能 | 上游源码与记录 | marble 接入内容 |
| --- | --- | --- |
| 亮度滑块、渐变调光 | sheng 提供 ShengBacklight、CoreBrightness 接入、硬件回读与中断传输；设备记录的 display 等级为 `implemented` | 复用系统服务与渐变调度；硬件传输采用本机 DSI DCS |
| 点按、双击唤醒 | sheng 提供普通触控 IRQ/HID 路径；Novatek 库提供 doze 初始化。系统唤醒连接列入新增工作 | Goodix 单击/双击手势、事件解析、锁屏亮屏事件、显示电源状态交接 |
| 夜览 | 所查 sheng、GopFramebuffer、AdrenoMetal 源码的功能范围集中于呈现、背光与 Metal；夜览色温输出后端列入新增工作 | 核对 22H384 CoreBrightness/IOMobileFramebuffer 色彩请求，连接 DPU 色彩后端 |
| 色彩滤镜、灰度、白点调整 | 色彩矩阵与 LUT 输出列入新增工作 | 核对系统请求格式；评估 PCC、IGC、GC、VLUT；按内容处理的滤镜接入合成器路径 |
| 原彩 | 环境光/色温输入、校准与系统色彩适配列入新增工作 | 查询传感器能力，接入 SSC/SLPI 数据，提供面板校准与 CoreBrightness 色彩适配 |

表中的新增工作由本次远端源码检索与接入点检查归类。相应功能的真机验收在实现阶段记录。

## sheng 的亮度实现

源码：[AppleARMBacklight.cpp](https://github.com/UniXNU/UniXNU-sheng/blob/460179565a018c741b93e0b1efa07c55d4de497a/package/AppleARMBacklight.cpp)、[BacklightProbe.cpp](https://github.com/UniXNU/UniXNU-sheng/blob/460179565a018c741b93e0b1efa07c55d4de497a/package/BacklightProbe.cpp)。

- 双 KTZ8866，I2C 地址 `0x11`，GENI 引擎 `0xa84000` 与 `0x988000`；亮度为 11 位，范围 0–2047。
- `IODisplayParameters.brightness` 发布 min/max/value。`setProperties` 消费 `brightness` 与 16.16 秒格式的 `FadePeriod`。
- 渐变以 16 ms 步长调度；请求合并保留目标值；传输由中断和工作循环推进。
- `backlight-control=true` 与 AppleARMBacklight 类身份参与 CoreBrightness 的内建屏识别和 libEDR 通知服务启动。源码保存了服务识别与显示服务等待链的诊断记录。
- arm64e 分支通过编译出的 `getMetaClass()` 实现身份应答；arm64 分支使用虚表复制。iOS 18 的接入采用 arm64e ABI。
- 源码注释说明 AppleSPU、AppleEmbeddedLightSensor、AppleM68Buttons 的类查询参与服务身份约束；传感器与原彩工作需要复查这些消费者和对象 ABI。

sheng 的 `porting.toml` 将显示标为 `implemented`。源码包含 CoreBrightness 请求、读回、超时与中断诊断。本调研按该记录等级描述完成度。

### 属性授权

上游补丁：[zzwb-native-sandbox-grant-set-properties.patch](https://github.com/UniXNU/UniXNU/blob/7307c26ea5053a9b7a3e58b81a12b6f1d5a2b226/patches/xnu/zzwb-native-sandbox-grant-set-properties.patch)。

marble 的 XNU `IOUserClient.cpp` 在 `is_io_service_open_extended` 检查 `NativeSandboxGrantProcess`；`is_io_registry_entry_set_properties` 保留 `mac_iokit_check_set_properties` 路径。亮度接入需要移植 setProperties 授权。授权范围按亮度服务、backboardd 与 brightness/FadePeriod 请求设计。

## marble 的面板与亮度

输入：`../marble_hackios/marble_live.dts`、`../marble_hackios/Mu-Silicium/Binaries/marble/RawFiles/Panel_M16t_36_0d_0b_amoled_dsc_vid.xml`。

- DTS 第 16 行的启动参数选中 `qcom,mdss_dsi_m16t_36_0d_0b_dsc_vid`。
- 对应节点从第 31702 行开始。`bl_ctrl_dcs` 指定 DSI DCS 亮度，最大值 `0xfff`，最小值 4，初始值 `0x133`。
- 固件面板 XML 声明 12 位亮度，并在初始化序列中包含 `39 51 00 00`。
- DTS 还声明 `mdss-dsi-bl-inverted-dbv`、首帧后更新亮度、dimming 延时、AOD/doze 与 ULPS 策略。这些编码与状态转换属于本机后端输入。

亮度实现路径：原版 iOS 滑块 → CoreBrightness → arm64e 亮度服务 → 渐变/请求合并 → DSI DCS 命令队列 → 面板。实现时核对实际面板 ID、DSI 命令传输、完成状态、DBV 字节序及低亮度映射，保存固件传输配置。

验收包含滑块低/中/高档、渐变、锁屏熄屏、亮屏恢复、启动初值与传输错误计数。

## 点按唤醒

sheng 参考：[TouchProbe.cpp](https://github.com/UniXNU/UniXNU-sheng/blob/460179565a018c741b93e0b1efa07c55d4de497a/package/TouchProbe.cpp#L717)、[NvtProtocol.c](https://github.com/UniXNU/ShengNovatekTouch/blob/69fdd6223ed2572aa8bd8351e77c53a238276130/NvtProtocol.c)。前者实现普通触控 IRQ 与 HID 上报；后者包含 doze 延时与初始化命令。

marble 使用 Goodix GT9916R，DTS 第 12648 行附近记录 SPI 连接、GPIO21 中断、GPIO20 复位及固件/配置文件。

本地厂商参考 `../marble_hackios/touch_reference/goodix_ts_core.h` 第 66 行声明：

- `GESTURE_SINGLE_TAP = 1 << 0`。
- `GESTURE_DOUBLE_TAP = 1 << 1`。
- `GESTURE_FOD_PRESS = 1 << 2`。

`goodix_brl_hw.c` 第 292 行起提供 `brl_gesture()`，Berlin A 使用命令 `0x12`，其他分支使用 `0xa6`；第 1214 行起解析 gesture event、gesture type 和附加数据。固件信息含 `gesture_feature` 字段。

接入内容：查询实际 IC 类型与固件手势能力；进入屏幕关闭的手势模式；保留触控供电与中断；解析手势并映射 iOS 唤醒事件；恢复面板与普通触控扫描。锁屏亮屏与深度休眠唤醒分别验收，深度休眠需要平台电源及 IRQ 唤醒路由。

## 夜览与色彩滤镜

DTS 第 25878 行声明 DSPP 基址偏移；第 25953 行起的 `qcom,sde-dspp-blocks` 声明：

| 模块 | 相对偏移 | 版本值 |
| --- | --- | --- |
| PCC | `0x1700` | `0x40000` |
| IGC | `0x1260` | `0x40000` |
| GC | `0x17c0` | `0x10008` |
| VLUT | `0xa00` | `0x10008` |
| Gamut | `0x1000` | `0x40003` |

这些是硬件描述输入。实现时读取固件的活动管线、DSPP 路由与使能状态，核对对应版本的寄存器布局、系数格式、更新锁存与恢复次序。

性能方案优先采用显示硬件色彩处理，矩阵/LUT 变化通过寄存器或硬件队列更新；GPU 保留合成与呈现路径。夜览、灰度、色偏和白点变化分别核对系统请求与实际输出。智能反转等按内容处理的效果接入系统合成器语义并验收 Metal 着色器结果。

iOS 18 侧的工作包括 CoreBrightness/IOMobileFramebuffer 请求布局、能力发布、颜色空间、矩阵/LUT 编码、持久设置与锁屏切换。GopFramebuffer 和 AdrenoMetal 的现成路径提供呈现与 GPU 接口，色彩功能需要新增系统到显示后端的连接。

## 原彩与环境光

marble DTS 第 11893 行附近声明 SLPI，第 11940 行的 SSC 节点选择 `slpi2.mdt`。传感器器件、环境光通道、RGB/CCT 能力与校准输入需要从传感器服务和厂商配置读取。

原彩接入包含环境光/色温采样、传感器覆盖能力、面板白点与色域校准、CoreBrightness 色彩适配及 DPU 色彩输出。自动亮度与原彩分别记录所需传感器输入和系统服务。

补充上游参考：UniXNU 目录中的 OnePlus 13T/PaganiSensors 条目标为 `implemented`，内容为 iOS 传感器服务发布与 ADSP/SSC 数据接口；该条目的证据说明为 host-only，设备数据验收列在计划中。此项可提供服务发布参考。

[Pagani 目录源记录](https://github.com/UniXNU/UniXNU/blob/7307c26ea5053a9b7a3e58b81a12b6f1d5a2b226/docs/.vitepress/device-records/oneplus13t-shad0w23333.toml)。

## 其他板级案例

UniXNU 目录中的 starlte/Samsung S9 记录将显示列为 `device-verified`：iOS 16.5 控制中心滑块连接 DSIM0，S6E3HA8 通过 gamma/AOR 调节亮度。该记录提供滑块与面板控制桥接案例；marble 的硬件输入按本机 DSI DCS 描述实现。

[starlte 目录源记录](https://github.com/UniXNU/UniXNU/blob/7307c26ea5053a9b7a3e58b81a12b6f1d5a2b226/docs/.vitepress/device-records/starlte.toml)。

## 实现入口与验收记录

| 链路 | 复用输入 | 新增接入 |
| --- | --- | --- |
| 亮度 | ShengBacklight、CoreBrightness、setProperties 授权 | marble DSI DCS、DBV 编码、显示电源关联 |
| 点按唤醒 | Goodix 手势命令、固件能力与事件结构、现有实体按键/HID 经验 | 手势模式、iOS 唤醒事件映射、面板恢复 |
| 夜览/滤镜 | DTS 的 PCC/IGC/GC/VLUT、现有显示与 GPU 呈现 | 22H384 色彩 ABI、硬件色彩更新与恢复 |
| 原彩/自动亮度 | SSC/SLPI 硬件描述、CoreBrightness 服务 | 传感器数据、色温能力、校准与色彩适配 |

每条链路保存源码版本、服务发布、系统请求、硬件读回与屏幕结果。GPU 帧时间和带宽参与亮度/色彩功能的性能验收。

公共参考：[显示与亮度](https://unixnu.com/zh/porting/display)、[触控与按键](https://unixnu.com/zh/porting/input)、[设备适配记录](https://unixnu.com/zh/porting/records)。
