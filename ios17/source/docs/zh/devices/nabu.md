# 小米平板 5（nabu）

搭载 SM8150 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [unixnu-nabu](https://github.com/Lux12138/unixnu-nabu) | [Lux12138](https://github.com/Lux12138) |
| [unixnu-nabu](https://github.com/zzc775/unixnu-nabu) | [zzc775](https://github.com/zzc775) |
| [unixnu-nabu-build](https://github.com/3214791900-del/unixnu-nabu-build) | [3214791900-del](https://github.com/3214791900-del) |
| [uefi-nabu-build](https://github.com/3214791900-del/uefi-nabu-build) | [3214791900-del](https://github.com/3214791900-del) |
| [NativeDwc3](https://github.com/Lux12138/NativeDwc3) | [Lux12138](https://github.com/Lux12138) |

## 当前状态

XNU 已到达 `load_init_program()` 并尝试执行 `/sbin/launchd`。记录中的运行返回 `errno 2`（`ENOENT`），需要修正根卷内容。源码内核可在本地 Linux 构建。

记录中的内存布局来自扩容至 12 GiB RAM 的平板，使用配置前需要测量目标设备的 DRAM 和分区参数。

## 来源

- [状态: Lux12138/unixnu-nabu](https://github.com/Lux12138/unixnu-nabu/blob/2eff53cdba9f45228e70c80102158fd0bc755236/STATUS.md)

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
