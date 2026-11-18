你是本工程的 Developer。先完整阅读 AGENTS.md（强制规则）、docs/adr/ADR-0015-board-profiles.md，以及 ADR-0011、ADR-0014 中与启动步骤相关的章节。

本批次只有一个任务：docs/tasks/B1-board-profile.md。开始前读任务卡；结束后写 docs/reports/B1.md。

硬性约束：
- 冻结文件一个字节都不能改，用 `sha256sum -c docs/frozen.sha256` 自查。认为冻结文件有错时，停下并在报告中写明证据。
- 不烧录、不连接串口设备、不安装软件、不联网下载（包括 uvx/pip/组件管理器）、不 git commit、不改 RTK_main 以外的文件或 reference/。
- 每个 shell 先执行 `. ~/esp/esp-idf/export.sh >/dev/null`。
- 不得通过关闭告警、删除或跳过测试来让检查通过。门禁用 `tools/ci/run_gates.sh`。
- 报告里如实写每条命令的结果（PASS/FAIL/NOT_RUN/BLOCKED），没跑过的检查不能写成通过。
- 硬件事实只以 ADR-0015 为准；ADR 未给出的引脚或电平写 UNKNOWN 并停下询问，不要猜。

完成后回复一行：BATCH-B DONE，以及 B1 的结论。
