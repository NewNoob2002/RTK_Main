# C7 · 旧 EEPROM 配置迁移（第一阶段字段 + JSON/ThingsBoard 原始帧）

依据 ADR-0022（已批准）§2.4、§2.5 第一阶段表与 JSON/ThingsBoard 两行、§4 C7 行。基于 main 上本卡提交开分支 `feat/c7-migration`，
worktree `../RTK_Main-c7`。前置 C6 已合并（`02083f5`）：双 NVS 实例、`facbk/*`、`mig` 命名空间常量、启动期 scratch 缓冲
（`components/nvs_store/include/NvsStore.h:30` `NVS_STORE_EEPROM_MAX`、`:33` `nvs_store_boot_scratch_fn`）均已就绪。
第二阶段字段（APN、NTRIP、围栏）**不在本卡**，只在常量表中留注释位置。

## A 迁移框架（§2.4）
- 新增纯逻辑模块（建议 `components/nvs_store` 内 `LegacyEeprom.{c,h}` 或 `main/modules/storage/LegacyMigration.{hpp,cpp}`，实现者择一并在报告说明理由）：
  输入 blob 字节 + 长度，输出“字段 → 规范值”列表与计数；不直接碰 NVS，便于主机测试。
- 接入 `main/app/BootFlow.cpp:176` `stepEarlyConfig`：两实例初始化之后、读用户配置之前，按 §2.4 步骤 1–6 执行：
  `mig/done` 已有 → 跳过；无 blob → `mig/done=none`；长度非 2280/2560 → W + `mig/done=bad_len`；
  目标键已存在不覆盖；写入后 `nvs_commit` 并读回校验；全部成功写 `mig/done=v1`、`mig/src_len`、`mig/stats`（迁移/跳过/非法计数），
  打一行 I `Legacy config migrated: N fields, M skipped, K invalid`；中途失败不写 `mig/done`，下次重试。旧 blob 不删不改。
- blob 缓冲用 C6 的启动期 scratch（≤2560 B，不增加常驻 RAM）；实测栈峰值写报告。
- 每个版本的地址表放在只读常量表（`constexpr`/`static const`）；V2.7 与 V3.1 地址相同时共用一张，表头注释引用 ADR-0022 §2.5。

## B 第一阶段字段（§2.5 表）
- SN 200/16 → `fac/sn` + `facbk/sn`，与 `factory.sn` 同一校验（`components/settings/src/Registry.cpp:247` `validateSn`）。
- PN 216/16 → `fac/pn` + `facbk/pn`，原文保存不解析。
- 星座 300–303 → `gnss.const.gps/bds/glo/gal`（0/1，其他值未设置）。
- 输出频率 304–308（GGA/GSA/GSV/GST/RMC）→ `gnss.out.*` 周期 ms：正数 Hz → 1000/Hz，0 = 关，负值按 0；按当前 `gnss.rate` 对齐，
  不能整除取最近合法值并计入“非法”。
- 截止角 310 int32 → `gnss.cutoff`（1–90；旧固件 ≤0 视为 15，迁移时按“未设置”处理并在报告说明）。
- SBAS 314 → `rtk/sat_cfg` sbas 位（0/1；F9P 无 SBAS 能力按能力掩码清零）。`sat_cfg` 不存在时以当前默认帧为底再置位。
- 所有写入经现有 settings/StoreKeys 校验路径，不绕过 Registry。

## C JSON / ThingsBoard 原始帧（§2.5 两行）
- JSON 服务器 320/32 + 端口 352 int32 → `rtk/json_srv`（`PhoneJsonServerConfig`，`components/link_core/private_include/PhoneCompatService.hpp:43`，
  0x32 帧 42 B）；ThingsBoard 开关 356、client_id 360/16、用户 376/16、密码 392/16、主机 408/32、端口 440 int32 → `rtk/tb_cfg`
  （`PhoneThingsboardConfig`，`:44`，0x33 帧 86 B）。
- 实现者须从 `PhoneCompatService.cpp` 的 0x32/0x33 解码与旧固件 `user_src`/`meaasge.ino` 对照出帧内布局，写进报告；与旧字段宽度不一致处
  （截断/补零）列表说明。全未设置时不写键。功能仍按 ADR-0021 只预留。

## D 测试
- 新增 `test/` 下迁移主机测试（可并入 `test/nvs_store` 或新建目录；新建须接入 `tools/ci/run_gates.sh` G2/G2b 列表）：
  - 金样：`test/fixtures/legacy_eeprom/v27_snpn.bin`（2280 B，P2 v1.41 真机 Arduino V2.7 读出，2026-11-26；SN `202061126` 偏移 200、
    PN `1322150301032` 偏移 216 为测试值，其余为旧固件默认 0xFF/0x00）；`v27_snpn_nvs.bin` 为同时刻 0x9000 整区 20 KB 存档，
    可选用于端到端解析测试。该样本只覆盖 SN/PN；其余字段用在金样上按 §2.5 地址打补丁的合成 blob 覆盖。
  - V3.1 金样暂缺（外场无该线设备），用 2560 B 合成 blob（同地址）覆盖长度分支，报告标 NOT_RUN。
  - 必测：新板（无 blob）、`bad_len`、全未设置、每字段合法/非法/边界、频率对齐取整、目标键已存在不覆盖、中途写失败不写 `mig/done` 且重试幂等、
    已迁移不再迁移、`freset` 后不再迁移（`mig` 保留）、SN/PN 两副本一致。
- 修复前后对照：新测试在 main `c81f172` 上应失败（或不存在对应接口），写报告。

## 冻结文件
不预期修改冻结文件。若必须改 `components/nvs_store/include/NvsStore.h`，先停下在报告写 NOT_DECIDED 并说明理由，不得自行改 `docs/frozen.sha256`。

## 验收与约束
- 全量门禁 `tools/ci/run_gates.sh --prefix c7 --base <本卡提交>` 全通过（ASan/TSan 现状不退化，已知 s_spp 竞态除外）；环境：
  `export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env`。
- DRAM 增量 ≤ 128 B，IRAM 0，无运行期堆；flash 增量记录；启动耗时增量记录（迁移那次与之后各次）。
- 授权修改：`components/nvs_store/*`（不含冻结头）、`main/modules/storage/*`、`main/app/BootFlow.cpp`（仅插入迁移步骤）、`components/settings/*`（仅暴露现有校验，不改语义）、
  `test/*`、`tools/ci/run_gates.sh`（仅加测试目录）、`docs/PROGRESS.md`、`docs/reports/C7-legacy-eeprom-migration.md`（新建）。
- 报告须含：改动清单、字段映射实现表（地址/宽度/新键/校验/金样覆盖情况）、JSON/TB 帧布局对照、资源、测试结果与修复前后对照、
  NOT_RUN（HIL 由 Architect 与用户做，按 ADR-0022 §4 HIL 1–5）。
- 不 push、不烧录、不连串口、不改 main；完成后提交，单独一行输出 C7-FINISHED。
