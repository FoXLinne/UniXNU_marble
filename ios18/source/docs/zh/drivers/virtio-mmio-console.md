# VirtioMmioConsole

使用 split queue 的 VirtIO 1.2 MMIO 控制台传输。

[源码仓库](https://github.com/UniXNU/VirtioMmioConsole).

组件通过匹配的 MMIO 控制台传输日志和诊断字节，设备配置提供 MMIO 范围与中断路由。

## 集成

组件目录记录实现范围、硬件与 OS 要求、板级依赖、源码版本、构建阶段、输出和运行时标识符。所选配置按依赖顺序运行构建命令。

```sh
./unixnu build kernel -D <device-repository>
```

[组件目录](/zh/reference/components)
