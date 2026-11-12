你是本工程的 Developer。先完整阅读 AGENTS.md，以及 docs/adr/ADR-0002-cpp-esp-idf.md、docs/adr/ADR-0003-c-cpp-split.md。

本批次按顺序完成：docs/tasks/T14-store-app.md（修订版）→ docs/tasks/T15-test-app.md。
每个任务结束后写 docs/reports/<任务号>.md。

硬性约束同 DISPATCH-E：冻结文件不可改；不烧录、不安装、不联网下载、不 commit；不关闭告警或跳过测试；每个任务结束跑 host ctest 回归（普通与 ASan/UBSan）。已验收的 .c/.cpp 不要改，除非构建确实需要，并在报告中说明原因。

全部完成后回复一行：BATCH-F DONE，以及每个任务的结论。
