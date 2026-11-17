# S5：注释清理与 Doxygen 补齐（ADR-0013 §2.2、CODING-STYLE §6.2）

状态：ISSUED 2026-11-17，基线 `feat/gnss` HEAD（T0 = 本卡所在提交；S4 已完成 `d97e9b2`）。
实施：omp + workbuddy/deepseek-v4.1-flash，按批次；审查/验收/提交：Claude（每批一个提交）。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md` §2/§6、`docs/adr/ADR-0013-readability.md` §2.2、`docs/acceptance.md`。

## 0 目标与度量（全部只改注释，代码 token 不变）

| 项 | 起点（Claude 测，T0） | 终点 |
|---|---|---|
| Doxygen 覆盖 `python3 -I tools/doc_check/doc_check.py --report` | 1342 个函数，526 合格（39%） | 每个组件 100% |
| 代码注释里的阶段号/任务号（`P3b`、`Q2`、`R2`、`S4-B3`、`T13`、`K6`…） | 约 82 处 | 0 处；需要追溯写 ADR 节号 |
| 文件头注释 | 多处超过 3 行 | ≤ 3 行：做什么；线程/ISR 约束；堆与阻塞约束 |
| `NOLINT` | 72 处 | 只剩 C ABI、宏边界和 §2.1（S4）保留函数；每处有理由；同一检查多处抑制的改目录级 `.clang-tidy` |

起点覆盖率（按组件）：app_fw 34%、bt_reconnect 0%、cli 20%、device_proto 1%、gnss 84%、link_core 1%、
nvs_store 20%、rtos 7%、spp_server 58%、stream_demux 6%、main 43%。

## 1 批次

| 批 | 范围 |
|---|---|
| B1 | app_fw、rtos、cli、nvs_store、bt_reconnect |
| B2 | link_core、device_proto、stream_demux、spp_server |
| B3 | gnss（含 ComNav/u-blox 源文件） |
| B4 | main/**；全仓终检（--report 合计 100%、阶段号 0 处、NOLINT 清单）；顺带下列遗留 |

B4 遗留项（来自 S2–S4 验收）：
- S4 拆分产生的“旧单行注释 + 新 Doxygen”重复注释块（如 `SppServerTx.c`、`SppServerRx.c`；B2 遇到的就地处理）。
- `components/gnss/src/drivers/UbloxReceiver.cpp`、`UbloxUbx.cpp` 顶部约 30 行 `using ublox::kX;`：改为在使用处所在的
  `gnss::ublox` 命名空间内直接引用（把用到常量的匿名命名空间函数放进 `namespace gnss::ublox { namespace { ... } }`），
  或统一写 `ublox::kX`。只动这两处，B3 做。
- `main/modules/storage/StoreCatalog.cpp` debug.hex 帮助文本里的过时内部名 `(tlog_hex)` 删去（这是控制台输出，唯一允许的输出文本改动）。

## 2 规则

- **只改注释**（含 `NOLINT` 注释本身）。代码、字符串、日志文本、控制台输出不变（上面的 `tlog_hex` 是唯一例外）。
  例外：删除某处 `NOLINT` 后 tidy 报警，若改写代码能消除且行为不变，可以改，报告里逐条列出。
- Doxygen 格式、位置、豁免按 CODING-STYLE §6.2：公开函数写在头文件声明处，`.cpp` 定义处不重复；只在 `.cpp` 出现的写在定义处。
  内容必须对照代码写真话：线程/任务、ISR、阻塞、持锁、启动期限制写在 `@note`；`@retval` 列出每种返回结果；
  不确定的地方读调用方确认，仍不确定就写进报告“疑问”一节，不要编。
- 冻结头文件（`docs/frozen.sha256` 列出的 `components/**/include/**`、`main/def/GnssDef.hpp`）**本卡授权只改注释**，
  Claude 用去注释 token 比对验收并重算哈希；omp 不重算 frozen。
- 实现内注释只写理由、约束、副作用；删掉复述代码的注释和注释掉的代码。
- 阶段号替换：有设计依据的改成 ADR 节号；纯历史的直接删（历史在 git 里）。`docs/` 下的文档不在本卡范围。
- 不用 uvx / lizard；不提交；不重算 frozen；不烧录。

## 3 门禁（每批）

- `python3 -I tools/doc_check/doc_check.py --report`：本批组件 100%；`--base <上一批提交>` 0 不合格。
- 阶段号扫描（本批范围）：`rg -n '\b(P[0-9]+[a-z]?|Q[0-9]+|R[0-9]+|S[0-9]+(-B[0-9]+)?|T[0-9]+[a-z]?|K[0-9]+|J[0-9]+[a-z]?|L[0-9]+[a-z]?)\b'`
  只看注释，结果为空或逐条说明不是阶段号（如寄存器名、协议字段）。
- clang-format 0；G5b tidy 全仓 0 条（命令见 `docs/reports/S4.md`）。
- `idf.py -B builds/s5-bN build` 0 warning，bin 大小与 S4（800608 B）**完全相同**（只改注释）；
  B3 额外跑 test/gnss 默认与 all-drivers；B4 跑全部主机套件（main_host_check、t13_host_check、test/services、test/app_fw、test/host）。
- frozen：只允许本批范围内的冻结头因注释改动而失败，报告列出。

## 4 报告

`docs/reports/S5.md`，每批一节：起止覆盖率、阶段号处理表（文件:行 → 改成什么/删除）、NOLINT 处理表（删除/保留+理由/改目录配置）、
文件头改动清单、例外（代码改动）、疑问。完成后打印 `S5-Bn DONE` 并停下等 Claude 验收。
