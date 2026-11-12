# R7a 组件按用途改名 + 去掉 `rtk::`（ADR-0008 §9）

执行方：pi + codex/gpt-6.1-sol。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0008-code-organization.md`（尤其 §9）、`docs/CODING-STYLE.md`。参考已完成的同类任务 `docs/tasks/R3-namespaces-include-roots.md`。

## 目标（只改名与路径，行为不变）
1. 组件目录改名（`git mv`）：`components/rtk_bt→spp_server`、`rtk_proto→device_proto`、`rtk_store→nvs_store`、`rtk_os→rtos`、`rtk_core→link_core`、`rtk_console→cli`；同步所有 `CMakeLists.txt` 的 `REQUIRES/PRIV_REQUIRES`、`idf_component_register`。
2. 命名空间：去掉顶层 `rtk::`。`rtk::bt→spp`、`rtk::proto→proto`、`rtk::os→rtos`、`rtk::core→core`、`rtk::console→cli`、`rtk::fw→fw`、`rtk::demux→demux`、`rtk::reconnect→reconnect`、`rtk::app→app`（`rtk::app::data→app::data`）。嵌套协议常量空间（`proto::msg_type`、`proto::alarm`、`proto::demux_frame`、`sender` 等）保留。注意 ADR §9 约束 2：不得出现顶层 `namespace alarm/link/sync`。
3. include 根：`include/rtk/<x>/` → `include/<ns>/`（例如 `components/stream_demux/include/demux/demux.hpp`，`#include "demux/demux.hpp"`；`rtk_bt` 的 C++ 适配头 → `include/spp/...`；`rtk_os` → `include/rtos/...`；`rtk_console` → `include/cli/...`；`app_fw` → `include/fw/...`；`rtk_core` → `include/core/...`）。C 头文件（`rtk_bt_spp.h`、`rtk_proto_*.h`、`rtk_store.h`）本步**文件名不变**，只随目录移动（R7b 改）。
4. 测试目录：`test/rtk_console → test/cli`；`test_apps/` 不改名。
5. 引用路径/名称的配置与文档同步：`.clangd`、`.clang-tidy*`、`tools/tidy_check/tidy_check.py`、`tools/g7b_check/g7b_check.py`（组件列表）及 `heap_allowlist.txt`、`tools/main_host_check`、`tools/t13_host_check`、`test/**` 的 CMake、`test_apps/rtk_spp_peer`（如引用 bt_reconnect 等）、`AGENTS.md` 依赖/冻结表、`docs/CODING-STYLE.md` 与 `main/modules/_template/README.md` 示例、`docs/acceptance.md`。`docs/adr/**`（§9 已写）、`docs/reports/**`、`docs/tasks/**` 历史文件、`reference/**` 不改。
6. 文件首行注释中的旧路径/命名空间同步更新。Kconfig 选项名 `RTK_*` 不改（菜单标题可去掉 “RTK”）。

建议顺序：rtk_os → app_fw/stream_demux/bt_reconnect（只改命名空间与 include 根）→ rtk_proto → rtk_store → rtk_bt → rtk_core → rtk_console → main。每完成一组跑一次固件构建。

## 冻结文件
经用户同意修改冻结头。迁移后把 `docs/frozen.sha256` 中的旧路径换成新路径（条目数应不变；有增删在报告说明），用
`sha256sum $(awk '{print $2}' docs/frozen.sha256) > docs/frozen.sha256` 重算，`sha256sum -c` 全 OK。

## 自检（写入 `docs/reports/R7a.md`，逐项 PASS/FAIL，附命令）
1. 先 `IDF_COMPONENT_MANAGER=0 idf.py -B builds/r7a-base build`（改前），再 `-B builds/r7a build`：0 warning；`idf.py size` 对比 DRAM/IRAM/Flash Code/镜像大小，预期一致（只差符号/路径字符串时解释原因，例如 `__FILE__`/assert 字符串）。
2. `idf.py -C test_apps/rtk_spp_peer -B builds/r7a-peer build` 0 warning。
3. 主机测试全部通过且用例数不变：`tools/main_host_check`（普通 + ASan/UBSan，30/30）、`tools/t13_host_check`（29）、`test/host`（7）、`test/app_fw`、`test/stream_demux`、`test/stream_demux_dev`、`test/bt_reconnect`、`test/services`、`test/cli`；构建目录 `builds/r7a-*`。
4. `python3 -I tools/g7b_check/g7b_check.py --build builds/r7a` PASS。
5. clang-format `--dry-run --Werror` 覆盖所有改动的 C/C++ 文件。
6. 残留搜索（应为空，`reference/`、`docs/adr|reports|tasks` 除外）：`rg -n 'rtk::|namespace rtk|include/rtk/|"rtk/|components/rtk_|rtk_os|rtk_core|rtk_console|rtk_proto[^_.]|rtk_bt[^_]|rtk_store[^_.]'`；列出命令与输出。注意 C 标识符 `rtk_bt_spp_*`、`RTK_PROTO_*` 等本步保留。
7. 命名空间清单改前/改后（`rg -o --no-filename '^namespace [a-z_:]+' | sort | uniq -c`）。
8. 冻结清单重算 + `sha256sum -c` 全 OK。

报告最后一行 `R7a DONE` 或 `R7a BLOCKED: <原因>`。

## 禁止
改变行为、日志文本、公开 API 语义、NVS 命名空间/键、蓝牙名、协议字节、Kconfig 选项名；修改 `reference/**`、历史报告/ADR/任务卡；烧录、连接串口、联网安装、git commit。只用 `builds/r7a*`，不用 `build/`。
