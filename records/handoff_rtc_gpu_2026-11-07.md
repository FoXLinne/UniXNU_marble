# 2026-11-07 marble 时间同步与显示交接

## 接手入口

工作区：`/Volumes/Extra/https-github-com-oosexgang-hackios-xnu`，设备仓库：`work/UniXNU-marble`。
用户本轮要求：完成整理、确认系统启动、发送交接。设备运行状态应持续使用本次现场；候选切换须遵守用户当轮指令。截图和看图属于禁止范围。长进程使用完成事件或间隔等待，诊断使用限定次数的现场采样。

## 当前实机

- 运行镜像：`/Volumes/Extra/https-github-com-oosexgang-hackios-xnu/outputs/UniXNU_marble/ios18_gpu_v1_2026-11-07/hardware/rtc-gpu-command-surface-sync/marble_temporary_boot.img`。
- 对应符号：`/Volumes/Extra/https-github-com-oosexgang-hackios-xnu/outputs/UniXNU_marble/ios18_gpu_v1_2026-11-07/hardware/rtc-gpu-command-surface-sync/kernel.macho`。
- 最后确认：RootStage phase=`kernel`；SpringBoard PID 27；backboardd PID 60；PosterBoard PID 284；timed PID 64。
- 显示完成计数 3785，前一份现场记录为 1162；GPU 最后提交 8152、最后完成 8151。累计 wait 调用 21645、超时 15061。
- LaunchServices 状态 1025。设置进程的启动与响应验收列为待处理。
- 性能验收列为待处理。累计超时计数、设备当时的同一任务推进、线程堆栈应结合使用。
- 现场文件：`/Volumes/Extra/https-github-com-oosexgang-hackios-xnu/outputs/UniXNU_marble/ios18_gpu_v1_2026-11-07/hardware/rtc-gpu-command-surface-sync/handoff-running-status.json`。

## 关键区别：运行镜像与构建镜像

最新 CIVAC 镜像：`/Volumes/Extra/https-github-com-oosexgang-hackios-xnu/outputs/UniXNU_marble/ios18_gpu_v1_2026-11-07/hardware/rtc-gpu-civac/marble_temporary_boot.img`。
该镜像完成构建、签名、预链接和打包，实机应用列为待执行。
当前手机运行的是上节的 `rtc-gpu-command-surface-sync`。交接时设备控制操作停止在启动确认之后。

CIVAC 的关键源码是设备仓库 HEAD 原有的 `patches/AdrenoMetal/ios18-surface-cache.patch`。它在 iOS 18 的 `flushSurfaces` 中按 64 字节缓存行执行 `dc civac`，覆盖命令提交和完成两个交接点。仓库原有说明记载 22H384 的 `sys_dcache_flush` 路径仅执行 DSB ISH，维护原有的手动 CIVAC 路径十分关键。
运行中的镜像使用启用 `flushSurfaces`、调用 `sys_dcache_flush` 的前端；完整手动 CIVAC 位于待应用镜像。

## 时间同步的结果与实现

- 校时与重启持久化测试通过，重启采样与主机时间差约 0.21 秒。
- PMK8350 硬件 RTC：SPMI SID 0，外围块 0x61，秒读数 0x6148；读取包含最低字节回绕重试。
- 固件当前的 GetTime/SetTime 库是占位实现：固定 2019 年与运行计数，SetTime 返回 EFI_UNSUPPORTED。时间来源采用 PMIC RTC。
- 软件偏移存储在 unixnu 分区首个 4096 字节头部：1024 起的 24 字节为 magic、signed offset、offset complement。SHA-256 在偏移 4064，更新校时记录时重算。内核通过已有串行 UFS 写入窗口、FUA 和同步缓存持久化该扇区。
- 校时前头部备份：`/Volumes/Extra/https-github-com-oosexgang-hackios-xnu/outputs/UniXNU_marble/ios18_gpu_v1_2026-11-07/hardware/rtc-sync/rtc-header-before.bin`。
- 固件向 KernelSeed 尾部 1 MiB mailbox 的 2048 位置写入 128 字节时钟交接，两条缓存行分别由固件和内核写入。
- `package/MarbleRtc.cpp` 接入 PEGetUTCTimeOfDay/PESetUTCTimeOfDay，发布 IORTC，并根据硬件计数器推进 UTC。RootRtc.c 使用现有固件服务处理校时请求。
- `scripts/root_time.py sample --kernel IMAGE/kernel.macho` 读取实际系统日历；`sync` 使用主机 UTC 校时并触发持久化。
- 核心文件：MarbleRtc.cpp、MarbleRtc.h、MarblePkg/RootRtc.c、MarbleUfs.cpp、NativeResident.cpp、patches/xnu-11417/native-rtc.patch。

## GPU 改动与试验边界

- 原新候选把 GPU 可写导入表面强制映射为 WB/Inner Shareable，并禁用 iOS 18 前端表面 CIVAC。
- 卡顿现场：显示完成停在 279，GPU 提交 654、完成 648，1566 次等待超时；触控事件计数有增长，USB 和内核在运行。
- 试验中强制所有可写导入为 NC/Non Shareable，引入 `BoMap failed: 0xe00002c2`。Turnip 实际调用请求是 writeback=0、shareability=2（Outer Shareable）。缓存与共享属性应遵守调用约定。
- 当前 NativeAdrenoUserClient.cpp 的可写导入使用调用方参数；只读导入的原有 coherent 策略继续使用。
- 试验中增加过对全部已映射导入的每次提交/完成维护。NativeAdrenoVm.cpp 当前使用 Adreno740 HEAD 策略；表面交接的补齐位于逐命令的 IOSurface 前端 CIVAC。
- CIVAC 补丁恢复为设备仓库 HEAD 版本；其他型号、USB WFE、空闲控制线程 16 ms、LaunchServices 与 statfs 路径修复继续存在工作区。

## 上游与 Git

- work/UniXNU、work/upstream/UniXNU、GPU 构建树 UniXNU 更新到 b56f17c00aa0753ad9cf0714dfcac55c24e799c0。
- Adreno740：6fa7bae2a5b3b523e790eb3e0348af4d98969e4d。
- AdrenoMetal：e82788dd367753802ec2e00476ab27addfb46834。
- ArmSmmu4K：83969cd874503e65997ff7639f7dcf3d0a92cd17。上述三个 GPU 组件执行 fetch 后，本地提交与远端一致。
- work/UniXNU 更新产生的 7 个本地适配冲突完成处理；Python 语法与 shell 语法检查通过。老 Native* 子仓库目录在主仓库迁移后仍是实际构建输入，应按依赖关系处理。
- 工作区带有本轮之前的改动和未跟踪文件。时间同步源码和交接记录作为本轮提交范围；其他已有改动有独立范围。提交时按实际暂存差异操作。
- 上游同步备份目录：`/var/folders/wp/72cjqq5d3xd8wh2bkdc5bt540000gn/T/unixnu-upstream-local-eu187mus`。

## 可复用执行入口

Python：`work/marble_hackios/venv/bin/python`。
构建根：`outputs/UniXNU_marble/ios18_arm64e`。
内核：`scripts/build_ios18_arm64e.py kernel --output BUILD_ROOT`。
预链接：同脚本 `prelink --output BUILD_ROOT`。
打包：`scripts/pack_ios18_ufs.py --root BUILD_ROOT --assets OUT --images outputs/UniXNU_marble/ios18_gpu_v1_2026-11-07/hardware/coherent-idle/storage_images.json --data-seed outputs/UniXNU_marble/ios18_arm64e/session-20261105/ufs/Data.rdisk --temporary --skip-setup`。

RootStage 运行态 USB 序列号 `marble`，Android bootloader 序列号 `556fdfe`。`fastboot devices` 的 fastboot 字样应结合 `getvar phase` 解释。
运行内核的内存读取使用 `root_ui.memory_for(channel, symbols)`。完整 capture 入口包含截图；本轮诊断使用 capture_init 和限定的内存读取。
进程结构偏移：pid=96、state=100、name=1405、next=0；完整线程布局在 BUILD_ROOT/kernel.layout.json。线程采样前按名称取得本次进程地址。
msgbuf 尾部须从 `(cursor-N)%size` 开始，按环形缓冲分段读取；仅读取缓冲开头会遗漏本次现场。

## 最重要的交接结论

系统启动有本次实机运行记录。时间同步有校时与重启记录。卡顿、设置响应、手动 CIVAC 镜像的实机结果是接手后的任务范围。当前运行镜像和待应用镜像必须对应各自的运行证据。
