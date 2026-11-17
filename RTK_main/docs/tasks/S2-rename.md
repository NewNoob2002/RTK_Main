# S2：C++ 命名迁移到 lowerCamelCase + 列宽 100（ADR-0013 §2.1）

状态：ISSUED 2026-11-17，基线 = 本卡所在提交（T0）。实施：omp + workbuddy/deepseek-v4.1-flash；审查/提交：Claude。
先读：`AGENTS.md`、`docs/adr/ADR-0013-readability.md` §2.1、`docs/CODING-STYLE.md` §3.1、`.clang-tidy`（命名段已由 Claude 改好）、
`.clang-format`（ColumnLimit 已改为 100）、`tools/tidy_check/tidy_check.py`（如何用 host/IDF/gnss 编译库跑 clang-tidy）。

## 0 规则（最终形态）

| 对象 | 写法 | 例 |
| --- | --- | --- |
| C++ 函数、方法、局部变量、参数、数据成员（含 public struct 字段） | `lowerCamelCase` | `pollController`、`nowMs`、`rxPeriodMs` |
| 私有/受保护成员 | `lowerCamelCase_` | `lastTickMs_` |
| 文件作用域可变静态 | **不变** `s_snake_case` | `s_link_state` |
| 类型、常量、枚举值、`kName` 只读对象、命名空间、宏、文件名 | 不变 | |
| C 文件、C 头（`.h`）、`extern "C"`、`app_main`、IDF/FreeRTOS 回调签名要求的名字 | 不变 `snake_case` | `spp_server_init` |

- 缩写按单词：`parseGga`、`rtcmMsg`；单位后缀保留：`timeoutMs`。
- 在 C++ 翻译单元里出现的 C 接口名：在 `.clang-tidy` 用 `readability-identifier-naming.FunctionIgnoredRegexp` 等
  放行（正则只列组件前缀，如 `^(spp_server|nvs_store|dev_proto)_`，加注释写理由）；只有零散个例才用带理由的 `NOLINTNEXTLINE`。
- **只改名字和换行，不改任何逻辑、字符串字面量、日志文本、CLI 命令字、NVS 键、Kconfig 名、CMake 目标名。**
  宏里拼出来的名字（`##`、X-macro 展开、`fw::make_ops<T>()` 依赖的方法名）由你人工同步。

## 1 批次（每批结束停下，Claude 审查提交后再发下一批）

| 批 | 内容 |
| --- | --- |
| B0 | 只做格式：对全部自有 C/C++ 文件跑 `clang-format -i`（100 列）。排除：`reference/`、`managed_components/`、`builds/`、`test/stream_demux_dev/h2/H2Vectors.h`（生成物）、`test/gnss/golden/*.inc`。不改名。 |
| B1 | 改名：声明在 `components/app_fw`、`components/rtos`、`components/cli` 的 C++ 符号 |
| B2 | 改名：`components/stream_demux`、`components/device_proto`、`components/link_core`、`components/bt_reconnect`、`components/spp_server`、`components/nvs_store` 中的 C++ 符号（C 部分不动） |
| B3 | 改名：`components/gnss`（全部驱动）+ `test/gnss` |
| B4 | 改名：`main/**`、`test/**` 其余、`test_apps/**`、`tools/*_host_check`；收尾：`.clang-tidy` 命名检查对全仓 0 条 |

每批改名都要同步修改所有引用处（跨组件调用方、测试、test_apps）。改完的层之后不再出现 snake_case 函数/变量。

## 2 方法（建议）

- 用 `run-clang-tidy -checks='-*,readability-identifier-naming' -export-fixes <yaml>` 在 host、gnss 全驱动、IDF 三个编译库上导出修复
  （IDF 库的 xtensa 选项过滤照 `tools/tidy_check/tidy_check.py`），**只保留声明位置在本批目录里的诊断**，合并去重后用
  `clang-apply-replacements` 应用。不要逐个 TU `--fix`（头文件会被重复改坏）。
- 剩下的（宏参数、模板依赖名、未进编译库的文件）手工补；用 `rg -w` 确认旧名在仓库代码里清零（`docs/` 除外）。
- 每批输出改名表 `docs/reports/S2-rename-map.tsv`（追加）：`批次<TAB>旧名<TAB>新名<TAB>种类<TAB>声明文件:行`。Claude 用它同步 docs。

## 3 不做

- 不改 `docs/**`（`docs/reports/S2.md` 与 `docs/reports/S2-rename-map.tsv` 除外）、`docs/frozen.sha256`、`.clang-format`。
- `.clang-tidy` 只允许新增 `*IgnoredRegexp` 行及其注释。
- 不 git commit；不改代码逻辑；不顺手修别的告警（发现的问题写进报告“遗留”）。

## 4 每批门禁（结果追加到 `docs/reports/S2.md` 对应批次小节，逐项 PASS/FAIL/NOT_RUN/BLOCKED）

构建目录 `builds/s2-<批>-*`。

1. 主机测试普通构建全部通过：`test/gnss`（默认 + 全驱动）、`test/host`、`test/services`、`test/app_fw`、`test/cli`、
   `test/stream_demux`、`test/bt_reconnect`、`tools/main_host_check`、`tools/t13_host_check`。B4 另加全部 ASan+UBSan（`-fno-sanitize=vptr`）。
2. `idf.py -B builds/s2-<批> build` 0 warning；`test_apps/rtk_pure` esp32 与 esp32c3 构建成功。
3. 代码量不变：`xtensa-esp32-elf-size -A builds/s2-<批>/rtk_main.elf` 与上一批对比，`.flash.text`、`.iram0.text` 大小相同；
   `.flash.rodata` 若变化，逐项解释（`__func__`/`__LINE__` 之类），并贴 bin 大小差值。
4. `test/gnss/golden/*.inc` 无改动（`git diff --stat` 为空）。
5. `python3 -I tools/g7b_check/g7b_check.py --build builds/s2-<批>` PASS。
6. `clang-format --style=file --dry-run --Werror`：B0 起对全部自有文件（排除同 B0）通过。
7. G5b `tools/tidy_check/tidy_check.py`：已完成批次的目录里 `readability-identifier-naming` 0 条；其他检查 0 条。
   未改名的层的命名告警允许存在，报告里给数量。B4 结束时全仓 0 条。
8. `doc_check`：改名会让函数被识别为“改动”，本卡不跑 `--base` 门禁（Doxygen 补齐是 S5）；只跑 `--report` 记录覆盖率，不得低于上一批。
9. 冻结哈希：不检查（Claude 每批重算）。

批次完成后在终端打印 `S2-<批> DONE`，等待下一条指令。

## 5 交付

- `docs/reports/S2.md`：每批一节（改动范围、工具命令、手工补改清单、门禁输出、遗留）。
- `docs/reports/S2-rename-map.tsv`。
