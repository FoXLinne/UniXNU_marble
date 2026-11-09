# sheng 与 marble UFS 读写比较

结论：双方使用相同的 UFS 命令提交代码，单槽串行、轮询完成、128 KiB DMA 缓冲区和 FUA 写入属于共有设计。替换成 sheng 驱动对这些性能限制的作用有限。marble 值得借鉴 sheng 的读取耗时统计、每个只读卷独立的缓冲区，以及 FUA 写入后的同步处理。Data 架构迁移涉及磁盘格式与内存占用，适合作为单独评估项。

## 来源

本次通过 `git ls-remote` 核对远端 main，与本地远端引用一致。

| 来源 | 提交 |
| --- | --- |
| UniXNU-sheng | `9109d4ca01bbf28d89170fc26ae4115fb7c1e117` |
| ShengUfs，sheng 设备 gitlink 同版 | `af14068c329f4d8693d20a3ed38a435ade04bb75` |
| RamBlock，UniXNU 组件 gitlink | `4be2d7eb05ef5a98126c91e0302c149140c0e3e1` |
| marble | 工作区 `package/MarbleUfs.cpp`、`package/MarbleRootDisk.cpp` 与 `ios18/sources/ShengUfs/` |

RamBlock 比较读取上述提交的文件；本机 NativeRamBlock 检出目录包含本地修改，比较使用 git 对象中的原文。

[sheng 设备配置](https://github.com/UniXNU/UniXNU-sheng/blob/9109d4ca01bbf28d89170fc26ae4115fb7c1e117/board.toml)、[UFS 命令实现](https://github.com/UniXNU/ShengUfs/blob/af14068c329f4d8693d20a3ed38a435ade04bb75/RootCommands.h)、[UFS 卷接入](https://github.com/UniXNU/ShengUfs/blob/af14068c329f4d8693d20a3ed38a435ade04bb75/NativeUfs.cpp)、[Data 驱动](https://github.com/UniXNU/RamBlock/blob/4be2d7eb05ef5a98126c91e0302c149140c0e3e1/NativeRamBlock.cpp)、[Data 直接读写](https://github.com/UniXNU/RamBlock/blob/4be2d7eb05ef5a98126c91e0302c149140c0e3e1/DirectDisk.h)、[持久页位图](https://github.com/UniXNU/RamBlock/blob/4be2d7eb05ef5a98126c91e0302c149140c0e3e1/PersistOverlay.h)。

## 实现比较

| 项目 | sheng | marble | 性能含义 |
| --- | --- | --- | --- |
| 命令提交 | 请求槽 0，共用 host 锁 | 相同 | 队列深度为 1；读写共用控制器串行执行 |
| 完成等待 | 调用线程轮询，`IODelay(2)` | 相同 | I/O 等待占用执行线程与 CPU |
| 传输上限 | 128 KiB，单 PRDT 数据段 | 相同 | 大请求分块执行 |
| DMA 与缓存 | 专用连续 DMA 缓冲区；提交前 clean/invalidate，完成后 invalidate | 相同 | 两者承担相同的缓存维护开销 |
| 数据复制 | DMA 区、bounce/stage、调用者缓冲区之间复制 | 相同结构 | sheng 也承担两次 CPU 复制 |
| OS/SystemOS 读取 | 每个卷持有 windowLock 与 bounce | 两个卷共用 readLock 与 readScratch | marble 在控制器 host 锁之外增加跨卷串行范围 |
| Data 读取 | 内存保存不可变种子；修改页从 UFS 读取；连续修改页合并，最多 32 页 | Data 全卷从 UFS 读取，单次最多 32 页 | sheng 的种子页读取省去 UFS 访问，但占用常驻内存 |
| Data 写入 | 连续页合并；FUA 写数据；首次修改页追加持久位图写入 | 连续块写入；每条写入带 FUA | sheng 同样等待持久化，首次修改页增加位图写入 |
| Data 同步 | NativeRamBlock 的同步回调返回成功，依据是写入与位图在完成前持久化 | 同步回调发 SYNCHRONIZE CACHE | marble 可能多承担 flush；实际收益取决于同步调用频率 |
| 读写对齐 | 对外 512 B、内部 4 KiB；整页写合并，边缘页读改写 | 同样转换为 4 KiB；含部分块的处理分块先读后写 | marble 可把边缘块与中间完整块分开，减少部分请求的预读字节数 |
| 读取统计 | `native_ufs_read_ticks` 与 `native_ufs_verify_ticks` | 上层读写次数计数 | sheng 可区分读取耗时与校验耗时；其中 read_ticks 包含 host 锁等待和 DMA 到 bounce 的复制 |

`RootCommands.h`、`RootQueue.h`、`RootTransportLayout.h`、`RootWindowRead.h` 与 marble 对应文件逐字相同。marble 的只读卷接入使用自己的读取循环。

sheng 默认 `sheng-data-persist=2`。该模式从内存种子读取原始页，从 UFS 读取修改页，写入在持久化后完成。它是种子加持久页的架构。修改页的后续读取仍需访问 UFS；将此方案描述为整个 Data 卷的 RAM 缓存会误判其性能。

## 优化取舍

| 顺序 | 方案 | 结论与约束 |
| --- | --- | --- |
| 1 | 增加锁等待、读写命令、同步命令、复制、传输字节数与耗时分布统计 | 借鉴 sheng 的计时入口并细分阶段。将卡顿窗口与存储等待关联，决定后续改动 |
| 2 | 每个 OS/SystemOS 卷使用独立 scratch 与锁 | 可借鉴 sheng，缩短跨卷串行范围；硬件队列深度保持 1 |
| 3 | 核对 FUA 写入后的同步处理 | sheng 的 Data 同步回调有可借鉴依据。marble 需要确认全部写入已完成、FUA 支持与错误传播、同步与并发写入的顺序后采用对应处理 |
| 4 | 拆分部分块与完整块写入 | 保留首尾 4 KiB 块的读改写，中间整块写入省去预读；收益取决于实际请求对齐情况 |
| 5 | 保持现有 Data 格式，引入符合 IOKit 语义的写缓存/FUA/barrier 策略 | 属于新设计；IOStorageAttributes 提供 ForceUnitAccess 选项，现有入口丢弃 attributes。需要同时实现写缓存状态、同步顺序与失败处理 |
| 6 | 异步请求与中断完成、多槽队列、调用者缓冲区 DMA | 属于双方共有底层的新实现。异步工作线程解决调用线程阻塞，中断完成减少轮询，多槽队列才增加设备并发；三者作用各有区别 |
| 7 | 迁移 sheng 的种子＋持久页 Data 架构 | 冷读种子页有优势；需要种子内存、位图及额外写入，涉及现有 Data 格式迁移。当前源码比较支持保留 marble 的原生 UFS Data 卷 |

UFS 当前 gear、lane、UniPro/AXI 时钟与带宽属于另一个测量项。两套接管代码依赖固件建立的链路，源码本身无法给出当前链路速率或带宽瓶颈结论。

## 实际性能判断

marble 启动记录 `outputs/UniXNU_marble/metal-update-20261107/ui-d84-native-points-20261108-ufs/ufs-startup.json` 在 93.400916 秒窗口内，读取计数增加 198466，写入计数增加 41454。该计数混合上层请求与 Data 分块，耗时、字节数与排队分布需要补充采集。

源码证据支持 UFS 是性能排查重点，支持上述机制比较；其对当前桌面卡顿的占比及每项优化的实际收益，需要运行时耗时采样。临时启动与 UFS 启动进入系统后共用该存储实现，双方共有的 UFS 限制会同时存在。
