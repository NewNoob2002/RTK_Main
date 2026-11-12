# J2b：LinkService 切换到 CommandRouter（ADR-0007 §3.5）

- 执行方：pi + workbuddy/deepseek-v4.1-flash
- 人工负责人：UNKNOWN（待人工审查签字）
- 依据：`docs/tasks/J2b-link-service-router.md`、`AGENTS.md`、`docs/adr/ADR-0007-app-services-split.md`、`docs/reports/J1.md`、`docs/reports/J2a.md`、修订后的冻结头 `components/rtk_core/include/rtk/core/link_service.hpp`

## 1. 结论

**PASS。** 分发链路已从 `CommandDispatcher` 切换到 `CommandRouter` + 4 个服务，旧 dispatcher 退役到 `reference/cpp-v3-dispatcher/`；
`test/host`、`test/services`、`tools/main_host_check`（两种策略）普通与 ASan/UBSan 全通过，`idf.py build` 零告警。
`test_link_service` 的 12 个用例与全部断言一字未改，仍然全过。

## 2. 改动清单

| 文件 | 状态 | 改动 |
| --- | --- | --- |
| `components/rtk_core/link_service.cpp` | 修改 | 构造函数改为 `(Transport&, LinkManager&, CommandRouter&, const services::AlarmService&)`；`on_data` 用 `router_.handle(...)`；`tick` 用 `alarm_.build_status_push(...)`；其余（白名单脏标志、心跳记录、断开动作、错误计数、解析器槽）逐行未动；文件头注释更新溯源 |
| `components/rtk_core/CMakeLists.txt` | 修改 | SRCS 删除 `command_dispatcher.cpp`（保留 J2a 加入的 router/services 与 `PRIV_INCLUDE_DIRS`），注释说明旧实现已移入 reference |
| `main/bt_link_module.cpp` | 修改 | 删除 `CommandDispatcher` 与它的静态存储；静态构造 4 个服务（`DeviceService` → 先；`AlarmService(s_alarm, s_link_manager, s_device_service->info())`）+ `PairService`，用 4 个 `table()` 组成 `RouteTable[4]` 调 `s_router.init()`；init 失败记日志并返回 `kInvalidState`；`s_dispatcher->info()` 改为 `s_device_service->info()`（SN / 版本写入）；`LinkService` 改传 `s_router` 与 `*s_alarm_service` |
| `reference/cpp-v3-dispatcher/command_dispatcher.cpp` | 移动 | 自 `components/rtk_core/`，内容未改 |
| `reference/cpp-v3-dispatcher/rtk/core/command_dispatcher.hpp` | 移动 | 自 `components/rtk_core/include/rtk/core/`，内容未改（见 §6.1 目录说明） |
| `reference/cpp-v3-dispatcher/test_command_dispatcher.cpp` | 移动 | 自 `test/host/`，内容未改，不再参与构建 |
| `test/host/CMakeLists.txt` | **授权修改** | `rtk_pure`：`command_dispatcher.cpp` → `command_router.cpp` + `services/{device,config,alarm,pair}_service.cpp`，加 `private_include` 私有 include（`PRIVATE`）；测试列表去掉 `test_command_dispatcher` |
| `test/host/test_link_service.cpp` | **授权修改（仅对象构造）** | 新增 3 个服务头 include；`Fixture` 用 `DeviceService` + `ConfigService` + `AlarmService` + `PairService` + `CommandRouter.init()` 替代 `CommandDispatcher`，`LinkService` 传 router/alarm。**12 个测试函数与全部 `TEST_ASSERT` 未改** |
| `test/services/CMakeLists.txt` | 修改 | `rtk_svc` 旧实现源改为 `reference/cpp-v3-dispatcher/command_dispatcher.cpp`，新增 `-I reference/cpp-v3-dispatcher`（rtk_svc 与 golden 用例各一处） |
| `test/services/test_services_golden.cpp` | 修改（1 行注释） | 在旧实现 include 上方注明它经 `-I reference/cpp-v3-dispatcher` 指向只读基准；比较逻辑与期望未改 |
| `tools/main_host_check/CMakeLists.txt` | 修改 | `main_pure` 源列表同步替换为 router + 4 服务，加 `private_include` |
| `docs/reports/J2b.md` | 新增 | 本报告 |

未修改 `components/rtk_core/services/*`、`command_router.cpp`（J2a 产物）、`components/rtk_console`、`components/bt_reconnect`、`components/app_fw`、
`main` 的其他文件、`tools/main_host_check/*.cpp`（源码不含 dispatcher 引用，只需 CMake 同步）、`docs/frozen.sha256`。
未烧录、未连接串口、未联网、未 git commit。

## 3. 实现要点

- `LinkService` 只换了两个协作者；`reply_` 复用缓冲、写超时、`kPushPeriodMs` 判定、`pushed_once_`、`push_seq_` 递增、
  断开应用顺序全部保持原样。`CommandRouter::handle` 与 `AlarmService::build_status_push` 都先 `reply.clear()`，
  与旧 dispatcher 的可见行为一致（J2a 的 golden 测试已逐字节证明）。
- `main` 装配顺序：`DeviceService` →（写入 SN/版本）→ `ConfigService` → `AlarmService(s_alarm, s_link_manager, s_device_service->info())`
  → `PairService` → `s_router.init({4 个 table()})` → `LinkService(s_transport, s_link_manager, s_router, *s_alarm_service)`。
  `AlarmService` 持有 `DeviceService::info()` 的引用，构造顺序是硬约束，已在代码里注明。
- 4 个服务的构造函数不是 `constexpr`（冻结头未提供），因此沿用 J1 对 `LinkService` 的做法：静态 `alignas` 存储 + `init()`
  中 placement new，避免动态初始化顺序依赖、不使用堆。`CommandRouter` 默认构造是 `constexpr`，直接 `constinit` 静态构造，
  `init()` 在 `bt_link_init` 中只调用一次（`Executor::init` 对每个模块调用一次；与 J1 的 dispatcher 行为一致）。
- 沿用 J1 §6 的 include 规则：新增的 `rtk/core/services/*` 头与其它冻结 OS 头一样放在文件顶部、`namespace rtk::app` 之前。

## 4. 自检结果

| # | 检查 | 命令 | 结果 |
| --- | --- | --- | --- |
| 1 | 冻结校验（只允许第 4 条授权的 4 项失败） | `sha256sum -c docs/frozen.sha256` | **PASS**：158 OK + 4 项预期失败（见下表） |
| 2a | `test/host` 普通 | `cmake -S test/host -B build-j2b-host && cmake --build build-j2b-host -j16 && ctest --test-dir build-j2b-host --output-on-failure` | **PASS** 7/7 |
| 2b | `test/host` ASan/UBSan | 同上 + `build-j2b-host-asan`，三个 sanitizer 变量 | **PASS** 7/7 |
| 2c | `test/services` 普通 | `cmake -S test/services -B build-j2b-services && cmake --build build-j2b-services -j16 && ctest --test-dir build-j2b-services --output-on-failure` | **PASS** 2/2（golden 现在对比 reference 里的旧实现） |
| 2d | `test/services` ASan/UBSan | `build-j2b-services-asan` + 三个 sanitizer 变量 | **PASS** 2/2 |
| 2e | `test/app_fw`、`test/bt_reconnect`、`test/stream_demux` 不回归 | 三个目录各做普通与 ASan/UBSan 构建 + `ctest` | **PASS** 2/2、1/1、4/4（普通与 ASan 均同） |
| 3a | `tools/main_host_check` 两种策略普通 | `cmake -S tools/main_host_check -B build-j2b-mainhost && cmake --build build-j2b-mainhost -j16 && ctest --test-dir build-j2b-mainhost --output-on-failure` | **PASS** 27/27（concurrent + preempt） |
| 3b | 同上 ASan/UBSan | `build-j2b-mainhost-asan` | **PASS** 27/27 |
| 4 | 主工程构建 | `. ~/esp/esp-idf/export.sh >/dev/null && idf.py build` | **PASS**（EXIT=0，`warning:` 0 条）。首次构建因 CMakeLists 变化触发 reconfigure，出现 5 条 IDF 自带 `esp_wifi`/`wpa_supplicant` 私有 include 的 CMake Warning（与 J1 报告同源，非本项目组件）；紧接着的第二次 `idf.py build` 为 `warning:` 0、`CMake Warning` 0 |
| 4b | 资源 | `idf.py size`、`idf.py size-components` | 见 §5 |
| 5 | 旧实现残留检查 | `grep -rn "CommandDispatcher\|command_dispatcher" components main test tools` | **PASS**：命中项全部是注释，或 `test/services` 中对 reference 的引用（详见 §6.2） |
| 6 | 格式 | `clang-format --style=file --dry-run --Werror components/rtk_core/link_service.cpp main/bt_link_module.cpp test/host/test_link_service.cpp test/services/test_services_golden.cpp` | **PASS**（先 `-i` 格式化改动文件；`.txt` 不在 clang-format 门禁范围内） |
| 附加 | clang-tidy（改动的纯组件源） | `clang-tidy -p build-j2b-host components/rtk_core/link_service.cpp` | **PASS**：用户代码 0 告警（其余为 non-user code 抑制项） |
| 附加 | reference 内容未改 | `sha256sum reference/cpp-v3-dispatcher/rtk/core/command_dispatcher.hpp reference/cpp-v3-dispatcher/test_command_dispatcher.cpp` | 与 `docs/frozen.sha256` 中对应条目逐字节相同（`dc6b6bc…`、`09bfb0b…`） |
| — | 权威 astyle_py 门禁 | 需联网下载 | **NOT_RUN**（AGENTS.md §5：由验收方/用户执行） |
| — | 烧录 / 串口 / HIL / 实际栈水位 | — | **NOT_RUN**（任务卡禁止；留给 J5） |
| — | 主动 LSP 诊断 | — | **NOT_RUN**：clangd 未使用 `build-j2b-services` 数据库时会对 golden 用例报 `rtk/core/command_dispatcher.hpp` 找不到。已用编译数据库里的真实命令 `-fsyntax-only` 复核（`HAS_REF_INCLUDE=True`、退出码 0），并以实际编译 + ctest 为准 |

### 4.1 冻结校验的 4 项失败（均为任务卡第 3/4 条授权）

```
components/rtk_core/include/rtk/core/command_dispatcher.hpp: FAILED open or read   # 已移入 reference（第 3/4 条）
test/host/CMakeLists.txt: FAILED                                                  # 第 4 条授权修改
test/host/test_command_dispatcher.cpp: FAILED open or read                        # 已移入 reference（第 3/4 条）
test/host/test_link_service.cpp: FAILED                                           # 第 4 条授权修改（仅对象构造）
```

其余 158 条全部 OK；`components/rtk_core/include/rtk/core/link_service.hpp`（Claude 已修订）与 `docs/frozen.sha256`
当前一致。按要求**未**修改 `docs/frozen.sha256`，等待复核后重新生成。

### 4.2 `test/host` 每个测试的前后数量对比

用例数取 Unity 运行汇总（`N Tests 0 Failures`）与源文件 `RUN_TEST` 计数，两者一致。

| 测试可执行 | 改动前 | 改动后 | 说明 |
| --- | ---: | ---: | --- |
| `test_bytes` | 7 | 7 | 未改动 |
| `test_frame` | 13 | 13 | 未改动 |
| `test_alarm_codec` | 9 | 9 | 未改动 |
| `test_link_manager` | 17 | 17 | 未改动 |
| `test_command_dispatcher` | 16 | —（不再构建） | 按第 3/4 条移入 `reference/cpp-v3-dispatcher/`，16 个用例内容原样保留 |
| `test_link_service` | 12 | **12** | **用例数与全部断言未改**，只替换 Fixture 的对象构造 |
| `test_proto_frame_c` | 14 | 14 | 未改动 |
| `test_proto_alarm_c` | 5 | 5 | 未改动 |
| 合计 | 8 个可执行 / 93 个用例 | 7 个可执行 / 77 个用例 | 减少的 16 个用例是退役 dispatcher 的专属测试，其覆盖已由 `test/services` 的 36907 次 golden 比较与 router 单测承接 |

## 5. 资源（对比 J1）

| 项目 | J1 | J2b | 增量 |
| --- | ---: | ---: | ---: |
| 静态 DRAM | 76,709 B | **77,325 B** | +616 B（ADR 门限 +4 KB 内） |
| DRAM .bss | 49,744 B | 50,360 B | +616 B |
| DRAM .data | 26,965 B | 26,965 B | 0 B |
| IRAM | 82,779 B | 82,779 B | 0 B |
| Flash Code | 412,832 B | 413,444 B | +612 B |
| Flash Data | 118,448 B | 118,624 B | +176 B |
| size 报告 total image | 641,064 B | 641,852 B | +788 B |
| 实际 app bin | 641,184 B (`0x9c8a0`) | 641,968 B (`0x9cbb0`) | +784 B |
| app 分区（`ota_0`） | 1,703,936 B | 1,703,936 B | — |
| app 剩余 | 1,062,752 B / 62.37% | **1,062,480 B / 62.35%** | −272 B，仍远高于 55% |

DRAM 增量的来源：删掉 dispatcher 自身（指针 + `DeviceInfo` + 3 块配置），换成 4 个服务 + `CommandRouter`（48 × 16 B 索引表），
净增约 600 B。链路栈配置 6144 B 未变。

## 6. 说明与待确认项

### 6.1 reference 目录布局

任务卡要求“保持文件名”。移入的 `command_dispatcher.cpp` **内容不得修改**，它写的是 `#include "rtk/core/command_dispatcher.hpp"`，
所以退役头放在 `reference/cpp-v3-dispatcher/rtk/core/command_dispatcher.hpp`，构建时用 `-I reference/cpp-v3-dispatcher` 解析
（文件名未变，只是保留原来的相对包含路径）。若复核希望扁平放置（`reference/cpp-v3-dispatcher/command_dispatcher.hpp`），
需要改为增加一个转发头，或者修改退役源的 include——后者违反“不得修改其内容”，因此本任务选了前者。

### 6.2 `grep` 命中分类（自检 5）

- 注释：`link_service.hpp:60`、`reply.hpp:1`、`command_router.hpp:3`、`services/*.hpp` 与 `services/*.cpp` 的文件头与行内注释、
  `private_include/service_support.hpp`、`command_router.cpp:2`、`components/rtk_core/CMakeLists.txt:4`、
  `test/host/test_link_service.cpp:38`、`test/services/CMakeLists.txt:31,75`、`test/services/test_services_golden.cpp:1`。
- `test/services` 对 reference 的引用：`test/services/CMakeLists.txt:64`（`${RTK_REF_DISPATCHER}/command_dispatcher.cpp`）、
  `test/services/test_services_golden.cpp:12`（include，经 `-I reference/cpp-v3-dispatcher` 解析）、`:30`（`using`）、
  `:178`（`CommandDispatcher old_disp;`，golden 两侧对比用）。
- `components`、`main`、`tools`、`test/host` 中**没有**任何代码引用（只剩注释）。

### 6.3 其它

- `bt_link_init` 未加“重复 init”保护：与 J1 的 `CommandDispatcher` 做法一致（`Executor::init` 对每个模块只调用一次）。
  若以后出现重复 init，`s_router.init()` 会返回 `kBusy` 并让模块 init 失败，不会静默重复登记。
- `main_host_check` 的源文件无需改动（`main_host_check.cpp` / `app_test_adapter.hpp` 不含 dispatcher 引用，
  适配器直接 `#include "../../main/bt_link_module.cpp"`，自动跟随新装配）。
- 全程只用默认 `build/`（任务卡第 4 条明确要求 `idf.py build`）与自建 `build-j2b-*` 目录；未清理或改动其他 agent 的构建目录，
  构建期间没有因他人文件失败。

## 7. 未完成 / 后续

- 冻结头复核与 `docs/frozen.sha256` 重新生成：由 Claude 执行（本任务按授权只改了 4 项）。
- 权威 astyle_py 门禁、上板 HIL、实际栈余量与 overrun 观测：留给 J5。
- J3（rtk_console 组件）与 J4 的组件不受本次改动影响。

J2b DONE
