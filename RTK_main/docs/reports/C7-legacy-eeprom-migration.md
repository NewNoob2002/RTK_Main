# C7 · 旧 EEPROM 的 SN/PN 迁移

实现分支为 `feat/c7-migration`，门禁与资源基线为 `fec6ce8`。验证平台为 ESP32 / ESP-IDF v6.1，应用代码按 C++20 T2。迁移范围以已在本分支的 `5f04f9f`（2026-11-26 用户决定）为准。[ADR-0022 §2.4、§2.5、§4](../adr/ADR-0022-legacy-partition-migration.md)

原任务卡还要求迁移星座、NMEA 频率、截止角、SBAS、JSON 服务器、ThingsBoard，并记录两个手机帧的布局。ADR 修订取消了这些项目及对应测试。升级后这些功能沿用新固件已有配置或默认值，需要调整时由用户在 nav App 重新配置。V3.1 长度分支仍以合成样本验证。[原 C7 任务卡](../tasks/C7-legacy-eeprom-migration.md)

## 改动与迁移流程

| 路径 | 改动 |
| --- | --- |
| `main/modules/storage/LegacyMigration.{hpp,cpp}` | 沿用启动迁移和提交框架，只从 EEPROM 解析 SN/PN。删除此前的 `LegacyEeprom.{hpp,cpp}`、其余字段的地址、编码和测试依赖。 |
| `main/app/BootFlow.cpp`、`components/nvs_store/{NvsStore.c,boot_include/NvsStoreBoot.h}` | `stepEarlyConfig` 注册一次性钩子。两个实例就绪且 C6 已比对身份副本后、读取用户配置前运行。迁移复用 C6 的身份键名、命名空间、双写、读取和启动缓冲。 |
| `test/nvs_store/` | 删除已取消字段的用例与替身接口，保留并扩展身份迁移及重试覆盖。 |
| `docs/PROGRESS.md`、本报告 | 同步范围、门禁基线和资源测量。 |

已有 `mig/done` 时直接跳过。系统 NVS 缺少 `eeprom/eeprom` 时提交 `none`，长度既非 2280 B 也非 2560 B 时提交 `bad_len`。这三条路径无需借用缓冲。有效长度使用 C6 启动缓冲读取全 blob。任一身份目标键已存在时保留该身份。新值写入两实例并逐份读回，随后提交和校验 `mig/src_len`（u16）与 `mig/stats`（迁移、跳过、非法，各 1 字节），最后提交 `mig/done = v1`。中途失败由下一次启动重试。旧 blob 始终以只读方式访问。`freset` 保留身份、`mig` 和旧 blob，后续启动跳过迁移。

**重试计数口径**。日志和 `mig/stats` 记录最终提交标记那一轮的字段处理结果。一轮中未设置和已有目标键均计入 `skipped`。例如首轮写入 SN 后 PN 写失败，次轮正常完成会记录 `1 migrated / 1 skipped / 0 invalid`，累计实际导入了两个字段。保留这一本轮口径，因为已有键也可能来自产线或用户修改，现有键值无法证明它是上一轮迁移写入的。测试还在两轮之间更新 SN，确认重试保留新值。无故障的 V2.7 金样期望为 `2 / 0 / 0`。

## 字段映射

| 旧字段 | 偏移 / 宽度（B） | 目标键 | 校验与编码 | 覆盖 |
| --- | --- | --- | --- | --- |
| SN | 200 / 16 | `fac/sn`、`facbk/sn` | 复用 `factory.sn` 的 Registry 校验。有效文本为 1–16 字节、ASCII 0x21–0x7E，保存为 NVS 字符串。 | V2.7 真机、V3.1 合成、满宽和非法值 |
| PN | 216 / 16 | `fac/pn`、`facbk/pn` | 按 C6 的定长接口接受可打印 ASCII，原文保留到首个 NUL，末尾补零至 16 字节。 | V2.7 真机、V3.1 合成、满宽和非法值 |

两版共用只读偏移表。空串、全擦除字节、包含 ASCII 控制字符或 DEL 的文本按未设置处理。

## 资源与启动测量

| ESP32 默认构建 / B | `fec6ce8` | SN/PN 版 C7 | 对基线增量 |
| --- | ---: | ---: | ---: |
| 静态 DRAM | 98,619 | 98,627 | +8 |
| IRAM | 116,139 | 116,139 | 0 |
| Flash code | 1,143,244 | 1,144,724 | +1,480 |
| Flash data | 275,580 | 275,820 | +240 |
| `rtk_main.bin` | 1,588,400 | 1,590,112 | +1,712 |

`idf.py size` 对照同一工具链重建的基线，日志见 `builds/c7-logs/{baseline,current}.size.log`。相比返工前的 `def6761`，镜像减少 2,576 B，每槽剩余 375,968 B。G7b 通过。自有运行期代码新增堆分配为 0，首启迁移临时借用 2560 B 的 C6 BootArena scratch，回调返回后释放。

Xtensa `-fstack-usage -fcallgraph-info=su` 测得 scratch 提供者 928 B、迁移入口 64 B、迁移回调 80 B、字段处理 288 B。从 `app_main` 到 SN 的 Registry 校验入口，已统计栈帧合计 2304 B，记录见 `builds/c7-fix-stack/*.su`。此小计未计入 SDK 任务入口和库内部栈。主任务配置栈为 3584 B，设备高水位 **NOT_RUN**。

Linux MinSizeRel 构建用内存 NVS 与 V2.7 金样执行 2500 次对照。初始化对照与首次迁移的中位时间分别为 149 ns、738 ns（+589 ns），后续初始化对照与命中 `done` 分别为 152 ns、173 ns（+21 ns）。日志见 `builds/c7-logs/host-timings.log`。设备 `earlyConfig.tookMs` 的首次与后续增量 **NOT_RUN**。

## 测试结果

| 检查 | 结果 |
| --- | --- |
| `tools/ci/run_gates.sh --prefix c7 --base fec6ce8` | **PASS 43/43**。使用指定的 `IDF_PATH` 和 `IDF_PYTHON_ENV_PATH`，启用 ASan/UBSan 与 LeakSanitizer。门禁日志为 `builds/c7-logs/summary.log`。 |
| G2/G2b `test/nvs_store` | 普通与 ASan/UBSan 各 **PASS 29/29**，含 13 个 C6 和 16 个 C7 用例。覆盖新板、异常长度、全未设置、金样、2560 B 合成版本、SN/PN 满宽和非法值、同类型和异类型已有键、写入和读回故障、标记提交失败、缓冲不足、重启幂等、旧 blob 保留和 `freset`。 |
| 修复前后对照 | `c81f172` 无迁移入口。返工前的 `def6761` 对 V2.7 SN/PN 金样记录了 `13 / 8 / 0`，新测试验证只统计 SN/PN 时为 `2 / 0 / 0`。 |
| TSan `tools/t13_host_check` | 初版基线和 C7 均为 **45 PASS / 1 FAIL（既有）**，仅 `t13_init_start_timeout` 报 `s_spp` 竞态。返工未涉及该套件及生产依赖。 |
| V3.1 真机金样、V3.1 bootloader 冒烟、20 KB NVS 整区端到端解析 | **NOT_RUN**。V3.1 使用扩展至 2560 B 的合成样本，V2.7 使用任务提供的 2280 B 真机 blob，均通过系统 NVS 替身验证。 |

## HIL 由 Architect 与用户执行

| ADR-0022 §4 | 本卡结果 |
| --- | --- |
| 1. 准备 Arduino V2.7 旧 bootloader、分区表及应用，在旧 App 写 SN/PN。 | **NOT_RUN** |
| 2. 读出系统 NVS 整区，对照 SN/PN。 | **NOT_RUN** |
| 3. 旧网页 OTA 后确认引导、迁移计数、身份双副本和蓝牙绑定。 | **NOT_RUN** |
| 4. 新网页 OTA 和切回旧槽，验证 otadata、旧配置和系统 NVS 保留。 | **NOT_RUN** |
| 5. `freset` 后核对用户配置回默认、身份和迁移状态保留，并确认启动跳过迁移。 | **NOT_RUN**，主机回归已覆盖。 |
