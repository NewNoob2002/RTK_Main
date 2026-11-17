# S1：GNSS 驱动层次改为受保护析构 + final 叶子类（ADR-0013 §2.5，ADR-0012 §2.8）

状态：ISSUED 2026-11-17，基线 `feat/gnss` 当前 HEAD（T0 = 本卡所在提交）。
实施：omp + workbuddy/deepseek-v4.1-flash；审查/验收：Claude。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`（§4.1 类设计、§6.2 注释）、`docs/adr/ADR-0012-gnss-receiver-drivers.md`
§2.8（尤其最后一条“析构与叶子类”）、`docs/acceptance.md` G7b，再读 `components/gnss/**`、`test/gnss/**`、`tools/g7b_check/g7b_check.py`。

## 0 背景

P3s 为了让家族类既能派生又能直接实例化，给 `UnicoreReceiver`、`UbloxReceiver`、`ComnavReceiver` 加了公有虚析构。
这违反 CPP-011（多态基类受保护非虚析构、只实例化 final 类），还让编译器生成删除析构函数（`D0Ev`，含 `operator delete`），
G7b 只好开豁免。本卡把家族类改成纯基类，实例全部换成 final 叶子类，然后删掉 G7b 豁免。

## 1 要做

### 1.1 家族类（三个公共头，本卡授权修改，仅限下列改动）
`components/gnss/include/gnss/drivers/{UnicoreReceiver,UbloxReceiver,ComnavReceiver}.hpp`：
- 删除 `virtual ~X() = default;` 及其上方说明注释；默认构造改为 `protected:` 下的 `constexpr X() noexcept = default;`，
  并在 `protected:` 下加 `~X() = default;`（非虚）。拷贝/移动保持删除（可移到 protected 段，保持 delete）。
- 类注释说明“家族基类，只能经 final 叶子类实例化（ADR-0012 §2.8）”。
- 新增叶子类，声明放在各自家族头里、家族类之后：
  - `class Um980Receiver final : public UnicoreReceiver`（UM980/UM981 共用，不覆盖任何虚函数，只有 `public: constexpr Um980Receiver() noexcept = default;`）；
  - `Um982Receiver` 保持现有覆盖（`name`/`caps`/`for_model`），只补 `public` constexpr 默认构造（若需要）；
  - `class ZedF9pReceiver final : public UbloxReceiver`、`class K803Receiver final : public ComnavReceiver`，同 `Um980Receiver`。
- 访问器签名不变（`unicore_receiver()`、`um982_receiver()`、`ublox_receiver()`、`comnav_receiver()` 返回 `const Receiver &`）。
- 不改其他声明、不改 `Receiver.hpp`。

### 1.2 实例与访问器
- `components/gnss/src/DriverInstances.hpp` 与各驱动 .cpp：`detail::` 下的实例改为叶子类型
  （`kUm980Receiver`、`kUm982Receiver`、`kZedF9pReceiver`、`kK803Receiver`，`constinit const`），旧名删除。
- 访问器返回对应叶子实例：`unicore_receiver()` → `kUm980Receiver`，`ublox_receiver()` → `kZedF9pReceiver`，
  `comnav_receiver()` → `kK803Receiver`。
- `UnicoreReceiver::for_model` 逻辑不变（`kUm982` → `um982_receiver()`，其余 `*this`）；它由 `Um980Receiver` 继承，
  所以经 `unicore_receiver()` 调用的行为与现在完全一样。u-blox/ComNav 的 `for_model` 继续用基类默认（返回 `*this`）。
- `name()` 字符串一律不变（"unicore"、"unicore-um982"、u-blox 与 ComNav 现值）。

### 1.3 G7b 去豁免（`tools/g7b_check/g7b_check.py`）
- 删除删除析构函数豁免的全部代码与模块说明：`DTOR_TEXT_RE`、COMDAT 组收集函数（约 :172）、使用处（约 :242）、
  文件头 :17–23 的说明，以及输出里“豁免”列表。`operator delete` 与其他堆符号同样处理（未登记白名单即 FAIL）。
- 规则以 `docs/acceptance.md` G7b 为准（已由 Claude 改好）。

### 1.4 测试（`test/gnss`）
- 黄金向量期望字节一个都不许改（`test/gnss/golden/*.inc`）。
- 现有 `for_model` 用例（`TestGnssUnicoreReceiver.cpp:363-374`、`TestGnssUbloxReceiver.cpp:245-247`、`TestGnssComnavReceiver.cpp:166-168`）
  应不改即通过；若需改，只改调用路径，不改判据，并在报告说明。
- 新增一个编译期用例文件或在现有文件里加 `static_assert`：
  - `std::is_trivially_destructible_v` 对四个叶子类均为 true；
  - `!std::is_constructible_v<gnss::UnicoreReceiver>`（及 u-blox、ComNav 家族）——家族类不能直接实例化；
  - `std::is_final_v` 对四个叶子类为 true。

### 1.5 注释
- 新增/改动函数按 CODING-STYLE §6.2 写 doxygen；文件头不写阶段号。

## 2 不做
- 不改 `main/`、`Receiver.hpp`、`Registry.cpp`、`DriverList.inc`、Controller、Kconfig。
- 不改 `docs/**`（`docs/reports/` 除外）、`docs/frozen.sha256`（Claude 重算）。
- 不做 ADR-0013 的改名（S2）。

## 3 验收门禁（输出摘要贴进报告，逐项 PASS/FAIL/NOT_RUN/BLOCKED）
1. `test/gnss`：默认与全部驱动开启 × 普通 / ASan+UBSan（`-fno-sanitize=vptr`）全部通过。构建目录 `builds/s1-*`。
2. `tools/main_host_check`、`tools/t13_host_check`、`test/services` 普通 + ASan 通过。
3. `. ~/esp/esp-idf/export.sh && idf.py -B builds/s1 build` 0 warning；记录 bin 大小与基线（800336 B）差值。
4. `python3 -I tools/g7b_check/g7b_check.py --build builds/s1` PASS，且输出里没有任何豁免条目；
   `xtensa-esp32-elf-nm -C builds/s1/esp-idf/gnss/libgnss.a | rg 'D0Ev|operator delete'` 为空（贴输出）。
5. `python3 -I tools/tidy_check/tidy_check.py --host-build <s1 host 构建> --idf-build builds/s1 --gnss-build <s1 gnss 全驱动构建> --test-app-build builds/q4-rtk_pure-esp32` 0 条。
6. `test_apps/rtk_pure` esp32 与 esp32c3 构建成功（`builds/s1-rtk_pure-esp32`、`builds/s1-rtk_pure-c3`）。
7. `sha256sum -c docs/frozen.sha256`：只允许三个家族头失败，其余全部 OK。
8. `clang-format --style=file --dry-run --Werror` 改动文件通过（bash 中展开列表）。
9. `python3 -I tools/doc_check/doc_check.py --base <基线提交>` 0 不合格。

## 4 交付
- 报告 `docs/reports/S1.md`：改动清单、新增断言、门禁输出、bin 差值。
- 不要 commit，由 Claude 审查后提交。完成后在终端打印 `S1 DONE`。
