你是本工程的 Developer。先完整阅读 AGENTS.md（强制规则），以及 docs/adr/ADR-0002-cpp-esp-idf.md 和 docs/adr/ADR-0003-c-cpp-split.md。

背景：架构已修订（ADR-0003）：协议核心、SPP 驱动、NVS 改用 C；rtk_core、rtk_os、适配器与 main 保持 C++。
你上一批 T13 被 QA 中途停下：起草的 rtk_os RAII 头保留，C++ SppServer 草稿与旧接口已移入 reference/cpp-v2-draft/（只读，作废）。
BATCH-C 已验收的 rtk_core/*.cpp 不要改。

本批次按顺序完成：docs/tasks/T16-proto-c.md → docs/tasks/T13-os-bt.md（修订版）。
每个任务开始前读任务卡及其 frozen_inputs；结束后写 docs/reports/<任务号>.md。

硬性约束：
- 冻结文件一个字节都不能改，用 `sha256sum -c docs/frozen.sha256` 自查；认为有错时停下并在报告中写明证据。
- 不烧录、不连接串口设备、不安装软件、不联网下载、不 git commit、不改 RTK_main 以外的文件或 reference/。
- 每个 shell 先执行 `. ~/esp/esp-idf/export.sh >/dev/null`。
- 不得通过关闭告警、删除或跳过测试来让检查通过；每个任务结束都跑 host ctest（普通与 ASan/UBSan 构建）。
- 报告如实写每条命令的结果（PASS/FAIL/NOT_RUN/BLOCKED）。

全部完成后回复一行：BATCH-E DONE，以及每个任务的结论。
