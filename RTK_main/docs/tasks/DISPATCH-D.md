你是本工程的 Developer。先完整阅读 AGENTS.md（强制规则）、docs/adr/ADR-0016-console-profiles.md，以及 ADR-0009 §2.8、§2.10、§2.12。

本批次只有一个任务：docs/tasks/D1-console-profile.md。开始前读任务卡；结束后写 docs/reports/D1.md。

硬性约束：
- 冻结文件一个字节都不能改，用 `sha256sum -c docs/frozen.sha256` 自查。认为冻结文件有错时，停下并在报告中写明证据。
- 不烧录、不连接串口设备、不安装软件、不联网下载（包括 pip/组件管理器；`uvx --offline` 用本机缓存可以）、不 git commit、不改 RTK_main 以外的文件或 reference/。
- 每个 shell 先执行 `export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf6.1_py3.13_env"; . ~/esp/esp-idf-v6.1/export.sh >/dev/null`（本机 IDF 在 esp-idf-v6.1，run_gates.sh 会沿用已设置的 IDF_PATH）。
- 不得通过关闭告警、删除或跳过测试来让检查通过。门禁用 `tools/ci/run_gates.sh`。
- 报告里如实写每条命令的结果（PASS/FAIL/NOT_RUN/BLOCKED），没跑过的检查不能写成通过。
- ADR-0016 未规定的行为写 UNKNOWN 并停下询问，不要猜。

完成后回复一行：BATCH-D DONE，以及 D1 的结论。
