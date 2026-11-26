# C7 · 旧 EEPROM 配置迁移

实现分支为 `feat/c7-migration`，门禁和资源基线为发令指定的 `fec6ce8`。验证档位为 ESP32 / ESP-IDF v6.1，C++20 T2。依据 [C7 任务卡](../tasks/C7-legacy-eeprom-migration.md) 和 [ADR-0022 §2.4、§2.5、§4](../adr/ADR-0022-legacy-partition-migration.md)。

## 改动清单

| 路径 | 改动 |
| --- | --- |
| `main/modules/storage/LegacyEeprom.{hpp,cpp}` | 纯 C++ 解码器。V2.7 和 V3.1 共用只读地址表。选在应用存储层，以便共用 Settings Registry、GNSS 输出编码和手机兼容帧类型。 |
| `main/modules/storage/LegacyMigration.{hpp,cpp}` | 检查旧键和完成标记，借用启动缓冲，按目标键提交并读回，最后写 `mig/done`。现有目标键优先，键的存在性检查与其 NVS 类型无关。 |
| `components/nvs_store/{NvsStore.c,CMakeLists.txt,boot_include/NvsStoreBoot.h}`、`main/app/BootFlow.cpp` | 在 `stepEarlyConfig` 注册一次性钩子。两个实例均初始化成功且 C6 已比对身份副本后运行，早于启动计数及用户配置读取。迁移用既有 C6 scratch 提供者和系统 EEPROM 只读路径。 |
| `test/nvs_store/` | 在既有 G2/G2b 套件加入真实迁移路径的 C++ 测试和定向 NVS 故障注入。 |
| `docs/PROGRESS.md`、本报告 | 记录实现与验证状态。G1 已验证冻结清单，包括既有 `NvsStore.h`。 |

有效长度为 2280 或 2560 B。源缺失与长度异常分别提交 `none`、`bad_len`，两者和已有 `done` 路径均无需借缓冲。迁移完成写 `mig/src_len`（u16）和 `mig/stats`（按迁移、跳过、非法排列的 3 个 u8），验证后提交 `mig/done = v1`。统计以旧字段为单位，SN/PN 的两个副本各合计为一个源字段。同一目标 blob 内的有效字段一起写入或跳过。频率取整同时计迁移和非法，遇已有目标键时则同时计跳过和非法。中途失败保留本轮成功提交的键供下次启动跳过，因此重试后的统计反映完成标记提交当轮的处理结果。系统 NVS 的 `eeprom/eeprom` 保留原始字节。

## 字段映射

| 旧字段 | 偏移 / 宽度（B） | 目标 | 校验与编码 | 覆盖 |
| --- | --- | --- | --- | --- |
| SN | 200 / 16 | `fac/sn`、`facbk/sn` | `factory.sn` 的 Registry 校验，1–16 字节、ASCII 0x21–0x7E，C6 双写并分别回读。 | V2.7 真机金样、合成边界 |
| PN | 216 / 16 | `fac/pn`、`facbk/pn` | C6 接受的可打印 ASCII，原文保留，NUL 后零填充成 16 字节。 | V2.7 真机金样、合成边界 |
| GPS / BDS / GLO / GAL | 300–303 / 各 1 | `gnss.const.*` → `rtk/sat_cfg` | 接受 0/1，其余值按未设置。按现有 6 B 编码和 Registry 校验。 | 合成覆盖，金样为旧默认 |
| GGA / GSA / GSV / GST / RMC | 304–308 / 各 1 | `gnss.out.*` → `gnss/out` | 旧有符号字节。`0xFF` 为未设置，0 与其他负值关闭。正值选最接近 `1000/Hz` 的可编码且与定位周期整除的正周期，同距取较长值。GSA/GSV 最短正周期为 1000 ms，GSV 用固件现有 SATSINFO/GSV 共享周期。 | 合成覆盖，金样为旧默认 |
| 截止角 | 310 / 4，int32 LE | `gnss.cutoff` → `rtk/sat_cfg` | 1–90 度。≤0 按未设置，沿用默认 15 度。>90 计非法。 | 合成覆盖 |
| SBAS | 314 / 1 | `rtk/sat_cfg` 最后一字节 | 接受 0/1，未设置沿用默认关。启动迁移早于接收机探测，先保存旧设置。探测后沿用 Survey 0x06 的能力掩码，F9P 无 SBAS 支持时查询值清零，重放跳过 SBAS 命令。 | 合成覆盖，既有 F9P HPG 1.12/1.13+ 主机回归 |
| JSON 地址、端口 | 320 / 32、352 / 4 | `rtk/json_srv` | 保留有值的旧地址及 4 B LE 端口，按下表零填充未设置字节。 | 合成覆盖 |
| TB 开关、client ID、用户、密码、主机、端口 | 356 / 1、360 / 16、376 / 16、392 / 16、408 / 32、440 / 4 | `rtk/tb_cfg` | 开关接受 0/1，字符串和端口原始字段按下表定位。 | 合成覆盖，金样为旧默认 |

字符串读取到字段宽度或首个 NUL。空串、全擦除字节、包含 ASCII 控制字符或 DEL 的文本按未设置。合成载荷保留完整的 16/32 字节定长文本边界。端口的 `0xFFFFFFFF` 是未设置，其余值原样保留 4 B，包括零和最高位，符合预留帧的原始数据语义。

固件实际将星座、截止角和 SBAS 共存于 `rtk/sat_cfg`，顺序是截止角、GPS、GLO、BDS、GAL、SBAS。任务卡的 0x24 在现有手机代码中对应天线高，卫星设置使用 Survey 0x06。输出共存于 8 B `gnss/out`，保留默认 HEADINGA，并使用现有 `periodMsToInterval` 与 `GnssOutConfig` 编码。对尚未探测到的接收机，按已保存的合法 `gnss/rate` 或固件缺省值对齐，具体型号的周期收敛由已有 GNSS 启动路径处理。

## JSON / ThingsBoard 手机载荷

V2.7 `meaasge.ino:497–566` 与 V3.1 `main/user_src/user_bt_message.c:559–628` 的 Nav 布局相同。V3.1 `main/user_src/user_eeprom.c:62–71,194–201` 确认了持久化字段。现行 `PhoneCompatService.cpp` 令 JSON SET 至少带 38 B，复制到零初始化的 42 B。ThingsBoard SET 复制完整的 86 B。两个 QUERY 返回相同定长的存储载荷。

| 帧 | 载荷偏移 / 宽度（B） | 字段 | 宽度处理 |
| --- | --- | --- | --- |
| 0x32 / 42 B | 0 / 32、32 / 4 | 地址、LE 端口 | 等宽。地址首个 NUL 后填零，端口逐字节保留。 |
| 0x32 / 42 B | 36 / 1、37 / 1、38 / 4 | enable、status、保留 | EEPROM 没有对应持久化字段，全部填零。 |
| 0x33 / 86 B | 0 / 1、1 / 1 | enable、status | enable 等宽，旧运行状态 status 填零。 |
| 0x33 / 86 B | 2 / 16、18 / 16、34 / 16、50 / 32、82 / 4 | client ID、用户、密码、主机、LE 端口 | 等宽。文本首个 NUL 后填零，满宽文本保留全部字节，端口逐字节保留。 |

持久化源字段与帧内字段等宽，无截断。某帧完全未设置时省略该 NVS 键。两帧仍为 ADR-0021 的存储预留，APN、NTRIP、围栏由后续卡迁移。

## 资源和启动测量

| ESP32 默认构建 / B | `fec6ce8` | C7 | 增量 |
| --- | ---: | ---: | ---: |
| 静态 DRAM | 98,619 | 98,627 | +8 |
| IRAM | 116,139 | 116,139 | 0 |
| Flash code | 1,143,244 | 1,146,988 | +3,744 |
| Flash data | 275,580 | 276,124 | +544 |
| `rtk_main.bin` | 1,588,400 | 1,592,688 | +4,288 |

每槽剩余 373,392 B。基线从 `fec6ce8` 归档到 C7 的构建目录并用相同工具链重建，大小日志为 `builds/c7-logs/{baseline,current}.size.log`。G7b 通过，自有运行期代码新增堆分配为 0。迁移首启临时借用 2560 B 的 C6 BootArena scratch，回调返回后释放。

实际 Xtensa 编译参数增加 `-fstack-usage -fcallgraph-info=su` 后，`withNvsBootScratch` 为 928 B，迁移入口 64 B，迁移回调 272 B，纯解码 592 B。由 `app_main` 至卫星 Registry 校验入口的已统计栈帧合计 2800 B（`builds/c7-stack/*.su`），此小计不含 SDK 任务入口或库内部。主任务配置栈为 3584 B。设备栈高水位为 **NOT_RUN**。

Linux 主机用内存 NVS 和 2500 次样本测量 init 到迁移返回，首次对照中位 180 ns、执行迁移 1539 ns（+1359 ns），后续对照 161 ns、命中 done 163 ns（+2 ns）。构建为 MinSizeRel，结果在 `builds/c7-logs/host-timings.log`。设备 Flash I/O 和日志时延需以同一设备的基线、首次迁移和后续重启 `earlyConfig.tookMs` 对照，设备时间增量为 **NOT_RUN**。

## 测试结果

| 检查 | 结果 |
| --- | --- |
| `tools/ci/run_gates.sh --prefix c7 --base fec6ce8` | **PASS 43/43**。使用任务指定的 `IDF_PATH` 与 `IDF_PYTHON_ENV_PATH`，保留 ASan/UBSan 和 LeakSanitizer。包括 G1 冻结、G3/G3b 所有目标构建、G5/G5b/G6 和 G7b/G7c。日志 `builds/c7-logs/summary.log`。 |
| G2/G2b `test/nvs_store` | **PASS 26/26**，两种构建分别执行，含 13 个已有 C6 场景和 13 个新增场景。覆盖新板、两种不支持长度、全未设置、2280 B 金样、2560 B 合成、字段边界、现有值优先、写入和读回故障、标记提交失败、缓冲不足、重启幂等和 `freset`。同时验证 SN/PN 双副本与旧 EEPROM 字节保留。 |
| 修复前后对照 | `git cat-file -e c81f172:RTK_main/main/modules/storage/LegacyMigration.hpp` 证实基线缺少迁移入口，`NvsStoreBoot.h` 同样不存在。C7 新用例如上通过。 |
| TSan `tools/t13_host_check` | 基线与 C7 均为 **45 PASS / 1 FAIL（既有）**。均只在 `t13_init_start_timeout` 报告 `s_spp` 竞态（`SppServerInit.c:41,53`），无新增失败。日志 `builds/c7-logs/tsan-{baseline,current}.log`。 |
| V3.1 真机金样、V3.1 旧 bootloader 冒烟 | **NOT_RUN**。任务未提供该线设备，2560 B 分支用同地址的合成样本验证。 |
| 20 KB NVS 原区端到端解析 | **NOT_RUN**。使用任务提供的独立 2280 B 真机 blob，经系统 NVS 替身覆盖启动迁移。 |

## HIL 由 Architect 与用户执行

| ADR-0022 §4 | 本卡结果 |
| --- | --- |
| 1. 准备 Arduino V2.7 旧 bootloader、分区表和应用，在旧 App 设置各字段。 | **NOT_RUN** |
| 2. 读出系统 NVS 整区，对照完整旧配置金样。 | **NOT_RUN** |
| 3. 旧网页 OTA 后验证引导、迁移计数、各键、SN/PN 双副本和蓝牙绑定。 | **NOT_RUN** |
| 4. 新固件网页 OTA 与切回旧槽，验证 otadata、旧配置和系统 NVS 保留。 | **NOT_RUN** |
| 5. `freset` 后核对用户默认值、身份与迁移状态保留，以及后续启动跳过迁移。 | **NOT_RUN**，主机回归已覆盖。 |
