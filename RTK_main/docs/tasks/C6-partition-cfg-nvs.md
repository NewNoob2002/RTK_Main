# C6 · 分区表与旧固件统一 + cfg NVS + SN/PN 双副本 + 网页手动切槽

依据 ADR-0022（已批准）§2.1–§2.3、§2.6、§4 C6 行。基于 main `c620f22` 之后的本卡提交开分支 `feat/c6-partition`，worktree `../RTK_Main-c6`。
旧 EEPROM 迁移（ADR-0022 §2.4/§2.5）**不在本卡**，属 C7；本卡只需保证旧 `eeprom/eeprom` blob 在系统 NVS 中不被本固件改写或擦除。

## A 分区表（ADR-0022 §2.1）
- `partitions.csv` 改为与旧固件逐项相同：nvs 0x9000/0x5000、otadata 0xE000/0x2000、ota_0 0x10000/0x1E0000、ota_1 0x1F0000/0x1E0000、
  spiffs（data/spiffs）0x3D0000/0x20000、coredump 0x3F0000/0x10000。删除 phy_init、nvs_fac、fence；表头注释改为引用 ADR-0022。
- 确认 `CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` 未开；`idf.py partition-table` 输出与旧表一致（报告附对照，旧表来源
  `P2 手持式GNSS接收机项目/源码/P2 V2.7 基于V2.6增加电子围栏多区块/P2_V2.7/build/esp32.esp32.esp32/P2_V2.7.ino.partitions.bin`，用 `gen_esp32part.py` 解出）。
- 核对构建产物镜像头 flash 模式/频率/大小（`esptool.py image_info`），与旧 bootloader 4MB/DIO/80m 的关系写进报告（sdkconfig 为 QIO：说明镜像头实际值与是否需要改；不得擅自改 sdkconfig 的 flash 模式，有疑问写入报告 NOT_DECIDED）。
- 同步更新依赖旧偏移的测试/工具（如 `tools/main_host_check/WebModuleCheck.cpp:158` 的 0x20000）。

## B cfg NVS 实例（§2.2）
- `components/nvs_store` 增加 cfg 实例：`esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs")` +
  `nvs_flash_init_partition_ptr()`；之后用 `nvs_open_from_partition("spiffs", ns, …)` 打开。先在 IDF 6.1 头文件/文档确认这两个接口对 spiffs 子类型可用并在报告引用出处。
- 本固件自有命名空间全部改到 cfg 区：`fac`、`rtk`、`gnss`、`link`、`dbg`、`boot` 及其他现有自有命名空间（实现者全量 `rg nvs_open` 列出清单写报告）；
  `main/modules/storage/StoreKeys.cpp`、`main/modules/gnss/GnssModuleMaintenance.cpp:73` 等直接 `nvs_open` 的地方统一经 nvs_store 提供的打开函数。
  预留 `net`、`ntrip`、`fence`、`mig` 命名空间常量即可，不实现功能。
- 系统 NVS（默认 `nvs`）只留 IDF 自用（PHY、Wi-Fi、`bt_config`）、旧 `eeprom` blob（只读，本卡不读不写）与 `facbk/sn`、`facbk/pn`。
- 启动日志/`StorageModule` 统计改为 `nvs` 与 `spiffs` 两个实例（`nvs_get_stats`），报告记录两实例已用/总条目。

## C SN/PN 双副本与擦除策略（§2.2）
- `fac/sn`、`fac/pn`（cfg）与 `facbk/sn`、`facbk/pn`（系统）：启动比较，一处缺/坏用另一处补写；两处不同以 cfg 为准并打 W。`factory sn`、PN 写入同时写两处。
  现有 `rtk/pn` 迁入 `fac/pn`（cfg 内部搬迁，读到旧键则搬过去并删旧键；幂等）。
- 系统 NVS 初始化遇 `ESP_ERR_NVS_NO_FREE_PAGES`/`ESP_ERR_NVS_NEW_VERSION_FOUND`：先尽力只读拷出 `facbk/*` 与 `eeprom/eeprom`（2560 B 上限缓冲，仅启动期，不增常驻 RAM），擦除后写回，打 E；拷不出也照擦并打 E。
- cfg NVS 同样错误：擦除 spiffs 分区，从 `facbk/*` 恢复 `fac/*`，打 E。两侧都不得擦对方分区。
- 删除 `nvs_fac` 相关代码路径与旧 SN 迁移逻辑中对 `nvs_fac` 的依赖（若旧开发板 0xA000 布局迁移代码存在，按 ADR-0022 §3 删除，测试板整片重烧）。

## D 恢复出厂（§2.3）
- `freset`/网页 freset：cfg 区用户命名空间逐个 `nvs_erase_all`（`gnss`、`rtk`、`link`、`dbg`、`boot`、`net`、`ntrip`、`fence`）；清蓝牙绑定与 Wi-Fi 配置（用现有接口）；
  保留 `fac`、`facbk`、`mig`、`eeprom` blob、PHY、coredump。不再整片 `nvs_flash_erase()`。
- `factory erase`（现有解锁条件不变）：再清 `fac`、`facbk`；保留 `eeprom` blob 与 `mig`。

## E 网页手动切槽（§2.6，§6 Q4）
- `POST /ota/rollback`（ADR-0019 OTA 页同一鉴权/忙判定规则）：另一槽 `esp_ota_get_partition_description` 合法且非本次已 begin 未提交时，
  `esp_ota_set_boot_partition(other)` 后按现有 reboot 流程重启；否则返回 409 `no_valid_other`；OTA 进行中 409 `busy`。错误/成功 JSON 格式沿用 ADR-0019 §2 表。
- 网页 OTA 页加“切回另一版本”按钮，显示另一槽版本，二次确认。`tools/web_mock` 同步该接口与 e2e。
- 控制台 `ota rollback [yes]` 调同一实现（调试用；发布版控制台关闭不影响网页）。
- 更新 ADR-0019 §2 接口表增加该行（文档修订，注明 ADR-0022 §2.6）。

## 冻结文件
本卡经 ADR-0022 批准，授权修改冻结清单中的 `partitions.csv`、`components/nvs_store/include/NvsStore.h`、`docs/adr/ADR-0014-runtime-config.md`
（仅在受影响小节加“由 ADR-0022 取代”标注，不改原文），并同步 `docs/frozen.sha256` 对应行。其他冻结文件不得改动。

## 验收与约束
- 全量门禁 `tools/ci/run_gates.sh --prefix c6 --base 7393202` 全通过（含 ASan/TSan 现状不退化）；环境：
  `export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env`。
- 主机测试（`test/nvs_store` 假件扩展为多分区）：双副本四种情况（都有/缺 cfg/缺系统/不一致）、两侧擦除恢复、`rtk/pn`→`fac/pn` 搬迁幂等、
  freset 保留清单、factory erase、`/ota/rollback` 三种应答。修复前后对照写报告。
- DRAM 增量 ≤ 256 B，IRAM 0，无运行期堆；app 镜像必须 ≤ 0x1E0000 并记录余量。
- 授权修改：上述文件，`components/nvs_store/*`、`main/modules/storage/*`、`main/modules/system/FactoryReset.cpp`、`main/modules/gnss/GnssModuleMaintenance.cpp`、
  `main/modules/ota/*`、`main/modules/web/*`（OTA 部分）、`main/app/BootFlow.cpp`（仅 NVS 初始化顺序）、`test/*`、`tools/main_host_check/*`、`tools/web_mock/*`、
  `docs/adr/ADR-0019-web-ota.md`（接口表一行）、`docs/PROGRESS.md`、`docs/reports/C6-partition-cfg-nvs.md`（新建）。
- 报告须含：改动清单、两实例命名空间分布、资源（DRAM/IRAM/flash/镜像余量）、测试结果、镜像头核对、NOT_RUN（HIL 由 Architect 与用户做）。
- 不 push、不烧录、不连串口、不改 main；完成后提交，单独一行输出 C6-FINISHED。
