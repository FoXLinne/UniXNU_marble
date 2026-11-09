# Marble_UniXNU

marble 的 UniXNU 适配代码。板级默认目标为 iOS 18.7.11（22H384）；iPadOS 17.7.11（21H461）提供 GPU 真机验收基线。

iOS 17 源码位于 `ios17/source/` 和 `ios17/sources/`；iOS 18 源码位于 `ios18/source/` 和 `ios18/sources/`。这些目录由本设备仓库管理，源码来源记录在 `source_origins.json`。上游检出位于工作区 `work/upstream/`，用于更新和对照。

UEFI 固件源码位于 `firmware/source/`，硬件输入位于 `firmware/inputs/`。本机构建工具位于工作区 `tools/marble-build/`。

构建入口从本仓库复制源码到 `outputs/UniXNU_marble/` 下的构建目录。

```sh
bash scripts/update_upstream.sh
python3 scripts/prepare_firmware.py
firmware/venv/bin/python scripts/prepare_sources.py --os 17
firmware/venv/bin/python scripts/prepare_sources.py --os 18
```

GPU 修复范围、逐项状态与总验收条件见 [GPU_REPAIR_GOAL.md](GPU_REPAIR_GOAL.md)。

| 目录或文件 | 内容 |
| --- | --- |
| package | RAM 块设备、驻留返回和 USB 加载器 |
| scripts | 构建、镜像生成、打包和 USB 控制 |
| upstream_patches | 上游仓库集成补丁 |
| upstream_revisions.json | 上游来源与提交版本 |
| records | 阶段验证记录 |

执行 `python3 scripts/snapshot_upstream.py` 生成本仓库源码摘要 `source_snapshot.json`。

Wi-Fi 板级入口为 `package/MarbleWifi.cpp`，目标为 SM7475 PCIe0 上的 QCA6490。
`marble-wifi=1` 采集时钟和 GPIO 交接状态；`marble-wifi=2` 执行电源投票、
XO/WLAN_EN 时序、PHY 初始化、Gen2 链路训练与端点识别。阶段 7 记录 PCI ID、
subsystem ID、BAR、SoC 和 MHI 状态。真机端点为 `17cb:1103`、WCN6855 hw2.1，链路为 Gen2 ×1。
`marble-wifi=3` 配置 128 MiB DMA 窗口；`marble-wifi=4` 启动 ath11k 与扫描。
DMA 使用 stream `0x1c01`、IOVA `0xa0000000`，输出宽度来自 SMMU IDR2，
ATOS 能力来自 IDR0。固件输入由 `profiles/wifi-wcn6855-hw21.json` 固定摘要，
打包参数为 `--wifi-firmware DIR`。`MarbleWifiDiagnostics` 提供 IORegistry 状态与日志。
`build_marble_wifi_userspace.py` 构建 iOS 18 认证服务与签名启动缓存，
打包参数 `--wifi-runtime DIR` 安装服务、WLAN 设备树节点与 MAC 地址。
原生 USB 采集入口为 `scripts/capture_marble_wifi.py OUTPUT_DIR`。
真机结果见 `records/wifi-smmu-2026-11-10.json` 与 `records/wifi-service-2026-11-10.json`：
WCN6855 固件进入 Mission Mode，原生 en0 扫描、WPA2 认证与 DHCP 通过。
22H384 的关联请求与事件布局记录于 `records/wifi-network-abi-22H384.json`。
测试镜像保留内核 USB、8 核与原生服务，状态经 Apple USB diagnostics relay 读取。
PHY 表来自原机设备树，ath11k 构建入口为 `scripts/build_marble_wifi.py`。
源码与构建记录见 `records/wifi-adaptation-2026-11-10.json`。

系统盘与内核产物位于工作区 `outputs/UniXNU_marble`。`scripts/build_compact_system.py` 使用仓库内的 `scripts/mkapfs.py` 生成紧凑系统盘。

通过 `root_control.py start --kernel` 更新内核时，传输包附带 1 MiB 回传区，起始 64 KiB 保存日志。panic 返回 RootStage 后读取内核日志与 panic 文本：

```sh
firmware/venv/bin/python scripts/root_control.py logs --output ../../outputs/UniXNU_marble/kernel.log
```

`start --kernel` 可附加 `--observe-seconds 180`，在 SystemOS 挂载阶段启动单次计时，期满通过 panic 保存日志并返回 RootStage。默认值为 0，由内核自身的退出路径返回。

`start --kernel --data-seed PATH` 使用 `DBRDISK1` 种子，读取同目录的 `seed.json`，核对摘要与卷 UUID；传输包包含压缩 APFS 种子，内核解压后通过 `RamSparseDisk` 发布可写 RAM Data 卷。每轮启动从种子创建 Data。`--boot-args=TEXT` 附加用户空间及后续初始化阶段读取的启动参数。

`--boot-args="rootstage-panic-process=SpringBoard"` 在指定进程首次崩溃退出时保存寄存器、共享缓存偏移和用户栈。`scripts/symbolicate_root_log.py` 使用同版共享缓存与 `.symbols` 解析地址。

`root_control.py screen --output PATH.png` 读取崩溃或观测期结束前的帧缓冲缩略图，并输出完成的像素复制次数。

`profiles/ios-18.7.11-22H384.toml` 选择 iOS 18 arm64e。使用 `scripts/build_ios18_arm64e.py` 的 `fetch`、`kernel`、`prelink`、`bootimg` 命令，均通过 `--output` 指定独立产物目录；`bootimg` 接收 `--unique-chip-id HEX` 或 `UNIXNU_UNIQUE_CHIP_ID`。打包要求完整预链接映射，设备身份和 trust cache 来自该 profile 固定的 IPSW。结果与启动记录见 `records/ios18_arm64e_2026-11-04.json`。

iOS 18 GPU 构建使用 `scripts/build_ios18_gpu.py`。`userspace` 构建 arm64e Turnip、启用 LTO 的 Rust 桥接层、Metal 插件、owner trust cache 与 NGPUADD2 addon；`kernel` 构建 A725 内核归档及 22H384 accelerator PAC 绑定。系统卷与 Cryptex 卷挂载后，`all` 完成用户态、RELEASE 内核、预链接与临时启动镜像：

```sh
firmware/venv/bin/python scripts/build_ios18_gpu.py all \
  --root ../../outputs/UniXNU_marble/ios18_arm64e \
  --images ../../outputs/UniXNU_marble/ios18_arm64e/session-20261105/interaction-upstream-app-publication/storage_images.json \
  --data-seed ../../outputs/UniXNU_marble/ios18_arm64e/session-20261105/ufs/Data.rdisk \
  --assets ../../outputs/UniXNU_marble/ios18_arm64e/gpu/boot --skip-setup
```

该命令使用保留安装的启动模板与 Data 身份。`pack_ios18_ufs.py` 默认携带 GPU addon、配套 trust cache 和 120 Hz 呈现请求。运行配置采用 580 MHz / BW11、双优先级环模式 2、CP 页表切换、GPU 中断唤醒、IOSurface 映射复用与 GPU 扫描缓冲搬运。驱动启动完成后进入用户空间；故障恢复执行 GPU 复位和任务重放；驻留返回在 XNU 次级核心停止后关闭 GPU DMA。

GPU addon 包含从 22H384 提取的 Metal 库清单，运行时缓存保存在持久 Data。产物、版本、构建结果与真机验收状态记录在 `records/ios18_gpu_2026-11-05.json`。

GPU 真机交互使用 `scripts/root_ui.py`，命令包含 `tap`、`swipe`、`longpress`、`home`、`switcher` 和 `screen`。屏幕坐标为 1080×2400；`screen` 使用本次部署的 `--kernel` 与 `--output` 路径。驻留固件通过 `input-control` 能力和内核共享请求字传递触控及截图请求。

CPU 归属定义位于 `package/MarblePkg/MarbleCpuOwnership.h`。驻留 USB 服务使用 MPIDR `0x100` 效率核心，XNU 使用 `0x000`、`0x200` 至 `0x700`，覆盖 2.9184 GHz 核心。`getvar:usb-affinity` 读取 USB 命令处理核心的 MPIDR；打包清单记录双方归属。

驻留 USB 在 `ReturnTest` 高字发布带标记的秒级心跳，低字传递输入和复位请求。内核 watchdog 工作线程检测心跳变化，停滞 30 秒时记录状态。`reboot-bootloader` 在 USB 应答传输完成后发布复位请求。观测记录包含 `_marble_usb_heartbeat` 与 `_marble_usb_stalls`。

`scripts/observe_ios18_boot.py --once --skip-threads` 保存进程列表、异常退出、GPU 状态与内核日志。`scripts/capture_ios18_gpu_trace.py --swipes 6` 记录滑动窗口、提交到退役时序与交换间隔，输出 `events.json` 和 `summary.json`。交换退役时序与面板刷新时序分别记录。

marble 的通话状态采用空闲策略：22H384 的通话数量与桌面通话查询返回空闲，通话初始状态获取和刷新入口立即返回。内核在签名校验后的页面核对原始指令并应用七处修补，启动策略关闭 CommCenter 辅助服务、callservicesd、语音信箱及通话记录同步服务。来源与运行记录见 `CURRENT.json` 的“通话空闲策略”。

RootStage 保存初始显示管线和显示 SMMU 状态，内核返回时恢复 GOP 扫描输出。USB 替换 KernelSeed 期间暂停旧显示、RTC 共享区访问，启动前绑定新共享区。`scripts/test_root_display_handoff.py` 检查共享区迁移、管线与 SMMU 恢复、呈现超时和恢复。基线打包可通过 `pack_ios18_ufs.py --firmware-binaries PATH` 选择本轮构建的 MarbleBoot 应用。

iOS 18 用户态构建携带 arm64e `MarblePhoneUI.dylib` 和配套 owner trust cache。`build_ios18_gpu.py ui` 为用户态产物构建屏幕适配器。Metal 的 2D 纹理能力查询填充 22H384 的内嵌 `limits` 表，供 Tungsten 与 PhotosUICore 读取。

显示寄存器采样需要显式启动参数 `marble-display-mmio=1`。发行镜像采用默认关闭配置，操作 5 返回权限状态。采样路径使用内核 IOKit 映射，显示电源与时钟归属验证属于启用前提。帧提交与交换间隔观测读取 RAM 中的事件记录。

iOS 18 UFS 打包使用 `profiles/marble-storage-22H384.json` 校验 Data 容量、UUID 和 OS build。设备 Data 容量为 64 GiB，元数据输入为 `outputs/UniXNU_marble/ios18_arm64e/session-20261105/ufs/Data.rdisk` 旁的 `seed.json`。
