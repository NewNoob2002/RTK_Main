# ADR-0013：可读性整改：命名、间接层、长度、注释

状态：已批准（2026-11-17，用户批准；§2.1 文件作用域可变静态保留 `s_snake_case`，见下表）。§2.5 已由 S1 实施（a25b636）
关联：ADR-0008（代码组织）、ADR-0012 §2.8（驱动层次）、`docs/CODING-STYLE.md`、技能 embedded-engineering v0.1
（`references/cpp/readable-cpp.md`，规则 CPPF-001/CPPF-002/DOC-001/CPP-011）。

## 1 背景

现行规则本身规范，门禁也齐全，但用户反馈（2026-11-17）人工审查和排查问题时读起来绕、理不清思路。
用户确认以下四类问题都要整改：命名风格、注释噪声、间接层过多、函数与文件过长。

2026-11-17 的现状数据（不含 `_template/`，共 184 个自有源文件，约 3.2 万行）：

| 项 | 数据 | 来源 |
| --- | --- | --- |
| Doxygen 覆盖 | 1293 个函数中 315 个合格（24%） | `tools/doc_check --report` |
| 超长或过复杂的函数（>60 行或圈复杂度 >15） | 42 个，最长 `spp_server_worker_submit` 99 行，最复杂 `rtos::Executor::init` CCN 30 | `uvx lizard -L 60 -C 15` |
| 超过 500 行的文件 | 14 个，最长 `main/modules/gnss/GnssModule.cpp` 1253 行、`components/gnss/src/NmeaParser.cpp` 1159 行 | `wc -l` |
| 代码注释里的阶段号（P3b、Q2…） | 21 处 | `rg` |
| `NOLINT` | 58 处 | `rg` |
| X-macro 清单 | 2 份：`main/module_list.inc`（展开 4 处），`components/gnss/include/gnss/DriverList.inc`（经访问器 + `detail::kX` + `DriverInstances.hpp`） | `rg` |
| CPP-011 违规 | `UnicoreReceiver`/`UbloxReceiver`/`ComnavReceiver` 用公有虚析构且被直接实例化 | Q4 前审查 |

## 2 决定

### 2.1 命名：C++ 改用 readable-cpp v1，C 保持 snake_case

| 对象 | 现在 | 改为 | 例 |
| --- | --- | --- | --- |
| 类型、概念、模板参数 | `PascalCase` | 不变 | `LinkManager` |
| 函数、方法 | `snake_case` | `lowerCamelCase` | `poll_controller` → `pollController` |
| 局部变量、参数 | `snake_case` | `lowerCamelCase` | `now_ms` → `nowMs` |
| 私有/受保护成员 | `name_` | `lowerCamelCase_` | `last_tick_ms_` → `lastTickMs_` |
| 公有数据成员（struct 字段） | `snake_case` | `lowerCamelCase` | `rx_period_ms` → `rxPeriodMs` |
| 文件作用域可变静态 | `s_snake_case` | 不变（用户 2026-11-17 决定：保留 `s_` + snake，与 C 侧一致，也和常量 `k` 区分） | `s_link_state` |
| 命名空间作用域只读对象（`const`/`constinit const`） | `kName`（部分靠 `NOLINT`） | `kName`，由 clang-tidy GlobalConstant 规则覆盖，删掉对应 `NOLINT` | `kReceivers` |
| 常量、枚举值 | `kName` | 不变 | `kRetryMs` |
| 命名空间 | `lower_snake` | 不变 | `core::services` |
| 宏 | `UPPER_CASE` | 不变 | `FW_LOGI` |
| 文件名 | `PascalCase` | 不变 | `GnssModule.cpp` |

- 缩写按单词处理：`parseGga`、`rtcmMsg`、`NmeaParser`，不写 `parseGGA`。单位后缀保留：`nowMs`、`timeoutMs`、`lenBytes`。
- **不改的范围**：C 源文件（`.c`）、C 接口头（`.h`）、`extern "C"` 声明，以及 IDF 规定的名字（`app_main`）。
  这些沿用 IDF/BARR-C 的 snake_case（CODING-STYLE §8），在 C++ 翻译单元里触发命名告警时，在声明处写带理由的
  `NOLINTNEXTLINE(readability-identifier-naming)`，不整目录关闭检查。
- 列宽：自有 C/C++ 统一由 120 改为 100（readable-cpp 目标值）；不可断开的字符串可以超出。
- 迁移工具：`.clang-tidy` 改为新的命名配置后，用真实编译库跑 `clang-tidy --fix`：host 构建覆盖纯组件和 gnss（全部驱动），
  IDF 构建覆盖只在目标上编译的文件；宏参数、X-macro 展开处和模板依赖名由人工补改。
  改完后报告里的历史函数名不改；现行 ADR 与 CODING-STYLE 里的名字同步改。

理由：
- 函数用 `lowerCamelCase`、类型用 `PascalCase`，看到 `pollController` 就知道是操作，`Controller` 是类型，
  不用再靠下划线位置来分辨。
- 技能 readable-cpp v1 已给出 clang-format/clang-tidy 配置和门禁（style_gate），工具可以直接复用。
- 现在的 `_` 成员后缀、`kName` 常量、`s_` 文件静态、`PascalCase` 文件名全部保留，迁移只改函数、方法、变量、参数和成员这几类，属于机械改名。

### 2.2 注释：只写理由和约束

- 代码注释不写阶段号与任务号（`P3b`、`Q2`、`R2`…）。演进历史看 git 与 ADR；需要追溯时写 ADR 节号（`ADR-0012 §2.8`）。
- 文件头不超过 3 行：做什么；线程/ISR 约束；堆与阻塞约束（CODING-STYLE §2）。
- `NOLINT` 只用于确实无法改写的地方，每处写理由。同一检查在多个文件都要抑制时，改配置（目录级 `.clang-tidy` 带注释），
  不在每处写 `NOLINT`。目标是把现有 58 处降到只剩 C ABI 和宏边界。
- Doxygen 规则不变（CODING-STYLE §6.2）。补齐放在最后一步（§3 S5），因为改名、拆分函数都会改动签名和参数名。

### 2.3 间接层：从调用点到实现最多两跳

- 读者从调用点出发，最多两跳（调用点 → 接口或分派表 → 实现）就要看到实现。只做转发的一行包装函数要删掉，
  除非它是 C ABI 边界或测试接缝，并且注释里写明是哪一种。
- X-macro 只在同一份清单要生成两种及以上产物时使用。展开点要在清单文件头列全，读者从清单出发能找到所有展开处。
  - `main/module_list.inc` 保留：一份清单要生成计数、成员、绑定、执行器表 4 种产物。按上面的要求补全展开点。
  - GNSS 驱动的 `DriverList.inc` 改为 `Registry.cpp` 里的一张 `constinit` 表：直接取各驱动叶子实例的地址，按
    `#if CONFIG_...` 选择编进哪些驱动。表、实例、访问器原来分散在 4 个文件，这样只剩表加实例 2 处。
    测试中依赖访问器的地方改成通过注册表查询。
- 其余间接层（模板跳板 `make_ops`、`Slot`、`FanoutRing`）是有意的设计，不在本 ADR 范围内。S3 的审计如果发现
  新的只做转发的包装函数，列入清单逐项处理。

### 2.4 长度：函数不超过 60 行、复杂度不超过 15；文件不超过 500 行

- 函数：不超过 60 行，圈复杂度不超过 15。门禁用 clang-tidy 的 `readability-function-size`（LineThreshold 60）
  和 `readability-function-cognitive-complexity`（Threshold 25），不新增工具依赖。
- 文件：不超过 500 行，门禁写进 `docs/acceptance.md`。确实以表为主的文件（解码表、命令表）可以超出，但要在
  `docs/acceptance.md` 按文件登记理由。
- 拆分的方式：按职责拆成具名的小函数，或者把同一职责的函数移到新文件。不机械地按行数切段，
  也不为了凑行数引入 lambda 或宏。

### 2.5 驱动层次的析构函数（CPP-011，用户已批准，先于 S2 做）

- `UnicoreReceiver`、`UbloxReceiver`、`ComnavReceiver` 是可以派生的家族类，析构函数改成受保护、非虚。
- 每个型号只实例化 `final` 叶子类：新增 `Um980Receiver final : UnicoreReceiver`（UM980/UM981 共用）、
  `ZedF9pReceiver final : UbloxReceiver`、`K803Receiver final : ComnavReceiver`；`Um982Receiver` 保持现状。
- `for_model` 返回叶子实例。
- G7b 门禁里的删除析构函数豁免（`.text.*D0Ev`）删掉，恢复为“自有目标文件不得引用 `operator delete`”。

## 3 实施顺序（每张卡由 omp + deepseek 实施，Claude 验收）

| 卡 | 内容 | 验收要点 |
| --- | --- | --- |
| S1 | §2.5 驱动析构 | gnss 全部驱动测试 normal+ASan、黄金字节不变；IDF 构建 0 告警；G7b 不再需要 D0Ev 豁免 |
| S2 | §2.1 命名 + 100 列；按层分批：app_fw/rtos/cli → stream_demux/device_proto/link_core/bt_reconnect → gnss → main/test | 所有 host 测试；IDF 构建 0 告警；黄金字节不变；`.text` 大小与改名前一致（改名不改代码）；clang-tidy 命名 0 告警；frozen 由 Claude 重算 |
| S3 | §2.3 间接层：先出转发包装清单，再按决定改 GNSS 驱动表 | 同上，另加 HIL：`gnss status`、`gnss probe` |
| S4 | §2.4 拆分 42 个函数和 14 个文件，打开函数长度/复杂度门禁 | tidy 0 告警；文件行数门禁；行为测试不变 |
| S5 | §2.2 注释清理与 Doxygen 补齐，按组件分批 | `doc_check --report` 合计 100%；阶段号 0 处；NOLINT 清单与理由 |

S2 期间冻结其他功能开发，避免大规模改名和功能分支冲突。每张卡完成后，单独更新 CODING-STYLE 对应章节和 §10 状态。

## 4 后果

- 优点：
  - 函数名和类型名一眼就能分开。
  - 调用链短了，排查时跳转次数少。
  - 函数和文件都有上限，每块代码能单独读懂。
  - 技能的 style_gate 能直接使用。
- 代价：
  - S2 一次触及几乎所有 C++ 文件，`git blame` 会断开。缓解办法：每批改名单独提交，提交信息带 `rename-only`，
    并把这些提交登记到 `.git-blame-ignore-revs`。
  - 现行 ADR 文本要随着改名同步更新。
- 不变：
  - 架构、依赖表、堆分级、并发契约、日志格式。
  - C 代码风格。
