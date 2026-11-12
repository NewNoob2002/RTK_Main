# 验收计划（QA，独立于实现方）——C/C++ 混合版（ADR-0002 + ADR-0003）

实现方：omp / DeepSeek-V4.1-Flash。验收方：Claude（独立角色）。最终签字由人完成。
测试在实现前冻结（`test/host/**`：C++ 74 个用例 + C 19 个用例），满足 workflow §7 的角色分离条件 (b)。

## 1. 软件门禁（每批次结束由验收方在独立构建目录重跑，不采信实现方报告）

| ID | 检查 | 命令 / 方法 | 通过标准 |
| --- | --- | --- | --- |
| G1 | 冻结文件未改 | `sha256sum -c docs/frozen.sha256` | 全部 OK |
| G2 | host 单元测试 | `cmake -S test/host -B /tmp/rtk-host && cmake --build /tmp/rtk-host && ctest --test-dir /tmp/rtk-host` | 8/8 可执行文件，93 用例 0 失败（C 测试只链接 C 库） |
| G2b | ASan/UBSan | 同上，加 `-fsanitize=address,undefined` | 0 失败，无 sanitizer 报告 |
| G3 | 目标构建 | 干净构建目录 `idf.py -B /tmp/rtk-tgt build` | 退出码 0，0 warning |
| G3b | 双架构（ESP-008） | `test_apps/rtk_pure` 分别构建 esp32 与 esp32c3 | 两者都成功（BATCH-D 起） |
| G4 | 体积 | `idf.py size`、`size-components` | app ≤ 1.2 MB；记录各组件占用 |
| G5 | 格式 | `clang-format --dry-run --Werror` 检查全部自有 .cpp/.hpp；权威检查为 astyle_py 3.4.7 + IDF 规则 | clang-format 0 处差异；astyle 需用户授权联网，未授权时记 NOT_RUN |
| G5b | 静态分析 | `python3 -I tools/tidy_check/tidy_check.py --host-build builds/host --idf-build build --test-app-build builds/rtk_pure-esp32`：覆盖 rtk_proto、rtk_core、test/host、main、rtk_bt、rtk_os、rtk_store、test_apps/rtk_pure；C++ 用根 `.clang-tidy` 及目录继承配置，C 用 `.clang-tidy-c` 加脚本内分区排除项；ESP 侧按 `.clangd` 方式过滤 GCC 专有选项 | 0 发现（2026-11-11 用户授权扩大范围；豁免只能是具名 NOLINT 并附理由，目录级放宽见各 `.clang-tidy` 注释） |
| G6 | 分层与语言边界 | `grep -rnE '#include ["<](esp_\|freertos/\|sdkconfig\|nvs)' components/rtk_proto components/rtk_core` 无输出；C 文件（`components/*/*.c`）不包含任何 `.hpp`；C 公共头都有 `extern "C"` 保护 | 全部满足 |
| G7 | 禁用特性 | 自有源码中 grep `new \|delete \|malloc\|free(\|std::function\|<vector>\|<string>\|<map>\|throw\|typeid\|dynamic_cast\|volatile`（placement new 例外，逐条审查） | 无未解释的命中 |
| G7b | C++ 运行时（CPP-001/002/003） | `python3 -I tools/g7b_check/g7b_check.py --build build`；`cpp_cost.py --prefix xtensa-esp32-elf- check build/rtk_main.elf --tier T2` 只报告 | 自有目标文件中异常、RTTI、堆符号均为 0；全 ELF 的异常/RTTI 符号只能是白名单 6 个（`__cxa_begin_catch`、`__cxa_end_catch`、`std::bad_alloc`/`std::exception` 的 typeinfo 与 typeinfo name），且 map 拉入链必须为 IDF `nvs_encrypted_partition.cpp.obj` → libstdc++ `new_opvnt.o` → `eh_catch.o`/`new_opv.o` → `new_op.o` → `bad_alloc.o`；任何新增符号或链路变化判 FAIL（2026-11-11 裁决，证据见 `docs/reports/T14-REVIEW.md`、`docs/reports/G7B-DECISION.md`）。heap 类来自 IDF 蓝牙栈，只报告 |
| G8 | 代码审查 | 验收方逐条审查：ESP-001（portMUX）、ESP-003（字节栈/绑核）、ESP-004（esp_err_t）、CPP-009/010/011/013/014/020、回调不阻塞、单一所有者、有界等待、错误路径释放 | 没有未处理的 High 问题 |

## 2. 硬件在环（HIL，需用户明确授权烧录指定 E1 板后执行）

前置：授权 `idf.py -p <port> flash monitor` 到指定板；确认引脚 26 的电源保持问题（ADR-0001 §6 WARNING）。

| ID | 场景 | 步骤 | 通过标准 |
| --- | --- | --- | --- |
| HIL-0 | test_apps/rtk_pure 上板 | pytest-embedded 运行冒烟用例 | 全部 PASS |
| HIL-1 | 广播与手机连接 | 上电，手机搜索 | 名称 `E1-Lite-<SN>`，无多余字符；免 PIN 配对；`status` 显示 1 个 PHONE |
| HIL-2 | 手机 App 兼容 | 用现有 App 查询 0x01/0x02/0x03/0x07/0x0D/0x14/0x15，并设置 0x07/0x14 | App 正常显示，无超时；设置后再次查询，回显一致 |
| HIL-3 | 手机与报警器同时在线（scatternet） | 手机连接后，报警器开机自动连接；保持 30 min，期间手机持续查询 | 两个会话一直在线；0x40 推送间隔 1000±200 ms；手机应答 ≤ 100 ms。**不通过则把 `RTK_LINK_POLICY` 切换为 PHONE_PREEMPT，并做 HIL-3b** |
| HIL-3b | 手机优先降级 | PHONE_PREEMPT 下重复 HIL-3 | 手机接入时报警器被断开；手机断开后报警器 ≤ 15 s 内自动回连 |
| HIL-4 | 报警链路 | 手机发 0x42 设为 ALERT/3；关闭报警器后再开 | 报警器 ≤ 1.5 s 鸣响；RTK 在 3 s 内断开静默的报警会话；报警器重开后自动回连并恢复状态 |
| HIL-5 | 配对与持久化 | 清空白名单 → `pair` → 报警器发 HELLO → 断电重启 | 重启后报警器直接被识别为 ALARM；配对窗口外的未知 HELLO 被断开 |
| HIL-6 | 资源 | 两个会话在线 10 min 后执行 `status` | 最小空闲堆 ≥ 80 KB；链路任务栈余量 ≥ 30 %；CRC 错误计数不持续增长；TWDT 不触发 |

## 3. 报告

每项给出 `PASS / FAIL / NOT_RUN / BLOCKED`，并附日志位置。单元测试或仿真通过不等于硬件通过；没有执行的 HIL 一律记 `NOT_RUN`。最终签字由人完成（W10）。
