# iOS 18 OSArray 容量构造入口

22H384 原始入口 `0xfffffff00842b858` 与当前原生符号 `__ZN7OSArray12withCapacityEj` 完成逐指令核对。原始函数长 180 字节，SHA-256 为 `eeefed206a101013a49fb23197a27f5c665566a83e82ac6d8ab8c9a8ec69cc44`。

核对内容包括元类分配槽位 +0x68、容量初始化槽位 +0xd0、失败释放槽位 +0x28，以及三个认证调用的鉴别值。原生函数保持分配、初始化、释放和返回路径；编译器的认证寄存器复制方式存在差异。历史完整映射将该原始入口匹配到同名原生符号。

固定原始镜像 SHA-256、函数字节摘要和原生符号名称写入 reviewed fixture。原生地址随构建符号解析。

执行 `scripts/build_ios18_arm64e.py prelink --output ../../outputs/UniXNU_marble/ios18_arm64e` 完成。原先共用此入口的 221 个引用完成绑定，闭包缺失引用 0，函数占位 0。日志：`/tmp/marble-gpu-flow-prelink.log`，构建根 `prelink.log`。

显示、GPU 性能与渲染行为记录使用各候选的实机文件。此批次记录地址绑定。
