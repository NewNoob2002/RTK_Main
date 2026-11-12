# TIDY-FIX ESP 侧自有代码 clang-tidy 清零

日期：2026-11-11。实现者：omp + DeepSeek-V4.1-Flash。人工负责人：UNKNOWN（待人工审查签字）。

范围：`main/app_console.cpp`、`main/app_console.hpp`、`main/app_link.cpp`、`main/app_link.hpp`、`main/app_main.cpp`、
`components/rtk_bt/rtk_bt_spp.c`，以及 `components/rtk_os/include/rtk/os/static_task.hpp` 第 87 行一处获授权的 const 修正。
`test_apps/` 的发现不属本任务，未改动。

## 结论

6 个目标文件 clang-tidy 发现从 **98 条降到 0 条**（基线 113 条 = 98 + `test_apps/rtk_pure` 15 条）。协议语义、时序、队列深度、
临界区范围、分块发送与 ACK 匹配逻辑均未改变；改动为类型收紧（C 数组/指针+长度 → `std::array`/`std::span`）、const 化、
魔数命名与等价分支合并。`app_console.hpp` 无需改动。

## 改动文件

| 文件 | 内容 |
|---|---|
| `main/app_link.hpp` | `Command::sn`、`StatusSnapshot::sessions/whitelist` 改 `std::array`；`derive_sn_from_mac`/`build_device_name` 改为固定长度 `std::span` + `std::string_view`；新增 `kDeviceNameBuf` 与 `<array>`/`<string_view>` |
| `main/app_link.cpp` | `kHexDigits` 改 `std::string_view`；placement new 存储与 RX 块、白名单缓冲区改 `std::array`；两处 `CriticalSection` const 化；未用任务参数匿名化；魔数命名；MAC 半字节与设备名生成重写 |
| `main/app_main.cpp` | `valid_sn` 改 `std::string_view`（`find_first_of` 等价替换逐字符扫描）；`resolve_sn`/`build_bt_name` 改 span/view；SN、BT 名、MAC 缓冲改 `std::array`；`err` const 化 |
| `main/app_console.cpp` | 6 个 `esp_console` 回调去掉指针下标（改 `std::span<char *>`）；2 个回调未用参数匿名化、3 个保留 `char **` 签名并加 NOLINT；协议上限与参数下标命名；`kCommands[]` 改 `constexpr std::array`（CTAD） |
| `components/rtk_bt/rtk_bt_spp.c` | `0xFFFFU` → `UINT16_MAX`；PIN 改为按 `RTK_BT_SPP_PIN_LEN` 逐字节复制；`rtk_bt_spp_settle_write` 合并两个相同失败分支；GAP 回调删除与 `default` 相同的空 `MODE_CHG` 分支 |
| `components/rtk_os/include/rtk/os/static_task.hpp` | 仅第 87 行 `auto *self` → `const auto *self`（获授权的一处；接口与其他行未动） |

## 各类发现的处理方式

| 发现类别 | 处理 |
|---|---|
| `cppcoreguidelines-avoid-c-arrays`（数据成员/局部/形参/类型别名中的 C 数组） | 结构体成员、局部缓冲、命令表改 `std::array`；纯函数形参改固定长度 `std::span`；白名单 blob 用 `WhitelistRaw = std::array<std::array<uint8_t,6>,4>` + 静态断言同布局；placement new 存储用 `std::array<std::byte,N>` 别名 |
| `cppcoreguidelines-pro-bounds-array-to-pointer-decay` | 全部改为显式 `.data()`/`.size()` 或隐式转 `std::span`/`std::string_view`（后者不是数组退化） |
| `cppcoreguidelines-pro-bounds-pointer-arithmetic` | `argv[i]` 改 `std::span` 下标；`sn[idx]` 改 `std::string_view` 查找或 span 下标；MAC 半字节改循环 |
| `misc-const-correctness` | `CriticalSection` 守卫、`esp_err_t err` 加 const；无用的 `argc/argv` 改匿名形参；`esp_console` 三处签名受函数指针类型约束，加具名 NOLINT |
| `readability-magic-numbers` | 协议/时序常量命名：`kAlarmStateMax/kAlarmLevelMax`、`kAlarmStateArg/kAlarmLevelArg`、`kCommandSendMs`、`kMacSnHighByte/kMacSnLowByte/kNibbleMask/kNibbleShift`；`0xFFFFU` 换标准宏 `UINT16_MAX`；PIN 的 4 个硬编码下标改为按 `RTK_BT_SPP_PIN_LEN` 循环 |
| `bugprone-branch-clone` | `rtk_bt_spp_settle_write` 的 `if/else if` 同为 `ESP_FAIL`，合并为单一 `failed` 判定（注释保留短写判定依据）；GAP 回调中与 `default` 相同的空 `MODE_CHG` 分支删除 |
| `cppcoreguidelines-pro-type-member-init` | RX 块 `std::array<uint8_t,64> chunk{}` 零初始化（每块 64 字节 memset，相对 StreamBuffer/队列操作可忽略） |

行为等价性说明：`build_device_name` 的 `%s` 改 `%.*s`（长度由 `string_view` 给出，精度 ≤ 16，缩窄到 `int` 安全）；
`valid_sn` 的逐字符换行扫描改 `find_first_of("\r\n")`；`derive_sn_from_mac` 的 4 个字面下标改为半字节数组循环，输出字节位置不变。

## 全部 NOLINT 清单

| 文件:行 | 检查名 | 理由 |
|---|---|---|
| `main/app_console.cpp:166` | `misc-const-correctness` | `esp_console_cmd_func_t` 固定为 `int(int, char **)`；把形参改为 `char *const *` 会改变函数类型、无法注册到命令表，无安全改写 |
| `main/app_console.cpp:204` | `misc-const-correctness` | 同上（`cmd_wl`） |
| `main/app_console.cpp:220` | `misc-const-correctness` | 同上（`cmd_sn`） |
| `main/app_link.cpp:159` | `cppcoreguidelines-pro-type-reinterpret-cast`、`cppcoreguidelines-avoid-c-arrays` | `rtk_store_set_alarms` 为冻结 C 接口，形参是“指向 6 字节数组的指针”；从连续 `std::array<std::array<uint8_t,6>,4>` 取地址做等价指针转换，布局由 `static_assert` 保证 |
| `main/app_link.cpp:539` | 同上 | `rtk_store_get_alarms` 同理（差量缓冲 `rows` 指针） |

无无检查名的 NOLINT，无 `NOLINTBEGIN` 大段豁免，未改任何 `.clang-tidy*` 配置。

## 实际执行的命令与结果

每个构建 shell 先执行 `. ~/esp/esp-idf/export.sh >/dev/null`。

| 检查 | 命令 | 结果 |
|---|---|---|
| 目标构建 | `idf.py -B build-tidy-fix build` | **PASS**：exit 0；4 个改动 TU 全部重编（`app_console.cpp`、`app_main.cpp`、`rtk_bt_spp.c`、`app_link.cpp`）；`warning:`/`error:` 计数 **0** |
| G5b clang-tidy | `python3 -I tools/tidy_check/tidy_check.py --idf-build build-tidy-fix` | 6 个目标文件 **0 发现**；`test_apps/rtk_pure/main/test_main.cpp` 仍 15 条（不属本任务）；脚本整体 `G5b FAIL`、exit 1 仅由这 15 条导致 |
| G7b | `python3 -I tools/g7b_check/g7b_check.py --build build-tidy-fix` | **PASS**：自有目标文件 17 个；ELF 异常/RTTI 6/6 白名单内，拉入链一致 |
| 体积 | `idf.py -B build-tidy-fix size` | **PASS**：Total image size **620,408** 字节；`rtk_main.bin` **620,528** 字节（T14 记录 620,720 → 少 192 字节），远低于 1.2 MB |
| rtk_bt 回归 | `cmake -S tools/t13_host_check -B build-t13-tidy && cmake --build build-t13-tidy -j 4`，然后 `ctest --test-dir build-t13-tidy` | **PASS**：构建 exit 0；**22/22 场景通过**（6.52 s） |
| host 回归 | `cmake -S test/host -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure` | **PASS**：8/8 |
| host ASan/UBSan | `cmake -S test/host -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" && cmake --build build-asan && ctest --test-dir build-asan --output-on-failure` | **PASS**：8/8，无 sanitizer 报告 |
| 格式 | `clang-format --style=file --dry-run --Werror main/app_console.cpp main/app_console.hpp main/app_link.cpp main/app_link.hpp main/app_main.cpp components/rtk_bt/rtk_bt_spp.c components/rtk_os/include/rtk/os/static_task.hpp` | **PASS**：exit 0 |
| 冻结清单 | `sha256sum -c docs/frozen.sha256` | 除 `components/rtk_os/include/rtk/os/static_task.hpp` 外全部 **OK**；该文件 **FAILED 为预期**（获授权的一行 const 修正，新哈希 `80d685ae44b671c35c682a2f759ff39aefedc835e63a3d5dafe06d9a71eb350c`，待架构方更新清单） |
| 权威 astyle_py | — | **NOT_RUN**：项目禁止实现方下载/安装，由验收方执行 |
| 实板 HIL / 控制台交互 | — | **NOT_RUN**：禁止连接串口/烧录 |

## 未解决问题与风险

- `test_apps/rtk_pure/main/test_main.cpp` 的 15 条发现（C 数组、退化、const）仍存在，按任务边界未处理；因此 G5b 脚本整体仍返回 `FAIL`/exit 1。
- `docs/frozen.sha256` 中 `static_task.hpp` 一条需由架构方按授权更新（本报告已给出新哈希）。
- `main/` 无 host 回归工程（T14 的临时 smoke 已按规范删除），控制台参数解析与 SN/设备名生成的改动只有类型等价论证 + 目标构建证据，未做运行时验证；实板 HIL、UART0 交互、双会话时序仍为 NOT_RUN。
- `derive_sn_from_mac`/`build_device_name` 的空指针检查随固定长度 `std::span` 取消（类型层面排除空指针与越界），调用方均传栈上 `std::array`。

TIDY-FIX DONE: PASS 6 个目标文件 clang-tidy 发现 98→0，构建 0 告警，G7b/22 项驱动回归/8 项 host 与 ASan 回归全通过，仅剩 test_apps 既有 15 条与待架构方更新的冻结哈希。
