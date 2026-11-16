# P3r：gnss 组件内部方法结构整理（命令库分层 + Controller 状态分派）

状态：DONE 2026-11-16。基线 `feat/gnss` 6bfaef8（任务卡提交 a7dcc6c 只加卡片本身）。
实施：pi + workbuddy/deepseek-v4.1-flash。纯重构，未改任何冻结头、黄金向量与测试期望。

## 1 改动清单

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `components/gnss/src/AsciiCmd.hpp` | 新增（184 NLOC） | `gnss::ascii`：两份驱动里重复的 ASCII 文本工具合并为一份 |
| `components/gnss/src/drivers/UnicoreReceiver.cpp` | 重构（571→548 NLOC） | `cmd::`/`resp::` 命令库、3 步脚本表、`judge` 拆成前缀/整帧/半帧三个命名检查 |
| `components/gnss/src/drivers/ComnavReceiver.cpp` | 重构（362→344 NLOC） | 同上（`cmd::`/`resp::`、3 步脚本表、`check_line`） |
| `components/gnss/src/drivers/UbloxReceiver.cpp` | 重构（336→459 NLOC） | `cmd::`/`resp::`、`Writer` 去指针算术、2 步脚本表、`build_output` 拆出 `nav_period_ms`/`epochs` 计算 |
| `components/gnss/src/Controller.cpp` | 重构（547→648 NLOC） | `ControllerImpl` 状态分派 + 命名动作（`enter_*`/`advance_*`/`arm_deadline`/`mark_reply`），`poll()` 只留外层循环 |

未修改：所有 `components/gnss/include/**`（含 `Controller.hpp` 私有区）、`test/gnss/**`、`golden/*.inc`、
`main/**`、Kconfig、`.clangd`、其它组件。`docs/frozen.sha256` 未重算。

`cmd::`/`resp::`/`script_*`/`Writer` 都放在各驱动的文件内匿名命名空间里（`gnss::unicore::(anon)::cmd` 等），
因此不新增任何导出符号、不改变组件 ABI；只有 `AsciiCmd.hpp` 是新增的私有头（`gnss::ascii::`，内联）。

## 2 新函数一览（按文件，一句话用途）

### `src/AsciiCmd.hpp`（`gnss::ascii`）

| 名字 | 用途 |
| --- | --- |
| `is_hex` / `hex_value` | 十六进制数字判定与取值（原来 Unicore/ComNav 各一份） |
| `is_printable` / `is_body_byte` | 可打印 ASCII 与"可作报文体"判定（排除 `$`、`*`） |
| `append_uint` | 十进制追加（`0` 不写字节，保持基线语义） |
| `append_period` | 周期毫秒→秒文本（`1`/`0.5`/`0.2`/`0.05`，只接受 10 ms 整数倍） |
| `next_quoted` / `quoted_equals` / `quoted_starts_with` | 引号字段提取与比较 |
| `starts_with` / `bytes_starts_with` | 文本前缀 / 字节前缀比较（回显比较用后者，避免 span→string_view 转换） |
| `has_hex_tail` | `*hh\r\n` 尾部结构判定（合并两份 NMEA 风格校验尾检查） |
| `strip_line_end` | 去掉已发送命令末尾的 CR/LF |
| `xor_checksum` | NMEA 风格 XOR 校验 |

### `src/drivers/UnicoreReceiver.cpp`

| 名字 | 用途 |
| --- | --- |
| `cmd::unlog` / `cmd::mode_rover` / `cmd::config_baud` | `UNLOG COMn` / `MODE ROVER` / `CONFIG COMn <baud>` |
| `cmd::log_msg` / `cmd::unlog_msg` | `<name> COMn <period>` / `UNLOG COMn <name>` |
| `cmd::version_query` | `VERSIONA`（探测） |
| `resp::has_valid_tail` | 帧尾是否为合法 `*hh\r\n` 且长度够 |
| `resp::find_response_tag` | 找 `,response: ` 位置（`std::optional<size_t>`） |
| `resp::echo_matches` | 回显是否就是本步命令 |
| `resp::response_text` | 取应答文本（去尾部空格） |
| `check_prefix` / `check_full_frame` / `check_partial_frame` | `judge`（原 85 行/CCN 28）拆成三个独立检查 |
| `checksum_matches` | 标准 XOR 与 UM980 `$` 兼容变体 |
| `need_more` / `complete` / `reject` | 结论构造器，替代 8 处重复的 `verdict{}` 赋值 |
| `model_from_text` / `fill_probe_result` | 版本文本→型号、填 `ProbeResult` |
| `step_unlog_all` / `step_mode_rover` / `step_port_baud` | 脚本三步（第三步由 `port_baud_enabled` 决定是否参与） |
| `always` / `port_baud_enabled` | 步骤参与条件 |
| `kRoverScript` | `constexpr std::array<ScriptStep,3>` 步骤表，替代 `if (index == next)` 阶梯 |

### `src/drivers/ComnavReceiver.cpp`

| 名字 | 用途 |
| --- | --- |
| `cmd::unlog_all` / `cmd::mode_rover` / `cmd::com_baud` | `UNLOGALL COMn` / `MODE ROVER` / `COM COMn <baud>` |
| `cmd::log_ontime` / `cmd::unlog_msg` / `cmd::version_query` | `LOG COMn <name> ONTIME <period>` / `UNLOG COMn <name>` / `LOG VERSIONA ONCE` |
| `resp::is_ok` / `resp::is_error` | `<OK` / `<ERROR` 类别判定 |
| `check_line` | 整行检查（CRLF、首位 `<`、中间可打印） |
| `reject` | 结论构造器 |
| `step_unlog_all` / `step_mode_rover` / `step_port_baud` + `kRoverScript` | 同 Unicore 的步骤表 |

### `src/drivers/UbloxReceiver.cpp`

| 名字 | 用途 |
| --- | --- |
| `Writer::u8/u16/u32/view` | 载荷写入（下标定位，去掉 `payload[pos]` 指针算术） |
| `cmd::build_frame` / `cmd::fletcher` | UBX 帧组装与 Fletcher 校验 |
| `cmd::poll_mon_ver` | MON-VER 轮询帧（校验字节命名 `kMonVerPollCkA/CkB`） |
| `cmd::configure_receiver` | CFG-RATE-MEAS + RTCM3X 输入 + NMEA 输出协议（一次 VALSET） |
| `cmd::set_baudrate` | UART1 波特率 VALSET |
| `cmd::valset_msgout` | 单键 CFG-MSGOUT VALSET |
| `cmd::begin_valset` / `add_u1_item` / `add_u2_item` / `add_u4_item` | VALSET 载荷构造 |
| `resp::payload_len` | 帧头长度字段 |
| `resp::has_valid_ack` / `resp::ack_class_id` | ACK/NAK 结构校验与被 ACK 的 class/id |
| `read_mon_ver` / `ext_text` / `copy_text` | MON-VER 扩展项扫描（型号/固件） |
| `nav_period_ms` | 频率夹到 1..20 Hz 并换算周期 |
| `step_configure` / `step_baud` + `kRoverScript` | 两步脚本表 |

### `src/Controller.cpp`

| 名字 | 用途 |
| --- | --- |
| `ControllerImpl::dispatch` | 按 `state_` 一状态一行分派 |
| `on_off` / `on_power_up` / `on_probing` / `on_configuring` / `on_output` / `on_running` / `on_failed` | 每个状态的处理函数 |
| `probe_start` / `probe_await` | 探测的"发下一个候选"与"等应答/超时" |
| `config_send` / `config_await` / `config_no_reply` / `config_switch_baud` | 配置脚本的 4 个子阶段 |
| `output_send` / `output_await` | 输出阶段的"下一项"与"等应答" |
| `enter_probing` / `enter_configuring` / `enter_output` / `enter_failed` | 状态迁移（复位游标、掩码、退避） |
| `advance_step` / `advance_output` | 推进游标并清重试计数 |
| `arm_deadline` / `mark_reply` | 设置截止时间、记录应答判定 |
| `power` / `set_baud` / `write` / `none` | `Action` 构造器 |
| `Round` | `kAgain`（状态已迁移需重分派）/`kDone`（本轮有结论） |

`poll()` 现在是：外层 `for (guard…)` + 每次 `action = Action{}; if (dispatch(...) == kDone) return action;`，
与原来的 `continue` 语义一一对应；`on_frame()` 用 `mark_reply()` 收敛了配置/输出两处重复的判定赋值。

## 3 门禁结果

### 3.1 `test/gnss`（黄金向量逐字节一致）

| 构建 | 结果 |
| --- | --- |
| `builds/p3r-gnss`（`-DRTK_GNSS_DRV_COMNAV=ON`） | **8/8 PASS** |
| `builds/p3r-gnss-off`（默认 ComNav=OFF） | **7/7 PASS** |
| `builds/p3r-gnss-asan`（`-DSANITIZE=ON -DRTK_GNSS_DRV_COMNAV=ON`） | **8/8 PASS** |
| `builds/p3r-gnss-asan-off`（`-DSANITIZE=ON`） | **7/7 PASS** |

黄金对比（`plan_check::plan_mismatch` / `ubx_plan_mismatch`，Unicore 324 / u-blox 108 / ComNav 108 组组合）
以及全部既有用例均未改动、全部通过 —— 说明重构后的命令字节与步骤映射完全一致。

### 3.2 `tools/main_host_check`

| 构建 | 结果 |
| --- | --- |
| `builds/p3r-mhc` | **44/44 PASS** |
| `builds/p3r-mhc-asan`（`-DSANITIZE=ON`） | **44/44 PASS** |

### 3.3 IDF 构建与体积

`idf.py -B builds/p3r build`：**Project build complete，0 warning**（`grep -Ei "warning:|error:"` 无输出）。

基线 6bfaef8 用 `git archive 6bfaef8 | tar -x -C builds/p3r-base-src` + `idf.py -B builds/p3r-base build` 现构：

| 项 | 基线 6bfaef8 | P3r | 差值 |
| --- | --- | --- | --- |
| Flash Code | 544832 | 545912 | **+1080** |
| Flash Data | 144856 | 144904 | +48 |
| IRAM | 86187 | 86187 | 0 |
| DRAM | 64207 | 64207 | 0 |

纯重构增加了约 1.1 KB 代码：`cmd::`/`resp::`/`step_*`/`ControllerImpl::on_*` 这些单层包装不再被内联回原位置
（原实现是一段连续代码，现在是"成员函数→cmd:: 函数→CmdBuf"，例如 `build_output` 从 1 段变成 2 层），
加上 `std::optional` 返回值与 `Writer` 的下标写入各占几条指令。DRAM/IRAM 不变（无新增静态数据）。

### 3.4 冻结清单

`sha256sum -c docs/frozen.sha256`：180 条全部 `OK`，未重算（本卡未改任何冻结文件）。

### 3.5 clang-format

```
clang-format --style=file --dry-run --Werror \
  components/gnss/src/AsciiCmd.hpp components/gnss/src/Controller.cpp \
  components/gnss/src/drivers/{Unicore,Ublox,Comnav}Receiver.cpp
→ PASS（无输出）
```

首次检查 UbloxReceiver.cpp 有 10 处换行风格不符（数组初始化的行尾、`&&` 断行），已 `clang-format -i` 修正。

### 3.6 lizard 前后对比

命令：`uvx --offline lizard components/gnss/src -l cpp`（1.26.0，本地缓存）。

总体：

| 指标 | 前 | 后 |
| --- | --- | --- |
| 函数数 | 150 | 230 |
| 总 NLOC | 2867 | 3234 |
| 平均 NLOC | 17.6 | **12.6** |
| 平均 CCN | 5.5 | **3.9** |
| CCN>15 或 NLOC>50 的函数 | 11 | **5**（全部在 `NmeaParser.cpp`） |

按文件：

| 文件 | 前 NLOC / 函数数 / 平均 CCN | 后 NLOC / 函数数 / 平均 CCN |
| --- | --- | --- |
| `drivers/ComnavReceiver.cpp` | 362 / 27 / 3.9 | 344 / 36 / **2.4** |
| `drivers/UbloxReceiver.cpp` | 336 / 27 / 3.1 | 459 / 42 / 2.9 |
| `drivers/UnicoreReceiver.cpp` | 571 / 33 / 5.4 | 548 / 47 / **3.1** |
| `Controller.cpp` | 547 / 15 / 8.0 | 648 / 43 / **3.3** |
| `AsciiCmd.hpp` | — | 184 / 14 / 4.7 |
| `NmeaParser.cpp`（本卡不动） | 762 / 27 / 9.4 | 不变 |

超过 50 NLOC 的函数：

| 前 | 后 |
| --- | --- |
| `gnss::unicore::judge`（85 NLOC，CCN 28） | 已消失（拆为 `check_prefix` 21 / `check_full_frame` 19 / `check_partial_frame` 31，最大 CCN 12） |
| `gnss::Controller::poll`（323 NLOC，CCN 61） | 已消失（`poll` 自身 9 NLOC/CCN 3；最大块为 `probe_start` 39/7、`output_send` 38/7、`config_await` 36/9、`output_await` 36/9） |
| `parse_gga`/`parse_gsa`/`parse_gsv`（NmeaParser） | 不变（卡 §2 明确不动） |

CCN>15 的函数（lizard 警告）：前 11 个 → 后 5 个，且后 5 个全在 `NmeaParser.cpp`。
即 `components/gnss/src` 里除 `NmeaParser.cpp` 外已无 NLOC>50 或 CCN>15 的函数。

## 4 未拆/保留的理由

1. `NmeaParser.cpp` 的 `parse_gga`/`parse_gsa`/`parse_gsv`/`talker_to_constellation`：卡 §2 明确不在本卡范围（另行处理），
   保持原样。
2. `unicore::explain_verdict`（45 NLOC，CCN 10）、`comnav::explain_verdict`、`state_name`、`dispatch`：
   都是一张 `switch` 查表，每个 case 就是一句 `text.literal(...)`/一行转调，再拆只会把同一张表切成几段，
   反而看不出完整性（卡 §0 反对为了行数而机械拆分）。它们的 CCN 来自 case 数而非嵌套分支。
3. `UbloxReceiver.cpp` 的 NLOC 由 336 升到 459：`Writer` 与每个 VALSET 构造器都是"一个动作一个函数"，
   名称本身就是文档；字节输出与原来逐步对应，没有引入新逻辑。
4. `cmd::`/`resp::` 保留在匿名命名空间（而不是对外可见的 `gnss::unicore::cmd`）：卡 §1.2 允许"驱动 .cpp 内"，
   放在内部命名空间避免新增组件级导出符号，也不影响测试（测试只通过 `Receiver` 接口验证字节）。

## 5 与契约的偏离

- 无行为偏离：所有既有测试与黄金向量未改且通过（§3.1）。
- 结构选择：`cmd::`/`resp::` 位于各驱动 .cpp 的匿名命名空间内（见 §4.4）；`AsciiCmd.hpp` 为头文件内联实现，
  未新增 `AsciiCmd.cpp`，因此无需改 `components/gnss/CMakeLists.txt`。
- `Controller.hpp` 未改：`friend struct ControllerImpl` 早已存在，`ControllerImpl` 定义在 `src/Controller.cpp`
  的 `namespace gnss` 内。

## 6 未验证项

- 实物（HIL）：本卡是纯重构且黄金向量逐字节一致，未连任何接收机（授权边界也不允许）；上板验证留待后续。
- `pi-lens` 对 `NmeaParser.cpp`/`.hpp` 反复报旧的"级联语法错误"：`clangd --check` 实测 0 个编译错误
  （只有 61 条 style 提示），`g++ -fsyntax-only` 与 IDF/host 构建均干净；且该文件不在本卡范围。

## 评审补记（Claude）

- 差分测试（scratchpad p3r_diff，基线 a7dcc6c vs 本次工作区，同一程序分别链接，ASan+UBSan）：
  驱动面（build_probe、caps/output_mask × 8 型号、rover_step 遍历 rate×gsv×port×baud、build_output 遍历 msg×period×rate×port）、
  4 万次语料+变异帧 × 6 解码器（classify_reply/match_probe/classify_frame/frame_tag/match_readback）、
  600 个种子的 Controller 随机仿真（探测/ACK/NACK/丢包/乱码应答，穿插 reconf/reprobe/request_output/stop/start）。
  - 非逐帧输出（脚本、输出命令、仿真轨迹与终态统计）共 420,128 行逐字节一致。
  - 发现并修复 1 处偏离：u-blox `probe_match` 长度检查丢了 2 字节校验和（`frame.size() < 6 + len`，原为 `8 + len`），
    导致截断的 MON-VER 帧被认作命中；已改回 `kPayloadOffset + len + kCrcBytes`，最小帧长同步改为头+校验。
  - 保留 1 处偏离：Unicore 应答判定新增 `has_hex_tail`，对“校验位不是十六进制字符”的帧由 ACK/NACK 变为 kNone。
    这类帧解码器 `UnicoreResp` 已拒收（差分中全部 v=0），运行中不可达，判定更严，接受。
- 门禁复跑：test/gnss ComNav OFF/ON × 普通/ASan 7/7/8/8，main_host_check 44/44，frozen 0 失败，clang-format 通过，
  `idf.py -B builds/p3r build` 0 warning，app 0xc2c60。
- 上板（UM980 R4.10Build11833，/dev/ttyACM0）：冷启动 `probes=1 steps=8 nacks=0 to=0 fail=0`，applied=0x7B；
  1 Hz 实测 1.0 Hz；`gnss rate 5` 实测 4.5 Hz（10 s 窗口含切换时间）；`gnss out heading 1` → unsupported；
  `gnss out gga off/1` 正常；`gnss rate 1` 恢复。未测：u-blox/ComNav 实机（无硬件）。
