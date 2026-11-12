# J2a：CommandRouter + 4 个服务（BATCH-J，ADR-0007）

- 执行方：pi + workbuddy/deepseek-v4.1-flash
- 人工负责人：UNKNOWN（待人工审查签字）
- 依据：`docs/tasks/J2a-router-services.md`、`AGENTS.md`、`docs/adr/ADR-0007-app-services-split.md`
- 基线（唯一真相）：`components/rtk_core/command_dispatcher.cpp`（行为）+ `test/host/test_command_dispatcher.cpp`（旧期望）

## 1. 交付内容

| 文件 | 状态 | 行数 | 说明 |
| --- | --- | --- | --- |
| `components/rtk_core/command_router.cpp` | 新增 | 100 | `init` / `handle` / `route_count`，查找顺序与旧 dispatcher 一致 |
| `components/rtk_core/services/device_service.cpp` | 新增 | 198 | 0x01 / 0x02 / 0x03 / 0x04 / 0x05 / 0x06 / 0x0D |
| `components/rtk_core/services/config_service.cpp` | 新增 | 137 | 0x07 / 0x14 / 0x15（持有三块配置的唯一副本） |
| `components/rtk_core/services/alarm_service.cpp` | 新增 | 182 | 0x40 / 0x41 / 0x42 / 0x43 + `build_status_push` |
| `components/rtk_core/services/pair_service.cpp` | 新增 | 93 | 0x44 |
| `components/rtk_core/private_include/service_support.hpp` | 新增（私有头） | 62 | `reply_build` / `reply_ok` / `reply_fail` / `kResultOk|Fail`，自旧 dispatcher 匿名命名空间原样迁出 |
| `components/rtk_core/CMakeLists.txt` | 修改 | 16 | SRCS 增加 5 个 .cpp，新增 `PRIV_INCLUDE_DIRS "private_include"`；`command_dispatcher.cpp` 保留（J2b 移走）；未新增 `REQUIRES app_fw`（无 app_fw 用法） |
| `test/services/CMakeLists.txt` | 新增 | 77 | 独立 host 工程（Unity 取自 IDF，`-fno-exceptions -fno-rtti`，严格告警组同 `test/host`） |
| `test/services/test_command_router.cpp` | 新增 | 294 | router 单元测试，9 个用例 / 52 条断言 |
| `test/services/test_services_golden.cpp` | 新增 | 597 | golden 等价测试（旧 `CommandDispatcher` vs 新 `CommandRouter`+4 服务） |

未修改任何冻结文件；`main/**`、`tools/**`、`components/rtk_os/**`、`components/app_fw/**`、
`components/bt_reconnect/**`、`test_apps/**`、`components/rtk_core/link_service.cpp` 均未触碰。

## 2. 实现要点

### 2.1 路由表与 handler

- 每个服务的 `routes()` 返回函数内 `static constexpr std::array<Route, N> kRoutes`（15 条路由合计：
  设备 7 + 配置 3 + 报警 4 + 配对 1），8 个 server 语义与旧 `is_known_id` / `is_phone_command` 集合完全一致。
- handler 是 `noexcept` 静态成员函数，第一个参数 `void *self` 用 `static_cast` 回到服务类型；注释写明
  `self` 来自 `table()`（`RouteTable{this, routes()}`），类型必然正确。
- 角色限制按旧行为落在 `RoleMask`：0x01–0x0D、0x14、0x15、0x42、0x43、0x44 为 `kRolePhone`；
  0x40、0x41 为 `kRoleAlarm`。0x40 只接受 QUERY、0x41 只接受 PUSH，其他 type 由 handler 返回 `kDenied`
  （与旧 `dispatch_alarm` 的两条 `if` 等价）。

### 2.2 冻结头未暴露私有状态时的取法（需要审查确认）

`device_service.hpp`（只有 `info()` 公开）、`config_service.hpp`（只读视图）、`alarm_service.hpp`（只有
`build_status_push`）、`pair_service.hpp`（无任何访问器）都没有暴露 handler 需要写入/读取的私有成员，
而 `RouteHandler` 是普通函数指针、`self` 是 `void *`。实现方式：在各自 `routes()` 里声明一个**函数内的局部
结构体** `Handlers`，其静态成员函数承担 handler；按 C++ 标准 `[class.access]`（"A local class of a member
function may access the same members that the member function itself may access"），局部类与所在成员函数
访问权限相同，因此 `static_cast<ConfigService *>(self)->work_mode_` 之类合法。实际逻辑仍在匿名命名空间的
自由函数里（`handle_work_mode(std::span<uint8_t>, ...)` 等），局部结构体只做取参转发，`routes()` 长度可控。

- 标准层面合法，gcc 15.2 / clang 23 均通过；无全局登记表、无 `reinterpret_cast`、无 UB、无运行时开销。
- 代价是可读性偏怪。建议后续解冻这些头时二选一：加只读/可写访问器，或把 handler 直接声明成类的
  `static` 成员函数（这样更直白）。本轮按“冻结头一个字节都不能改”处理，未改头。

### 2.3 其它

- `Route kRoutes[]` 写作 `std::array<Route, N>`：`.clang-tidy` 启用了 `cppcoreguidelines-avoid-c-arrays`
  且 `WarningsAsErrors: '*'`，C 数组会直接报错。语义与 ADR §3.3 的写法等价。
- `command_router.cpp` 出错路径统一把 `count_` 清 0 并返回（`kBusy` / `kInvalidArg` / `kNoSpace`），
  保证“出错后路由器保持未初始化，handle 一律 clear + kNoReply”。
- `AlarmService` 持有 `const DeviceInfo &`，因此 **`DeviceService` 必须先构造**（golden 测试的 `Harness`
  与未来 J2b 的组装根都按此顺序声明）。这一点需写进 J2b 的接线说明。

## 3. 自检结果

| # | 检查 | 命令 | 结果 |
| --- | --- | --- | --- |
| 1a | `test/services` 普通构建 + ctest | `cmake -S test/services -B build-j2a-services && cmake --build build-j2a-services && ctest --test-dir build-j2a-services --output-on-failure` | **PASS**（2/2，0.06 s） |
| 1b | `test/services` ASan+UBSan | `cmake -S test/services -B build-j2a-services-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && cmake --build build-j2a-services-asan && ctest --test-dir build-j2a-services-asan --output-on-failure` | **PASS**（2/2） |
| 2a | `test/host` 回归（旧 dispatcher 测试仍在） | `cmake -S test/host -B build-j2a-host && cmake --build build-j2a-host && ctest --test-dir build-j2a-host --output-on-failure` | **PASS**（8/8，含 `test_command_dispatcher`、`test_link_service`） |
| 2b | `test/host` ASan+UBSan 回归 | 同上，目录 `build-j2a-host-asan` + 三个 sanitizer 变量 | **PASS**（8/8） |
| 3 | 主工程构建 | `. ~/esp/esp-idf/export.sh && idf.py -B build-j2a -D SDKCONFIG=build-j2a/sdkconfig build` | **PASS**（EXIT=0，编译告警 0 条；日志中 5 条 CMake Warning 全部来自 IDF 内部 `esp_wifi`/`wpa_supplicant` 的私有头校验，与本次改动无关） |
| 4a | clang-tidy（新 .cpp，`-p build-j2a-services`） | `clang-tidy -p build-j2a-services components/rtk_core/command_router.cpp components/rtk_core/services/*.cpp` | **PASS**（用户代码 0 告警；报告的都是 `non-user code` 抑制项） |
| 4b | clang-format dry-run | `clang-format --style=file --dry-run --Werror <上述 6 个文件 + 两个测试 .cpp>` | **PASS**（先按 `--style=file` 格式化过） |
| 4c | 冻结校验 | `sha256sum -c docs/frozen.sha256` | **PASS**（160/160 OK） |
| — | 权威 astyle 门禁 | 需联网下载 astyle_py | **NOT_RUN**（AGENTS.md §5：由验收方/用户执行） |
| — | 上板 / 串口 / 烧录 | — | **NOT_RUN**（任务卡禁止） |

补充证据（golden 测试确实有区分能力）：临时把 `device_service.cpp` 的 `writer.u8(info.battery_pct)` 改成
`battery_pct + 1`，golden 测试立即出现 2 处失败；改回后重新全绿。说明它不是恒真测试。

## 4. 用例与覆盖

### 4.1 数量

| 测试 | 内容 | 规模 |
| --- | --- | --- |
| `test_services_golden::test_msg_id_sweep` | 全部 msg_id × 角色 × type × 负载形状 | **36864** 次双向比较（256 × 4 × 4 × 9） |
| `test_services_golden` 有状态序列 | 设备 / 配置 / 配对 / 报警（含心跳、状态推送） | **39** 次双向比较 + **4** 次 `build_status_push` 逐字节比较 |
| `test_command_router` | 9 个用例，52 条断言 | 空白表、重复 msg_id、空 handler、roles=0、self 为空、`kMaxRoutes+1`、恰好 `kMaxRoutes`、重复 init、未 init、角色不符、未登记先于角色检查、handler 参数与 `reply.clear()` |

golden 总计 **36907** 次等价比较；每次比较对齐 `DispatchResult`、`reply.len`、应答帧全字节、
`whitelist_changed`、`heartbeat{seq,battery_pct,local_state}`（≈29.5 万条断言）。

扫描维度的 9 种负载形状：`empty`、`pair_open1`、`pair_clear1`、`test_set3`、`test_clear3`、`short2`、
`wm44`（`kWorkModeSetLen`）、`typical50`（典型负载）、`full256`（`kMaxPayload` 超长）。4 种 type：
`QUERY`、`SET`、`PUSH`、`0x07`（非法）。角色：`PHONE`、`ALARM`，**并额外覆盖 `kNone`、`kPending`**
（旧 dispatcher 对两者也是 `kDenied`）。

### 4.2 已登记 msg_id 表（旧 dispatcher 实测矩阵，新实现逐项一致）

`R` = `kReply`，`n` = `kNoReply`，`d` = `kDenied`；列顺序 Q/S/P/X = `QUERY`/`SET`/`PUSH`/非法 type `0x07`。

| msg_id | 命令 | PHONE Q/S/P/X | ALARM Q/S/P/X |
| --- | --- | --- | --- |
| 0x0001 | 设备信息 | R R R R | d d d d |
| 0x0002 | 设备状态 | R R R R | d d d d |
| 0x0003 | 电池/存储 | R R R R | d d d d |
| 0x0004 | GGA（占位） | R R R R | d d d d |
| 0x0005 | 卫星（占位） | R R R R | d d d d |
| 0x0006 | 卫星跟踪 | R R R R | d d d d |
| 0x0007 | 工作模式 | R R R R | d d d d |
| 0x000D | COM 口 | R n n n（5.4 行为：SET 不回复） | d d d d |
| 0x0014 | 电台 | R R R R | d d d d |
| 0x0015 | 日志 | R R R R | d d d d |
| 0x0040 | 报警状态 | d d d d | R d d d（仅 QUERY） |
| 0x0041 | 心跳/HELLO | d d d d | d d n d（仅 PUSH，写 `reply.heartbeat`） |
| 0x0042 | 测试报警 | R R R R | d d d d |
| 0x0043 | 电子围栏（预留，回失败） | R R R R | d d d d |
| 0x0044 | 配对/白名单 | R R R R（SET clear 时 `whitelist_changed=1`） | d d d d |

其余 **241 个 msg_id（0x00–0xFF 中未登记的，含 5.4 保留 ID 0x1E、0x21–0x25、0x28–0x34）** 在两种角色、
四种 type 下都是 `kNoReply` 且 `reply.len == 0`（未登记判定先于角色检查），两侧逐项一致。

### 4.3 有状态序列覆盖

- 设备：默认值 7 条查询、改写后（SN/fw/board_sn/board_fw/radio_fw/on_battery/battery_pct/temp/com_baud）
  3 条查询、0x06 与 0x0D 的 SET 分支。
- 配置：0x07 SET 44 B 成功 + 回读 48 B + 43 B 失败不回滚；0x14 SET 15 B（第 17 字节保持 0x05）+ 回读 +
  14 B 失败 + 28 B 全量；0x15 SET 50 B + 回读 + 49 B 失败 + PUSH 走只读分支。
- 配对：打开窗口 → 报警器 HELLO 入白名单 → 查询（1 字节数量 + 6 字节 MAC）→ 清空（`whitelist_changed=1`）
  → 查询（数量 0）→ 非法动作字节失败。
- 报警：手机在线 → 空闲状态查询/推送 → 0x42 设置覆盖（alert/level 3）→ 覆盖后的查询与推送 →
  0x41 心跳（seq/battery/local_state 对齐）→ 心跳后电池变化再推送 → 清除覆盖 → 非法状态失败且状态不变 →
  0x43 预留失败 → 0x40 SET 与 0x41 QUERY 的 `kDenied`。

## 5. 与旧实现的不一致

**无。** 36907 次比较全部逐字节一致，`AlarmService::build_status_push` 与旧 `build_status_push` 的整帧
（20 B 头 + 16 B payload + 4 B CRC）一致。测试期望未被修改；也没有发现需要修改服务的分歧。

未发现旧 `CommandDispatcher` 的缺陷（0x0D SET 不回复、0x43 恒回失败均为 5.4 既定行为，头文件已注明）。

## 6. 偏离与待确认项

1. **handler 取私有状态的写法**（见 §2.2）：用“成员函数内的局部类”取得访问权限。合法但不常见，
   需要人工审查确认是否接受；若不接受，建议解冻 `components/rtk_core/include/rtk/core/services/*.hpp`
   增加访问器或直接声明 handler。
2. **`std::array` 代替 C 数组路由表**（见 §2.3）：受 `.clang-tidy` 的 `cppcoreguidelines-avoid-c-arrays`
   约束，语义不变。
3. **新增私有头 `private_include/service_support.hpp`**（AGENTS.md §4 允许在 `src/` 或 `private_include/`
   下新增私有头），并在两个构建系统里分别用 `PRIV_INCLUDE_DIRS` / `target_include_directories(PRIVATE)`
   注册。副作用：若某个工具（例如编辑器 LSP）不读这两个构建数据库，会报 `service_support.hpp file not
   found`；`build-j2a`（IDF）与 `build-j2a-services`（host）的 `compile_commands.json` 都含该目录，
   真实编译与 clang-tidy 均无问题。
4. **`REQUIRES` 未增加 `app_fw`**：本任务的服务与路由不引用 app_fw（任务卡写“视需要”）。
5. **`kMaxRoutes = 48` 与现有 15 条路由**：容量余量充足；未做编译期重号 `static_assert`（`RouteTable` 是
   运行期 `init` 展开，重复 msg_id 由 `init` 返回 `kInvalidArg` 覆盖，已有单测）。

## 7. 未完成 / 后续

- J2b：把 `LinkService` 的依赖从 `CommandDispatcher &` 换成 `CommandRouter & + AlarmService &`，`main` 接线
  （注意 `DeviceService` 必须先于 `AlarmService` 构造），旧 dispatcher 与其 `test/host/test_command_dispatcher.cpp`
  移入 `reference/`。本任务未触碰这些文件。
- 权威 astyle 门禁与上板验证：由验收方/用户执行（本轮禁止联网与烧录）。
- 并行任务导致的大目录构建：本轮全程用 `build-j2a*` 目录，未使用默认 `build/` 与根 `sdkconfig`，
  构建期间未出现因其他 agent 文件导致的失败。
