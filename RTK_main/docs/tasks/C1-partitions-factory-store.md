# C1：分区表、出厂数据分区、SN 迁移与存储启动日志（ADR-0014 §2.1–2.3）

状态：ISSUED 2026-11-18，基线 `feat/gnss` HEAD（本卡所在提交）。
实施：codex + gpt-6.2-sol；审查/验收/提交/烧录：Claude。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0014-runtime-config.md`（本卡只做 §2.1、§2.2、§2.3 第 4 条）、
ADR-0010 §2.10、`docs/acceptance.md`、`components/nvs_store/**`、`main/modules/storage/**`、`main/app/BootFlow.cpp`。

## 1 要做的

1. **分区表** `partitions.csv`：删 `spiffs`；加 `nvs_fac`（data/nvs，0x360000，0x6000）、`coredump`（data/coredump，0x370000，0x10000）。
   其余行与偏移不变（显式写出 0x360000/0x370000，ota 槽仍为 0x1A0000）。文件头注释说明来源与 ADR-0014 §2.1。
2. **coredump**：`sdkconfig.defaults` 打开 `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`（ELF 格式、CRC 校验按 IDF 缺省）；
   启动时用 `esp_core_dump_image_get()` 检查，存在则 `FW_LOGW(kSys, "Core dump found, %u bytes", ...)`。本卡**不**做读取命令。
3. **nvs_store 两个分区**（C 组件，`fw/LogC.h` 日志）：
   - `nvs_store_init()`：先初始化 `nvs`（只有它允许 `NO_FREE_PAGES`/`NEW_VERSION_FOUND` 时擦除重试），再 `nvs_flash_init_partition("nvs_fac")`；
     `nvs_fac` 失败**不擦除**，记 E，返回值仍只反映 `nvs`（保持启动语义），另提供查询函数让上层知道 fac 是否可用。
   - SN 读写改到 `nvs_fac` 命名空间 `fac` 键 `sn`；新增 `fac/reg` 注册码槽位常量（只定义键名和最大长度 64，不实现读写命令）。
   - **迁移**：`fac/sn` 缺失且 `rtk/sn` 存在 → 写入 `fac/sn`、commit、读回比较一致 → 删除 `rtk/sn`；任一步失败保留旧键，下次启动重试，
     日志一句 I `SN migrated to factory partition` 或一句 W 说明失败步骤。`get_sn` 在 fac 不可用时回退读 `rtk/sn`。
   - 修 S5 记录的待办：`nvs_store_get_sn` 出错时把 `out[0]` 置 0。
   - `components/nvs_store/include/NvsStore.h` 是冻结头，**本卡授权修改**：可新增函数与常量，已有函数签名不变（`nvs_store_get_sn`/`set_sn`
     的语义改为 fac 分区，写进 Doxygen）。
4. **存储启动日志**（SparkFun 风格，`[t] [STORE] <sentence>`，见记忆里的日志风格与 ADR-0009 §2.12）：
   - 一句 NVS 用量：`NVS used N of M entries (nvs), K of L (nvs_fac)`（`nvs_get_stats`）。
   - SN 一句、白名单一句（已有则保留，改措辞对齐）。
   - 每个目录组一句：`<group>: N saved, M default`；有加载失败的组额外每个失败键一行 W（键名 + 错误名）。
   - `loadKeys` 改为返回逐组计数（saved/default/failed），不再只返回“最后一个失败键”；调用方与测试同步。
   - 逐键明细（`key = value`）只在 `CONFIG_RTK_BOOT_VERBOSE` 打开或该组有失败时打印，格式与 `config` 命令完全相同——
     把 `main/modules/storage/ConfigCmds.cpp` 的单键格式化抽成一个共用函数，`config` 与启动日志都调它。
5. **主机测试**：
   - `tools/main_host_check`（或 storage 对应用例）：loadKeys 计数（全缺省、部分保存、单键失败）；共用格式化与 `config` 输出一致。
   - nvs_store 目前没有主机测试（Q1 记录的 TEST-002 缺口）：用 host 桩 NVS（内存 map，支持两个分区与注入错误）新建 `test/nvs_store`，
     覆盖：迁移成功、迁移中写失败/读回不一致/删除失败各自保留旧值、fac 不可用回退、`get_sn` 错误清空缓冲、`nvs` 擦除重试不触碰 fac。
     要求普通与 ASan/UBSan 两种构建，接入 `docs/acceptance.md` 的主机套件清单。

## 2 不做

- 不做 `freset`、`factory erase`、早期加载 `stepEarlyConfig`、启动失败计数（C3/C4）。
- 不改 GNSS 配置键（C2）。
- 不提交、不重算 `docs/frozen.sha256`、不烧录（Claude 做）。不用 `uvx`。

## 3 门禁

- `. ~/esp/esp-idf/export.sh; idf.py -B builds/c1 build`：0 warning；记录 bin 大小与 `idf.py -B builds/c1 partition-table` 输出。
- `idf.py -B builds/c1 size` 记录 DRAM/IRAM 变化。
- G5b：`python3 -I tools/tidy_check/tidy_check.py --host-build <host> --idf-build builds/c1 --gnss-build builds/s5v-gnss-all --test-app-build builds/q4-rtk_pure-esp32` 0 条，
  覆盖自检通过（新测试目录要被覆盖）。
- G7b：`python3 -I tools/g7b_check/g7b_check.py --build builds/c1` PASS。
- clang-format 0；`python3 -I tools/doc_check/doc_check.py --report` 保持 100%。
- 主机套件全过（普通 + ASan）：main_host_check、t13_host_check、test/services、test/app_fw、test/host、test/gnss、test/cli、test/bt_reconnect、新 test/nvs_store。
- `sha256sum -c docs/frozen.sha256` 只允许 `partitions.csv`、`sdkconfig.defaults`、`components/nvs_store/include/NvsStore.h` 失败。

## 4 报告

`docs/reports/C1.md`：改动清单、日志样例（模拟输出即可，Claude 上板补实测）、测试列表、门禁结果、bin/DRAM 变化、未决问题。
完成后最后单独一行打印 `C1 DONE` 并停下。

## 5 Claude 验收时上板做（实施方不做）

- 先烧当前固件，`factory sn` 写入测试 SN、加一个白名单；再整片烧 C1（含分区表）→ 确认 SN 迁移日志、`config`/`status` 中 SN 不变、白名单仍在。
- 二次重启确认不再迁移；`config` 显示正常；启动日志符合 §1.4；启动时间变化。
