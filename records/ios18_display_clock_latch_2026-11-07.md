# 显示总线时钟与缓冲锁存

原厂 DisplayDxe 使用 `gcc_disp_ahb_clk` 和 `disp_cc_mdss_ahb_clk`。固件通过 EFI Clock 协议取得并启用两个时钟，然后读取 CTL 和固件源管线。协议前缀 ABI 来自 marble 固件树 `QcomPkg/Include/Protocol/EFIClock.h`。

手机原厂 `msm_drm.ko` 的 CTL stage 表给出 VIG0/1/2/3 字段位 0/3/6/26，DMA0/1 字段位 18/21；高位来自 LAYER_EXT，DMA2/3 来自 LAYER_EXT2。板级代码按实际混合器配置选择源管线。

固件拥有显示寄存器访问，XNU 和固件通过 KernelSeed mailbox 的 4096 偏移交接 buffer slot。状态与请求分别位于两个 64 字节缓存行。源帧 GPU 复制结束后提交 slot；CTL flush 清除后完成显示锁存。

候选 `gpu-display-clock-latched`：

- 内核 SHA-256：`4c743bf65e96c43893ddb3ab4a8dd7faae54b68b1a465821d0d6acb50743c37f`
- 原生内核 SHA-256：`36d80b9ed6d600f13f4f56ede6dc6dc5439b051b5c7d43084d9800a43763d258`
- 实机 setup=2，VIG0 管线掩码=1，CTL=0x16000，flush 掩码=1。
- GOP=1080×2400，stride=4320，slot span=10371072。
- SpringBoard PID 27，backboardd PID 60。
- 实机 GPU 呈现与锁存计数：18782；锁存超时计数：0。
- 运行中一次 GPU 队列恢复完成并继续呈现，现场恢复数组：`[1, 1, 0, 33970, 0, 48, 0, 0]`。
- 现场文件：候选目录 `unlock-notification.json`、`before-producer-island.json`、`render-kernel.log`。

用户录像显示拖动时图标区域被水平边界截断；用户图片显示后台白色卡片及启动内容黑白条块。渲染完成同步和官方灵动岛布局属于接续批次。

`getvar:display-latch` 返回 ready、error、pipe mask、checkpoint、锁存次数。`getvar:display-clock` 读取活动接口的时钟计数与帧计数。后者包含于接续的 `gpu-producer-island` 固件构建。
