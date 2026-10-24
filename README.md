# Marble_UniXNU

marble 的 UniXNU 适配代码。系统目标为 iPadOS 17.7.11（21H461）。

| 目录或文件 | 内容 |
| --- | --- |
| package | RAM 块设备、驻留返回和 USB 加载器 |
| scripts | 构建、镜像生成、打包和 USB 控制 |
| upstream_patches | 上游仓库集成补丁 |
| upstream_revisions.json | 上游来源与提交版本 |
| records | 阶段验证记录 |

执行 `python3 scripts/snapshot_upstream.py` 导出工作目录中的集成补丁。补丁对应 `upstream_revisions.json` 中的提交。

系统盘与内核产物位于工作区 `outputs/UniXNU_marble`。`scripts/build_compact_system.py` 使用仓库内的 `scripts/mkapfs.py` 生成紧凑系统盘。

通过 `root_control.py start --kernel` 更新内核时，传输包尾部包含 64 KiB 日志区。panic 返回 RootStage 后读取内核日志与 panic 文本：

```sh
../marble_hackios/venv/bin/python scripts/root_control.py logs --output ../../outputs/UniXNU_marble/kernel.log
```

`start --kernel` 可附加 `--observe-seconds 180`，在 SystemOS 挂载阶段启动单次计时，期满通过 panic 保存日志并返回 RootStage。默认值为 0，由内核自身的退出路径返回。

`start --kernel --data-seed PATH` 读取同目录的 `seed.json`，核对摘要与卷 UUID；传输包包含压缩 APFS 种子，内核解压后发布可写 RAM Data 卷。每轮启动从种子创建 Data。`--boot-args=TEXT` 附加用户空间及后续初始化阶段读取的启动参数。

`--boot-args="rootstage-panic-process=SpringBoard"` 在指定进程首次崩溃退出时保存寄存器、共享缓存偏移和用户栈。`scripts/symbolicate_root_log.py` 使用同版共享缓存与 `.symbols` 解析地址。
