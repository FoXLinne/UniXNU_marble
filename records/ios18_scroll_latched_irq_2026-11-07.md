# 2026-11-07 marble 滑动与撕裂修复候选

## 任务与实机边界

用户确认 `rtc-gpu-civac` 启动正常，反馈帧率低、响应慢、滑动画面撕裂，要求核对 Sheng 参考实现并修改。用户充电指令之后，设备操作暂停；本记录的候选完成主机检查与打包，帧率、响应延迟及面板撕裂的实机验收等待手机交回。

## 上游拉取与阅读

| 组件 | 本轮拉取后的远端版本 | 候选构建版本 |
| --- | --- | --- |
| UniXNU | b56f17c00aa0753ad9cf0714dfcac55c24e799c0 | 同左 |
| Adreno740 | 6fa7bae2a5b3b523e790eb3e0348af4d98969e4d | 同左及 A725 适配 |
| AdrenoMetal | e82788dd367753802ec2e00476ab27addfb46834 | 同左及 iOS 18 适配 |
| ArmSmmu4K | 83969cd874503e65997ff7639f7dcf3d0a92cd17 | 同左 |
| GopFramebuffer | e47c1b8de169cd2bd0b885c7d85e433010e3cdf2 | 同左及缓冲呈现补丁 |
| UniXNU-sheng | 679908d22cee24d15adfb1fceed9a8bf4e0d36ea | 参考源码 |
| metal2vulkan development | 963f0e64f103d2e682cd0cc454a8fe50f52c8cbb | bf3ce3813c029c96d02594f8568f2ee8343c0ed8 及现有 iOS 18 补丁 |

阅读范围包括 UniXNU 中文移植文档的 GPU、显示、GPU 帧诊断、固件与构建正文，Adreno740/AdrenoMetal 组件正文，Sheng 的 `GpuPowerProbe.cpp`、触控与慢帧报告，以及 Qualcomm Gen7 中断表。metal2vulkan 新版的提交调度修复有源码阅读记录；本候选的运行时输入与 shader 缓存构建标识保持现有值。

## 三项改动

1. `MarbleAdreno.cpp` 按 Sheng 和 Qualcomm Gen7 的定义启用 bit 20 `CP_CACHE_FLUSH_TS`。初始化与恢复采用同一掩码。CP_SW bit 8 的抢占完成由高优先级 thread call 执行，硬中断负责确认状态、排队与唤醒。
2. 默认采样策略恢复 AdrenoMetal 上游的单采样。环境变量 `NATIVE_METAL_SINGLE_SAMPLE=0` 保留请求的采样数。纹理、pipeline、resolve 路径使用现有上游实现。
3. GPU 复制目标采用三个离屏缓冲区。GPU 复制完成后更新 DPU 源地址，通过 CTL flush 锁存后完成 swap。固件画面使用预留区首个缓冲，三个目标位于其后；范围由 Android `cont_splash_region` 与 UEFI `Display Reserved` 的 `0xb8000000 + 0x02b00000` 约束。启动读取源地址、stride、裁剪、单矩形状态、CTL video 状态和 SMMU 路由。VIG/DMA flush 位遵循 Qualcomm 表。硬件布局满足条件时启用轮换；状态与错误保存在 `marble_scanout_state`。启动参数 `sheng-early-retire=0` 使源帧释放发生在完成路径，`marble-display-flip=1` 开启板级呈现。

缓冲协议通过 `NGPMode.version=2` 与 56 字节 `NGPBufferedSwap` 传递目标编号；版本 1 的 48 字节传输保留。GopFramebuffer 与 AdrenoMetal 的补丁进入构建入口，补丁重放结果与工作树逐文件一致。

## 主机结果

- `scripts/check_marble_gpu_scanout.py` 通过：Gen7 完成中断与抢占线程分派；ASan/UBSan；预留内存范围；裁剪边界；VIG/DMA flush 位；锁存拥有关系；实际 Apple Metal 复制像素；三缓冲轮换；版本 1 兼容与越界目标拒绝。
- 上游 `interface_regressions_host.mm` 在 MoltenVK 上的 normal、single、default 三种模式通过；single/default 包含 resolve 像素检查。
- arm64e 内核编译与预链接通过：完整映射，stub 表为空；候选内核与预链接报告、本机构建哈希匹配。
- iOS 18 插件构建、签名与 owner trust module 打包通过。
- Android v4 启动镜像中的固件、KernelSeed、FAT 卷与输入匹配；FAT 内 GPU addon 与 manifest 哈希匹配。Data UUID、大小及 storage profile 与 CIVAC 基线一致。

## 产物

候选目录：`/Volumes/Extra/https-github-com-oosexgang-hackios-xnu/outputs/UniXNU_marble/ios18_gpu_v1_2026-11-07/hardware/scroll-latched-irq`。

- 启动镜像：`marble_temporary_boot.img`
- 镜像 SHA-256：`298971c72b62676084116eefc67ffbd18cad19f8bf65210fa70a4cdc7b1567bd`
- 内核 SHA-256：`32c60af3fcdec17fce58d691fa574048e7c5b041e6bbae156fee68c78ddb6279`
- 本机内核 SHA-256：`ffb3058a6cef72018a03690845a2b8f93daf69516e51bd0b5bdb48115a56e55e`
- 完整报告与源码哈希：`qualification.json`
- 构建与像素日志：`final-image-build.log`、`userspace-final.log`、`interfaces.log`、`pack.log`
- 充电指令前的运行基线：`baseline-civac/`；采样入口：`sample.py`

## 手机交回后的验收入口

重启前读取当次 USB phase、进程与显示计数；使用候选目录内的 `kernel.macho` 解释新内核符号。启动后读取 `marble_gpu_irq[0..7]`，其中 `[3]` 是 fence 中断计数，`[4]` 是抢占中断数，`[7]` 是工作线程完成调用数。

`marble_scanout_state` 为 17 个 uint64：`[0]` 为 setup（2 表示缓冲路径启用），`[1]` 为错误，`[2]` 为 source pipe 掩码，`[3]` 为 CTL 偏移，`[4]` 为完成锁存数，`[5]` 为锁存超时，`[6]` 为最长锁存微秒，`[7..8]` 为显示与待锁存 slot，`[9]` 为 SMMU 路由，`[10]` 为 DPU 版本，`[11..13]` 为 GOP 地址、stride 与缓冲跨度，`[14..15]` 为 CTL 配置与接口状态，`[16]` 为 flush 掩码。setup 错误 1..6 依次对应范围、MMIO、source pipe、空 pipe 集、CTL/SMMU 条件和锁分配；运行错误使用 IOReturn。

在持续滑动场景记录完成帧间隔、输入到呈现延迟、fence 与锁存推进，并由用户观察撕裂与响应。截图与看图沿用交接文件中的设备约束。
