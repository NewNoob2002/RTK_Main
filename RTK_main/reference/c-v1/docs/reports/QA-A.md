# QA 验收报告：BATCH-A（T00–T03）

验收方：Claude（独立于实现方 omp/DeepSeek-V4.1-Flash）。日期 2026-11-10。

| ID | 结果 | 证据 |
| --- | --- | --- |
| G1 冻结文件 | PASS | `sha256sum -c` 全部 OK；只新增了预期文件 |
| G2 host 测试 | PASS（修复前） | 独立构建目录 `/tmp/rtk-host`，4/4 可执行文件，52 用例 0 失败 |
| G2b ASan/UBSan | **FAIL** | `test_cmd_disp`：global-buffer-overflow，`cmd_disp.c:127 copy_field` ← `cmd_disp.c:676 cmd_disp_init` |
| G3 目标构建 | PASS | 干净构建 `/tmp/rtk-tgt`，0 warning |
| G4 体积 | PASS | 136,873 B（尚未链接蓝牙栈，T04 后需重测） |
| G5 BARR-C | PASS（4 个 .c）；冻结头文件 FAIL → 已由 Architect 修正 | clang-tidy（`-p` 指向 host 编译库）0 发现；头文件 clang-format 与 magic-number 问题见下 |
| G6 分层 | PASS | 纯模块无 `esp_*`/FreeRTOS 包含 |
| G7 无 Arduino/动态内存 | PASS | grep 无输出 |
| G8 代码审查 | FAIL（1 High） | 见下 |

## 发现

1. **High（实现方）**：`cmd_disp_init` 用 `copy_field(dst, "V1.1.0", 8)` 等方式从短字面量读取固定宽度，越界读 1–2 字节（未定义行为），越界字节可能进入 0x01 应答的 hw_ver/board/radio_type 字段。注释“字面量含结尾 0，宽度足够”不成立。原测试只比较可见前缀未能发现；已加强 `test_dev_info_0x01`（比较整个字段宽度）。
2. **Medium（Architect 自身）**：冻结头文件未按 `.clang-format` 排版；`cmd_disp.h` 字段宽度为魔数。已修正（新增 `CMD_STR8_LEN/CMD_STR16_LEN`，`clang-format -i`），语义不变，重算 `docs/frozen.sha256`。
3. **Low（工具）**：`check.sh` 在无根目录 `compile_commands.json` 时 clang-tidy 找不到头文件；验收改用 `clang-tidy -p <host build>`。
4. **Info**：为规避 readability-magic-numbers，实现中出现 `CMD_ONE`、`CMD_DS_OFS_9` 这类无语义常量名，可读性差，不阻塞。

## 结论

BATCH-A：**未通过**（G2b/G8 High）。退回 DeepSeek 修复发现 1，修复后重跑 G1–G8。

## 复验（修复后，2026-11-10）

| ID | 结果 | 证据 |
| --- | --- | --- |
| G1 | PASS | 冻结文件全部 OK；源码只改动 `components/cmd/cmd_disp.c` 与 `docs/reports/T03.md` |
| G2 | PASS | 52 用例 0 失败（含加强后的 `test_dev_info_0x01`） |
| G2b | PASS | ASan/UBSan 四个可执行文件均无报告 |
| G3 | PASS | 目标构建 0 warning |
| G5 | PASS | `cmd_disp.c` clang-tidy 0、clang-format 0 |
| G8 | PASS | 新增 `copy_str()` 以 `\0` 或字段宽度为界并补 0，已人工核对 |

**BATCH-A 结论：PASS（软件证据）**。硬件证据全部 NOT_RUN。最终签字须由人完成。
