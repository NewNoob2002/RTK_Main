你是本工程的 Developer。先完整阅读 AGENTS.md（强制规则）和 docs/adr/ADR-0002-cpp-esp-idf.md。

本批次按顺序完成：docs/tasks/T13-os-bt.md → T14-store-app.md → T15-test-app.md。
T13 开始写代码前，先对照 reference/c-v1/docs/reports/T04.md 和 IDF v6.1 源码，确认 SPP 的三个结论仍然成立；不成立时立即停止并报告。
每个任务结束后写 docs/reports/<任务号>.md。

硬性约束同 DISPATCH-C：冻结文件不可改；不烧录、不安装、不联网下载、不 commit；不关闭告警或跳过测试；每个任务结束都跑 host ctest 回归（普通构建和 ASan/UBSan 构建）。BATCH-C 已通过验收的 .cpp 文件不要改，除非构建确实需要，并在报告中说明原因。

全部完成后，回复一行：BATCH-D DONE，以及每个任务的结论。
