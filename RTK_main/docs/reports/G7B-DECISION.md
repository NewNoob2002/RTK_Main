# G7b 门禁修订记录

日期：2026-11-11。裁决人：用户（项目负责人）选择方案 1。执行：Claude Opus 5.5（Claude Code）。人工负责人 UNKNOWN（待人工审查签字）。

## 裁决

G7b 从“全 ELF exceptions、rtti 为 0”改为：

1. 自有目标文件（`build/esp-idf/{main,rtk_*}/**/*.obj`）中异常、RTTI、堆符号（定义或引用）均为 0；
2. 全 ELF 的异常/RTTI 符号只能是以下 6 个的子集：`__cxa_begin_catch`、`__cxa_end_catch`、`typeinfo for std::bad_alloc`、`typeinfo for std::exception`、`typeinfo name for std::bad_alloc`、`typeinfo name for std::exception`；
3. 白名单符号存在时，map 拉入链必须为：IDF `libnvs_flash.a(nvs_encrypted_partition.cpp.obj)` →(`_ZnajRKSt9nothrow_t`) `new_opvnt.o` → `eh_catch.o` / `new_opv.o` → `new_op.o` → `bad_alloc.o` → `eh_exception.o`；
4. 任何白名单外符号、链路变化、自有目标文件命中均判 FAIL。`cpp_cost.py --tier T2` 保留为报告项。

理由与证据见 `T14-REVIEW.md`「G7b 根因与剩余裁决」：NVS 无条件编译该源文件（`CONFIG_FLASH_ENCRYPTION_ENABLED` 未设置），异常/RTTI 均在 sdkconfig 中关闭，自有代码不产生这些符号。

## 改动文件

| 文件 | 类型 | 改动 |
|---|---|---|
| `tools/g7b_check/g7b_check.py` | 新增 | 门禁脚本（仅 Python 标准库 + 工具链 nm） |
| `docs/acceptance.md` | 冻结，经授权修改 | G7b 行 |
| `docs/adr/ADR-0002-cpp-esp-idf.md` | 冻结，经授权修改 | “C++ 运行时”行 |
| `docs/tasks/T14-store-app.md` | 冻结，经授权修改 | observable_outcome 中 G7b 条件 |
| `docs/frozen.sha256` | 清单 | 以上 3 个文件的哈希 |

未修改 SDK、工具链、sdkconfig、源码或其他冻结文件。

## 验证

| 命令 / 检查 | 结果 | 说明 |
|---|---|---|
| `sha256sum -c docs/frozen.sha256`（修改前） | PASS | 全部 OK |
| `sha256sum -c docs/frozen.sha256`（修改后） | PASS | 全部 OK |
| `python3 -I tools/g7b_check/g7b_check.py --build build-t14-review` | PASS | 17 个自有目标文件；白名单命中 6/6；退出码 0 |
| 负向：map 中把 `nvs_encrypted_partition.cpp.obj` 改为其他对象 | FAIL（预期） | 报“拉入链不符”，退出码 1 |
| 负向：把 IDF `nvs_encrypted_partition.cpp.obj` 放入 `esp-idf/main/` 冒充自有对象 | FAIL（预期） | 报 `free`、`_ZdaPv`、`_ZnajRKSt9nothrow_t` 堆符号 |
| 负向：ELF 出现白名单外异常/RTTI 符号 | NOT_RUN | 未构造此 ELF；逻辑为集合差，代码审查覆盖 |
| 重新 `idf.py build` | NOT_RUN | 本次未改源码，沿用 `build-t14-review` 产物 |

负向测试在 scratchpad 中用软链接和 map 副本完成，未改动 `build-t14-review/`。

## 局限

GNU ld map 每个归档成员只记录第一个引用方。若日后其他 IDF 组件也引用 `__cxa_begin_catch` 等，而 `eh_catch.o` 已被 `new_opvnt.o` 先拉入，脚本无法区分；自有对象仍由第 1 条兜底。IDF 或工具链升级后链路可能变化，届时脚本会 FAIL，需要重新裁决。
