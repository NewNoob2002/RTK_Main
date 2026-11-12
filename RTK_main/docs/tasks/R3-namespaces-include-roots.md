# R3 命名空间与 include 根统一（ADR-0008 §5 R3）

执行方：pi + codex/gpt-6.1-sol。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0008-code-organization.md`、
`docs/CODING-STYLE.md`（尤其 §3.2、§3.3，已于 2026-11-12 按用户要求改为“按可读性判断”）。

## 目标
三个组件的命名空间与 include 根没有 `rtk` 前缀，与其余组件不一致。本阶段**只做改名与路径迁移，行为、布局、符号语义不变**。

| 组件 | 现在 | 改为 |
| --- | --- | --- |
| `components/app_fw` | `namespace app`，`#include "app/xxx.hpp"` | `namespace rtk::fw`，`components/app_fw/include/rtk/fw/xxx.hpp` |
| `components/stream_demux` | `namespace demux`（含 `demux::rtcm`、`demux::nmea`、`demux::ubx`、`demux::unicore_bin`、`demux::unicore_hash`、`demux::crc`），`#include "demux/..."` | `namespace rtk::demux`（子空间保持一层：`rtk::demux::rtcm` 等），`include/rtk/demux/...` |
| `components/bt_reconnect` | `namespace btrc`，`#include "bt_reconnect/reconnect.hpp"` | `namespace rtk::reconnect`，`include/rtk/reconnect/reconnect.hpp` |

同时处理：
1. 改名后变得多余的 `namespace x = …` 别名与 `::app::` 限定，改成一级限定名（例如在 `rtk::app` 中写 `fw::Slot`、`demux::Sink`）。只删冗余的，读起来清楚的局部别名可以保留（§3.3）。
2. 深度超过 `rtk::a::b` 三层的：`rtk::core::services::detail` → 移入对应 `.cpp` 的匿名命名空间，若被头文件模板使用则改为 `rtk::core::detail`；`rtk::app::data` 可保留（三层）。
3. **不改**：C 头文件（`rtk_bt_spp.h`、`rtk_proto_*.h`、`rtk_store.h`）保持根目录；`rtk::proto::msg_type`/`sender`/`alarm`/`demux_frame`、`store_flag` 等协议常量子空间保持（§3.3 允许）；`reference/**` 不动。
4. 文件第一行/第二行注释若提到旧路径或旧命名空间，同步更新；本阶段不清理 `Generated-by` 头（R4 做）。
5. 引用旧路径的配置与文档同步：各 `CMakeLists.txt`、`.clangd`、`.clang-tidy*`、`tools/tidy_check/tidy_check.py`、`tools/g7b_check/*`（如有路径）、`AGENTS.md` 依赖/冻结表中的路径、`docs/CODING-STYLE.md` 示例。`docs/adr/**` 与 `docs/reports/**` 历史记录不改。

建议顺序：app_fw → stream_demux → bt_reconnect，每个组件改完跑一次固件构建 + 该组件测试再进入下一个，并在报告中记录每步的 `git diff --stat`。

## 涉及的使用方（至少）
`main/**`、`components/rtk_os`（executor.hpp、spin_lock.hpp 引用 app_fw）、`components/rtk_proto`（demux_frame）、`components/rtk_core`（如有）、`test/app_fw`、`test/stream_demux`、`test/stream_demux_dev`、`test/bt_reconnect`、`test/services`、`test/rtk_console`、`test/host`、`tools/main_host_check`、`tools/t13_host_check`、`test_apps/rtk_spp_peer`。用 `rg` 全量搜 `btrc|bt_reconnect/|::app::|"app/|"demux/|namespace demux|namespace app\b` 确认无残留（reference/ 与 docs 历史除外）。

## 冻结文件
本阶段经用户同意修改冻结头（ADR-0008 §7 第 3 条）。迁移后 `docs/frozen.sha256` 中的旧路径要换成新路径（条目数不变，除非文件确实增删，需在报告说明），然后用
`sha256sum $(awk '{print $2}' docs/frozen.sha256) > docs/frozen.sha256` 重算并 `sha256sum -c` 全部 OK。

## 自检（写入 `docs/reports/R3.md`，逐项 PASS/FAIL，附实际命令）
1. `. ~/esp/esp-idf/export.sh >/dev/null && IDF_COMPONENT_MANAGER=0 idf.py -B builds/r3 build`：0 warning；`idf.py -B builds/r3 size` 与改前（先构建 `builds/r3-base`）对比，DRAM/IRAM/Flash 预期一致，有差异需解释。
2. `idf.py -C test_apps/rtk_spp_peer -B builds/r3-peer build` 0 warning。
3. 主机测试全部通过且用例数不变：`tools/main_host_check`（普通 + ASan/UBSan，29/29）、`tools/t13_host_check`、`test/host`（7）、`test/app_fw`、`test/stream_demux`、`test/bt_reconnect`、`test/services`、`test/rtk_console`；构建目录统一 `builds/r3-*`。
4. `python3 -I tools/g7b_check/g7b_check.py --build builds/r3` PASS。
5. tidy：与改前同范围比较无新增诊断（可参考 `docs/reports/R2-tidy-compare.py`）。
6. clang-format `--dry-run --Werror` 覆盖所有改动文件。
7. 残留搜索结果（应为空，列出命令与输出）；命名空间清单改前/改后（`rg -o "^namespace [a-z_:]+" | sort | uniq -c`）。
8. 冻结清单重算并 `sha256sum -c` 全 OK。

报告最后一行写 `R3 DONE` 或 `R3 BLOCKED: <原因>`。

## 禁止
改变行为、日志文本、公开 API 语义；修改 `reference/**`、历史报告/ADR；烧录、连接串口、联网安装、git commit。只用 `builds/r3*`，不用 `build/`。
