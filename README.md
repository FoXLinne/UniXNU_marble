# Marble_UniXNU

marble 的 UniXNU 适配代码。板级默认目标为 iOS 18.7.11（22H384）；iPadOS 17.7.11（21H461）提供 GPU 真机验收基线。

| 目录或文件 | 内容 |
| --- | --- |
| package | RAM 块设备、驻留返回和 USB 加载器 |
| scripts | 构建、镜像生成、打包和 USB 控制 |
| upstream_patches | 上游仓库集成补丁 |
| upstream_revisions.json | 上游来源与提交版本 |
| records | 阶段验证记录 |

执行 `python3 scripts/snapshot_upstream.py` 导出工作目录中的集成补丁。补丁对应 `upstream_revisions.json` 中的提交。

系统盘与内核产物位于工作区 `outputs/UniXNU_marble`。`scripts/build_compact_system.py` 使用仓库内的 `scripts/mkapfs.py` 生成紧凑系统盘。

通过 `root_control.py start --kernel` 更新内核时，传输包附带 1 MiB 回传区，起始 64 KiB 保存日志。panic 返回 RootStage 后读取内核日志与 panic 文本：

```sh
../marble_hackios/venv/bin/python scripts/root_control.py logs --output ../../outputs/UniXNU_marble/kernel.log
```

`start --kernel` 可附加 `--observe-seconds 180`，在 SystemOS 挂载阶段启动单次计时，期满通过 panic 保存日志并返回 RootStage。默认值为 0，由内核自身的退出路径返回。

`start --kernel --data-seed PATH` 使用 `DBRDISK1` 种子，读取同目录的 `seed.json`，核对摘要与卷 UUID；传输包包含压缩 APFS 种子，内核解压后通过 `RamSparseDisk` 发布可写 RAM Data 卷。每轮启动从种子创建 Data。`--boot-args=TEXT` 附加用户空间及后续初始化阶段读取的启动参数。

`--boot-args="rootstage-panic-process=SpringBoard"` 在指定进程首次崩溃退出时保存寄存器、共享缓存偏移和用户栈。`scripts/symbolicate_root_log.py` 使用同版共享缓存与 `.symbols` 解析地址。

`root_control.py screen --output PATH.png` 读取崩溃或观测期结束前的帧缓冲缩略图，并输出完成的像素复制次数。

`profiles/ios-18.7.11-22H384.toml` 选择 iOS 18 arm64e。使用 `scripts/build_ios18_arm64e.py` 的 `fetch`、`kernel`、`prelink`、`bootimg` 命令，均通过 `--output` 指定独立产物目录；`bootimg` 接收 `--unique-chip-id HEX` 或 `UNIXNU_UNIQUE_CHIP_ID`。打包要求完整预链接映射，设备身份和 trust cache 来自该 profile 固定的 IPSW。结果与启动记录见 `records/ios18_arm64e_2026-11-04.json`。

iOS 18 GPU 构建使用 `scripts/build_ios18_gpu.py`。`userspace` 构建 arm64e Turnip、启用 LTO 的 Rust 桥接层、Metal 插件、owner trust cache 与 NGPUADD2 addon；`kernel` 构建 A725 内核归档及 22H384 accelerator PAC 绑定。系统卷与 Cryptex 卷挂载后，`all` 完成用户态、RELEASE 内核、预链接与临时启动镜像：

```sh
../marble_hackios/venv/bin/python scripts/build_ios18_gpu.py all \
  --root ../../outputs/UniXNU_marble/ios18_arm64e \
  --images ../../outputs/UniXNU_marble/ios18_arm64e/session-20261105/interaction-upstream-app-publication/storage_images.json \
  --data-seed ../../outputs/UniXNU_marble/ios18_arm64e/session-20261105/ufs/Data.rdisk \
  --assets ../../outputs/UniXNU_marble/ios18_arm64e/gpu/boot --skip-setup
```

该命令使用保留安装的启动模板与 Data 身份。`pack_ios18_ufs.py` 默认携带 GPU addon、配套 trust cache 和 120 Hz 呈现请求。运行配置采用 580 MHz / BW11、双优先级环模式 2、CP 页表切换、GPU 中断唤醒、IOSurface 映射复用与 GPU 扫描缓冲搬运。驱动启动完成后进入用户空间；故障恢复执行 GPU 复位和任务重放；驻留返回在 XNU 次级核心停止后关闭 GPU DMA。

GPU addon 包含从 22H384 提取的 Metal 库清单，运行时缓存保存在持久 Data。产物、版本、构建结果与真机验收状态记录在 `records/ios18_gpu_2026-11-05.json`。
