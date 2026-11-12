你是本工程的 Developer。先完整阅读 AGENTS.md，它是强制规则。

本批次按顺序完成：docs/tasks/T04-bt-spp.md → T05-store.md → T06-app.md。
T04 必须先完成源码核实（三个问题，引用 IDF 文件:行号）再写代码；核实结果触发 stop_conditions 时立即停止并报告。
每个任务结束后写 docs/reports/<任务号>.md。

硬性约束：
- 冻结文件一个字节都不能改；BATCH-A 已通过验收的 .c 文件不要改，除非构建需要且在报告中说明。
- 不烧录（禁止 idf.py flash/monitor/erase-flash）、不安装软件、不 git commit、不改 RTK_main 以外的文件。
- 每个 shell 先执行 `. ~/esp/esp-idf/export.sh >/dev/null`。
- 不得通过关闭告警、删除/跳过测试来让检查通过；每个任务结束跑一次 host ctest 回归。

全部完成后，回复一行：BATCH-B DONE，以及每个任务的结论。
