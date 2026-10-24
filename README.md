# Marble_iOS

marble 的 UniXNU 适配代码。

| 目录或文件 | 内容 |
| --- | --- |
| package | RAM 块设备、驻留返回和 USB 加载器 |
| scripts | 构建、镜像生成、打包和 USB 控制 |
| upstream_patches | 上游仓库集成补丁 |
| upstream_revisions.json | 上游来源与提交版本 |
| records | 阶段验证记录 |

执行 `python3 scripts/snapshot_upstream.py` 导出工作目录中的集成补丁。补丁对应 `upstream_revisions.json` 中的提交。

系统盘与内核产物位于工作区 `outputs/UniXNU_marble`。`scripts/build_compact_system.py` 使用仓库内的 `scripts/mkapfs.py` 生成紧凑系统盘。
