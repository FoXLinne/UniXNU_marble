# Redmi Note 10 Pro（sweet）

搭载 SM7150 的社区设备移植。当前状态依据下方固定版本的维护者记录。

| 仓库 | 维护者 |
| --- | --- |
| [UniXNU-sweet](https://github.com/CrackerCat/UniXNU-sweet) | [CrackerCat](https://github.com/CrackerCat) |
| [UniXNU-sweet](https://github.com/zeroninevibedslop/UniXNU-sweet) | [zeroninevibedslop](https://github.com/zeroninevibedslop) |

## 当前状态

CrackerCat 源码记录了内核/平台构建和 QEMU CPU/定时器检查，该设备树的固件与根卷集成待完成。

独立的 zeroninevibedslop 移植以 iOS 16.7.16 / Darwin 22 为目标，维护者报告 UFS 根卷挂载、`launchd`、SpringBoard/backboardd surface 交换和 USB CDC-ECM 遥测。Adreno 618 供电及 GMU/HFI 启动已有记录，GPU 命令提交和 Metal 加速仍在开发。各仓库使用各自的 OS 输入和构建路径。

## 来源

- [源码基线: CrackerCat/UniXNU-sweet](https://github.com/CrackerCat/UniXNU-sweet/blob/d3dd6fdf9e889f08b2564d92f7fef5b5c913263e/README.md)
- [iOS 16 移植: zeroninevibedslop/UniXNU-sweet](https://github.com/zeroninevibedslop/UniXNU-sweet/blob/843c8ed5398388cf1116b095fb6ef30dbed6b2ad/README.md)

## 构建

主机检查：

```sh
python3 scripts/check-host.py
python3 scripts/make-sweet-adt.py --output out/sweet.adt
```

设备仓库集成：

```sh
python3 ../UniXNU/unixnu defconfig sweet_defconfig -D . --out out/driver
python3 ../UniXNU/unixnu status -D . --out out/driver
```

## 使用此移植

1. 阅读上方仓库的构建说明和固定版本记录，准备对应的固件、系统镜像和板级输入。
2. 按该仓库的命令构建，通用配置见[构建与启动](/zh/building)。
3. 按[启动验证顺序](/zh/porting/bring-up)逐项验证，通过[日志抓取](/zh/porting/log-capture)保存结果。
4. 更新[移植记录](/zh/porting/records)，记录实际使用的源码版本及最后确认的阶段。
