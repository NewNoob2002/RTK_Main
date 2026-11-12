# BATCH-E 复审与项目进度

日期：2026-11-10。Profile：esp-idf；依据为项目 `sdkconfig.defaults`、IDF 组件构建和 ADR-0003 的 C/C++ 分工。本次只审查代码，未修改生产源码、公共接口、冻结输入或既有报告；新增本报告。

本次结论：**G8 代码复审 FAIL；不维持 BATCH-E 无阻塞通过的判断。** T16 未发现新缺陷；T13 发现 3 项 High、1 项 Medium 的 SPP 问题，以及 1 项 Medium 的事件组包装契约问题。另有 1 项 Low 的既有说明问题。构建、host 测试、格式和纯组件静态检查通过，不能覆盖驱动的异步失败与跨核竞争。以下缺陷由源码路径确认，未做硬件复现。

## 当前进度

| 任务 | 状态 | 当前证据与剩余范围 |
| --- | --- | --- |
| T10 工程骨架 | 已有 QA-C 软件验收 | ESP32 工程可构建；`main/app_main.cpp:60` 当前仅打印版本与复位原因 |
| T11 协议基础 | 已有 QA-C 软件验收 | C++ 协议入口在 T16 中改为委托 C 核心 |
| T12 核心链路逻辑 | 已有 QA-C 软件验收；本次 host 回归通过 | LinkManager、CommandDispatcher、AlarmSource、LinkService 已实现，尚未接入运行任务 |
| T16 C 协议核心与 C++ 外观 | 已有 QA-E 软件验收；本次协议复审与回归通过 | 检查 CRC、最大长度、重同步、输出生命周期、失败输出与错误映射，未发现新的可证实缺陷 |
| T13 OS 包装与 SPP | 已有 QA-E 软件验收；本次复审发现阻塞项 | 3 项 High、2 项 Medium，见下方发现；修复后需重新验收，HIL 仍 NOT_RUN |
| T14 NVS 与应用集成 | 未实现 | `components/rtk_store/CMakeLists.txt:1` 仍为占位；无 `rtk_store.c`；main 无链路任务与控制台实现 |
| T15 双架构测试应用 | 未实现 | 尚无 `test_apps/rtk_pure`，esp32/esp32c3 双架构门禁未完成 |

当前处于 BATCH-E 后、BATCH-F 前的组件阶段。此进度针对 ADR-0002/0003 定义的阶段；GNSS、电台、电源、LED 等未来模块不在本阶段范围。不能把软件组件通过当作整机功能完成。

## 复审发现

### 1. High：快速断开重连时，旧读取与新会话 reset 并发破坏接收状态

- 位置：`components/rtk_bt/rtk_bt_spp.c:112`、`:622`、`:627`；依据 ESP-001 共享状态同步要求与会话隔离约束。
- 触发：链路任务已找到旧会话并进入非阻塞读取，BTC 随后依次处理旧会话 CLOSE 与新会话 OPEN，复用同一槽并调用 `xStreamBufferReset`。
- 具体交错：旧读取已经取得 64 字节可读及 tail=0；reset 把 head/tail 清零；旧读取随后把 tail 写回 64。2048 字节环形存储因此呈现 head=0、tail=64，下一次被解释为有 1984 字节可读，新会话会读到无效数据或丢失数据。
- 证据：IDF `components/freertos/FreeRTOS-Kernel/stream_buffer.c:557` 的 reset 只排除正在阻塞的读写者；`:1013` 的零等待接收不持有 reset 使用的锁，`:1152` 保存 tail，`:1190` 在复制结束后写回 tail。驱动查找函数退出临界区后仅返回槽指针，不保护随后整次接收的生命周期。
- 最小修复方向：在查找/读取期间登记并保护活跃读者，旧读者退出前禁止槽 reset/复用；BTC 回调不能等待，可延后复用或拒绝该次新连接。不得把 StreamBuffer API 放入项目禁止调用 FreeRTOS API 的外层临界区来规避。
- 不修复的影响：快速重连时可能把旧槽状态带入新会话。既有 `QA-E.md:43` 将该竞争列为 Low，但本次已确认具体状态损坏机制，应提升优先级；HIL 不是确认此同步缺陷存在的前置条件。

### 2. High：异步 SPP 初始化失败后，调用者仍得到成功且资源不回滚

- 位置：`components/rtk_bt/rtk_bt_spp.c:255`、`:354`、`:601`；依据 ESP-004 和冻结 `rtk_bt_spp.h:47` 的失败回滚契约。
- 触发：`esp_spp_enhanced_init` 请求成功入队，随后 BTC 创建 SPP mutex/event group 失败；或者初始化完成后启动服务端分配 RFCOMM 槽失败。
- 实际结果：驱动已设置 `s_initialized=true` 并返回 `ESP_OK`。INIT/START 失败回调只记日志，控制器、Bluedroid 和驱动 RTOS 对象未清理；再次 init 被 `ESP_ERR_INVALID_STATE` 拒绝，设备保持无法提供 SPP 服务的状态。
- 证据：IDF `components/bt/host/bluedroid/api/esp_spp_api.c:54` 仅返回请求投递结果；`btc/profile/std/spp/btc_spp.c:559`、`:590` 通过 INIT 回调报告实际初始化失败，`:754`、`:778` 通过 START 回调报告服务端资源失败。
- 最小修复方向：回调发布静态完成状态，由初始化任务等到最终 INIT/START 成功后才返回成功；异步失败交由任务上下文逆序释放。不能在 BTC 回调中阻塞执行依赖 BTC 的 disable/deinit。
- 不修复的影响：启动流程会把不可用的蓝牙服务当作就绪，且本次启动内无法正常重试。

### 3. High：连续发送没有等待 WRITE 完成，底层 TX 队列满后上层仍计为成功

- 位置：`components/rtk_bt/rtk_bt_spp.c:339`、`:673`、`:680`；依据发送可靠性与完成状态传播要求。
- 触发：同一会话连续提交 11 个发送请求，期间尚未处理发送完成事件且驱动尚未观察到拥塞。`components/rtk_core/link_service.cpp:108`、`:126` 对一批收到的查询逐帧发送应答，能够形成该类突发。
- 实际结果：`esp_spp_write` 的同步成功只表示请求已投递；IDF 深度 10 的每会话 TX 队列拒绝第 11 个请求，并随后报告 WRITE 失败。驱动仅打印错误，没有待完成状态或错误回传；LinkService 已把同步成功计入 `replies_tx`，该应答实际未发送。
- 证据：IDF `components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c:37` 设置 `SLOT_TX_QUEUE_SIZE=10`，`:167` 用该容量初始化队列，`:925`–`:943` 在入队失败时报告 `ESP_SPP_NO_RESOURCE`；`:1140` 仅在发送完成处理时释放队列中的已完成条目。
- 最小修复方向：用静态发送完成状态约束未完成提交，处理 WRITE 的 status/len/cong，避免向已满队列继续提交，并把完成错误反馈给调用者；相关等待共用剩余超时预算。
- 不修复的影响：突发查询会无声丢失应答，软件统计仍可显示发送成功。

### 4. Medium：write 的实际提交可无限等待，未受 timeout_ms 约束

- 位置：`components/rtk_bt/rtk_bt_spp.c:680`；依据冻结 `rtk_bt_spp.h:58` 的总等待约束与 T13 的有界等待要求。
- 触发：BTC 工作队列已满；即使 `timeout_ms=0`，驱动通过拥塞检查后仍直接调用 `esp_spp_write`。
- 实际结果：递减超时仅覆盖拥塞解除等待，提交进入 IDF 后使用 `OSI_THREAD_MAX_TIMEOUT`，最终是 `xQueueSend(..., portMAX_DELAY)`，链路任务可能阻塞超过指定超时。
- 证据：`esp_spp_api.c:226` → `btc_spp.c:1452` → `components/bt/common/btc/core/btc_task.c:366`；`components/bt/common/osi/include/osi/thread.h:18` 将该上限定义为 `OSI_SEM_MAX_TIMEOUT`，`components/bt/common/osi/thread.c:121`–`:126` 将它映射为无限队列等待。
- 最小修复方向：必须覆盖实际提交阶段；IDF 公共 write API 无超时参数，如维持总时限，可由持有静态数据副本的提交任务调用 IDF，链路任务在预算内等待结果。超时后的数据所有权和完成状态也须明确定义，不能只修改现有拥塞循环。
- 不修复的影响：链路任务停在提交阶段，无法按计划处理其他会话和周期工作。

### 5. Medium：事件等待超时的行为与公共注释不符

- 位置：`components/rtk_os/include/rtk/os/static_event_group.hpp:43`、`:49`。
- 依据：公共接口“超时返回 0”的契约；错误路径必须可区分（审查清单 A）。
- 触发：事件组只有 bit 0，调用 `wait(0x03, false, true, timeout_ms)` 等待全部两位。bit 1 一直未置位，等待超时。
- 实际结果：包装直接返回 `xEventGroupWaitBits` 的结果，返回值为 `0x01`，不是 0。调用方若按注释以非零判断成功，会在等待条件未满足时继续运行。即使 `timeout_ms=0`，同样可达。
- 源码证据：本地 IDF `components/freertos/FreeRTOS-Kernel/event_groups.c:378`–`:383` 在零等待失败时返回当前位；`:441`–`:466` 在阻塞超时后也返回当前位。
- 最小修复：拿到结果后，根据 `wait_all` 检查所请求的位是否满足，条件不满足返回 0；或由 Architect 明确改为 FreeRTOS 原生返回契约并同步调用方说明。
- 当前影响：LSP references 仅发现定义，没有现有生产调用方；是 T14 使用前需要消除的接口陷阱。该头目前在冻结清单内，本次未改动，需由有冻结输入变更权限的角色处理。没有执行这条路径的 RTOS 运行测试。

### 6. Low：既有栈高水位“单位修复”的说明不准确

- 位置：`docs/reports/QA-E.md:42`、`:61`；`components/rtk_os/include/rtk/os/static_task.hpp:73`。
- 既有说明把 IDF 内核按 `StackType_t` 计数等同于非字节，称 task.h 的字节注释有误。
- 本地事实：`~/esp/esp-idf/components/freertos/FreeRTOS-Kernel/portable/xtensa/include/freertos/portmacro.h:78` 将 `portSTACK_TYPE` 定义为 `uint8_t`，`:81` 定义 `StackType_t`；RISC-V port 同样使用 `uint8_t`。内核 `tasks.c:4817` 虽除以 `sizeof(StackType_t)`，当前目标这个值为 1。
- 因此，当前支持目标上原先直接返回的数值已是字节；乘以 `sizeof(StackType_t)` 不改变数值。现代码没有因此产生数值错误，但“修复了单位错误”的结论及“task.h 注释有误”需要由报告/冻结注释所有者更正。本次保留原始验收记录，未修改冻结头。

## 本次执行的检查

以下命令在工程根目录执行；涉及 IDF 的 shell 先执行 `. ~/esp/esp-idf/export.sh >/dev/null`。`/tmp/rtk-host-rereview-e` 与 `/tmp/rtk-asan-rereview-e` 均由本次重新配置、编译。

| 检查 | 实际命令与结果 |
| --- | --- |
| G1 冻结校验 | **PASS**：`sha256sum -c docs/frozen.sha256 --quiet`，exit 0 |
| G2 普通 host | **PASS**：`cmake -S test/host -B /tmp/rtk-host-rereview-e && cmake --build /tmp/rtk-host-rereview-e -j 4 && ctest --test-dir /tmp/rtk-host-rereview-e --output-on-failure`，8/8 可执行文件通过；冻结测试集定义为 93 用例 |
| G2b ASan/UBSan | **PASS**：`cmake -S test/host -B /tmp/rtk-asan-rereview-e -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && cmake --build /tmp/rtk-asan-rereview-e -j 4 && ctest --test-dir /tmp/rtk-asan-rereview-e --output-on-failure`，8/8 通过，无 sanitizer 报告 |
| G3 当前目标构建 | **PASS（增量）**：`idf.py build`，exit 0；本次没有重新编译生产 TU，不能把它记为本次干净构建。既有 QA-E 记录了独立干净构建 |
| G4 当前镜像体积 | **PASS（当前骨架）**：`idf.py size`，exit 0；镜像 `.bin` 137,392 B，size 工具未填充总量 137,281 B；app 分区剩余约 92%。完整集成后的体积尚未测量 |
| G5 格式 | **PASS**：`clang-format --style=file --dry-run --Werror` 检查 `components/` 与 `main/` 下自有生产 `.c/.cpp/.h/.hpp`，exit 0；权威 astyle_py **NOT_RUN**，未下载/安装 |
| G5b C 静态分析 | **PASS（用户代码）**：`clang-tidy -p /tmp/rtk-host-rereview-e --config-file=.clang-tidy-c components/rtk_proto/rtk_proto_frame.c components/rtk_proto/rtk_proto_alarm.c`，用户代码 0 发现；抑制 2 条 non-user-code 告警 |
| G5b C++ 静态分析 | **PASS（用户代码）**：`clang-tidy -p /tmp/rtk-host-rereview-e` 对 rtk_proto 的 frame/alarm_codec/bytes/status 与 rtk_core 的 alarm_source/link_manager/command_dispatcher/link_service 共 8 个 `.cpp` 执行；用户代码 0 发现，抑制 16,221 条 non-user-code 告警；无需额外 header-filter 覆盖 |
| 运行集成状态 | **PASS（调查证据）**：用当前编译器同目录的 `xtensa-esp-elf-nm -C --defined-only build/rtk_main.elf` 统计，`rtk_bt_spp_`、`rtk::bt::SppTransport`、`rtk::core::LinkService` 定义符号均为 0；`app_main` 为 1。当前镜像没有实际接入这些组件 |
| G3b 双架构 | **NOT_RUN**：T15 测试应用尚不存在 |
| C++ 运行时专项 | **NOT_RUN**：本次未重新执行 cpp_cost.py 或自有目标文件堆符号全检；既有 QA-E 提供替代 nm 检查，本次不把它冒充为新证据 |
| G8 代码复审 | **FAIL**：已确认上述 3 项 High、2 项 Medium，另有 1 项 Low；T16 本次未发现新缺陷。上述 SPP 路径未被纯组件 host 测试覆盖，尚无实际 RTOS/硬件复现 |
| HIL-0..6 | **NOT_RUN**：本次未连接串口或进行任何烧录/硬件操作 |

环境提示：IDF export 在非交互 shell 输出 `bash: line 41: complete: command not found`，后续配置、构建、测试和检查均实际执行并成功。未改变工具环境来压制该提示。

## 后续顺序

先修复本次发现 1–5 并重新验收 T13；第 5 项涉及冻结头，须由有冻结输入变更权限的角色处理。第 6 项应更正说明。随后按 `docs/tasks/DISPATCH-F.md` 执行 T14 → T15：NVS、启动装配、链路任务与控制台集成，然后建立纯组件 esp32/esp32c3 冒烟应用。完成后重新测量集成固件体积和运行时约束，进入独立软件验收；真实连接、双会话、拥塞、协议兼容、重启持久化与资源水位须由 HIL 提供证据。

## 改动与遗留

本次唯一文件改动：新增 `docs/reports/QA-E-REVIEW.md`。未实施代码修复，以上 6 项发现均保留待处理；未修改任何冻结输入，未改变接口或任务范围。构建和纯组件回归结果只作为相应门禁证据，不替代 T13 驱动缺陷修复、BATCH-F 集成或硬件验收。
