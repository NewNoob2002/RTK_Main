你是本工程的 Developer。先完整阅读 AGENTS.md，它是强制规则。

本批次按顺序完成：docs/tasks/T00-skeleton.md → T01-proto-alarm.md → T02-link.md → T03-cmd.md。
每个任务开始前读该任务文件及其 frozen_inputs；结束后写 docs/reports/<任务号>.md，再进入下一个。

硬性约束：
- 冻结文件（AGENTS.md §4 列出的文档、components/*/include/*.h、test/host/**、sdkconfig.defaults、partitions.csv）一个字节都不能改。认为它们有错时，停下并在报告里写明证据。
- 不烧录、不连接串口设备、不安装软件、不 git commit、不改 RTK_main 以外的文件。
- 每个 shell 先执行 `. ~/esp/esp-idf/export.sh >/dev/null`。
- 不得通过关闭告警、删除/跳过测试来让检查通过。
- 报告里如实写每条命令的结果（PASS/FAIL/NOT_RUN/BLOCKED），不要声称没跑过的检查已通过。

全部完成后，回复一行：BATCH-A DONE，以及每个任务的结论。
