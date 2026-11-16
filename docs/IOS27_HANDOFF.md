# marble iOS 27.2 迁移

2026-11-16：当前运行版本为 v49，Wi-Fi、USB 看门狗与 PiliPlus 启动修复的来源及验收见 [CURRENT.json](../CURRENT.json) 和 [验收摘要](../records/ios27-24B119/device-acceptance-2026-11-16.json)。下文保留迁移阶段交接记录。

目标：iOS 27.2 / 24B119，marble / SM7475 / Adreno 725。
分支：`port/ios27.2-marble`；设备代码基线：`7a0d68ff`。

## 2026-11-14 纹理分配修补

`candidate-directory-search` 真机 driverkitd 保持 PID81，完成配置读取及
SystemExtensions 枚举。`MarbleExecutableDirectory27.c` 限定平台签名 driverkitd
自身可执行父目录，将目录元数据打开转换为 O_SEARCH，保留 FREAD/FWRITE 清空及
VFS/沙箱搜索权限检查。访问转换4次、目录打开转换3次；此前 Config 路径崩溃消失。

backboardd PID321 的完整主线程栈进入 NativeAdrenoMetal：
`memory::place -> placed_image_starting -> recycled_image -> new_texture`。
`memory.rs:5161` 在持有 `guard = lock(memory)` 的分支中重复取得同一个锁。
图像备用块分支改为 `&mut *guard`。arm64e 插件构建进行中，真机结果见 CURRENT.json。
记录：`records/ios27-24B119/pooled-image-deadlock.json`。

主机用户页表读取支持47位地址空间的2KiB根表，保留物理地址0x800偏移；
相邻半页根表回归检查通过。证据：`device-20261114/directory-search-images/`。

## 2026-11-13 真机交接修复

2026-11-14 深夜推进：`candidate-watchdog-lifecycle` 采用原始
AppleARMWatchdogTimer 调用的 IOWatchdog 初始化入口 `0x834d930`。
1204 字节函数 SHA256 `6fa7940728582fd95d340406acf95e013d22804a6b384872cae0714d66af1d12`
由驱动生成器在绑定时核对。入口分配锁、设置策略、发布服务并调用基类 start。
真机 watchdogd 打开客户端 `type=1 status=0`。
随后 `stack_snapshot_with_config` 进入 DebuggerXCallEnter，CPU6 调试中断等待触发 panic。
SPI IPI 候选停在内核入口；board.toml 恢复 SGI。
`gic-stackshot-capability.patch` 在 GIC 普通栈快照停核前返回 KERN_NOT_SUPPORTED，
调用方传播结果；同步停核快照适配待完成。CURRENT.json 记录构建和运行状态。


2026-11-14：`vnode-entitlement-query.patch` 使用 AMFI
`copyEntitlementAsOSObject` 复制请求键的值，保留签名上下文引用和对象所有权。
`candidate-vnode-entitlement` 越过整份权限字典复制 panic，进入 watchdogd
服务发现失败；watchdogd 的 `0x10000ebe8` 对应消息
`connect_to_kext() unexpectedly failed - unable to lookup watchdog KEXT service`。
`MarbleWatchdog::start` 补齐属性、`IOUserClientClass=IOWatchdogUserClient`
和 `registerService()`，候选 `candidate-watchdog-publication` 进行真机启动。
采集目录 `device-20261114/watchdog-publication/`，自动重复启动循环保持移除。


23:02 候选 `candidate-proc-pid` 修复两个原内核绑定：
`0xa6fa5b0 -> proc_ppid`（读取 +0x20），
`0xa6f91e4 -> proc_pid`（读取 +0x60）。原始与 native 的
`proc_selfpid`/`proc_selfppid` 指令及 `proc_internal.h` 字段顺序交叉核对。
真机沙箱日志记录 logd[28]、backboardd[68]、lockdownd[86]；显示客户端返回成功。
桌面待实测，设备随后回到 fastboot，采集记录位于
`device-20261113/proc-pid/`。自动重复启动循环移除；每次候选使用独立目录。

此前 logd 的 SIGABRT 调用栈定位到 IOKit
`__IOHIDEventSystemClientRefresh` / `IOHIDEventSystemClient.c:614`，
断言 `kern_result == KERN_SUCCESS`。另一次捕获 AMFI
`OSEntitlements.cpp:863` 的 OSDictionary 复制 panic。
证据：`device-20261113/logd-recovery/20261113-225225-338893/`。
驻留模式 panic 保留固件 USB 所有者的源码修复正在构建；结果见 CURRENT.json。


后续真机推进到 init、Data 挂载与 SystemOS 挂载入口。
`IOHIDDevice::newVendorIDNumber` 的 PAC `0x4897` 与继承的 `IOService::probe` 碰撞；
驱动生成器按类私有槽位及 generated opaque 名称中的准确槽号绑定，目标槽 174。
APFS 使用的 rw gate close/try_close 入口从 `x1` 取 gate，显式映射到双参数公开接口。
SystemOS 生成代码从 target profile 读取 UUID `46885FBD-E14C-4630-805A-8AC4BFD8D326`。
RELEASE 配置缺少 `config_union_mounts`，挂载在 XNU 入口返回 EPERM；
`release-native-union-mounts.patch` 启用已有 union 实现，源码构建进行中。

`candidate-firmware-awake` 真机读到 `real_ncpus=7` 与 `armcpu_smp_running=6`；
一核驻留 USB，七核进入 XNU。OS、SystemOS、Data 的 UFS 提供者发布完成。
显示初始化触发 PAC panic，原因为适配器的内核绝对地址跨 native 预链接失效。
显示选项字典、字符串与 `IOSurfaceMemoryRegion` 属性设置改用源码 API。
watchdog 的错误绝对启动跳板也改用源码 `IOService::start`。
证据见 `device-20261113/firmware-awake/early-011.json` 与消息环。

GIC 节点通过 `unixnu,adt-name = "intc"` 导出，redistributor stride 取设备固件的
`0x40000`。打包在候选目录中编译当前 DTS，记录 DTB 和 DTS 摘要。
UFS 继续由 MarbleRootDisk 的绝对 MMIO 发布路径负责。
CPU 初始化阶段出现硬重启；主机捕获 `kernel-ready=ready`，事件窗口待定位。
`native_marble_boot_events` 为静态记录环，记录函数保留计数器、事件、地址和值；
`root.trace` 携带符号到 kernel window 的偏移。固件在加载覆盖前快照该区域，
`oem previous-boot-log` 导出快照，`oem boot-log` 导出本轮记录。
`0xb7001000` 的跨重启签名读取为空，相关历史观察保存在 CURRENT。

32 页候选通过 MMU 交接，内核消息环记录 corecrypto POST、AMFI 与 IOKit 初始化。
panic 记录为 `CPU configuration in device tree is invalid @AppleARMCPU.cpp:111`；
其日志要求 ordinal `reg` 和 Apple IPI 属性。marble 设备树使用硬件 MPIDR，
由已编译进目录的 `ArmCpu` 处理。
目标 profile 移除 AppleARMPlatform 的根驱动选择，保留其依赖类；预链接元数据
确认该组件的硬件匹配规则移除。`MarbleBootWatchdog` 在 mode 3 保留健康采样，
硬件喂狗归驻留 USB 服务所有，主机命令租约控制恢复。
证据目录为 `device-20261113/bootstrap32-panic/`。

内核入口记录读回 `0xc1`。MMU 前的启动页表池耗尽：16 页分配为两个根表、
九页物理映射和五页虚拟映射，完整 8 GiB 窗口需要二十页。
`bootstrap-8g-capacity.patch` 扩展至 32 页，并通过 boot_args 偏移 `0x840`
记录耗尽时的游标，再进入固件返回入口；备选路径调用 PSCI。
物理读回见 `device-20261113/observer-cache/bootstrap-tables.json`。
`RootRead.S` 导出前使读取核心缓存失效，使 MMU 关闭后的写入可见。

`candidate-kernel-overlay` 的入口指令在物理地址 `0x982f79488` 读回 `0x1400001e`，
与选定内核一致。打包通过 `kernel.patch` 覆盖 UFS KernelSeed 的 RAM 副本，
更新内核前缀与启动参数，保留 DSP 偏移、Data UUID 和 UFS 内容。
早期候选将内核放在 ESP，DarwinBoot 的 RootStage 优先路径读取已安装 KernelSeed；
这项来源差异保存在 `CURRENT.json` 的 `physical_test.kernel_selection`。

驻留 USB 的效率核心为 affinity `0x100`，XNU 使用其余七个核心。
USB 服务在加载结束时接管，固件服务入口位于 Resident 保存中断状态之前。
真机连续六轮查询与固件日志导出成功。`reboot-bootloader` 由固件写入 PMIC 模式，
经过读回检查调用 PSCI；主机收到 Xiaomi fastboot 的 `product: marble` 响应。
32 秒硬件看门狗保留，主机命令间隔大于 10 秒时停止喂狗。
查询撤去后的 bootloader 响应记录见 `device-20261113/kernel-overlay/watchdog-recovery.json`。

`oem firmware-log` 后调用 RootUsb `read_back()` 获取 UTF-16LE 固件文本。
`run_ios27_phone.py` 完成临时启动、继续执行、日志与内存记录采集。
识别 bootloader 时要求 `556fdfe` 的命令响应。
被动交接记录位于 boot_args 页偏移 `0x840`，记录事件、地址与计数器；
记录写入之后执行启动路径。当前内核 `BootArgs` 全局值为零，启动阻塞位于交接阶段。

## 当前入口

`CURRENT.json` 保存当前阶段、来源与候选；
`records/ios27-24B119/progress.json` 保存阶段结果和完整要求。
阶段执行失败会保存错误、日志路径和续接位置。文件内容与构建产物决定完成状态。

| 输入 | 来源 |
| --- | --- |
| IPSW | Apple CDN 的 `iPhone12,1_27.2_24B119_Restore.ipsw` |
| BuildManifest | `records/ios27-24B119/inputs.json` 记录身份与 SHA-256 |
| 原版内核 | `outputs/UniXNU_marble/ios27_24B119/ipsw/24B119/kernelcache.decompressed`；摘要见目标 profile |
| XNU | `12377.121.6` / `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`，加上游 Darwin 26 接口补丁 |
| 构建工具 | `ios27/source/`；提交见 `records/ios27-24B119/build-source.json` |
| 驱动 | `ios27/sources/`；合入与差异见 `source-integration.json` |
| 板级代码 | `package/`、`hal/`、`package/ios27/` |
| 二进制 ABI | `profiles/abi/ios27-24B119/`，从 24B119 原版集合提取 |

命令从工作区根目录执行，设备仓库位于 `work/UniXNU-marble`。

```sh
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/ios27_migration.py status
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/ios27_migration.py fetch
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/ios27_migration.py assets
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/ios27_migration.py prepare
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/ios27_migration.py bootstrap
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/ios27_migration.py kernel
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/ios27_migration.py prelink
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/build_ios27_gpu_userspace.py all
```

系统镜像下载支持 ZIP_STORED 范围续传，完成时核对长度、CRC 和 SHA-256；
AEA 解密使用上游 `Ipsw.disk_image`。Apple 源码使用单独的固定版本引用树。
补丁顺序为架构、SoC、OS、上游 sheng、marble、性能与内存支持、目标调试。
每套补丁对应独立的 XNU 源码目录，摘要改变时生成对应目录。

## 功能范围

迁移涵盖启动与多核、UFS 与持久 Data、显示与背光、触控与按键、
Adreno 725 与 Metal、USB、WCN6855 hw2.1 Wi-Fi、RTC、电池与充电、
ADSP 扬声器、马达、AppSync、手机界面及本仓库系统服务修补。
历史音频锁屏故障、系统触觉入口与渲染问题属于验收范围。
源码合入、编译、预链接、系统启动和每项硬件结果分别记录。

CPU 拓扑启用八个核心。`native_marble_cpu_start` 选择 EPSS/L3 的最高有效档位；
`MarbleGmu` 选择板级 580 MHz GPU 档位与带宽索引 11；显示请求 120 Hz。
DarwinBoot 使用 8 GiB 管理窗口和内存排除表，窗口外空闲区域交给 VM 接管补丁。
实际可用内存、CPU 频率、GPU 频率和显示间隔通过真机采集记录。
USB 默认归 XNU；固件保留 panic 后返回与内存读取接口。
`ROOT_STAGE_DRAM_TABLE` 独立配置表保存窗口外 DRAM 预留，保留固件的启动路径使用这份归属表。
固件保留 16 MiB 返回工作区域；XNU 接管的窗口外区域在 UEFI 标记为 LoaderData。
重入使用同一预留表，panic 返回后的 USB 读取支持这些区域。

24B119 原版内核为 Darwin 27.2.0 / XNU 13432.42.3；构建源码沿用上游
12377.121.6 与兼容补丁。差异见 `kernel-version-gap.json`。
`MarbleABI` 首次 ENOSYS 日志记录调用编号、进程和 PC，调用结果保持对应内核实现。

## USB 与 panic 采集

`capture_ios27.py` 使用 usbmuxd 读取 Apple USB 系统日志和屏幕，
panic 返回固件后读取保留页表、内核日志、panic 文本、帧缓冲与进程状态。
RootStage 命令流使用 `marble-rootstage-observer.lock` 互斥。
原生 USB 的 `marble-debug` 服务监听设备回环地址的 TCP 23456，主机通过 iproxy
读取 `/status` 与 `/kernel.log`。内核 `kern.marble_log` 提供加锁日志快照，保留
`/dev/klog` 的消费游标。服务包含目标构建与源码摘要，采集记录观察到的核心数和内存。
采集写入所选 fileset 内核的 SHA-256；内核布局由对应构建生成。
`prepare_ios27_debug.py` 在候选副本加入显示探针，屏幕包含版本、内核摘要、panic、LR 和回溯；
采集脚本对保留 LR/BT 地址去除 PAC 位并匹配该候选内核与 kext 符号。
断线后的 Apple USB 日志进程在重连时重新建立。

```sh
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/capture_ios27.py \
  --root outputs/UniXNU_marble/ios27_24B119 \
  --output outputs/UniXNU_marble/ios27_24B119/captures/boot-001 \
  --duration 600 --interval 60
```

采集前核对候选内核、布局、启动参数与设备运行版本。
当前设备扫描结果保存在 `progress.json`。设备连接和系统信任确认由操作者完成。

`pack_ios27.py` 生成候选与 SHNGROOT 系统窗口清单，检查 24B119 预链接、runtime、
信任缓存、系统盘及原 Data UUID/几何。Data 位于既有 UFS 分区，22H384 seed 用于
核对安装身份；iOS 27 执行 Data 迁移。原 seed 文件作为来源保存。
`install_ios27.py inspect` 读取既有 unixnu 分区的系统清单与 KernelSeed 邮箱，核对
Data UUID、64 GiB 几何和 APFS superblock。`systems` 更新该分区内系统窗口，
逐段读回摘要，保存 RTC 校准并核对 Data superblock；`boot` 使用 fastboot 临时启动
与安装记录对应的候选。Android 分区表和 boot 槽位保持原布局。

## 任务续接

内核补丁、目标 ABI、组件源码合入分别保存到 `kernel-patches.json`、
`drivers.json`、`source-integration.json`。这些记录列出实际结果和待处理项。
当前并行任务的命令日志：

- `/tmp/marble-ios27-kernel-devin-active.log`
- `/tmp/marble-ios27-driver-devin-active.log`
- `/tmp/marble-ios27-source-devin-active.log`
- `/tmp/marble-ios27-runtime-devin-active.log`
- `/tmp/marble-ios27-assets.log`
- `/tmp/marble-ios27-plugin-build.log`
- `/tmp/marble-ios27-driver-devin-resumed5.log`
- `/tmp/marble-ios27-ioav-devin.log`
- `/tmp/marble-ios27-prelink-devin.log`

观察任务前通过进程或会话句柄确认运行状态。长任务按阶段变化观察。
保留原始 iOS 18 内核、系统输入、Data 和 Android 启动备份。
新的启动候选携带 24B119 的内核、信任缓存、系统与 runtime 摘要。
目标达成需要全部功能范围的真机结果以及性能、锁屏、解锁、重连和故障恢复记录。

## 2026-11-12 构建检查点

目标 OS/SystemOS 镜像、BuildManifest、内核集合、设备树与信任缓存保存 SHA-256。
18 个组件合入状态为 staged；Turnip、Rust arm64e Metal 桥接、PAC 边界和 4K 签名插件构建通过。
19 处目标系统文本修补核对通过，AppSync、Wi-Fi、HAL、壁纸和启动缓存按 24B119 构建。
最终 runtime 为 `outputs/UniXNU_marble/ios27_24B119/runtime-payload/`：716 个文件，28 个 CDHash。
原生 USB 服务通过 `kern.marble_log` 读取内核快照，主机协议与目标编译检查通过。

九个原版类的元类与各自 getter 核对通过；IOBlockStorageDevice 的源实现为 190 个 PAC 槽位、
实例 0x90 字节，与目标原版类匹配。全部驱动库的完整构建和适配方法绑定继续处理。
源码完整的 RELEASE 比较内核构建通过；24B119 闭包预链接分析列出 28 处引用缺口与
184 个函数映射缺口，分析产物位于 `prelink-analysis/`，供目标映射工作使用。
比较内核承担地址匹配输入；设备候选需包含本仓库硬件驱动及完整目标绑定。

活动续接会话保存在 `progress.json` 的 workers 字段。Devin 会话 `equinox-freeze`
处理驱动库，`butter-tanker` 处理 IOAV/热插拔，`graceful-light` 处理预链接映射。
对应终端日志分别为 `/tmp/marble-ios27-driver-devin-resumed5.log`、
`/tmp/marble-ios27-ioav-devin-resumed.log`、`/tmp/marble-ios27-prelink-devin.log`。
工作区 ADB、fastboot 和 Apple USB 扫描为空；设备连接结果随轮次记录。


## 2026-11-12 上游刷新与整机链接检查点

85 个上游仓库抓取记录时间为 02:22（北京时间）。UniXNU 构建工具基线为
`62be7bc54266f0a731796ee71395963abccc0687`；18 个组件执行增量合并，保留 marble 源码修补。
增量更新命令为 `scripts/stage_ios27_components.py --advance`，输入为已有组件树及记录的上游提交。
GPU 物理内存池加入 `gpupool-device-pager-close.patch`，等待设备 pager 的最后映射释放后复用页面。

三个驱动族通过编译和目标槽位检查。整机链接揭示构造函数与显示辅助导入缺口；
`driver_linkage` 阶段保留完整日志。构建脚本处理 const 方法、继承根声明、Itanium 类型替换、
USB 带参数方法的 PAC 名称、HID 私有头文件、ADSP 指针及 GPU 固件符号。
存储清单对齐 `build_ios18_storage.py`；保留 MarbleRootDisk、MarbleUfs、NativeUfs 和目标存储基类。

Turnip 使用更新后的 Mesa 构建通过。Metal 桥接继续构建；内存池增长逻辑按内存类型读取块上限。
GPU 调度检查通过，覆盖 BV 进度识别、环形队列、抢占、400 个任务恢复重放和诊断开关。

活动命令日志：

- 整机内核：`/tmp/ios27-kernel-bootstrap-pager.log`
- Metal：`/tmp/ios27-metal-refresh-build2.log`
- 构造函数推导（SWE-2，equinox-freeze）：`/tmp/marble-ios27-constructor-devin.log`
- 显示辅助函数（SWE-2，butter-tanker）：`/tmp/marble-ios27-display-helpers-devin.log`
- 预链接（SWE-2，graceful-light）：`/tmp/marble-ios27-prelink-devin-resumed3.log`

构造函数与显示辅助函数完成目标代码审核后接入驱动库，执行整机 RELEASE 链接和严格预链接。
设备启动、硬件功能、渲染、性能与生命周期结果待真机采集。


运行时刷新检查点：更新后的 Metal/Turnip、Phone UI、Wi-Fi、AppSync、音频、壁纸和 USB 日志服务完成组装。
载荷包含 716 个文件、28 个 CDHash、71,680,680 字节；SHA-256 为
`04fa3bd0057c4a0e6b67ad7a26b25f492a7adca3b123660c604468e73631b3c9`。
组件增量脚本保留根目录的 deps/out/build 缓存，Wi-Fi 固定版本依赖身份检查通过。
构造函数、显示辅助函数及预链接闭包处于推导阶段，整机候选待这些绑定与链接完成。


## 整机源内核链接与诊断检查点

DEVELOPMENT 整机源内核链接通过，`board-development-kernel.json` 保存摘要及 21 个硬件/诊断入口。
六个 protected 构造函数的完整目标指令保存在 `constructor-bindings.json`；构建逐字节核对。
AcceleratorES 两个构造函数的完整操作序列及父类调用相同，`constructor-review.json` 标明 equivalent-complete-bodies。
显示辅助库集成在启动驱动族；目标 GOT 键地址修补与损坏地址拒绝检查、默认 surface 尺寸/格式/平面数/步幅/容量检查见 `display-helper-parent-review.json`。

Panic 渲染器与保留内存抓取使用 4096 字节 panic 缓冲；保留日志预留 8192 字节，渲染结束发布缓存。
`host-test-status.json` 汇总主机测试范围；生产 DRAM 保留/重载/拒绝路径检查通过。
QEMU 可执行文件在当前主机工具路径发现结果为空，27.2 虚拟启动待执行。

RELEASE 驱动改用 RELEASE 编译记录，并输出可检查的非 LTO Mach-O；通用内核保持 LTO。
RELEASE 源内核首次链接通过；导出清单补齐实现符号的 linker alias 后进行最终链接。
导出配置保留实际实现及别名，通用 Apple 平台缺省接口列于 `release-exports.json`；目标 kext 引用继续严格预链接。
当前运行日志 `/tmp/ios27-release-link-final.log`，预链接两个 SWE-2 会话日志为
`/tmp/marble-ios27-prelink-devin-resumed3.log` 与 `/tmp/marble-ios27-memory-mappings-devin.log`。
设备观察见 `device-observation.json`：ADB、fastboot 为空，USB 枚举为主机外设。


## RELEASE 最终链接结果

完整板级 RELEASE 原生内核为 `outputs/UniXNU_marble/ios27_24B119/kernel.native.macho`，
SHA-256 `45877b748ebf90d03c149f041292788025709f032c6a19c7601f5310cae32824`。
21 个 DEVELOPMENT 硬件/诊断入口及 RELEASE 必备入口见两份 board-*-kernel.json。
RELEASE 保留 5943 个实际实现/别名导出，70 项平台缺省导出记录于 release-exports.json。
AMFI 专用 native_ios27_ce_page_shift 读取值为 14。
使用完整板级 RELEASE 执行严格预链接的结果保存于 `board-prelink-review/summary.json`：
34 处引用、18 个目标映射待完成，stubbed 为 0。此状态尚属源内核构建检查点，fileset 候选与真机结果待后续完成。


## 进程 API 映射逐项审核

`parent-proc-mapping-review.json` 保存 7 项目标函数的完整函数摘要与参数/返回合同。
修补原 fixture 的 proc_find_ident/validated、proc_parent、proc_getexecutablevnode、proc_pid 身份偏差，补入 proc_name/proc_best_name。
`ios27_migration.py prelink` 在运行前核对这些身份及摘要，防止刷新 fixture 时引入已知合同偏差。
`board-prelink-proc-review2/summary.json` 的严格结果为 20 处引用、15 个目标待映射、stubbed=0；
该结果之后修补 proc_pid 身份，待下一次严格运行。预链接工人继续处理其余接口与 CSM/image4。
首次上机候选等待完整闭包和 panic 包装，源 RELEASE 内核与运行时载荷保持上述摘要。


## QEMU 主机安装与虚拟启动（2026-11-12）

本机 Homebrew QEMU 11.1.3，库目录为外置盘 outputs/UniXNU_marble/ios27_24B119/qemu。
8 核 Cortex-A710、8 GiB RAM、4 GiB UEFI 连续窗口、GICv3、TCG、ramfb。
32 GiB qcow2 scratch 盘按需分配，磁盘附加状态见 qemu.json。
运行入口 scripts/run_ios27_qemu.py；每轮保存 EFI、内核与设备树摘要、变量盘、串口、寄存器、截图。
QMP 使用外置目录的短相对路径，满足 macOS Unix socket 长度约束。
首轮进入 XNU，捕获 Unexpected cluster type；虚拟设备树为 8 个 A710 添加 P 类型。
下一轮进入 AMFI 安全扩展启动，捕获 zone_require_ro。
目标 0xfffffff00a737f4c 的输出指针合同对应 code_signing_configuration，修补原 proc-taking 映射。
审核记录 parent-qemu-code-signing-review.json 纳入严格预链接。
marble 上机待用户连接设备，Android 与物理 Data 身份保留。

QEMU 接口修补后严格预链接 SHA-256：60da196a7540949ebd1738ec51cbcbd1504cf1c2099cf3976e43aa3e6d677c80。
诊断内核 SHA-256：91742252ef0de3254c367e7853f9098619f82ed53894861e4edbdabd764d3e38。
设备树补入 crypto-hash-method=sha2-384、amfi-only-platform-code=0。
末轮停点为 Undefined kernel instruction，ELR=0xfffffff00a9a302c，指令 smc #0；
符号为 watchdogWorker+0x28，来源 package/MarbleBootWatchdog.cpp 的高通看门狗调用。
虚拟平台工作需处理板级看门狗的匹配条件；桌面启动状态待完成。
旧 candidate-debug 保存原摘要；该镜像含本轮修补的配置接口绑定，替换打包待完成。


## QEMU platform and API contract checkpoint (2026-11-12)

MarblePlatformMatch.h reads root compatibility during STARTUP. Qualcomm watchdog and EPSS match xiaomi,marble.
MarbleDisplay retains GOP and original display objects; physical GPU, backlight, touch and charging use the board match.
ArmDTPlatform accepts an initialized IODTNVRAM instance: RELEASE IONVRAM.cpp removes nvram-proxy-data after consuming it.
Run run-20261112-042852: configured=1, CPU nubs=8, NVRAM=1. Root initialization reached the physical RootDisks check.
Reproducible source patch and evidence: armdt-release-nvram.json and patches/ios27-upstream-sheng/armdt-release-nvram.patch.

API reviews distinguish physical/local CPU queries, VFS signal/address-width/policy predicates, debugger and CPU-event callbacks,
getsockname, setIndexed, circular/shared queues, PPL page APIs, scheduler rail assertions and Image4 storage.
MarbleVfsPolicy27.c implements the actual process policy bit.
Provisioning-profile and compilation-service contracts are recorded in parent-profile-contract-review.json.
All parent reviews are checked before strict prelink. function-fixture-input-audit.json covers135 whole functions.
The four newer Image4 monitor interfaces remain under semantic audit.

MarbleVirtualStorage.h defines the VM storage integration. After source storage-base registration the VM component registers its metaclass.
UNIXNU_QEMU_ROOT_ARCHIVE links the optional signed VirtIO backend.
SWE-2 owns package/ios27-qemu and scripts/build_ios27_qemu_storage.py; main owns shared integration and QEMU.
Original APFS OS/SystemOS inputs remain read-only, with signed block manifests and public anchor.
VM disk data, keys and logs live on the external drive under qemu/storage.
run_ios27_qemu.py --virtual-storage generates the actual QEMU FDT and attaches the corresponding MMIO disks.
Panic breakpoint comes from the selected fileset symbol. Platform/SMP/storage counters and bounded logs are captured through GDB.


## Signed QEMU storage and monitor ABI checkpoint

The virtual-storage archive builds from pinned VirtioMmioBlock/VerifiedRoot sources. Both190-slot child vtables match the target.
NativeVirtioBlockData supports actual OUT writes, IN reads and negotiated FLUSH; used-ring byte accounting for writes expects the status byte.
The source memory-protection bridge uses mach_vm_protect(map,start,end-start,set_max,protection); source-private vm_map_protect has a different argument order.
Original OS2262016 blocks and SystemOS1508352 blocks pass real C signature and full-image block verification.
The upstream verifier refusal suite passes9 groups. Storage layout/commands/SMMU/DataPersistence host checks pass.
The scratch APFS Data UUID is EBB6F869-6B86-4C29-81CD-DB60F974D56D; its new32GiB image lives under qemu/disks/scratch-data-apfs.qcow2.
MarbleQemuData27.cpp requires this chosen UUID, writable IOMedia and a NativeVirtioBlockData ancestor before mounting it as Data.

Image4 newer monitor-only functions have consumer-qualified signatures and a typed ENOTSUP boundary on the source CS_MONITOR_TYPE_NONE backend.
Normal kext boot is expected to use its local Image4 plane and skip these monitor calls; counters must be inspected in runtime acceptance.
Source review records: devin-image4-contract-review.json and parent-image4-monitor-review.json.
The Apple fast-IPI physical-MPIDR export binds to ArmCpu's actual GIC SGI bridge; the scheduler sleep/rail callbacks use their verified native counterparts.

Signed manifests total115MiB. Embedding them in DATA_CONST collided with retained kext constants.
The VM archive now puts them in read-only __QEMUPOLICY; qemu-policy-segment.patch appends its independent root mapping after LINKEDIT and boot data.
Strict prelink preserves retained mappings, checks split relocations and emits the extra segment.
ios27_prelink.py removes prior candidate reports before emission so an emitter exception cannot inherit old success fields.

Upstream refresh advanced Adreno740 to a8fa18fa0973e00504fb6618c707d9b5e16bdbbe, AdrenoMetal to f6f38082baed4601f385160ebdf4e09e3b0f421a, Mesa to0d234946736de1f290f31eba328b1385be949afc.
Recovery conflict resolution preserves restart_fence for stranded jobs and upstream reason9 diagnostics; kernel GPU archive builds.
Host scheduling/recovery tests pass, including stranded replay and400-job backpressure. Metal/Turnip userspace rebuild remains pending.


## Physical marble phone candidate (2026-11-13)

Artifact: outputs/UniXNU_marble/ios27_24B119/candidate-phone/marble_ios27_temporary_boot.img.
SHA256 f9dcf99893bd2cf695903998b45a172644b3cb52286740e32741fa6b6c9ba562;97,931,264 bytes.
Host image audit passed:43 closure components,0 missing,0 stubbed,HMAC verified,eight CPU affinities.
AMFI void-registration metadata is present in the actual fileset; VM policy segment absent.
Runtime refresh SHA256 ca3384c354558dca620161db8f5aec608a3f40913b7dd2d30b63c08e7fa6963d.
Physical Android/Data inputs retained. Device execution pending USB connection.
Install using scripts/install_ios27.py with candidate-phone/candidate.json after current device and storage identities are checked.
System OS,SystemOS and KernelSeed windows accompany the temporary boot image; installed-UFS seed is required.


## Trust-cache ABI repair (2026-11-13)

Candidate: outputs/UniXNU_marble/ios27_24B119/candidate-trust-contract/candidate.json.
Temporary image SHA256 6e543eb98f262dadad268e84f65db8a51ad2a5e400e8177c3bcb347ca9875441.
The preceding target-overlays candidate reached init, mounted OS/SystemOS/Data, and initialized the display before trust-cache manifest overflow panic.
The 24B119 fixture assigned four trust-cache exports to incompatible functions. The repaired bindings preserve native query validation and typed load checks; records/ios27-24B119/trust-cache-contract.json contains the argument evidence.
AMFI obtains the hash length from csblob_get_cdhash_with_length: its GOT entry 0x7e9fb58 resolves to 0xa6bdbdc.
Native query tokens occupy 16 bytes, matching the retained library consumer.
Strict prelink and image audit pass with zero missing/stubbed references. Seven XNU CPUs and firmware USB affinity0x100 remain the diagnostic configuration.
Firmware reboot-to-bootloader and fastboot product query succeeded. fastboot boot completed at20:48 +08:00.
Observer: logs/trust-contract-device.log; runtime result pending capture.


The trust-contract candidate passed the preceding manifest panic and reached a vnode_put data abort at0xa36b56c.
Evidence: device-20261113/trust-contract/early-036.json and vnode-diagnostic.json.
AMFI method0x8f33f50 calls original0xa6fbfd0 through GOT0x7e9fe60, then its RAII cleanup calls vnode_put. The fixture bound this getter to csproc_get_blob, returning a read-only signature object.
Original0xa6fbfd0 retains p_textvp under the process lock and performs vnode_getwithref_noblock. Bind it to proc_getexecutablevnode_noblock, preserving vnode iocount acquisition and paired vnode_put.
records/ios27-24B119/executable-vnode-contract.json records the308-byte original hash and source contract.
Matching tests:27 pass. Strict prelink passes. Device product query returns marble in Xiaomi fastboot; executable-vnode image packaging underway.


The executable-vnode candidate reached genuine launchd execution and initialized WCN6855 firmware/en0.
Its dyld console records: shared cache map syscall failed; following check_np returns ENOMEM.
Evidence: device-20261113/executable-vnode/console-ring.log. Only launchd/kernel_task were present in the process list at panic.
run_ios27_phone.py now captures the native console ring beside msgbuf, preserving user dyld diagnostics.
CONFIG_NO_PRINTF_STRINGS removes the native shared-region trace text even when its runtime level is3.
patches/ios27-debug/shared-region-diagnostics.patch restores printf in bsd/vm/vm_unix.c and osfmk/vm/vm_shared_region.c; default trace level3.
A new signature-specific XNU source tree is building; CURRENT.json records its patch identity.


Shared-region native diagnostic result: map_file returns KERN_INVALID_ADDRESS, syscall EFAULT.
Error-level console records mapping start0x2f3538000 size0x2976c000, ending0x31cca4000, beyond source shared-region end0x300000000.
The original24B119 cache header declares sharedRegionSize0x18794c000 and maxSlide0x20000000; old source capacity0x180000000.
patches/ios27-performance/target-shared-region-capacity.patch sets ARM64 capacity0x200000000 (base0x180000000, end0x380000000).
Host audit of all80 original cache files/105 mappings with the maximum512MiB slide passes; record shared-region-capacity.json.
Capacity source kernel build and strict prelink running; CURRENT.json tracks the source patch identity.


Capacity repair passed on phone: dyld reports cache mapped system-wide, and launchd prints hello and enters boot tasks.
Next blocker: fsck launch constraint in-tc-with-constraint-category rejected; launchd panics on boot task failure.
Live static trust module contains fsck CDHash3d814b1ee0b2c5c585a6c112fb9be6e28be522af category3, launchd CDHash51d581be575bfc1e9cf2587eb39beddedb235158 category2.
Live static module SHA256f413a511afac50303068e35487a78f2f59c41e8eec3359e34ffb96d567a50504.
Typed query recorder package/ios27-runtime/MarbleTrustQuery27.c delegates to native query_trust_cache_with_length and retains its exact result/token.
Its64 records capture hash, query type, return, token, capabilities/category and library status.
Query-recorder kernel compiled and prelinked; temporary image packaging.


Launch-constraint blob repair passed on phone: fsck entered boot tasks and launchd advanced into logd/XPC services.
The next rejection is /usr/libexec/installd, error c[8]p[1]m[1]e[6] Constraint not matched; logd then exited.
Evidence: device-20261113/launch-blob/console-044.log and early-*.json. Firmware USB channel returned to Xiaomi fastboot after capture.


The no-lc diagnostic candidate reached service bring-up after the installd constraint rejection.
Observed: fsck and mount, logd, keybagd, configd, wifid, bluetoothd, watchdogd, amfid, lockdownd, sharingd, fairplayd, XPC services and network interface activity.
No kernel panic is present in the current sample; RootStage USB observer remains active.


## 2026-11-14：迁移与引导 Loading 修补

- 真机候选：`candidate-setup-build-marker`，镜像 SHA-256 `25027a6948da5ff4a8f83232f67d981f4bd3a6b1509082cf621bf883bee3fd72`。
- FairPlay 服务采用原 marble 启动策略停用 `com.apple.fairplayd.H2`，解除安装元数据查询等待。
- ADT 恢复 `kern.maxvnodes=66560` 和 `sleep-disabled=1`；真机节点上限 133120。
- 精确匹配平台签名 `audiomxd` 的自身目录搜索，解除照片迁移依赖链的音频服务等待。
- Setup 完成标记按 24B119 区分，保留旧标记；真机显示锁屏，Setup 进程退出。
- 证据：`/Volumes/Extra/https-github-com-oosexgang-hackios-xnu/outputs/UniXNU_marble/ios27_24B119/device-20261114/setup-build-marker`。解锁、桌面交互和休眠稳定性待验收。
- 原 64 GiB Data UUID 保留，使用临时 fastboot boot。

## 2026-11-14：桌面稳定性与自主控制

目标顺序：稳定性及 panic 路径、壁纸渲染与添加、GPU/Metal 上游方案及响应性能。设备操作由 USB 完成，保留 Android、Root 和原 Data。

运行候选、采集进程与证据目录读取 CURRENT.json 的 active_candidate、touch_in_range、scanout_late_ack 字段。

- ADT /buttons 声明 home-button-type=2，物理按键为 hold/voldown/volup。24B119 类型 2 表示无 Home 键；运行时 BSPlatform=2，UIDevice hasHomeButton=0。
- 触点报告增加 In Range，保持 62 字节。USB 上滑解锁、打开设置和列表滚动具备截图证据。
- 6 次深浅色切换通过，界面进程退出记录为空，GPU 失败计数为 0。结果位于 device-20261114/touch-in-range/appearance-stability.json。
- 右上角下拉进入控制中心，玻璃背景渲染具备截图证据。
- 返回桌面复现全黑：GPU 复制完成，显示提交返回 0xe00002d6，呈现客户端关闭。两个界面进程保持存活，panic 栏为空。
- 原 scanoutFlip 在 IOSleep 后检查期限，CPU 延迟唤醒时遗漏硬件确认。生产函数回归用例在原实现失败，修补后通过；真实未完成的提交保留超时返回。固件邮箱轮询采用相同完成优先顺序。

控制入口：scripts/remote_ios27_ui.py --evidence ABSOLUTE_EVIDENCE --name UNIQUE_NAME。动作参数支持 --tap X Y、--swipe X Y TO_X TO_Y --duration-ms 550 --hold-ms 800，以及 --button power。脚本使用文件事件等待动作确认与截图。

outputs/UniXNU_marble/ios27_24B119/tools/screen-ocr IMAGE.png 输出本机文字坐标，源文件为 scripts/ios27_screen_ocr.swift。

采集目录的 memory-request.json 支持按 symbol/address 读取内核，附带 pid 时读取进程地址空间；reboot-bootloader 使用固件复位。context-request.json 接收 processes 名称前缀数组与 max_threads，空数组请求截图。observer-stop-request 在 USB 操作边界停止采集；attach 路径跳过通用 fastboot 枚举。

下一步核对 scanout_late_ack 候选上机结果。marble_scanout_wait_stats 的 6 个 u64 字段为：提交数、延迟确认数、真实超时数、末次等待纳秒、末次 flush 寄存器、末次返回码。通过后继续控制中心、锁屏解锁和应用切换，再处理壁纸。


### 2026-11-14 远程操作与墙纸保存事务

scanout-late-ack 候选通过设置返回桌面、多任务、深浅色切换、控制中心、侧键唤醒和上滑解锁。显示等待统计记录一次超过50ms的成功确认，真实超时0，GPU失败0，主界面退出0。`device-20261114/scanout-late-ack/stability-results.json`保留结果；`lock-unlock-02.png`为解锁桌面。锁屏的熄屏状态需要侧键唤醒，触摸结果与实际画面分别核对。

墙纸图库、渐变编辑器可用，颜色面板关闭后可点击添加。点击设为墙纸组合返回空白墙纸预览，桌面保持黑色。`poster-save-trace`临时候选扩展已有文件调用环形记录，按进程采集PosterBoard、Preferences、GradientPosterExtension、NullPoster。`transaction-add-01`和`transaction-pair-01`保留保存前后记录。缓存写入、同步、重命名成功；PosterBoard的客户端签名状态、身份和DER权限查询出现EPERM，日志同步记录`process-info-codesignature`拒绝。

`poster-signature-query`为待真机比较的兼容候选：精确匹配平台签名的`/Applications/PosterBoard.app/PosterBoard`，目标须为同UID的平台签名进程，允许状态、身份、XML/DER权限只读查询；系统调用的审计token检查保持原调用顺序。生产函数提取边界检查通过，文件和凭据引用平衡。此项效果待保存事务真机比较，结果记入CURRENT.json。观察器新增`_marble_poster_signature_queries`计数。

85个上游仓库于2026-11-14T08:29:24+08:00完成拉取。Mesa的Chromium镜像连接失败后使用进程局部Git URL重写访问Mesa主站，提交`f4ab1f91b0749f34b6342885bc5ff98a731e92e3`。本批签名查询修补保留既有GPU运行包，GPU源码整合单列进行。


### 墙纸结果与 Glass 缓冲故障

`poster-signature-query`真机的`signature-first-screen-01.png`和`signature-unlock-01.png`显示蓝色渐变墙纸，桌面中心RGB116,167,255；设置页能读取当前墙纸及多组配置。进入新增墙纸时backboardd触发SIGSEGV，SpringBoard因服务重建退出。故障保存在`device-20261114/poster-signature-query/fault-gallery-glass-01`。PC为`GlassBackgroundFilter::render+0xbc8`；ESR0x92000046为写入的L2翻译故障，x28=0x12344ac50，FAR=x28+0x15。scanout真实超时0，GPU失败1与客户端退出同时出现。此处CPU指针来自`CA::OGL::MetalBufferPool::create_external_buffer`的返回对象首字段，分配请求28字节、16字节对齐，CA缓冲对象x23=0x1267e7db0。

`glass-buffer-capture`候选在进程退出前保留CA缓冲对象、故障CPU指针读取结果、MetalBufferPool、NBBuffer及Rust资源引用。全局`_marble_glass_fault`共8760字节，头部`<6Q2I`，16条记录每条544字节，格式`<QIi16s512s`。捕获匹配24B119精确PC。NBBuffer的_handle偏移24由当前运行包Objective-C元数据核对。新增采集属于待运行诊断，不宣称渲染故障修复。上机后复现新增墙纸并读取该全局。


### 2026-11-14 映射生命周期定位与主机写满恢复

`native-canvas-scanout`在缩放显示设置页复现相同的`GlassBackgroundFilter::render+0xbc8`。`metal-flow=0`也复现，故障与转换线程开关分列处理。证据`device-20261114/native-canvas-scanout/fault-zoom-glass-01`，全局转储`glass-buffer-crash-01.bin`：CA缓冲指针0x1344663a0，所属缓冲CPU基址0x134440000，长度0x40000，offset0x263a0，大小28；指针处copyin返回EFAULT14。NBBuffer_handle=0x146d94dc0，CPU-visible字节+0x48为1；Resource::Kept对应Cell的pinned字节（Arc指针+0x78）为1，缓冲对象存活。GPU恢复和SMMU故障计数均为0。

`buffer-mapping-history`候选增加`_marble_buffer_mapping_events`动态环形表（1024条，每条16个u64）。字段sequence、cntpct、owner_pid、kind、connection、address、bytes、handle、token、pc、lr、fp、4个调用者LR。kind1 taskMap，2成功taskUnmap，3连接dropViews，4成功munmap，5成功mmap（>=64KiB）。GPU映射记录>=64KiB，munmap记录全部。首次glass故障另存包含FAR的历史到`_marble_glass_mapping_history[32][16]`、条数`_marble_glass_mapping_history_count`。资源图采集加深至RuntimeBuffer及前12个词的指针。既有`_marble_glass_fault`格式仍为8760字节。编译marble-boot和marble-gpu后prelink。源码/构建树NativeAdrenoPublish.cpp为同一文件，编辑无需复制。

原生canvas试验保留到`native-canvas-profile.toml`与`runtime-payload-native-canvas`。显示模式仍1125x2436，点击Default完成未切换；LargerText确认弹窗未确认。诊断候选恢复375pt mini布局、dpu-scanout1，保留metal-flow0及pooled运行包。原生canvas/直接扫描性能试验等待稳定性修复。

外置盘ENOSPC于09:38:19导致旧观察器退出；后续attach发现Xiaomi fastboot。删除242个可重建的firmware.bin、volume.img、kernel.patch，保留所有最终启动镜像、KernelSeed、内核与设备证据，释放约19GiB。删除清单位于logs/marble-derived-cleanup*.json。观察器写满恢复改为转移到/private/tmp/marble-observer-*、重定向stdout/stderr并转移待处理控制请求，原目录写observer-output-redirect.json；UI客户端支持跟随该标记。每60秒检查剩余空间，128MiB以下转移，逐轮ENOSPC/EDQUOT捕获保持USB通道。生产函数故障注入检查位于host-tests/observer-storage/test_storage.py，检查通过；真实卷写满恢复未人为复现。
