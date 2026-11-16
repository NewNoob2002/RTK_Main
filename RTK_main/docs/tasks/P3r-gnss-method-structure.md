# P3r：gnss 组件内部方法结构整理（命令库分层 + Controller 状态分派）

状态：ISSUED 2026-11-16，基线 `feat/gnss` 6bfaef8。实施：pi + workbuddy/deepseek-v4.1-flash；审查/验收：Claude。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0012-gnss-receiver-drivers.md`，
再读 `components/gnss/**`、`test/gnss/**`（尤其 `test/gnss/golden/`）。

## 0 目的与原则

纯重构：**不改任何对外行为与冻结接口**，只把驱动和 Controller 内部写成"方法名即用途"的结构
（参考 SparkFun Unicore 库的 `setModeRover` / `setPortBaudrate` / `disableOutput` 这类命名，
但我们只生成字节、不阻塞发送）。

- 冻结头（`docs/frozen.sha256` 中 `components/gnss/include/**` 全部条目）**不得修改**，包括 `Controller.hpp`
  的 private 区。Controller 的内部方法放进 `src/Controller.cpp` 里已有的 `friend struct ControllerImpl`
  （静态成员函数，参数 `Controller &self`），或文件内匿名命名空间函数。
- 不是机械拆分：每个新函数必须有一个能用一句话说清的用途；不要为了行数把一段连续逻辑切成
  `part1/part2`。`Controller::poll()` 对外形式不变（任务循环调用，合理），只改内部。
- 不加新依赖，不改 Kconfig，不动 `main/`、其它组件、`.clangd`。
- 新增/改动的每个函数写 doxygen 注释（中文正文、英文标签）：`@brief`，有参数写 `@param 名: 含义(单位)`，
  有返回值写 `@retval`；文件内函数注释写在定义处。

## 1 要做

### 1.1 共享 ASCII 命令工具 `components/gnss/src/AsciiCmd.hpp`（+ 需要时 `.cpp`）

namespace `gnss::ascii`（内部头，不进 include/）。把 `src/drivers/UnicoreReceiver.cpp:41-140` 与
`src/drivers/ComnavReceiver.cpp:40-124` 两份重复的 `is_hex` / `hex_value` / `append_uint` / `append_period` /
`next_quoted` / `quoted_equals`（以及同类 NMEA 风格 `*hh` 校验、行尾 CR/LF 去除）合并为一份，两个驱动改为调用。
UbloxReceiver 若有可共用的（如十进制追加）也用它；UBX 二进制工具留在 u-blox 驱动内。

### 1.2 厂商命令库：命令"做什么"与流程分开

每个驱动 .cpp 内（或驱动私有头 `src/drivers/<Vendor>Cmd.hpp`）建立两组按用途命名的函数：

```cpp
namespace gnss::unicore::cmd {      // 只往 CmdBuf 写一条完整命令（含 \r\n），失败返回 false
bool unlog(CmdBuf &out, uint8_t port);                         // "UNLOG COMn"
bool mode_rover(CmdBuf &out);                                  // "MODE ROVER"
bool config_baud(CmdBuf &out, uint8_t port, uint32_t baud);    // "CONFIG COMn <baud>"
bool log_msg(CmdBuf &out, std::string_view name, uint8_t port, uint32_t period_ms); // "<name> COMn <period>"
bool unlog_msg(CmdBuf &out, std::string_view name, uint8_t port);                   // "UNLOG COMn <name>"
bool version_query(CmdBuf &out);                               // 探测命令
}
namespace gnss::unicore::resp {     // 应答解析，每个函数只判一件事
bool has_valid_tail(std::span<const uint8_t> frame);           // "*hh\r\n" 结构
std::optional<size_t> find_response_tag(std::span<const uint8_t> frame); // "response:" 位置
bool echo_matches(std::span<const uint8_t> frame, size_t tag_pos, std::span<const uint8_t> sent);
std::span<const uint8_t> response_text(std::span<const uint8_t> frame, size_t tag_pos);
}
```

ComNav、u-blox 按同样思路给出各自的 `cmd::` / `resp::`（名字按其协议实际操作取，例如 ComNav `log_ontime`、
u-blox `valset_msgout`、`ack_class_id`）。函数名以实际需要为准，上面是 Unicore 的目标形态。

驱动成员函数改为组合这些操作：
- `rover_step` 用步骤表代替 `if (index == next) ... ++next` 阶梯，例如
  `constexpr StepFn kRoverScript[] = {&step_unlog_all, &step_mode_rover, &step_port_baud};`，
  可选步骤（如 `port_baud == 0` 时跳过）由步骤函数返回"跳过"表达，保持现有索引→步骤的映射逐字节一致。
- `classify_reply` / `reply_ack` 依次调用 `resp::*`。
- `judge`（`UnicoreReceiver.cpp:210`，85 行）这类成帧判定按"找帧头 / 校验长度 / 校验和 / 分类"拆出命名函数。
- `build_output` 用 `cmd::log_msg` / `cmd::unlog_msg`；报文名查表（`OutputMsg` → 名字/是否支持）。

### 1.3 Controller 状态分派

`src/Controller.cpp` 的 `poll()` 内 323 行 switch 改为：`poll` 保留外层循环与守卫，`switch (state_)` 每个 case
一行调用 `ControllerImpl::on_power_up / on_probing / on_configuring / on_output / on_running / on_failed`；
重复操作抽为命名函数，例如 `send_step`、`advance_step`、`switch_host_baud`、`enter_state`、`next_probe`、
`arm_deadline`。`on_frame`（31 行，CCN 17）同理按"探测应答 / 配置应答 / 输出应答 / 运行数据"分派。
状态迁移顺序、计数器、stats 更新时机必须与现有完全一致。

### 1.4 规模目标（参考，不是硬门禁）

整理后 `lizard components/gnss/src` 中除 `NmeaParser.cpp`（本卡不动）外，函数尽量 ≤ 50 NLOC、CCN ≤ 15；
确实不宜再拆的（如一张大 switch 查表）在报告里说明理由。

## 2 不做

- 不改 `NmeaParser.cpp`（另行处理）。
- 不改任何冻结头、不改 `test/gnss/golden/*.inc`、不改测试期望。测试对不上 → 是重构错了，修实现。
- 不新增功能（$CONFIG 回读是 P3c）。

## 3 验收门禁（全部贴输出摘要到报告）

1. `test/gnss`：默认、`-DRTK_GNSS_DRV_COMNAV=ON`（以 CMakeLists 实际 option 名为准）× 普通 / ASan+UBSan，全部通过；
   黄金向量逐字节一致（现有 golden 测试即判据）。
2. `tools/main_host_check`（含 ASan）通过。
3. `. ~/esp/esp-idf/export.sh && idf.py -B builds/p3r build` 0 warning；记录 bin 大小与基线（6bfaef8）差值。
4. `sha256sum -c docs/frozen.sha256` 全部 OK（不得重算）。
5. clang-format：改动文件 `clang-format --dry-run --Werror` 通过（bash 中展开文件列表）。
6. `uvx lizard components/gnss/src` 前后对比表（函数数、超 50 NLOC / CCN 15 的清单）。

## 4 交付

- 报告 `docs/reports/P3r.md`：改动清单、新函数一览（按文件，名字 + 一句用途）、门禁输出、lizard 对比、未拆的理由。
- 不要 commit，由 Claude 审查后提交。
