# P3s：GNSS 驱动型号子类与命令/应答类化（ADR-0012 §2.8）

状态：ISSUED 2026-11-16，基线 `feat/gnss` a23ebc1 + 本卡 T0 改动（未提交，见 `git diff`）。
实施：pi + workbuddy/deepseek-v4.1-flash；审查/验收：Claude。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`（§5.1 日志、§6.2 注释）、`docs/adr/ADR-0012-gnss-receiver-drivers.md`
（重点 §2.2、§2.8），再读 `components/gnss/**`、`test/gnss/**`（尤其 `test/gnss/golden/`）。

## 0 已由 Claude 写好的冻结接口（T0，不得改动其签名）

- `components/gnss/include/gnss/Receiver.hpp`：新增 `virtual const Receiver &for_model(Model) const noexcept`（默认返回 `*this`）。
- `components/gnss/include/gnss/drivers/UnicoreReceiver.hpp`：`UnicoreReceiver` 去 `final`，新增
  `for_model` 覆盖、`protected` 的 `ScriptStep`、`virtual rover_script()`、`static cmd_*` / `resp_*`；新增
  `class Um982Receiver final : public UnicoreReceiver`（覆盖 `name`/`caps`/`for_model`）与 `um982_receiver()`。

## 1 要做

### 1.1 基类默认实现
`src/Receiver.cpp` 实现 `Receiver::for_model` 返回 `*this`。`NullReceiver` 不需覆盖。

### 1.2 Unicore 驱动（`src/drivers/UnicoreReceiver.cpp`）
- 把匿名命名空间里的 `cmd::*` 改为 `UnicoreReceiver::cmd_*` 静态成员，`resp::*` 改为 `UnicoreReceiver::resp_*`；
  逻辑逐字节不变。原 `cmd::` / `resp::` 命名空间删除，调用点全部改用成员。
- 步骤函数改为头里已声明的 protected static 成员 `step_unlog_all` / `step_mode_rover` / `step_port_baud` /
  `step_always` / `step_if_port_baud`（原文件内函数 `always`、`port_baud_enabled` 删除）。
- `rover_script()` 内定义函数局部 `static constexpr std::array<ScriptStep, 3> kScript{...}` 并返回它
  （成员函数作用域内才能取 protected 成员地址）。
- `rover_step` 遍历 `rover_script()`，
  映射与现在的 `script_step` 一致。
- `caps(Model)`：UM981 → `kRtcmIn|kBase|kIns|kRate20Hz`；UM980 与 UM982（家族视角）→ `kRtcmIn|kBase|kRate20Hz`；
  未知 → `kRtcmIn|kBase`。**不含 kHeading**。
- `output_mask` / `build_output` 的 HEADING 判断改为 `caps(model) & cap::kHeading`（虚调用，子类覆盖即生效）。
- `for_model`：`Model::kUm982` → `um982_receiver()`，其余 `*this`。
- `Um982Receiver`：同一 .cpp 内 `constinit` 静态实例；`name()` = `"unicore-um982"`；`caps` = 家族 UM980 能力 | `kHeading`；
  `for_model` 返回 `*this`。
- 删除不再使用的自由函数 `unicore::model_caps`（若测试或 main 引用，改为经驱动对象调用）。

### 1.3 Controller（`src/Controller.cpp`）
- `match_probe` 命中处：`detected_.receiver = &probe_receiver_->for_model(result.model);`
- `Controller.cpp:157` 一带发布 caps 用 `detected_.receiver`（为空时 0），不再用 `probe_receiver_`。
- 其余（rover_step、build_output、classify_reply、receiver()）本来就经 `detected_.receiver`，确认无遗漏。

### 1.4 u-blox / ComNav 同步整理（不建子类）
- 头文件里去掉 `final`；把各自 .cpp 匿名命名空间的命令生成/应答解析函数改为类的 `protected static` 成员
  （名字按协议操作取，如 u-blox `cmd_valset_msgout`、`resp_ack_class_id`；ComNav `cmd_log_ontime` 等）。
- 这两个头允许本卡修改，但只能：删除 `final`、新增 `protected:` 段的静态成员声明（每个带一行注释）。
- 逻辑逐字节不变。

### 1.5 测试（`test/gnss`）
- **黄金向量期望字节一个都不许改**（`test/gnss/golden/*.inc`）。若某处以 `unicore_receiver()` 直接按 `Model::kUm982`
  生成/判定 HEADING，改为经 `unicore_receiver().for_model(model)` 调用；期望不变。`GoldenGen.cpp` 同理改调用路径。
- 新增用例（放 `TestGnssUnicoreReceiver.cpp` 或新文件，记得登记 CMake）：
  1. `for_model(kUm980/kUm981/kUnknown)` 返回家族实例本身；`for_model(kUm982)` 返回 `um982_receiver()`，且后者
     `for_model` 返回自身，`family()` 仍为 `kUnicore`。
  2. 家族实例 `caps(kUm982)` 不含 `kHeading`、`output_mask(kUm982)` 不含 HEADING 位、`build_output(kHeading,...)` 返回
     `kNotSupported`；UM982 实例三者相反，且 HEADINGA 命令字节与黄金向量一致。
  3. Controller：喂 UM982 的 `#VERSIONA` 应答后 `detected().receiver == &um982_receiver()`、caps 含 kHeading；
     UM980 应答后为家族实例。
  4. u-blox/ComNav：`for_model` 任意型号返回自身。

### 1.6 注释与日志
- 新增/改动的每个函数按 CODING-STYLE §6.2 写 doxygen（英文标签、中文正文）；头里已有声明注释的，.cpp 定义处不重复。
- gnss 组件内本卡不加日志（日志接入在 Q1）。

## 2 不做
- 不改 `main/`（main 经 `detected().receiver` 已自动得到型号驱动；若编译需要改，先停下说明）。
- 不改 Kconfig、DriverList.inc、Registry（型号子类不进登记表）。
- 不新增 Um981Receiver；不加 UM982 定向/基线配置命令（无手册依据）。
- 不改 `NmeaParser.cpp`。

## 3 验收门禁（输出摘要贴进报告）
1. `test/gnss`：默认与全部驱动开启（以 CMakeLists option 为准）× 普通 / ASan+UBSan（`-fno-sanitize=vptr`）全部通过；黄金测试即逐字节判据。
2. `tools/main_host_check`、`tools/t13_host_check`、`test/services` 普通 + ASan 通过。
3. `. ~/esp/esp-idf/export.sh && idf.py -B builds/p3s build` 0 warning；记录 bin 大小与基线差值。
4. `sha256sum -c docs/frozen.sha256`：只允许 `Receiver.hpp`、`UnicoreReceiver.hpp`、`UbloxReceiver.hpp`、`ComnavReceiver.hpp`
   四个失败（Claude 验收时重算），其余全部 OK。
5. `clang-format --style=file --dry-run --Werror` 改动文件通过（bash 中展开列表）。
6. `python3 tools/doc_check/doc_check.py --base HEAD` 0 不合格。
7. `rg -n 'switch \(model\)' components/gnss/src/drivers` 的结果列入报告（Unicore 只应剩 `caps` 一处）。

## 4 交付
- 报告 `docs/reports/P3s.md`：改动清单、新增用例、门禁输出、bin 差值。
- 不要 commit，由 Claude 审查后提交。
