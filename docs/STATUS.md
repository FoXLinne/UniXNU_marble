# marble 阶段状态

日期：2026-11-11。系统目标：iOS 18.7.11 / 22H384，XNU 11417.140.69 arm64e RELEASE。

## 设备终态

用户终止设备调试，手机恢复 Android 17、槽位 A。恢复时 ADB 读取 `sys.boot_completed=1`、`ro.boot.slot_suffix=_a`。设备操作停止，后续工作范围为文档、源码归档和仓库维护。

## 扬声器与马达

扬声器锁屏后失声、播放等待的问题保持待修复。设置中调整缩放或语言触发软重启后，用户观察到声音恢复，持续至下一次锁屏。

| 候选 | 结果 |
| --- | --- |
| `boot-lock2` | 系统播放存在运行证据。锁屏后 StartIO/StopIO 停在 7/7；解锁播放期间计数保持一致 |
| `boot-lock3` | VirtualAudio 导入钩子触发 audiomxd 反复崩溃；追踪功能弃用 |
| `boot-lock-offline` | 增加流活动通知和循环路由快照；用户复现锁屏后卡住，计数 3/3，路由为 `pspk`，输出流活动值为 1 |
| `boot-lock-state0` | 对照参数 `sheng-aks-sls=0` 后 MediaExperience 锁定缓存为 0；DSP 通道注册竞态导致扬声器端点创建失败 |
| `boot-lock-state0-dsp` | GLINK 提前 OPEN 保留修复后，DSP ready、HAL 532 包、DSP 完成 566 缓冲；用户报告锁屏一次触发崩溃，候选验收失败 |

时间线 seed、按流 IsActive 与通知契约的修正均保留为源码改动。锁屏失声在这些修正后复现。`sls=0` 与通知时序对末轮崩溃的影响待定位。MobileKeyBag 锁定缓存、BluetoothBridge 同步等待作为调查线索保存。

马达内核自检震动，系统调用路径保持待接通。原始 Actuator 由 audiomxd 接管；VirtualAudioDevice_Actuator 查询 ID 为 0，AVHapticServer 初始化调用计数为 0，CoreHaptics 返回 -4810，Actuator HAL IO 计数为 0。

专题说明、摘要和采集转折点见 [音频阶段记录](../records/audio-2026-11-11/README.md)。最新音频候选属于失败样本。

## 其他功能

| 范围 | 证据与剩余工作 |
| --- | --- |
| UFS | ShengUfs 迁移到上游等待接口后，使用新驱动的候选可进入系统。Data 64 GiB 与身份沿用既有 profile；吞吐量及软重启按具体候选记录 |
| 显示与 GPU | 桌面、锁屏、触控与部分系统界面存在运行证据；Safari surface、渲染正确性、撕裂、模糊及响应延迟保持专项范围 |
| Wi-Fi | 原生 en0、扫描、WPA2 认证、DHCP 存在真机证据。关联事件、连接提示和重连分别记录 |
| USB | 固件驻留与内核 USB 均有运行记录；连接所有权、重插、PHY 校准及锁屏失联按候选区分 |
| 电池与充电 | ADSP 电量、电流、温度与 powerd 充电状态存在记录；Powerlog 64 位时间桶修正存在采样，设置统计页面结果见充电专题 |
| 应用安装 | AppSync 安装记录返回 Complete；应用自身运行与渲染另行验收 |

GPU 的七组目标保留于 [GPU_REPAIR_GOAL.md](../GPU_REPAIR_GOAL.md)。2026-11-07 的逐项表属于当日基线，2026-11-10 至 11 日的结果由本页与专题记录补充。

## 源码与构建来源

2026-11-11 同步 85 个上游仓库。部分文件完成合入，10 个组件保留待处理冲突清单。冲突结果在本地 `build/upstream/conflicts/`，已采用来源与待整合来源见 `source_origins.json`，提交内保存 [同步报告](../records/upstream-sync-2026-11-11.json)。

本仓库提交保存设备工作树的实际内容。音频内核的增量构建、预链接成功与上游全量整合状态分别记录。末轮内核预链接结果为 unresolved 0、stubbed 0；全树集成及功能验收保持待完成。

## 状态读取顺序

1. [CURRENT.json](../CURRENT.json)：当前设备状态、最后构建、来源及入口。
2. 本页和专题记录：候选结果与验收边界。
3. [完整阶段归档](../records/archive/current-2026-11-11.json)：原 CURRENT.json 全量内容，包含历史实验与当时计划。
4. 本地 `outputs/UniXNU_marble/`：镜像、内核、原始日志及截图；摘要由专题记录关联。
