# 构建、输入与主机检查

命令在工作区根目录执行。设备仓库为 `work/UniXNU-marble`，本地工具环境为 `tools/marble-build/venv`。Python 运行环境采用 3.11 或以上，以支持 `tomllib`。

## 输入归属

| 输入 | 位置与约束 |
| --- | --- |
| iOS profile | `profiles/ios-18.7.11-22H384.toml`，固定 OS、ABI 与 IPSW 来源 |
| 系统盘与 Cryptex | 本地挂载的 22H384 N841OS / N841SystemCryptex，构建时通过参数选取 |
| Data 种子 | `outputs/UniXNU_marble/ios18_arm64e/session-20261105/ufs/Data.rdisk` 与同目录 `seed.json` |
| 存储约束 | `profiles/marble-storage-22H384.json`，64 GiB Data、卷身份与系统版本 |
| DSP / Wi-Fi / 功放 | 固件输入、来源摘要、`profiles/wifi-wcn6855-hw21.json`、`profiles/audio/aw88263-music.json` |
| 来源树 | `ios18/source/`、`ios18/sources/`、`firmware/source/`，提交来源见 `source_origins.json` |

本地系统、Data、固件及设备启动备份单独保留。仓库提供源码、输入说明、摘要与构建入口。

## 来源与构建

上游同步修改本地来源树，并在 `build/upstream/latest.json` 保存合入结果与冲突。2026-11-11 的同步快照已归档至 `records/upstream-sync-2026-11-11.json`。

```sh
work/UniXNU-marble/scripts/update_upstream.sh
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/snapshot_upstream.py
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/build_ios18_arm64e.py kernel \
  --output outputs/UniXNU_marble/ios18_arm64e
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/build_ios18_arm64e.py prelink \
  --output outputs/UniXNU_marble/ios18_arm64e
```

来源整合遇到冲突时，采用仓库保存的设备实现，并将待整合提交保留在来源记录。全量构建结果与具体增量构建结果分别保存。

GPU、Wi-Fi、固件和镜像入口见 [集成说明](BRINGUP.md) 及各脚本 `--help`。打包携带所选内核、GPU addon、owner trust cache、AppSync、Wi-Fi runtime 与固件摘要。

## 音频插件

```sh
tools/marble-build/venv/bin/python work/UniXNU-marble/scripts/build_marble_haptics.py \
  --os-root /Volumes/CrystalUpdate22H384.N841OS \
  --output outputs/UniXNU_marble/audio-runtime \
  --diagnostic-probe \
  --wifi-runtime outputs/UniXNU_marble/wifi-service-native-20261110/runtime \
  --speaker-transport builtin
```

诊断构建含循环路由快照和系统测试音探针。`--routing-trace` 对应的导入钩子在 boot-lock3 触发 audiomxd 崩溃，弃用。当前插件与内核属于实验版本，锁屏恢复验收失败。

## 主机检查

以下检查使用主机缓冲区和模拟端点，覆盖协议及生命周期。

```sh
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror \
  work/UniXNU-marble/package/tests/charging_transport.c \
  work/UniXNU-marble/package/QcomGlink.c work/UniXNU-marble/package/QcomMdt.c \
  work/UniXNU-marble/package/QcomSmem.c work/UniXNU-marble/package/QcomSmp2p.c \
  work/UniXNU-marble/package/QcomQmp.c work/UniXNU-marble/package/QcomRpmh.c \
  -o /tmp/marble-charging-transport
/tmp/marble-charging-transport

xcrun clang -std=c11 -O2 -Wall -Wextra -Werror \
  -I work/UniXNU-marble/package \
  work/UniXNU-marble/scripts/tests/marble_haptics_hal.c \
  -framework CoreAudio -framework CoreFoundation -o /tmp/marble-haptics-hal
/tmp/marble-haptics-hal

xcrun clang -std=c11 -O2 -Wall -Wextra -Werror \
  -I work/UniXNU-marble/package/audio \
  work/UniXNU-marble/scripts/tests/marble_audio_graph.c \
  work/UniXNU-marble/package/audio/ShengGpr.c \
  work/UniXNU-marble/package/audio/ShengAudioReach.c -o /tmp/marble-audio-graph
/tmp/marble-audio-graph
```

真机结果保留各自候选和证据路径。手机处于 Android，设备操作停止；本页命令范围为主机构建与检查。
