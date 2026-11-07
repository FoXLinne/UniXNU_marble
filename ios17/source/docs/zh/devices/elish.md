# 小米平板 5 Pro Wi-Fi（elish）

SM8250-AC 社区设备移植，由 hpadmx 维护。

- [hpadmx/UniXNU-elish](https://github.com/hpadmx/UniXNU-elish)

## 当前状态

板级配置标识为 Wi-Fi 型号，仓库包含板级数据、分阶段 defconfig 和 ADT 脚本。维护者记录 `scripts/check-elish.py` 返回零错误和两个 TODO。

组装启动镜像前，需要测量 userdata 几何参数与 DRAM 大小，准备 `elishPkg`，并固定构建源码版本。

## 使用此移植

1. 阅读下方固定版本的源码说明和验证记录，准备该设备要求的固件和系统输入。
2. 克隆设备仓库，在检出目录内按其构建指南准备依赖：

```sh
git clone --branch master https://github.com/hpadmx/UniXNU-elish.git
cd UniXNU-elish
git checkout a357eae98a0da0796c8efec8519825367da1e3e7
git submodule update --init --recursive
```

3. 在该版本完成对应构建后，按[启动验证顺序](/zh/porting/bring-up)验证，并按[日志抓取](/zh/porting/log-capture)保存最后确认的阶段。
4. 将结果写入[移植记录](/zh/porting/records)。板级结构和组件选择见[移植设备](/zh/porting/steps)。

## 来源

- [README](https://github.com/hpadmx/UniXNU-elish/blob/a357eae98a0da0796c8efec8519825367da1e3e7/README.md)
- [板级配置](https://github.com/hpadmx/UniXNU-elish/blob/a357eae98a0da0796c8efec8519825367da1e3e7/board.toml)
