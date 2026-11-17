# P3s 记录：GNSS 型号子类（ADR-0012 §2.8）

日期：2026-11-17。基线 HEAD `5497187`（本任务未 commit、未切分支）。执行者：pi + 本任务模型。
任务卡：`docs/tasks/P3s-model-subclass.md`。

## 结论

**P3s PASS（软件门禁）**。`Receiver::for_model` 钩子落地：家族驱动默认返回自身，只有实现上真有差异的 UM982 换成型号驱动 `Um982Receiver`；控制器在探测命中后按型号换驱动，并按该驱动发布能力位。黄金向量文件一个字节未改，全部 host 测试、IDF 构建、堆门禁、冻结清单、格式与函数注释门禁通过。上板/串口验证 NOT_RUN（AGENTS.md §6 未授权）。

## 1. 改动文件

| 文件 | +/- | 内容 |
|---|---|---|
| `components/gnss/src/Controller.cpp` | +14/−3 | `enter_configuring` 改为从 `detected_.receiver` 取能力（:160）；`on_frame` 探测命中后 `detected_.receiver = &probe_receiver_->for_model(result.model)`（:730）；两处补 doxygen |
| `components/gnss/src/drivers/UnicoreReceiver.cpp` | +307/−312 | `cmd::*`/`resp::*` 匿名命名空间改为 `UnicoreReceiver` 的 protected 静态成员；`step_*` 成为头文件声明的静态成员（`always`/`port_baud_enabled` 删除）；`rover_script()` 返回函数内 `static constexpr std::array<ScriptStep,3>`；`rover_step` 遍历 `rover_script()`；HEADING 判定统一走 `caps(model) & cap::kHeading`；新增 `Um982Receiver` 实现与 `um982_receiver()`；删除自由函数 `unicore::model_caps` |
| `components/gnss/src/drivers/UbloxReceiver.cpp` | +194/−248 | 同上：`cmd::*`/`resp::*` 变 protected 静态成员，自由函数 `probe_cmd`/`probe_match`/`script_step`/`reply_ack`/`model_caps` 折叠进对应成员；脚本改为 `rover_step` 内的 `static constexpr std::array<ScriptStep,2>` |
| `components/gnss/src/drivers/ComnavReceiver.cpp` | +204/−238 | 同上（3 步脚本）；保留 `gnss::comnav` 只放解码器回调 `prompt_first_byte`/`prompt_step`/`explain_prompt` |
| `components/gnss/include/gnss/drivers/UbloxReceiver.hpp` | +24/−1 | 去 `final`；加 `protected:` 段 8 个静态成员声明 |
| `components/gnss/include/gnss/drivers/ComnavReceiver.hpp` | +25/−1 | 去 `final`；加 `protected:` 段 8 个静态成员声明 |
| `test/gnss/TestGnssUnicoreReceiver.cpp` | +149/−6 | 新增 3 个用例（见 §2）；`test_caps`/`test_build_output` 改走 `for_model` |
| `test/gnss/TestGnssClassify.cpp` | +2/−1 | `test_output_masks` 的 UM982 掩码改为经 `for_model` |
| `test/gnss/TestGnssUbloxReceiver.cpp` | +3 | `test_caps` 增 `for_model` 恒等断言 |
| `test/gnss/TestGnssComnavReceiver.cpp` | +3 | 同上 |
| `test/gnss/golden/GoldenCheck.hpp` | +10/−1 | `caps_mismatch` 先 `for_model(model)` 再取能力 |
| `test/gnss/golden/PlanCheck.hpp` | +13/−2 | `collect_plan` 用 `for_model(model)` 选驱动后跑脚本与输出命令 |

未改动：`main/`、`docs/**`（本报告除外）、`test/gnss/golden/*.inc` 全部黄金数据、Kconfig、`DriverList.inc`、`Registry.cpp`、`NmeaParser.cpp`、`components/gnss/include/gnss/Receiver.hpp` 与 `.../drivers/UnicoreReceiver.hpp`（T0 已写好的冻结头）。

## 2. 新增测试用例

| 用例 | 断言 |
|---|---|
| `test_for_model_splits_models` | `for_model(kUm980/kUm981/kUnknown/kZedF9p)` 返回家族实例自身；`for_model(kUm982)` 返回 `&um982_receiver()`，且它自己 `for_model` 任意型号返回自身、`family() == kUnicore`、`name() == "unicore-um982"`、解码器数量不变 |
| `test_controller_detects_model_driver` | 真实 `unicore_receiver()` + 缩短时序：喂入真实 `#VERSIONA,…;"UM982","R4.10Build11233",…` 后 `detected().receiver == &um982_receiver()`，下一次 `poll()` 写 `UNLOG COM2\r\n`、进入 `kConfiguring`、`detected().caps & kHeading != 0`；喂入 `"UM980"` 时 `detected().receiver == &unicore_receiver()` 且 `caps & kHeading == 0` |
| `test_golden_heading_through_model_driver` | 从 `GoldenUnicore.inc` 里取出基线脚本中的 `HEADINGA …` 原文，端口数字取自该原文，`for_model(kUm982).build_output(kHeading,…)` 与之逐字节相等（`TEST_ASSERT_EQUAL_UINT8_ARRAY`） |

`test_caps` 与 `test_build_output` 的 UM982 部分改为：家族驱动对 `kUm982` 无 `kHeading`、`output_mask` 无 HEADING 位、`build_output(kHeading,…) == kNotSupported`；`for_model(kUm982)` 之后三者都成立。

## 3. 门禁结果

| # | 命令 | 结果 |
|---|---|---|
| 1 | `cmake -S test/gnss -B builds/p3s-gnss -G Ninja` + build + `ctest` | PASS（0 告警，7/7） |
| 2 | `cmake -S test/gnss -B builds/p3s-gnss-all -DRTK_GNSS_DRV_COMNAV=ON` + build + `ctest` | PASS（0 告警，8/8，含 `test_gnss_comnav_receiver`） |
| 3 | 同上两份 + `-fsanitize=address,undefined -fno-sanitize=vptr`（`builds/p3s-gnss-asan`、`builds/p3s-gnss-all-asan`） | PASS（7/7、8/8，无 sanitizer 报告） |
| 4 | `test/host` 7、`test/app_fw` 8、`test/services` 6、`test/stream_demux` 4、`test/bt_reconnect` 1、`test/cli` 3、`tools/main_host_check` 44、`tools/t13_host_check` 43（普通构建） | PASS 全绿，0 告警 |
| 5 | `test/services`、`tools/main_host_check`、`tools/t13_host_check` 的 ASan/UBSan 构建 | PASS（6/6、44/44、43/43） |
| 6 | `. ~/esp/esp-idf/export.sh && idf.py -B builds/p3s build` | PASS，0 告警 |
| 7 | `idf.py -B builds/p3s size` | 总镜像 799 174 B；IRAM 86 187 B（65.76%）；DRAM 64 239 B（51.56%）。基线同法构建（`git archive HEAD` → `/tmp/p3s-base`）：798 942 B / 86 187 B / 64 239 B → **+232 B**，IRAM、DRAM 不变（只多一个型号驱动实例与其常量） |
| 8 | `idf.py -B builds/p3s-off -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;/tmp/rtk-trace-off.defaults"`（`CONFIG_RTK_TRACE_ENABLE=n`） | PASS，0 告警；总镜像 781 190 B，DRAM 63 823 B |
| 9 | `sha256sum -c docs/frozen.sha256` | 178 项 OK；FAILED 恰好 4 项，全部是允许的驱动头：`gnss/Receiver.hpp`、`gnss/drivers/UnicoreReceiver.hpp`（这两项在基线 HEAD 上就已 FAILED，属 T0 已写好的接口）、`gnss/drivers/UbloxReceiver.hpp`、`gnss/drivers/ComnavReceiver.hpp`（本卡 1.4） |
| 10 | `clang-format --style=file --dry-run --Werror`（本次改动的 12 个 C/C++ 文件） | PASS |
| 11 | `python3 -I tools/doc_check/doc_check.py --base HEAD` | PASS：改动函数 61 个，不合格 0 个 |
| 12 | `python3 -I tools/g7b_check/g7b_check.py --build builds/p3s` | PASS（G7b PASS，无新增堆分配） |
| 13 | `rg -n 'switch \(model\)' components/gnss/src/drivers` | 仅 1 处：`UnicoreReceiver.cpp:648`，位于 `caps(Model)` |

上板、串口、flash、monitor：**NOT_RUN**（AGENTS.md §6 禁止）。

## 4. 与卡片的偏离（已确认不影响契约）

1. **两个驱动头多出 5 行声明**。去 `final` 后 `-Wnon-virtual-dtor`（基类 `Receiver` 的析构是 protected 非虚）与 clang-tidy `cppcoreguidelines-special-member-functions` 报错；照冻结头 `UnicoreReceiver.hpp` 的既有写法补 `public: virtual ~X() = default;` 与 4 个 `= delete` 的拷贝/移动，并写中文注释说明原因。卡 1.4 的措辞只提「删 `final`、加 protected 静态声明」，这 5 行是让门禁通过的最小补充。
2. **u-blox/ComNav 的脚本步骤是 lambda，而非具名静态成员**。`ScriptStep::build` 是裸函数指针，匿名命名空间里的自由函数无法取 protected 成员地址，而 `ScriptStep` 类型的脚本数组只能放在成员作用域里；这两个头又不允许继续增加声明，因此步骤写成 `rover_step` 内 `static constexpr std::array<ScriptStep,N>` 的无捕获 lambda，直接调用 `cmd_*`。`UnicoreReceiver` 侧仍按冻结头保留具名 `step_*` 成员，所以两个驱动在这一点上形式不同 —— 行为与字节输出一致，且 u-blox/ComNav 的 `ScriptStep` 只在本文件内使用。
3. **测试与黄金比对器改为经 `for_model` 选驱动**（`GoldenCheck.hpp`、`PlanCheck.hpp`、`TestGnssClassify.cpp` 一处、`TestGnssUnicoreReceiver.cpp` 两处）：黄金 `.inc` 的期望字节与能力位数值完全未改，只是让「问哪个驱动」这一步符合 ADR-0012 §2.8（家族驱动对 `kUm982` 只报 UM980 能力）。

## 5. 未解决问题 / 遗留

- `test/gnss/TestGnssClassify.cpp`（第 40–144 行）与 `test/gnss/golden/GoldenCheck.hpp`（第 81–92 行）存在基线就有的 clang-tidy 风格告警（`reinterpret_cast`、C 数组、魔数），本次只改了 `for_model` 相关几行，未顺手整改，以保持 diff 聚焦；是否需要单开清理任务由架构侧决定。
- 卡片 §2 明确的非目标仍然如此：没有 `Um981Receiver`，没有 UM982 的 heading/baseline 配置命令，`DriverList.inc`/`Registry`/`main` 未动，因此 main 侧调用的仍是家族驱动入口（控制器内部已按型号换实例）。
- 本卡未在真实 UM982 上验证 `HEADINGA` 的实际回帧与双天线解算效果（无硬件授权）。
