# QA 验收报告：BATCH-E（T16、T13）

验收方：独立 QA（不采信实现方报告）。日期：2026-11-10。
所有构建使用独立目录：`/tmp/rtk-host-qa`、`/tmp/rtk-asan-qa`、`/tmp/rtk-tgt-qa`。

## 1. 软件门禁

| ID | 结果 | 证据 |
| --- | --- | --- |
| G1 冻结文件 | **PASS** | `sha256sum -c docs/frozen.sha256`：114 项全部 OK，exit 0（`.clang-tidy`、`.clang-tidy-c`、C/C++ 公共头、测试均在冻结清单内） |
| G2 host 测试 | **PASS** | 干净构建 8/8 可执行文件；93 用例（C++ 74：7+13+9+17+16+12；C 19：14+5），0 失败 |
| G2b ASan/UBSan | **PASS** | 同一套 8/8、93 用例，0 失败，无 sanitizer 报告 |
| G3 目标构建 | **PASS** | `idf.py -B /tmp/rtk-tgt-qa build` exit 0；编译器警告 0 条。`rtk_proto_frame.c`、`rtk_proto_alarm.c`、`frame.cpp`、`alarm_codec.cpp`、`rtk_bt_spp.c`、`spp_transport.cpp` 均实际编译并链接。另有 5 条 IDF v6.1 自带的 CMake component_validation 警告（`wpa_supplicant`/`esp_wifi` 私有 include 目录），与本工程无关 |
| G3b 双架构 | **NOT_RUN** | `test_apps/rtk_pure` 尚未创建，属 T15 |
| G4 体积 | **PASS** | `rtk_main.bin` = 0x218B0 = 137,392 B（134.2 KiB），app 分区 0x1A0000，剩余 92%；远小于 1.2 MB。`size-components` exit 0 |
| G5 格式 | **PASS** | 全部 36 个自有 `.c/.cpp/.h/.hpp`：`clang-format --dry-run --Werror` 0 处差异。astyle_py：NOT_RUN（需联网授权） |
| G5b 静态分析 | **PASS（附条件）** | C：`clang-tidy -p /tmp/rtk-host-qa --config-file=.clang-tidy-c` 对两个 `.c` 用户代码 0 发现（仅 glibc 头 3 条被 user-code 过滤抑制）。C++：实现文件 0 发现；全量运行产生 15 条 `readability-identifier-naming`/`cppcoreguidelines-use-enum-class`，全部位于冻结的 `rtk_proto_frame.h`/`rtk_proto_alarm.h`；`-header-filter='.*\.hpp$'` 时为 0。根因是冻结的 `.clang-tidy:49` 规则覆盖 `components/` 下的 C 头，见 §4 遗留项 |
| G6 分层/语言边界 | **PASS** | `rtk_proto`/`rtk_core` 无 IDF/FreeRTOS/sdkconfig/nvs 头；`.c` 文件不含 `.hpp`；3 个 C 公共头均有 `extern "C"` |
| G7 禁用特性 | **PASS** | 自有源码无 `new/delete`、`malloc/free(`、`std::function`、分配型容器、异常、RTTI、`volatile`；命中仅为 `= delete` 特殊成员与 `session_free(` 误匹配 |
| G7b C++ 运行时 | **PASS（替代检查）** | 自有目标文件（rtk_proto/rtk_bt/rtk_os/rtk_core/rtk_store/main）`nm` 无 `_Znw/_Zdl/malloc/free/__cxa_throw/_ZTI/_ZTV`；ELF 无 `.eh_frame`/`.gcc_except_table`。`tools/cpp_cost.py` 不存在，该工具 NOT_RUN |
| G8 代码审查 | **PASS（Low 2 条）** | 见 §2、§3 |

## 2. T16 审查结论

- `rtk_proto_frame.c`：CRC-32 反射实现与契约（zlib 等价、可分段累计）一致；同步状态机、头长/长度检查、CRC 失败与清除规则逐条对应冻结头文件；`buf` 写入上界 `expect ≤ 280`，无越界；`build_frame` 所有失败路径不写 `*out_len`。
- `rtk_proto_alarm.c`：0x40 的 offset 7 与尾部 4 字节、0x41 的尾部 4 字节均清零；`out_size` 检查先于写入；decode 忽略多余字节。
- C++ 外观：`build_frame` 先检查 `payload.size() > kMaxPayload` 再窄化；空 `out` 返回 `kNoSpace`（符合 C++ 头语义），失败不写 `out_len`；`FrameParser` 只用零初始化状态，符合 C 头“全零合法”约定；`FrameView` 逐字段拷贝，payload 生命周期与契约一致。
- `alarm_codec.cpp` 仅做结构体逐字段转换，无重复协议逻辑。
- CMakeLists 的 C/C++ 告警组用生成器表达式区分，符合 AGENTS.md §3。
- 偏离：G5b 的 15 条冻结 C 头误报，实现方已如实记录；属配置冲突，非实现缺陷。

## 3. T13 审查结论

- `rtk_bt_spp.c` 与 `reference/c-v1/components/bt_spp/bt_spp.c` 的初始化顺序、GAP/SPP 参数、会话与事件处理一致；按 ADR-0003 改为 `rtk_bt_spp_*`/`s_`/IDF Style。
- 会话表、StreamBuffer 存储、事件队列、事件组、`portMUX_TYPE` 全部文件作用域静态；临界区内只有字段读写，无 FreeRTOS API；BTC 回调只做 `xStreamBufferSend(...,0)`、标志更新、`xQueueSend(...,0)`、事件组位操作。
- 所有 `esp_err_t` 均检查或返回；初始化三条失败路径均逆序释放；write 等待用 tick 起点与剩余 tick 递减，有上界；未知句柄 write/disconnect 返回 `ESP_ERR_NOT_FOUND`，与冻结头一致。
- `spp_transport.cpp`：空 span 返回 `kInvalidArg` 且不调用驱动，其余经 `from_esp_err` 映射，无状态。
- CMake 两阶段守卫（依赖收集阶段未加载 sdkconfig）已由干净构建验证：esp32 真实构建编译并链接驱动；该偏离可接受。
- RAII 头实例化：用 `spp_transport.cpp` 的 Xtensa 编译参数对 `StaticTask/StaticQueue/StaticStreamBuffer/StaticEventGroup/CriticalSection` 做 `-fsyntax-only`，exit 0、0 warning。
- 发现（均不阻塞）：
  1. **Low（报告准确性）**：T13 报告称 `esp_status.cpp` 也在 `docs/frozen.sha256` 中；实际冻结清单只有 `rtk_os/include/**` 的 6 个头。该文件未改动是正确的（内容已满足契约），报告表述已更正（见 §6）。
  2. **Low（已修复）**：`static_task.hpp` 的 `stack_high_water_mark()` 原直接返回 IDF 内核的字数（IDF v6.1 内核按 `StackType_t` 字数返回，`task.h` 注释误写为字节），与接口声明的“字节”不符；已换算为字节，见 §6。
  3. **Low（参考实现同源）**：BTC 回调中的 `ESP_LOGW` 可能短暂争用日志锁；不打印数据内容，且与已验收的 C 参考实现一致。`session_alloc` 在释放临界区后 `xStreamBufferReset` 与链路任务非阻塞读存在理论并发窗口，参考实现相同，留待 HIL 观察。

## 4. 遗留项（交 Architect/人工）

1. ~~`.clang-tidy` 与 `.clang-tidy-c` 的 `HeaderFilterRegex` 覆盖冻结 C 头~~ **已修复（见 §6）**：`.clang-tidy` 已收窄到 `.*\.(hpp|hh|hxx)$`，C++ TU 不再对冻结 C 头报 15 条伪发现；`.clang-tidy-c` 只服务 C 文件，保持原样。
2. HIL（连接、吞吐、拥塞、scatternet、0x40 推送）全部 **NOT_RUN**，需用户授权烧录指定 E1 板。
3. `clang-tidy` 对 `rtk_bt`/`rtk_os` 的 Xtensa-only TU 仍不可用（本机 clang 无 Xtensa target）；验收门禁 G5b 不要求该目标，未计为失败。
4. BATCH-F（T14 → T15）未开始。

## 5. 结论

**BATCH-E：PASS（软件证据）**。T16、T13 均满足任务卡可观察结果；文档/配置类问题见 §3、§4。硬件证据 NOT_RUN。最终签字由人完成。

## 6. 验收后修复（2026-11-10）

| 项 | 改动 | 验证 |
| --- | --- | --- |
| clang-tidy 伪发现 | `.clang-tidy` 的 `HeaderFilterRegex` 收窄为 `.*/RTK_main/(components\|main\|test)/.*\.(hpp\|hh\|hxx)$`；更新 `docs/frozen.sha256` 中 `.clang-tidy` 的哈希 | 重新生成 host 构建后，`clang-tidy -p /tmp/rtk-host-qa2` 对 rtk_proto/rtk_core 全部 `.cpp` 退出 0、用户代码 0 发现（不再需要 `-header-filter` 覆盖） |
| 高水位单位 | `static_task.hpp::stack_high_water_mark()` 由“直接返回内核字数”改为 `words * sizeof(StackType_t)`，返回字节，与接口注释一致；更新冻结哈希 | Xtensa `-fsyntax-only` 实例化 StaticTask 等 5 个包装：exit 0；`idf.py build` exit 0、0 warning |
| 报告准确性 | `docs/reports/T13.md` 更正 `esp_status.cpp` 的冻结归属，并记录高水位修复 | 与 `docs/frozen.sha256` 内容一致 |
| LSP 配置 | 新增 `.clangd`：host 文件用 `build-host`，`main`/`test_apps`/`rtk_bt`/`rtk_os`/`rtk_store` 用 `build`；按平台补齐 host GCC 16 与 xtensa GCC 15.2 的 include 路径（无需 `--query-driver`）；移除 xtensa GCC 专有选项；C 文件/`.h` 关闭 C++ clang-tidy；`reference/` 不建索引 | `clangd --check`（不带额外参数）：`main/app_main.cpp`、`esp_status.cpp`、`spp_transport.cpp` 0 诊断；host 测试/纯组件加载 `build-host` 且无缺头 |

冻结校验：`sha256sum -c docs/frozen.sha256` 114 项全部 OK。
