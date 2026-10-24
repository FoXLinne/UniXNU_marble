使用中文。

每个修改批次完成后提交 commit，记录验证结果。提交前运行 scripts/snapshot_upstream.py 保存上游集成修改。阶段记录放在 records。大型镜像与构建产物保存在仓库外。

以 upstream_revisions.json 记录的上游版本为集成基线，更新上游时刷新版本记录。硬件验证以 USB 状态和屏幕日志为依据。
