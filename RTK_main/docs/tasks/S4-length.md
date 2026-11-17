# S4：拆分过长的函数与文件（ADR-0013 §2.4）

状态：ISSUED 2026-11-17，基线 `feat/gnss` HEAD（T0 = 本卡所在提交，S3 已完成 `05bbc97`）。
实施：omp + workbuddy/deepseek-v4.1-flash，按批次；审查/验收/提交：Claude（每批一个提交）。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0013-readability.md` §2.4、`docs/acceptance.md`。

## 0 目标与度量

- 函数：≤ 60 行，clang-tidy `readability-function-cognitive-complexity` ≤ 25。
- 文件：≤ 500 行（`wc -l`）。以表为主的文件（解码表、命令表、文本表）可以超出，但要在报告里列出理由，
  由 Claude 决定是否登记到 `docs/acceptance.md` 的例外表。
- 度量只用 clang-tidy（门禁 G5b 的 `tools/tidy_check/tidy_check.py`）和 `wc -l`；不用 uvx / lizard（AGENTS §6）。
  每批开工前，用临时 `.clang-tidy` 覆盖（只放在 `builds/s4-*/` 下，或用 `--config`/`--checks` 参数）
  `readability-function-size.LineThreshold: 60` + `readability-function-cognitive-complexity.Threshold: 25`
  跑一次，得到本批的“超限清单”写进报告；结束后再跑一次，本批清单须为空。
- 基线参考（lizard 60 行/CCN 15，Claude 测）：46 个函数、14 个文件超限。
  超 500 行的文件：`main/modules/gnss/GnssModule.cpp` 1277、`components/gnss/src/NmeaParser.cpp` 1168、
  `components/gnss/src/drivers/UnicoreReceiver.cpp` 901、`components/gnss/src/Controller.cpp` 886、
  `main/modules/bt_link/BtLinkModule.cpp` 845、`components/spp_server/SppServerTx.c` 756、
  `components/gnss/src/drivers/UbloxReceiver.cpp` 651、`main/modules/gnss/GnssCmds.cpp` 648、
  `components/spp_server/SppServerInit.c` 612、`main/modules/debug/TraceText.cpp` 611、
  `main/modules/system/SystemCmds.cpp` 605、`components/app_fw/src/FanoutRing.cpp` 593、
  `components/cli/src/Console.cpp` 532、`main/modules/debug/DebugModule.cpp` 530。

## 1 批次

| 批 | 范围 |
|---|---|
| B1 | `components/` 中除 `gnss` 外的全部：app_fw、rtos、cli、stream_demux、device_proto、link_core、spp_server、nvs_store、bt_reconnect |
| B2 | `components/gnss/**`（含 ComNav/u-blox，必须在 test/gnss all-drivers 配置下验证） |
| B3 | `main/**`；最后把根 `.clang-tidy` 改为 `LineThreshold: 60` 并启用 `readability-function-cognitive-complexity`（Threshold 25），全仓 G5b 0 条 |

## 2 拆分规则

- 按职责拆成有名字的小函数（名字说明做什么），或把同一职责的一组函数移到新文件（同组件、同目录，
  命名延续现有文件名前缀，例如 `GnssModule.cpp` → `GnssModuleProbe.cpp`）。新文件要加进对应 CMakeLists。
- 不机械按行切段；不为凑行数引入 lambda、宏或 `goto`；不增加跨组件依赖；新辅助函数放匿名命名空间/`static`，
  只在多个 .cpp 共用时才放私有头（`src/` 或 `private_include/`），不进公共头。
- 行为完全不变：日志文本、控制台输出、协议字节、状态机次序、锁/临界区范围不变。临界区内的代码拆出时，
  锁仍在原函数持有，辅助函数注释写明“调用方持锁”。
- 新函数按项目规则写 Doxygen（`doc_check` 门禁）；C 文件保持 C 风格（snake_case）。
- 冻结头（`docs/frozen.sha256`）一律不改；若某拆分必须改冻结头，停下来在报告里列为待定。
- 不改 docs/adr、AGENTS.md、CODING-STYLE、acceptance；不重算 frozen；不提交；不用 uvx。

## 3 每批门禁（日志放 `builds/s4-logs/`，构建目录 `builds/s4-*`）

1. 10 个主机测试库普通 + ASan 全过（同 S3 报告 §5 的清单；B2 必须含 test/gnss all-drivers）。
2. `idf.py -B builds/s4 build` 0 warning；GNSS 关闭配置 0 warning；`test_apps/rtk_pure` esp32 + esp32c3、
   `test_apps/rtk_spp_peer` esp32 编译通过。
3. 黄金字节 `test/gnss/golden/*.inc` 空 diff；G7b PASS；clang-format 0 违规；
   G5b（现行配置）仍只剩 `SppServerTx.c:422` 一条（B1 结束后它也应消失）；`doc_check --base <本批起点>` 0 不合格。
4. 本批超限清单为空（60 行 / 认知复杂度 25 / 文件 500 行或已列理由）。
5. `rtk_main.bin` 与上一批比较，写出差值；增加超过 512 B 须说明原因。
6. `sha256sum -c docs/frozen.sha256` 全部 OK。

## 4 交付

报告 `docs/reports/S4.md`，每批追加一节：开工前超限清单、每项的拆分方式（原函数 → 新函数/新文件）、
门禁结果、bin 差值、例外文件及理由、遗留问题。每批完成后在终端打印 `S4-Bn DONE` 并停下，等待下一批指令。
