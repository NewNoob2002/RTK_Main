# C6 · 统一分区、cfg NVS 与手动切槽

实现依据为 [C6 任务卡](../tasks/C6-partition-cfg-nvs.md)和 [ADR-0022](../adr/ADR-0022-legacy-partition-migration.md)。固件和资源比较使用 ESP32、ESP-IDF v6.1，基线为 `7393202`。

## 改动清单

| 任务 | 实现 |
| --- | --- |
| A | `partitions.csv` 沿用 P2 V2.7 的分区名称、子类型、偏移、大小与标志。网页主机测试和 mock 使用 `app0`、`app1` 标签，地址为 `0x10000`、`0x1F0000`。 |
| B | `components/nvs_store/{NvsStore.c,include/NvsStore.h}` 初始化系统和 cfg 实例。`main/modules/storage/{StoreKeys.cpp,StorageModule.cpp}`、`main/modules/gnss/GnssModuleMaintenance.cpp` 使用 cfg 打开入口。启动分别通过 `nvs_get_stats("nvs", …)` 和 `nvs_get_stats("spiffs", …)` 输出条目。 |
| C | `NvsStore.c` 修复 SN/PN 双副本并搬迁 cfg 的 `rtk/pn`。`main/modules/storage/StoreKeys.hpp` 将 PN 归入 `fac/pn`。`main/app/BootFlow.cpp` 为系统异常恢复提供启动期临时缓冲。 |
| D | `NvsStore.c` 按命名空间清空用户、射频或出厂配置。`main/modules/storage/StorageModule.cpp` 维持控制台和网页共用的写入门与擦除流程，`main/modules/system/FactoryReset.cpp` 更新确认信息。 |
| E | `main/modules/ota/{OtaRuntime.hpp,OtaRuntime.cpp,OtaPolicy.hpp,OtaCmds.cpp}` 共用手动切槽。 `main/modules/web/{WebOta.cpp,WebModule.hpp,WebModule.cpp,WebServer.cpp}` 注册路由并沿用回执后重启流程。`main/web/ui/app.js` 提供版本显示和二次确认。 |
| 验证与文档 | `test/nvs_store/*`、`test/ota/TestOta.cpp`、`tools/main_host_check/{CMakeLists.txt,WebModuleCheck.cpp,shim/NvsShim.c}`、`tools/web_mock/{web_mock.py,e2e_ota_ui.mjs}` 更新测试与替身。ADR-0014 标注被取代的小节，ADR-0019 接口表增加一行，`docs/frozen.sha256` 同步三个授权条目，`docs/PROGRESS.md` 记录 C6 进度。 |

## 分区与镜像头

从旧项目 `P2 手持式GNSS接收机项目/源码/P2 V2.7 基于V2.6增加电子围栏多区块/P2_V2.7/build/esp32.esp32.esp32/` 取得 Arduino V2.7 的 `P2_V2.7.ino.partitions.bin`。用 IDF 的 `gen_esp32part.py` 解出旧表和 `idf.py -B builds/c6-idf partition-table` 的新表，CSV 的 `diff -u` 无差异，二进制的 `cmp` 通过。

| 名称 | 类型 / 子类型 | Arduino V2.7 偏移 / 大小 | C6 偏移 / 大小 |
| --- | --- | --- | --- |
| `nvs` | data / nvs | `0x9000 / 0x5000` | `0x9000 / 0x5000` |
| `otadata` | data / ota | `0xE000 / 0x2000` | `0xE000 / 0x2000` |
| `app0` | app / ota_0 | `0x10000 / 0x1E0000` | `0x10000 / 0x1E0000` |
| `app1` | app / ota_1 | `0x1F0000 / 0x1E0000` | `0x1F0000 / 0x1E0000` |
| `spiffs` | data / spiffs | `0x3D0000 / 0x20000` | `0x3D0000 / 0x20000` |
| `coredump` | data / coredump | `0x3F0000 / 0x10000` | `0x3F0000 / 0x10000` |

生成的 `sdkconfig` 显示 `CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION` 关闭，`CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 与 `CONFIG_ESPTOOLPY_FLASHMODE="dio"` 并存。`python -m esptool --chip esp32 image-info` 检查 Arduino V2.7 的 bootloader/app 与 C6 的 bootloader/app，四个文件头均为 ESP32、4 MB、DIO、80 MHz。Flash 参数无需调整。旧头的芯片版本范围为 v0.0–v655.35，C6 头为 v1.0–v3.99，旧 bootloader 对新镜像头和 otadata 的识别留待 HIL。

Arduino app 描述符的项目名为 `arduino-lib-builder`，版本字段为 `esp-idf: v4.4.7 38eeba213a`。手动切槽显示目标槽描述符中的版本，并由 `esp_ota_set_boot_partition` 重新校验完整镜像。旧槽缺少 otadata 状态时仍可显示和选择，上传入口继续检查新镜像的项目名。

## NVS 归属、恢复和统计

| 实例 | 命名空间 | 用途 |
| --- | --- | --- |
| 系统 `nvs` | `phy`、`nvs.net80211`、`bt_config.conf` / `bt_config` | IDF PHY、Wi-Fi、Bluedroid；恢复出厂清空射频应用配置，并保留 PHY。 |
| 系统 `nvs` | `eeprom`、`facbk` | 旧 `eeprom/eeprom`、身份备份 `facbk/sn` 与 `facbk/pn`。启动遇系统分区初始化错误时，尽力只读备份再按原字节恢复。 |
| cfg `spiffs` | `fac`、`rtk`、`gnss`、`dbg`、`boot` | SN/PN 与产线数据、手机与白名单、GNSS、日志配置、启动计数。 |
| cfg `spiffs` | `link`、`net`、`ntrip`、`fence`、`mig` | `link` 纳入恢复清单，当前无直接持久化调用。其余预留，其中 `mig` 在恢复出厂时保留。 |

固件调用点通过 `rg -n '\bnvs_open' main components` 和 `StoreKey` 定义逐项核对。通用标量和 blob（PN 使用独立的双写接口）、GNSS 维护、报警器白名单使用 `nvs_store_open_cfg`。`nvs_open` 的两个裸调用都在异常恢复路径打开 `eeprom`。身份和清空辅助函数使用 `nvs_open_from_partition` 显式指定实例。蓝牙的 `bt_config.conf` 可见于 IDF `components/bt/host/bluedroid/btc/core/btc_config.c:24`，`components/bt/common/osi/config.c:474` 直接用该文件名打开 NVS 命名空间。Wi-Fi 命名空间字符串由 IDF 6.1 的 ESP32 `components/esp_wifi/lib/esp32/*.a` 核对。

IDF 6.1 `components/nvs_flash/include/nvs_flash.h:94`、`src/nvs_api.cpp:98` 和 `src/nvs_partition_manager.cpp:66` 显示，指针初始化直接构造分区适配器并按标签登记。`nvs_open_from_partition` 通过已登记的标签打开（`include/nvs.h:200`、`src/nvs_api.cpp:296`）。`nvs_flash_erase_partition_ptr` 实际转为按标签查找 NVS 子类型（`src/nvs_api.cpp:246–258`、`src/nvs_partition_lookup.cpp:16–24`），所以 cfg 的恢复使用 `esp_partition_erase_range` 直接擦除找到的 SPIFFS 分区。

| IDF 6.1 Linux flash 仿真，真实 `nvs_get_stats` | 系统 `nvs` 已用 / 总计 | cfg `spiffs` 已用 / 总计 |
| --- | ---: | ---: |
| 空分区初始化后 | 0 / 630 | 0 / 4032 |
| 人工写入 2280 B EEPROM、SN/PN 双副本与 cfg 启动计数 | 81 / 630 | 8 / 4032 |
| 同一夹具执行 `freset` 后 | 81 / 630 | 7 / 4032 |

仿真读回保留的 SN、PN 与 EEPROM 原始字节。样本没有启用 PHY、Wi-Fi、蓝牙或 GNSS，设备实际占用随使用状态而变化。探针源和日志分别在 `builds/c6-nvs-sdk-src`、`builds/c6-logs/nvs-sdk-stats.log`。

系统和 cfg 只擦各自分区。系统异常时启动分配器最多临时借出 2560 B，恢复完成即释放。IDF 冷启动初始化失败时可能尚未登记系统实例，公共只读 API 此时也会失败。E 日志分别记录三个备份结果，继续按任务卡擦除并从 cfg 补回身份；这一情况下旧 EEPROM 可能丢失。正常启动保持旧 EEPROM 原始数据，旧内容迁移由 C7 负责。cfg 异常后从系统备份补回出厂身份。双写先备份再提交 cfg，主副本写失败时尝试恢复原备份，启动比对时以 cfg 为准。PN 旧键只在双副本已就绪后删除，可重试清理。

## 资源

| ESP32 默认构建，`idf.py size` | 基线 `7393202` | C6 | 增量 |
| --- | ---: | ---: | ---: |
| 静态 DRAM | 98,619 B | 98,619 B | 0 B |
| IRAM | 116,139 B | 116,139 B | 0 B |
| Flash code | 1,141,440 B | 1,142,992 B | +1,552 B |
| Flash data | 275,164 B | 275,500 B | +336 B |
| `rtk_main.bin` | 1,586,176 B | 1,588,064 B | +1,888 B |

每个 app 槽为 `0x1E0000`（1,966,080 B），C6 镜像余量 `0x5C4A0`（378,016 B，约 19.2%）。运行期自有代码新增堆分配 0，IDF 的 NVS API 仍管理其内部句柄和缓存。Xtensa `-fstack-usage` 检查中，借用启动缓冲的函数使用 928 B 栈，系统恢复函数使用 112 B 栈，2560 B 的 EEPROM 缓冲由现有启动分配器管理。日志见 `builds/c6-logs/size{,-baseline}.log`，静态栈数据见 `builds/c6-stack`。

## 验证

| 执行项 | 结果 |
| --- | --- |
| `tools/ci/run_gates.sh --prefix c6 --base 7393202` | **PASS 43/43**。按任务卡导出 `IDF_PATH` 和 `IDF_PYTHON_ENV_PATH`，同时将已缓存的 lizard 1.26.1 加入 PATH。在沙箱外运行，保留 LeakSanitizer 和 ASan/UBSan。首轮沙箱内 LeakSanitizer 因 ptrace 失败。最终日志为 `builds/c6-logs/summary.log`。 |
| `test/nvs_store` 普通及 ASan/UBSan | **两种构建均 PASS 13/13**。基线以 `rtk/sn` 单次迁移和分区隔离为主。C6 检查双副本齐全、分别缺失、不一致告警、损坏、两侧异常擦除、系统只读备份成功与不可读降级、PN 搬迁及删除失败重试、写失败补偿、两级恢复与白名单归属。 |
| `tools/main_host_check` 普通及 ASan/UBSan | **两种构建均 PASS 205/205**。新增实际 HTTP handler 的成功、忙、无合法镜像、未提交脏槽、完整校验失败和 SDK 写失败，验证成功回执后重启。基线只有 OTA 上传和自动回滚相关检查。 |
| `python3 tools/web_mock/web_mock.py --selftest` 与 `node tools/web_mock/e2e_ota_ui.mjs` | **PASS**。验证切槽应答、待重启时忙、未提供 OTA 时 404，并覆盖另一槽版本、空槽禁用、两次点击及成功反馈。 |
| IDF 分区表、镜像头与 Linux flash 仿真 | **PASS**。二进制对照、Flash 参数、实例条目见上。 |
| `tools/t13_host_check` 单独用 `-fsanitize=thread -g -O1` 构建，并各跑一次 `ctest --output-on-failure` | 基线与 C6 都是 **45 PASS / 1 FAIL**。`t13_init_start_timeout` 两处既有数据竞争相同，均报告 `SppServerInit.c:41`、`SppServerInit.c:53`，日志见 `builds/c6-logs/tsan{,-baseline}.log`。 |
| HIL | **NOT_RUN**。由 Architect 与用户验证 Arduino/IDF 旧 bootloader 识别 IDF 6.1 app、网页切回旧槽、旧固件再次读取系统 NVS、设备上两实例条目及射频配置清空。 |
