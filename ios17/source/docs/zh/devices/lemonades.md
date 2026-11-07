# 一加 9R（lemonades）

SM8250-AC 社区设备移植，由 soooink 维护。

- [soooink/UniXNU-lemonades](https://github.com/soooink/UniXNU-lemonades)

## 当前状态

移植包含板级配置、defconfig、ADT 生成器和打包工具。Linux LLVM 19 构建可生成 3 MiB 固件卷和 header-v2 启动镜像。

固件实机检查、内核启动和根卷集成待完成。需要测量目标设备的 userdata 几何参数并完成 Synaptics S3908 触控集成。依赖版本按 `source-lock.json` 和仓库构建指南选择。

## 使用此移植

1. 阅读下方固定版本的源码说明和验证记录，准备该设备要求的固件和系统输入。
2. 克隆设备仓库，在检出目录内按其构建指南准备依赖：

```sh
git clone --branch main https://github.com/soooink/UniXNU-lemonades.git
cd UniXNU-lemonades
git checkout 5f52da00907a0cec41e73a81269549bc11991bff
git submodule update --init --recursive
```

3. 在该版本完成对应构建后，按[启动验证顺序](/zh/porting/bring-up)验证，并按[日志抓取](/zh/porting/log-capture)保存最后确认的阶段。
4. 将结果写入[移植记录](/zh/porting/records)。板级结构和组件选择见[移植设备](/zh/porting/steps)。

## 来源

- [板级配置: soooink/UniXNU-lemonades](https://github.com/soooink/UniXNU-lemonades/blob/b4bf1f0c2141564fcb65a63e0de2d3fefee053f2/board.toml)
