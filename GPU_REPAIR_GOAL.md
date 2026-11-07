# marble iOS 18 GPU 修复目标

设备：marble / SM7475 / Adreno 725。系统：iOS 18.7.11（22H384），XNU 11417.140.69 arm64e RELEASE。

用户指定 UI 型号：iPhone 15 Pro Max（`iPhone16,2`）。界面身份、设备名称与 artwork subtype 按该型号配置；实体显示为 1080×2400。状态栏、大屏布局、壁纸及图标着色共同验证。

## 修复范围

| 编号 | 用户问题与要求 | 验收内容 | 状态 |
| --- | --- | --- | --- |
| 1 | 相册、部分系统应用、主屏幕自定义闪退 | 相册启动、图库滚动、照片查看与编辑；系统应用启动；桌面布局、尺寸、明暗与图标着色；记录退出原因 | 相册照片查看有一次通过证据；其余待修复与验收 |
| 2 | 帧率稳定性差、左右滑动撕裂、高斯模糊渲染异常 | 桌面分页、通知中心、控制中心、文件夹、搜索与模糊背景；画面正确性及帧间隔 | 待修复与验收 |
| 3 | 应用启动出现白色与黑色斑块，任务切换器预览白屏或黑屏 | 冷启动、热启动、前后台切换、任务卡片生成与浏览；预览内容对应应用画面 | 待修复与验收 |
| 4 | 帧率约 30，低负载场景约 50–60，操作动画响应差 | 定位 CPU、内存、GPU、编码、提交、完成回调、扫描输出瓶颈；解除人为限制；提高容量不足的资源预算；保留性能与功耗证据 | 高频核心纳入 XNU；帧率及响应待修复与验收 |
| 5 | 壁纸全部显示失败，添加失败 | 默认壁纸、照片壁纸、壁纸选择器、添加、预览、保存、锁屏及桌面显示；配置与场景生命周期 | 待修复与验收 |
| 6 | UI 顶部红色错误调试信息 | 修复 `rdar:45025538` 对应的屏幕与状态栏布局匹配；时间、电量及状态栏显示 | 待修复与验收 |
| 7 | 设置子页面空白、添加壁纸页面空白、主屏幕自定义保存后还原 | 设置子页面内容加载；壁纸选择、添加与保存；主屏幕明暗、图标尺寸及着色保存，退出页面、应用切换及重启后的配置保持 | 待修复与验收 |

## 总验收

七组问题共同组成完成条件。真机采用本轮源码构建的内核、预链接产物及用户态组件。

建立可复现的场景目录，覆盖上述功能、首次使用、缓存复用、反复切换与并发负载。按面板实测刷新能力确定帧率门槛，至少 99% 场景达到帧率和稳定性要求；保存场景操作、帧时间分布、退出记录、渲染故障与响应延迟。

每项结果绑定源码提交、镜像摘要、系统版本及实机证据。截图保留能够定位缺陷或证明功能结果的画面；重复画面与均匀黑屏采用文字状态。诊断采用按需采集，发行配置关闭逐帧追踪及显示寄存器探测。运行开销与电池数据进入性能评估。

## 本轮实机证据

- `9ede489`：Photos PID 388 进入照片查看页，采集时存活；该次观察的 Photos 异常退出记录为空。
- 主屏幕图标着色触发 SpringBoard `SIGTRAP`。地址解析指向 `PBUIPosterViewController fetchWallpaperProminentColor:`，断点条件为 home/lock 壁纸控制器同时为空。
- 壁纸配置、资源访问、PosterBoard 场景建立及渲染链属于修复范围。空状态回调的补充属于崩溃路径处理，壁纸显示与添加功能保持独立验收项。
- 启动画面条纹、模糊错误、任务预览和低帧率保持待处理状态。
- 证据目录：`outputs/UniXNU_marble/ios18_gpu_v1_2026-11-06/hardware-20261106/live-9ede489`。
- `promax-ui`：`fastboot boot` 返回 `OKAY`；270 秒采样记录 `Marble RootStage USB is unavailable`；用户反馈设备停止响应。该候选状态为启动失败，六组功能验收状态保持待处理。
- 该候选 native SHA-256：`8b120a910a8c72333d4462f7702e9065f62cc65cee735c2ebe4f5e1af8bfb7b6`；fileset SHA-256：`a9528fca3096b2e4d714f6a946f26c96a3c63d0404601fb96eff168dda5ae67c`。BootShim、UniXNU、DarwinBoot 和 MarbleBootstrap 摘要与 `wallpaper-color` 对应文件一致。
- 启动故障定位入口：通过实体按键进入 FASTBOOT，接通加载及日志采集通道，读取固件与内核交接阶段。主机 fastboot、ADB 设备列表为空时，实体按键为实机操作依赖。

## 启动与渲染修复批次

- `a0a46d4`（2026-11-07）：真机截图的 PAC 故障位于 `native_marble_ui_dyld_policy`，板级对象按 DEVELOPMENT 布局从 `proc+0x5b0` 读取 executable vnode；RELEASE 内核字段位于 `+0x548`。访问路径改用 `proc_getexecutablevnode` 与 `vnode_put`，对象采用 RELEASE 编译记录。候选启动 launchd、SpringBoard 与 backboardd。
- 内核返回的 USB 流程由服务核停止控制器会话、清理传输状态并启动枚举。真机执行 `oem interrupt`，10 秒采样返回 `RootStage2`、`phase=firmware`、`returns=1`、`return-usb-status=0`；保留日志读取 1048576 字节，`oem retry` 进入 kernel 阶段且 USB 响应。证据：`records/ios18_pac_usb_return_2026-11-07.json`。

- `94ac38b`：BootShim 按输入区与目标区的重叠关系选择固件和追加负载的搬运顺序。汇编执行记录覆盖六个输入地址；候选在六个地址保留固件与负载。整包执行核对保留负载 SHA-256。实机装载地址为待采集项。
- `46be13d`：22H384 `_sys_dcache_flush` 的指令为 `dsb ish; ret`。Metal 前端在命令提交与完成边界，对命令引用的 IOSurface 去重并执行 `dc civac`。插件包含该指令，画面、帧时间及稳定性进入实机验收。
- 壁纸包包含四个 collection、103 个资源、24 个 PNG 图层，以及 iOS 17、Clownfish 两组 `360w-800h@3x~iphone` 变体。Pro Max 默认索引指向包内 collection。壁纸显示、照片壁纸添加、保存及场景生命周期保持验收项。
- 候选：`hardware-20261106/promax-wallpaper-cache/marble_temporary_boot.img`；SHA-256：`3a03e8aa9900ebff007bc480ceff67bd5785c27cb00d012a04686d36148479a7`。内核、预链接、Metal 插件、壁纸资源和镜像构建通过；真机启动与六组场景验收待设备连接。
- 阶段记录：`records/ios18_boot_wallpaper_cache_2026-11-06.json`。上游集成快照由 `scripts/snapshot_upstream.py` 生成；四个 GPU 组件的远端 HEAD 与构建基线一致。
