# 脚本入口

命令相对工作区根目录执行，Python 环境为 `tools/marble-build/venv/bin/python`。构建输入见 [BUILDING.md](../docs/BUILDING.md)。

| 范围 | 入口 |
| --- | --- |
| 上游与来源 | `update_upstream.sh`、`prepare_sources.py`、`snapshot_upstream.py` |
| 固件与内核 | `prepare_firmware.py`、`build_ios18_arm64e.py` |
| 系统与镜像 | `build_compact_system.py`、`pack_ios18_ufs.py` |
| GPU 与显示 | `build_ios18_gpu.py`、`capture_ios18_gpu_trace.py` |
| Wi-Fi | `build_marble_wifi.py`、`build_marble_wifi_userspace.py`、`capture_marble_wifi.py` |
| 音频与触觉 | `extract_marble_audio_acf.py`、`build_marble_haptics.py`、`capture_marble_haptics.py` |
| 主机检查 | `tests/marble_audio_graph.c`、`tests/marble_haptics_hal.c`、`../package/tests/` |
| 工作区维护 | `clean_workspace.py` |
| 设备诊断工具参考 | `root_control.py`、`root_ui.py`、`observe_ios18_boot.py` |

2026-11-11 手机恢复安卓，设备操作停止。设备工具作为历史接口保存。固件 USB 命令/应答流需要单一读取者，采集脚本使用 observer lock；自定义工具需沿用该互斥约束。

参数、路径和输出格式读取具体脚本 `--help`。最后音频镜像属于失败候选，其摘要与结果在 CURRENT.json 和 records 中保存。
