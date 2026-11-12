你是本工程的 Developer。先完整阅读 AGENTS.md（强制规则）和 docs/adr/ADR-0002-cpp-esp-idf.md。

背景：工程已从 C 改为 C++20（esp-idf profile）。旧的 C 实现在 reference/c-v1/，只读，可作为行为参考；接口和验收测试以冻结的 C++ 头文件（components/*/include/**）和 test/host/** 为准。

本批次按顺序完成：docs/tasks/T10-skeleton.md → T11-proto.md → T12-core.md。
每个任务开始前读任务卡及其 frozen_inputs；结束后写 docs/reports/<任务号>.md，再进入下一个。

硬性约束：
- 冻结文件一个字节都不能改，用 `sha256sum -c docs/frozen.sha256` 自查。认为冻结文件有错时，停下并在报告中写明证据。
- 不烧录、不连接串口设备、不安装软件、不联网下载（包括 uvx/pip/组件管理器）、不 git commit、不改 RTK_main 以外的文件或 reference/。
- 每个 shell 先执行 `. ~/esp/esp-idf/export.sh >/dev/null`。
- 不得通过关闭告警、删除或跳过测试来让检查通过。每个任务结束都跑 host ctest（普通构建和 ASan/UBSan 构建）。
- 报告里如实写每条命令的结果（PASS/FAIL/NOT_RUN/BLOCKED），没跑过的检查不能写成通过。

全部完成后，回复一行：BATCH-C DONE，以及每个任务的结论。
