# POCO X7 / Redmi Note 14 Pro 5G（malachite）

malachite 是搭载 MediaTek MT6878（Dimensity 7300-Ultra）的 POCO X7 与 Redmi Note 14 Pro 5G。下面的移植是独立的社区项目，UniXNU 组织不维护它。内核和根文件系统来自 HackiOS；UEFI 固件由 uefi-malachite（uefi-sheng / Mu-Silicium 的 MT6878 移植）提供。

| 仓库 | 维护者 | 状态 |
| --- | --- | --- |
| [hackios-xnu-kernel（malachite 分支）](https://github.com/ghidgdsr/hackios-xnu-kernel/tree/malachite) | [ghidgdsr](https://github.com/ghidgdsr) | XNU rootfs 已加载；iOS 用户态尚未正常启动 |
| [uefi-malachite](https://github.com/ghidgdsr/uefi-malachite) | [ghidgdsr](https://github.com/ghidgdsr) | UEFI 正常工作；UFS 正常 |

## 状态

| 阶段 | 状态 |
| --- | --- |
| UEFI 固件 | 工作；直接引导 XNU 加载器，内部存储和设备树暴露的唯一按键可用（没有启动菜单） |
| UFS | 正常；`userdata` 上的 iOS 16.5 系统镜像作为根（`md0`） |
| XNU | 在 core 0 启动（PSCI 把其他核放在 EL2） |
| XNU rootfs | 已加载；launchd 与 lsd 已运行 |
| iOS 用户态 | 尚未正常启动；lsd 与 usermanagerd 频繁报错，原因正在定位；加载器的日志监视核和日志槽是 bring-up 手段 |
| 显示 | 仅 LK 帧缓冲上的加载器文本 |
| USB | 未实现 |
| 触控 | 未实现 |
| GPU | 未实现 |
| Wi-Fi、蓝牙、音频 | 未实现 |

UEFI正常工作，UFS正常，XNU rootfs已加载，USB、触控等未实现。

## 构建

启动镜像是固件卷加上追加的 HackiOS XNUP 负载（BootShim、UEFI 固件卷和负载），打包为 Android boot image v4，内核为 `lz4 -l` 流。加载器和负载的打包命令在内核仓库的 `docs/malachite.md`。固件构建：

```sh
./build_uefi.py -d malachite -r RELEASE
```

刷入 `boot_a`、把 a 槽设为活动并重启：

```sh
fastboot flash boot_a Mu-malachite.img
fastboot --set-active=a
fastboot reboot
```

内核、iOS 系统镜像和负载是外部输入，不存放在仓库中。

## 维护

维护者拥有仓库和发布决定。UniXNU 文档记录移植及其状态，不接管维护。
