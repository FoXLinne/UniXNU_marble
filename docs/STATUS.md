# marble 阶段状态

## iOS 27.2 真机维护：2026-11-16

当前版本为 v49：Wi-Fi 内核沿用 v47，USB 固件沿用 v48，图形插件加入旧版 metallib 扩展头边界修复。

- Wi-Fi 自动连接及三次开关重连的 DHCP 用时为 1.271181、1.307573、0.859908 秒；v49 网络诊断接口响应 HTTP 200。
- USB 主机命令中断 10 秒后停止喂看门狗的策略移除。v48 的 60 秒端口禁用实验保留连续内核运行及图形输出记录，v49 保留该修复。
- 用户确认登录窗口期间的重启由 USB 断连触发，故障归入 USB 看门狗修复。
- PiliPlus 的 Flutter 旧版 metallib 在函数表后紧接元数据，解析器按节边界识别空扩展头。三个实际库共 65 个函数完成解码，20 个容器回归用例通过。
- v49 的 Runner PID 534 保持运行，推荐页封面、文字、滚动及热门页切换完成真机检查，应用停留在热门页。

验收摘要：[device-acceptance-2026-11-16.json](../records/ios27-24B119/device-acceptance-2026-11-16.json)。视频播放属于独立功能范围。

以下保留各日期的历史记录。

## iOS 27.2 真机维护：2026-11-14

`candidate-connection-lifetime` 通过桌面解锁、设置、墙纸图库与渐变编辑、四次深浅色切换、
控制中心、多任务、锁屏唤醒解锁及新增青色墙纸组合测试。SpringBoard 与 backboardd 退出计数为 0；
显示等待 4132 次，延迟确认成功 1 次，真实超时 0 次。桌面墙纸中心像素为 RGB(81,213,251)。

崩溃追踪定位到第二个 Vulkan 设备创建失败后的连接清理：进程共用的 GPU 连接被关闭，
首个设备持有的 8 MiB CPU 映射随 `dropViews` 撤销，玻璃效果写入触发 SIGSEGV。
Turnip 修复覆盖连接引用、共用 GPU 地址分配及设备独立缓存释放。
原关闭函数在主机回归触发映射存活断言；修复函数通过 AddressSanitizer、UndefinedBehaviorSanitizer
及 2400 次并发引用与分配测试。源码补丁见 `patches/ios27-gpu/turnip-shared-connection-lifetime.patch`。

证据、镜像摘要及测试边界见 [连接生命周期记录](../records/ios27-24B119/connection-lifetime.json)。
墙纸图库与保存卡片的缩略图、渲染响应性能及本次拉取的 GPU 上游源码整合属于进行中的维护范围。

## iOS 27.2 迁移：2026-11-12

分支 `port/ios27.2-marble`，目标 `24B119`。完整范围、阶段结果、来源与续接命令见
[迁移交接](IOS27_HANDOFF.md)、[迁移进度](../records/ios27-24B119/progress.json) 和 CURRENT.json。
18 个组件的上游与本地适配合入记录状态为 staged；Turnip、Metal 桥接、签名插件与
24B119 runtime 构建完成。最终 runtime 含原生 USB 内核日志服务，716 个文件、28 个签名摘要。
原版类 ABI、完整驱动库与预链接映射继续处理；真机启动与功能结果待采集。

以下内容保存 iOS 18 的历史阶段及设备状态。

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
