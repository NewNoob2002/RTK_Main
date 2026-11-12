# J2a CommandRouter + 4 个服务（BATCH-J，ADR-0007）

执行方：pi + workbuddy/deepseek-v4.1-flash。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0007-app-services-split.md`（重点 §3.3、§3.5、§9），
`components/rtk_core/include/rtk/core/{reply,command_router,command_dispatcher}.hpp`、`components/rtk_core/include/rtk/core/services/*.hpp`，
`components/rtk_core/command_dispatcher.cpp`（现有行为的唯一真相）与 `test/host/test_command_dispatcher.cpp`。

## 交付
1. `components/rtk_core/command_router.cpp`：实现 `command_router.hpp` 全部语义（init 的错误码、查找顺序：未登记 → kNoReply 先于角色检查；角色不符 → kDenied；`handle` 先 `reply.clear()`）。
2. `components/rtk_core/services/{device,config,alarm,pair}_service.cpp`：把 `command_dispatcher.cpp` 的各分支按头文件中的路由表搬到对应服务，**应答字节与旧实现逐字节一致**；`AlarmService::build_status_push` 与旧 `build_status_push` 一致。
3. `components/rtk_core/CMakeLists.txt`：SRCS 增加上述文件，REQUIRES 视需要增加 `app_fw`。保留 `command_dispatcher.cpp`（J2b 才移走）。
4. `test/services/`（新建，独立 CMake 工程，host g++，支持普通与 ASan/UBSan）：
   - **golden 等价测试**：同一组输入同时喂旧 `CommandDispatcher` 和新 `CommandRouter`+服务，比较 `DispatchResult`、`reply.bytes()`、`whitelist_changed`、`heartbeat`。输入至少覆盖：msg_id 0x00–0xFF 全部 × {PHONE, ALARM} × {QUERY, SET, PUSH, 非法 type} × {空负载, 典型负载, 超长负载}；以及有状态序列（SET 后 QUERY、配对窗口开关、白名单清空、报警覆盖设置/清除、心跳更新后 status push）。
   - router 单元测试：重复 msg_id、空 handler、roles=0、self 为空、超过 `kMaxRoutes`、重复 init 的返回值。
5. 报告 `docs/reports/J2a.md`：用例数、覆盖的 msg_id 表、golden 比对结果，以及任何与旧实现不一致之处（**不一致时以旧实现为准修改服务，不得修改测试期望**；若认为旧实现本身有缺陷，写证据，不要改）。

## 要求
- AGENTS.md 全部 C++ 规则；rtk_core 仍不得包含任何 IDF/FreeRTOS 头。路由表 `static constexpr`，handler 为 `noexcept` 静态函数，在其中 `static_cast` 回服务类型（写注释说明 self 的来源保证类型正确）。
- 源文件头 `Generated-by: pi + workbuddy/deepseek-v4.1-flash`，人工负责人 `UNKNOWN（待人工审查签字）`。

## 自检（报告逐项 PASS/FAIL 并给出命令）
1. `test/services` 普通与 ASan/UBSan 构建，ctest 全部通过。
2. `test/host` 普通与 ASan/UBSan 回归通过（旧 dispatcher 测试仍在）。
3. `. ~/esp/esp-idf/export.sh >/dev/null && idf.py -B build-j2a -D SDKCONFIG=build-j2a/sdkconfig build`：0 warning（新服务此时尚未被 main 使用，链接时会被裁掉，这是预期的）。
4. clang-tidy（`-p` 指向 test/services 的构建目录）对新 .cpp 无告警；clang-format dry-run 无违规；`sha256sum -c docs/frozen.sha256` 全部 OK。

## 并行约束
J1、J2a、J4 同时运行：主工程只能用 `-B build-j2a -D SDKCONFIG=build-j2a/sdkconfig` 构建（不得用默认 `build/` 与根目录 `sdkconfig`，它们属于 J1）；host 构建目录也必须带 `build-j2a` 前缀或放在自己的测试目录下。

## 禁止
修改冻结文件（AGENTS.md §4，含所有 include 头与 `test/host/**`）；修改 `main/**`、`tools/**`（J1）、`components/rtk_os/**`、`components/app_fw/**`、`components/bt_reconnect/**`、`test_apps/**`、`components/rtk_core/link_service.cpp`；
烧录、monitor、连接串口、联网安装、git commit。完成后最后一行输出 `J2a DONE` 或 `J2a BLOCKED: <原因>`。
