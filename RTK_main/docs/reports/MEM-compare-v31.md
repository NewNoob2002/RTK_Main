# ESP32 固件内存与资源占用对比分析（RTK_main 下一代 vs P2 V3.1）

> **分析范围与方法说明**：本文为只读静态与代码/日志取证报告。
> - **固件 A（下一代）**：`/home/gtc/Desktop/workspace/ESP32_PROJ/RTK_Main-c7/RTK_main`（分支 `feat/c7-migration`，HEAD `4911a3a`，构建目录 `builds/c7v-idf/`：`rtk_main.map`、`rtk_main.elf`、`config/sdkconfig.json`）。
> - **固件 B（旧版 V3.1 / IDF v5.5.5）**：`/home/gtc/Desktop/workspace/ESP32_PROJ/P2_V3.1_IDF`（`build/P2_IDF.map`、`build/P2_IDF.elf`、`sdkconfig`、`build/config/sdkconfig.json`、`main/`）。
> - **统计工具**：`$HOME/.espressif/python_env/idf6.1_py3.13_env/bin/python -m esp_idf_size`、`$HOME/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/xtensa-esp32-elf-{nm,objdump}`。
> - **口径区分**：所有**直接测量/代码实证**均标注 `file:line` 或命令来源；**推算/估算**与**待上板验证假设**均单独标明。

---

## 1. 两者静态占用对比（DRAM / IRAM / Flash）

### 1.1 总览对比

来源：`$HOME/.espressif/python_env/idf6.1_py3.13_env/bin/python -m esp_idf_size` 分别解析 `RTK_main/builds/c7v-idf/rtk_main.map` 与 `P2_V3.1_IDF/build/P2_IDF.map`。

| 内存区域 / 段 | 固件 A (`c7v-idf`) | 固件 B (`P2_V3.1_IDF`) | 差值 (A − B) | 说明 |
| --- | ---: | ---: | ---: | --- |
| **总镜像大小** | `1,589,998 B` | `1,389,983 B` | `+200,015 B` | A/B 共用 1.875 MiB (`0x1E0000`) OTA 槽 (`partitions.csv:5-6`) |
| **Flash Code (`.text`)** | `1,144,724 B` | `940,352 B` | `+204,372 B` | A 含 C++ 模块框架、全量组件与 CLI/Trace |
| **Flash Data** | `275,820 B` | `316,196 B` | `−40,376 B` | A `.rodata 275,548 + .appdesc 256 + .tdata 16`；B `.rodata 315,940 + .appdesc 256` |
| **IRAM 已用 / 总量** | `116,139 / 131,072 B` (`88.61%`) | `105,087 / 131,072 B` (`80.18%`) | `+11,052 B` | A 启用 `ESP_WIFI_IRAM_OPT` & `RX_IRAM_OPT` (`libpp.a` 增 `+19,781 B`) |
| **IRAM 余量** | `14,933 B` | `25,985 B` | `−11,052 B` | 见 `c7_boot3.log:20`：`4009C5AC len 0x3A54 (14,932 B)` |
| **DRAM 已用 / 总量** | `98,627 / 124,580 B` (`79.17%`) | `91,324 / 124,580 B` (`73.31%`) | `+7,303 B` | A `.data 53,275 + .bss 45,352`；B `.data 28,316 + .bss 63,008` |
| **DRAM 静态后余量** | `25,953 B` | `33,256 B` | `−7,303 B` | `124,580` 为链接器固定静态 DRAM 池（不含 D/IRAM 与 BT 保留区） |
| **RTC SLOW** | `68 / 8,192 B` (`0.83%`) | `56 / 8,192 B` (`0.68%`) | `+12 B` | — |

### 1.2 按组件排名前 15（`esp_idf_size --archives --format csv`）

#### 1.2.1 DRAM（`.data + .bss`）Top 15

| 排名 | 固件 A 组件 | A DRAM (`.data` / `.bss`) | 固件 B 组件 | B DRAM (`.data` / `.bss`) |
| ---: | --- | ---: | --- | ---: |
| 1 | `libmain.a` | `29,826` (`27,722` / `2,104`) | `libmain.a` | `29,006` (`4,898` / `24,108`) |
| 2 | `libbt.a` | `23,215` (`4,759` / `18,456`) | `libbt.a` | `23,621` (`4,758` / `18,863`) |
| 3 | `libnet80211.a` | `10,704` (`872` / `9,832`) | `libnet80211.a` | `9,409` (`860` / `8,549`) |
| 4 | `libspi_flash.a` | `6,471` (`6,455` / `16`) | `libhal.a` | `4,690` (`4,682` / `8`) |
| 5 | `libfreertos.a` | `4,976` (`4,228` / `748`) | `libpp.a` | `4,171` (`2,639` / `1,532`) |
| 6 | `libpp.a` | `4,211` (`2,639` / `1,572`) | `libfreertos.a` | `3,856` (`3,108` / `748`) |
| 7 | `libbtdm_app.a` | `3,714` (`1,022` / `2,692`) | `esp32\libbtdm_app.a` | `3,714` (`1,022` / `2,692`) |
| 8 | `libspp_server.a` | `1,928` (`8` / `1,920`) | `liblwip.a` | `2,482` (`12` / `2,470`) |
| 9 | `libwpa_supplicant.a` | `1,483` (`0` / `1,483`) | `libwpa_supplicant.a` | `1,371` (`8` / `1,363`) |
| 10 | `libcli.a` | `1,478` (`1` / `1,477`) | `libspi_flash.a` | `1,348` (`1,332` / `16`) |
| 11 | `libphy.a` | `1,225` (`584` / `641`) | `esp32\libphy.a` | `1,222` (`584` / `638`) |
| 12 | `libesp_libc.a` | `1,068` (`400` / `668`) | `libxtensa.a` | `1,044` (`1,044` / `0`) |
| 13 | `libxtensa.a` | `1,044` (`1,044` / `0`) | `libesp_system.a` | `738` (`429` / `309`) |
| 14 | `liblwip.a` | `913` (`4` / `909`) | `libcoexist.a` | `653` (`652` / `1`) |
| 15 | `libespcoredump.a` | `908` (`112` / `796`) | `no-rtti\libc.a` | `572` (`260` / `312`) |

> **要点（来源：`xtensa-esp32-elf-nm -S` 与 `esp_idf_size`）**：
> - A 中 `libmain.a` 几乎全部集中在单一静态上下文 `app::s_app`（`_ZN3app5s_appE` = `27,000 B` `.data`），B 的 `libmain.a` 则是传统全局数组（`GNSS_buffer` 4,096 + `GSM_RxBuffer` 4,096 + `GSV_buff` 3,072 + `BT_TX_BUFF` 3,072 + `BT_RX_BUFF` 2,048 等）。
> - A 启用了 `ESP_COREDUMP_ENABLE_TO_FLASH`（`libespcoredump.a` `+908 B`）、`FREERTOS_ISR_STACKSIZE=2096`（`port_IntStack` `4,192 B` vs B 的 `3,072 B`，`libfreertos.a` `+1,120 B`）、`libspp_server.a`（`+1,928 B`）与 `libcli.a`（`+1,478 B`）。
> - A 通过关闭 `LWIP_IPV6` 与调小 `LWIP_MAX_SOCKETS` 使 `liblwip.a` 比 B 少 `1,569 B`（`913` vs `2,482`）。

#### 1.2.2 IRAM Top 15

| 排名 | 固件 A 组件 | A IRAM (B) | 固件 B 组件 | B IRAM (B) |
| ---: | --- | ---: | --- | ---: |
| 1 | `libpp.a` | `21,728` | `esp32\libbtdm_app.a` | `19,840` |
| 2 | `libbtdm_app.a` | `19,852` | `libfreertos.a` | `15,178` |
| 3 | `libphy.a` | `9,122` | `esp32\libphy.a` | `9,110` |
| 4 | `libspi_flash.a` | `9,007` | `libspi_flash.a` | `8,793` |
| 5 | `libesp_hw_support.a` | `7,844` | `libesp_hw_support.a` | `7,433` |
| 6 | `libheap.a` | `6,529` | `libheap.a` | `6,319` |
| 7 | `libfreertos.a` | `5,481` | `libhal.a` | `6,167` |
| 8 | `libnet80211.a` | `5,437` | `libesp_ringbuf.a` | `4,525` |
| 9 | `libesp_system.a` | `3,880` | `libesp_system.a` | `3,695` |
| 10 | `libcoexist.a` | `3,506` | `libcoexist.a` | `3,267` |
| 11 | `libesp_ringbuf.a` | `2,542` | `libesp_driver_uart.a` | `2,537` |
| 12 | `libxtensa.a` | `2,220` | `libxtensa.a` | `2,216` |
| 13 | `libesp_hal_mspi.a` | `2,169` | `esp32\librtc.a` | `2,008` |
| 14 | `librtc.a` | `2,004` | `libpp.a` | `1,947` |
| 15 | `libbootloader_support.a` | `1,869` | `libbootloader_support.a` | `1,919` |

#### 1.2.3 Flash（Code + Data）Top 15

| 排名 | 固件 A 组件 | A Flash 总计 (`Code` / `Data`) | 固件 B 组件 | B Flash 总计 (`Code` / `Data`) |
| ---: | --- | ---: | --- | ---: |
| 1 | `libmain.a` | `210,571` (`181,433` / `29,138`) | `libbt.a` | `202,984` (`188,888` / `14,096`) |
| 2 | `libbt.a` | `207,498` (`193,456` / `14,042`) | `libnet80211.a` | `144,376` (`130,504` / `13,872`) |
| 3 | `libesp_stdio.a` | `154,531` (`860` / `153,671`) | `libmain.a` | `141,450` (`38,426` / `103,024`) |
| 4 | `libnet80211.a` | `145,978` (`131,113` / `14,865`) | `libesp_app_format.a` | `138,457` (`334` / `138,123`) |
| 5 | `libtfpsacrypto.a` | `81,239` (`68,232` / `13,007`) | `liblwip.a` | `86,570` (`83,285` / `3,285`) |
| 6 | `libbtdm_app.a` | `68,908` (`67,624` / `1,284`) | `libmbedcrypto.a` | `78,422` (`71,450` / `6,972`) |
| 7 | `liblwip.a` | `67,403` (`64,457` / `2,946`) | `esp32\libbtdm_app.a` | `68,802` (`67,534` / `1,268`) |
| 8 | `libpp.a` | `46,957` (`42,305` / `4,652`) | `libpp.a` | `66,553` (`61,597` / `4,956`) |
| 9 | `libwpa_supplicant.a` | `36,852` (`36,751` / `101`) | `libwpa_supplicant.a` | `65,209` (`63,627` / `1,582`) |
| 10 | `libgnss.a` | `34,876` (`33,886` / `990`) | `no-rtti\libc.a` | `54,398` (`51,308` / `3,090`) |
| 11 | `libphy.a` | `34,475` (`34,475` / `0`) | `esp32\libphy.a` | `34,227` (`34,227` / `0`) |
| 12 | `libc.a` | `17,248` (`15,789` / `1,459`) | `libnvs_flash.a` | `14,428` (`14,292` / `136`) |
| 13 | `libfreertos.a` | `16,377` (`14,597` / `1,780`) | `libhttp_parser.a` | `13,814` (`13,100` / `714`) |
| 14 | `libconsole.a` | `15,894` (`15,855` / `39`) | `libesp_http_server.a` | `13,319` (`12,772` / `547`) |
| 15 | `libnvs_flash.a` | `15,721` (`15,585` / `136`) | `libesp_hw_support.a` | `11,399` (`10,411` / `988`) |

> 注：A 的网页单页为 `index_html_gz`（`18,475 B` `.rodata`，`nm` 符号 `0x3f42c160..0x3f43098b`），而 B 在 `libmain.a` `.rodata` 中嵌入未压缩前端资源（`jquery_js` `30,093 B`、`bootstrap_min_css` `21,940 B`、`bootstrap_bundle_min_js` `21,930 B`、`bootstrap_min_js` `15,067 B`、`rtkSetup_png` `6,425 B` 等，合计约 `103 KB`）。

---

## 2. `sdkconfig` 中影响堆的关键差异对照表

来源：`RTK_main/builds/c7v-idf/config/sdkconfig.json` 与 `P2_V3.1_IDF/build/config/sdkconfig.json`。

| 类别 | 配置项 (`CONFIG_*`) | 固件 A (`c7v-idf`) | 固件 B (`P2_V3.1_IDF`) | 对堆/RAM 的影响 |
| --- | --- | --- | --- | --- |
| **Wi-Fi RX/TX** | `ESP_WIFI_STATIC_RX_BUFFER_NUM` | `6` | `10` | A 每少 1 个静态 RX 省约 1.6 KB，共少约 `6.4 KB` |
| Wi-Fi RX/TX | `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` | `12` | `32` | 限制突发 RX 峰值缓冲上限 |
| Wi-Fi RX/TX | `ESP_WIFI_STATIC_TX_BUFFER` | `False` | `False` | 均使用动态 TX 缓冲 |
| Wi-Fi RX/TX | `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | `12` | `32` | 限制突发 TX 峰值缓冲上限 |
| Wi-Fi 聚合 | `ESP_WIFI_AMPDU_TX_ENABLED` / `RX_ENABLED` | `True` (`BA_WIN=6/6`) | `False` (`<UNSET>`) | A 启用 AMPDU（受 `RX_BA_WIN=6` 约束静态 RX 不得低于 6） |
| Wi-Fi 管理帧 | `ESP_WIFI_STATIC_RX_MGMT_BUFFER` / `RX_MGMT_BUF_NUM_DEF` / `MGMT_SBUF_NUM` | `True` / `5` / `32` | `True` / `5` / `32` | 相同；约 3–4 KB 管理帧池 |
| Wi-Fi 安全 | `ESP_WIFI_ENABLE_WPA3_SAE` / `SOFTAP_SAE_SUPPORT` | `False` / `<UNSET>` | `True` / `True` | B 启用 WPA3 使 `wifi` 任务栈从 `3,584` 翻倍至 `6,656 B`（`+3,072 B`） |
| Wi-Fi IRAM | `ESP_WIFI_IRAM_OPT` / `RX_IRAM_OPT` | `True` / `True` | `False` / `False` | A 占更多 IRAM（`libpp.a`+`libnet80211.a`），不消耗 DRAM |
| Wi-Fi 核/NVS | `ESP_WIFI_TASK_PINNED_TO_CORE_0` / `NVS_ENABLED` | `True` / `True` | `True` / `True` | 相同 |
| **LWIP** | `LWIP_IPV6` | `False` | `True` | A 关闭 IPv6，减小 `liblwip.a` 与 PCB 开销 |
| LWIP | `LWIP_MAX_SOCKETS` | `8` | `10` | A 静态 `.bss` 少 `~120 B`，限制并发 FD 上限 |
| LWIP | `LWIP_MAX_ACTIVE_TCP` / `LISTENING_TCP` / `UDP_PCBS` / `RAW_PCBS` | `16` / `16` / `16` / `16` | `16` / `16` / `16` / `16` | 相同；按需从堆分配 PCB |
| LWIP | `LWIP_TCP_SND_BUF_DEFAULT` / `WND_DEFAULT` / `MSS` | `5760` / `5760` / `1440` | `5760` / `5760` / `1440` | 相同；单流窗口 4×MSS |
| LWIP | `LWIP_TCP_RECVMBOX_SIZE` / `UDP_RECVMBOX_SIZE` / `TCPIP_RECVMBOX_SIZE` | `6` / `6` / `32` | `6` / `6` / `32` | 相同 |
| LWIP | `LWIP_TCPIP_TASK_STACK_SIZE` / `IRAM_OPTIMIZATION` | `3072` / `False` | `3072` / `False` | 相同 |
| **mbedTLS** | `MBEDTLS_SSL_IN_CONTENT_LEN` / `OUT_CONTENT_LEN` | `16384` / `4096` | `16384` / `4096` | 相同（Wi-Fi AP 与当前固件未建 TLS 会话，不占运行期堆） |
| mbedTLS | `MBEDTLS_ASYMMETRIC_CONTENT_LEN` / `DYNAMIC_BUFFER` / `INTERNAL_MEM_ALLOC` | `True` / `False` / `True` | `True` / `False` / `True` | 相同 |
| **BT** | `BTDM_CTRL_MODE_BR_EDR_ONLY` / `BTDM` / `BLE_ONLY` | `True` / `False` / `False` | `True` / `False` / `False` | 相同；均仅使能经典蓝牙 BR/EDR |
| BT | `BTDM_CTRL_BR_EDR_MAX_ACL_CONN` / `MAX_SYNC_CONN` | `2` / `0` | `2` / `0` | 相同 |
| BT | `BTDM_CTRL_MODEM_SLEEP` | `True` (`ORIG`) | `False` | A 使能蓝牙 Modem Sleep |
| BT Host | `BT_BLUEDROID_ENABLED` / `BT_CLASSIC_ENABLED` / `BT_SPP_ENABLED` | `True` / `True` / `True` | `True` / `True` / `True` | 均编译 Bluedroid 静态 `.data+.bss ≈ 23 KB` |
| BT Host | `BT_ACL_CONNECTIONS` | `2` | `4` | B 在 BT 模式多支持 2 条 ACL |
| BT 栈 | `BT_BTC_TASK_STACK_SIZE` / `BT_BTU_TASK_STACK_SIZE` | `3072` / `4352` | `3072` / `4352` | 相同 |
| **FreeRTOS** | `FREERTOS_HZ` / `UNICORE` | `1000` / `False` | `1000` / `False` | 相同（双核 1 kHz） |
| FreeRTOS | `FREERTOS_ISR_STACKSIZE` | `2096` | `1536` | A 双核 `port_IntStack` 多占 `1,120 B` `.data`（`4,192` vs `3,072`） |
| FreeRTOS | `FREERTOS_IDLE_TASK_STACKSIZE` / `TIMER_TASK_STACK_DEPTH` | `1536` / `2048` | `1536` / `2048` | 相同 |
| **系统/Coredump** | `ESP_COREDUMP_ENABLE_TO_FLASH` / `ENABLE_TO_NONE` | `True` / `False` | `False` / `True` | A 增静态 DRAM `908 B`（`libespcoredump.a`）与启动检查 |
| 系统 | `ESP_MAIN_TASK_STACK_SIZE` / `SYSTEM_EVENT` / `TIMER` / `IPC` | `3584` / `2304` / `3584` / `1024` | `3584` / `2304` / `3584` / `1024` | 相同（均未开 `NEWLIB_NANO_FORMAT`，`TASK_EXTRA_STACK_SIZE=512`） |
| **HTTPD** | `HTTPD_MAX_REQ_HDR_LEN` / `MAX_URI_LEN` / `WS_SUPPORT` | `1024` / `512` / `True` | `1024` / `512` / `True` | 相同；A 在代码中配 `max_open_sockets=3, stack=4096`，B 配 `stack=16384, max_uri=80` |
| **SPIRAM/编译** | `SPIRAM` / `COMPILER_OPTIMIZATION_PERF` | `False` / `True` (`-O2`) | `False` / `True` (`-O2`) | 均无 PSRAM，均 `-O2` 优化 |
| **A 独有缓冲开关** | `RTK_CONSOLE_STACK_SIZE` / `UART_TX_BUF` | `4096` / `8192` | — | A 控制台动态占 `4,096 + 8,192 + 256` B |
| A 独有缓冲开关 | `RTK_TRACE_ENABLE` / `RTK_LOG_RING_BYTES` | `True` / `4096` | — | A `BootArena` 占 `3,072 + 4,096 + 4,096 = 11,264 B` |
| A 独有缓冲开关 | `RTK_GNSS_APP_RING_BYTES` / `RTK_OTA_ENABLE` / `CAPTIVE_PORTAL` | `4096` / `True` / `True` | — | A Wi-Fi 模式 `web.ota` 占 `2,048 B`，Door/DNS 占 7 URI + 1 UDP |

---

## 3. 任务清单、栈占用与 Web 端 Heap API 溯源

### 3.1 固件 A（`RTK_main`, `c7v-idf`）任务与栈清单

> 注：未启用 `NEWLIB_NANO_FORMAT` 时，IDF `esp_task.h:36` 定义 `TASK_EXTRA_STACK_SIZE = 512`。

| 任务名 | 来源 (`file:line`) | 创建方式/位置 | 核 / 优先级 | 栈大小 (B) | 活跃阶段 |
| --- | --- | --- | --- | ---: | --- |
| `link` | `AppContext.hpp:82`, `Executors.cpp:104-129` | 静态 (`BootArena`: `task.link`) | Core 1 / 10 | `4,096` | 常驻 |
| `io` | `AppContext.hpp:84`, `Executors.cpp:83-101` | 静态 (`BootArena`: `task.io`) | Core 0 / 9 | `4,096` | 正常启动常驻 |
| `trace_out` | `DebugModule.hpp:44`, `DebugModule.cpp:176` | 静态 (`BootArena`: `task.trace_out`) | Core 0 / 1 | `3,072` | `RTK_TRACE_ENABLE=y` 常驻 |
| `oled` | `OledModule.hpp:35`, `OledModule.cpp:31-32,443` | 静态 (`BootArena`: `task.oled`) | Core 1 / 1 | `3,072` | 含 `Feature::kOled` 常驻 |
| `console` (REPL) | `Console.cpp:355-370`, `sdkconfig.json` | 堆 (`esp_console_new_repl_uart`) | Core 0 / 2 | `4,096` | `dev` 档位常驻 |
| `spp_worker` | `SppServerStorage.h:18`, `SppServerInit.c:21-24,117` | 静态 (`BootArena`: `task.spp_worker`) | Core 0 / 5 | `2,560` | **仅 BT 模式** |
| `httpd` | `WebServer.cpp:741-749`, `esp_http_server.h:58` | 堆 (`httpd_start`) | Core 0 / 5 | `4,096` | **仅 Wi-Fi 模式** |
| `IDLE0` + `IDLE1` | `sdkconfig.json` (`FREERTOS_IDLE_TASK_STACKSIZE`) | 堆 (FreeRTOS 启动) | 0 & 1 / 0 | `3,072` (`1536×2`) | 常驻 |
| `Tmr Svc` | `sdkconfig.json` (`FREERTOS_TIMER_TASK_STACK_DEPTH`) | 堆 (FreeRTOS 定时器) | 默认 / 1 | `2,048` | 常驻 |
| `ipc0` + `ipc1` | `esp_ipc.c:29`, `sdkconfig.json` (`1024`) | 堆 (`esp_ipc_init`) | 0 & 1 / 24 | `2,048` (`1024×2`) | 常驻 |
| `esp_timer` | `esp_task.h:47` (`3584 + 512`) | 堆 (`esp_timer.c:601`) | Core 0 / 22 | `4,096` | 常驻 |
| `main` | `esp_task.h:57` (`3584 + 512`) | 堆 (`app_startup.c:81`) | Core 0 / 1 | `4,096` | **仅启动期**（`BootFlow.cpp:624` 返回后由 IDLE 回收） |
| `sys_evt` | `esp_task.h:52` (`2304 + 512`) | 堆 (`default_event_loop.c:101`) | 默认 / 20 | `2,816` | **仅 Wi-Fi 模式** |
| `tiT` (lwIP) | `esp_task.h:55` (`3072 + 512`) | 堆 (`esp_netif_init`) | 无绑定 / — | `3,584` | **仅 Wi-Fi 模式** |
| `wifi` | `config_get_wifi_task_stack_size` 反汇编 (`0x4019d478`) | 堆 (`esp_wifi_init`) | Core 0 / — | `3,584` | **仅 Wi-Fi 模式**（无 WPA3 故为 `3072+512`） |
| `btController` | `esp_task.h:40` (`3584 + 512`) | 堆 (`esp_bt_controller_init`) | Core 0 / 23 | `4,096` | **仅 BT 模式** |
| `hciT` | `hci_layer.c:45` (`2048 + 512`) | 堆 (Bluedroid HCI) | Core 0 / — | `2,560` | **仅 BT 模式** |
| `BTU_TASK` | `btu_init.c:48` (`4352 + 512`) | 堆 (Bluedroid BTU) | Core 0 / — | `4,864` | **仅 BT 模式** |
| `BTC_TASK` | `btc_task.c:97` & `btc_init` 反汇编 (`0xe00`) | 堆 (Bluedroid BTC) | Core 0 / — | `3,584` | **仅 BT 模式** |

- **A 栈合计（稳态，`main` 退出后）**：
  - **BT 模式**：应用任务 `20,992 B` + IDF 常驻 `11,264 B` + BT IDF `15,104 B` = **`47,360 B`（`46.25 KiB`）**（启动期含 `main` 峰值 `51,456 B`）。
  - **Wi-Fi 模式**：应用任务 `22,528 B` + IDF 常驻 `11,264 B` + Wi-Fi/lwIP/event IDF `9,984 B` = **`43,776 B`（`42.75 KiB`）**（`BootFlow` 执行 `WebModule::init()` 期间 `main` 尚未退出，峰值 `47,872 B`）。

### 3.2 固件 B（`P2_V3.1_IDF`）任务与栈清单

| 任务名 | 来源 (`file:line`) | 核 / 优先级 | 栈大小 (B) | 说明 |
| --- | --- | --- | ---: | --- |
| `main` (`P2_loop`) | `main.c:176-184`, `esp_task.h:57` | Core 0 / 1 | `4,096` | **永不退出**（主循环在 `main` 中运行） |
| `Loop_50ms` | `main.c:138-145` | Core 0 / 1 | `2,048` | 50 ms 周期任务 |
| `OLED` | `main.c:41-48` | Core 1 / 6 | `2,048` | 屏幕与按键刷新 |
| `GNSSRead` | `user_gnss_task.c:12-19` | Core 1 / 6 | `3,072` | GNSS 串口接收 |
| `GSMRead` | `user_gsm_task.c:12-19` | Core 1 / 5 | `4,096` | 4G 串口接收 |
| `spp_tx` | `xyz_bluetooth.c:134` | Core 0 / 10 | `4,096` | **仅 BT 模式**（切 Wi-Fi 时 `bluetooth_stop` 删除） |
| `httpd` | `xyz_webserver.c:92-96` | 无绑定 / 5 | **`16,384`** | **仅 Wi-Fi 模式**（`1024 * 16`, `max_uri_handlers=80`） |
| `IDLE0/1` + `Tmr Svc` + `ipc0/1` + `esp_timer` | `sdkconfig.json` + `esp_task.h` | — | `11,264` | 同 A（`3072+2048+2048+4096`） |
| `sys_evt` + `tiT` | `sdkconfig.json` + `esp_task.h` | — | `6,400` | **Wi-Fi 模式**（`2816 + 3584`） |
| `wifi` | `config_get_wifi_task_stack_size` 反汇编 (`0x401426a0`) | Core 0 / — | **`6,656`** | **Wi-Fi 模式**（启用了 WPA3 SAE：`6144 + 512`） |
| `btController` + `hciT` + `BTU` + `BTC` | `sdkconfig.json` + `btc_init` 反汇编 (`0xe00`) | Core 0 / — | `15,104` | **BT 模式**（`4096+2560+4864+3584`） |

- **B 栈合计**：
  - **BT 模式**：应用（含 `main`）`19,456 B` + IDF 常驻 `11,264 B` + BT IDF `15,104 B` = **`45,824 B`（`44.75 KiB`）**。
  - **Wi-Fi 模式**：应用（含 `main`）`31,744 B` + IDF 常驻 `11,264 B` + Wi-Fi/lwIP/event IDF `13,056 B` = **`56,064 B`（`54.75 KiB`）**。
  - 对比：Wi-Fi 模式下，A 的任务栈总额比 B **少 `12,288 B`**（主要是 B 的 `httpd` 16 KB vs A 4 KB、B 的 `wifi` 6,656 B vs A 3,584 B、以及 B 的 `main` 永驻）。

### 3.3 A 的 Web 端 Heap 显示 API 溯源

完整调用链（均直接读自源码）：
1. **采样点**：`main/modules/system/SystemModule.cpp:64-65, 117-118` 每轮 `poll()` 调用：
   - `status_.freeHeap = esp_get_free_heap_size();`
   - `status_.minFreeHeap = esp_get_minimum_free_heap_size();`
2. **Web 状态快照**：`main/modules/web/WebModule.cpp:182-183` 将其填充为 `snap.dev.heapFree` 与 `snap.dev.heapMin`。
3. **前端格式化**：`main/web/ui/app.js:137, 266` 使用 `kb(b) = (b / 1024).toFixed(1) + " KB"` 显示为 `Free memory: XX.X KB (min YY.Y KB)`（**即以 1024 换算的 KiB 数值**）。
4. **IDF 底层语义**：`$HOME/esp/esp-idf-v6.1/components/esp_system/port/esp_system_chip.c:65-77`：
   - `esp_get_free_heap_size()` → `heap_caps_get_free_size(MALLOC_CAP_DEFAULT)`
   - `esp_get_minimum_free_heap_size()` → `heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT)`
   - **口径结论**：
     - 取的是 **总空闲字节（free）与历史最低空闲字节（minimum free）**，**不是** `largest_free_block`（`largest` 仅在串口 `sys mem` / `sys heap` 打印，`SystemCmds.cpp:179,234`）。
     - `MALLOC_CAP_DEFAULT` (`esp_heap_caps.h:43`) 等价于可常规 `malloc/calloc` 的 **8-bit 可字节寻址堆（DRAM + D/IRAM 剩余区）**，**不包含** `4009C5AC` 处的纯 IRAM（`14,932 B`，仅 `MALLOC_CAP_EXEC`，`c7_boot3.log:20`），也不包含 RTC 内存。在无 PSRAM 的 ESP32 上，其数值与 `sys mem` 中的 `internal_8bit` 一致。

---

## 4. 运行期动态分配与已有实测对比

### 4.1 固件 A 启动后可分配堆池起点

来源：`c7_boot3.log:13-20`、`c7_ota.log:36-43`（硬件串口实测 `heap_init` 日志）：
- 上电 `heap_init` 初始可用 8-bit / DEFAULT 堆总计 **`194,904 B`（`190.3 KiB`）**：
  - `0x3FFAFF10` (`240 B`) + `0x3FFB6388` (`7,288 B`) + `0x3FFB9A20` (`16,648 B`) + `0x3FFD5CA8` (`41,816 B`) + `0x3FFE0440` (`15,072 B`) + `0x3FFE4350` (`113,840 B`)。
- **Wi-Fi 模式额外回收**（`BootFlow.cpp:210` 调用 `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)`）：
  - 实测代码（`bt.c:440-458, 1394, 1431-1459` 与 `esp_bt.h:24-50`）：释放 `_bt_controller_data` (`248 B`) + `_bt_controller_bss` (`2,716 B`) + `btdm_dram_available_region` 全部 7 段 (`6,192 + 25,480 + 6,688 + 52 = 38,412 B`)，共计 **`+41,376 B`（`+40.4 KiB`）**。
  - **关键缺口**：未调用 `esp_bt_mem_release(ESP_BT_MODE_BTDM)`（`bt.c:1461`），因此 Bluedroid Host 静态区 `_bt_data` (`4,576 B`) + `_bt_bss` (`18,472 B`) = **`23,048 B`（`22.5 KiB`）在 Wi-Fi 模式下仍永久占用、未归还堆池**。
  - 于是 A 在 Wi-Fi 模式下理论堆基线为 `194,904 + 41,376` = **`236,280 B`（`230.7 KiB`）**；而 BT 模式（`SppServerInitBt.c:62` 仅释放 `ESP_BT_MODE_BLE` 的 `4,148 B`）基线为 **`199,052 B`（`194.4 KiB`）**。

### 4.2 A 在 Wi-Fi + httpd（+ 未释放 BT Host）运行期动态占用拆解

以下为**代码确定值**与**IDF 内部估算值**分列：

| 模块 / 占用源 | 占用字节数 | 性质 | 依据与说明 |
| --- | ---: | --- | --- |
| **BootArena — `web.frames`** | `16,576 B` | 实测精确值 | `BootBuffers.cpp:58-59`，`WebModule.hpp:40`（`16 × sizeof(WebFrame)`，`WebFrame` 为 `1,036 B`） |
| **BootArena — `web.ota`** | `2,048 B` | 实测精确值 | `BootBuffers.cpp:169-170`，`WebOtaTransfer.hpp:14` |
| **BootArena — GNSS/RTCM** | `18,432 B` | 实测精确值 | `BootBuffers.cpp:133-162`：`gnss.demux` `4,096` + `rtcm.pipe` `4,096` + `gnss.app_ring` `4,096` + `gnss.latest` `6,144` |
| **BootArena — Debug/Trace** | `8,192 B` | 实测精确值 | `BootBuffers.cpp:123-130`：`trace.ring` (`256×16=4,096`) + `log.ring` (`4,096`) |
| **BootArena — OLED 帧缓冲** | `1,024 B` | 实测精确值 | `BootBuffers.cpp:181`，`OledModule.hpp:36` |
| **BootArena — 应用静态任务栈** | `14,336 B` | 实测精确值 | `task.link` `4,096` + `task.io` `4,096` + `task.trace_out` `3,072` + `task.oled` `3,072` |
| **Console REPL + UART0 驱动** | `~14.0–15.0 KB` | 确定主体+估算 | REPL 栈 `4,096 B` + UART0 TX `8,192 B` + RX `256 B` (`ConsoleUart.cpp:24,132`) + `linenoise`/`esp_console` |
| **GNSS UART2 驱动** | `~5.3 KB` | 确定主体+估算 | RX `4,096 B` + TX `1,024 B` (`GnssModule.hpp:45-46`) + 驱动结构体 |
| **IDF 任务栈（Wi-Fi 稳态）** | `21,248 B` + TCB | 实测精确值 | `IDLE0/1` `3,072` + `Tmr Svc` `2,048` + `ipc0/1` `2,048` + `esp_timer` `4,096` + `sys_evt` `2,816` + `tiT` `3,584` + `wifi` `3,584`（另 TCB 约 `~3.2 KB`） |
| **NVS 双分区缓存 (`nvs`+`spiffs`)** | `~4.5–5.0 KB` | 反汇编+估算 | `NvsStore.c:365,382` 挂载 5+32=37 扇区；`PageManager::load` 反汇编 `sizeof(Page)=84 B`，37 页 `mPages` 约 `3.1 KB` + `HashListBlock` (`128 B`/页) |
| **httpd 任务与控制结构** | `~7.5–8.5 KB` | 确定主体+估算 | `httpd` 栈 `4,096 B` + `httpd_data`/`sock_db[3]`/`hd_calls[15]`/`15` 条 URI 副本 + 3 socket + WS 会话 |
| **Wi-Fi 驱动（静态 RX/管理池+AP）** | `~25–30 KB` | 估算（SDK 闭源） | `6 × ~1.6 KB` 静态 RX 缓冲（`~9.6 KB`）+ `32` 管理帧 + 5 RX mgmt + AP/coex/NVS Wi-Fi 状态（含 1 台 STA 关联态） |
| **lwIP + netif + DHCPS + WebDns** | `~5–8 KB` | 估算 | `esp_netif` AP + DHCPS 租约表 + WebDns UDP socket + TCP PCB/pbuf 稳态 |
| **其余外设/启动开销** | `~2.5 KB` | 估算 | I2C master (`Sh1106`)、ADC、GPIO、BootArena 块头与对齐碎片（以及未释放的 BT Host `23,048 B` 已计入静态 `.data+.bss`） |

> **A 预期稳态剩余堆推算**：`236,280 B` − 上述已用合计 `~163–167 KB` ≈ **`~69–73 KB`（约 `~68–72 KiB`）**。
> **B 动态对比**：B 在 `c7_ota.log:11-14` 实测：BT 模式启动后 `heap size: 105,928 B`，切换 Wi-Fi + HTTP 服务后 `heap size: 80,556 B`；B 没有 `BootArena` 60 KB 预留，但 `httpd` 栈占 `16,384 B`、`wifi` 栈占 `6,656 B`、`main` 永驻 `4,096 B`、且 `xyz_websocket.c:44` 与 `user_web_from.c:372` 每帧/每次请求均有 `calloc`/`cJSON_Print` 堆震荡，且 `bluetooth_stop()` (`xyz_bluetooth.c:301-326`) 未释放 BT Host 静态内存。

### 4.3 与仓库既有实测和 ADR-0021 预算对比

| 里程碑 / 测量来源 | 模式 / 条件 | Free (B) | Min Free (B) | Largest (B) | 预算要求 (`HB.md:7`, `ADR-0021:16`) |
| --- | --- | ---: | ---: | ---: | --- |
| 主线 Wi-Fi 基线 (`HB.md:18`, `ADR-0021:16`) | Wi-Fi 启动后 | — | `58,624` | — | `>= 40,960 B` (`adr0017`) |
| `h1-idf` 调优前 (`HB.md:127`) | BT + 10 min RTCM | `58,112` | `54,956` | `57,344` | `>= 40,960 B` |
| `h1-idf` 调优前 (`HB.md:128`) | Wi-Fi + 1 客户端 600 s | `74,484` | `55,932` | `73,728` | `>= 40,960 B` |
| `h1-idf` 调优前 (`HB.md:129`) | Wi-Fi + 2 客户端 300 s | `73,820` | `55,932` | `69,632` | `>= 40,960 B` |
| `hb-doc` H1c 定稿 (`HB.md:130,177-179`) | Wi-Fi + 1 客户端 60 s | `86,836` | `70,180` | `86,016` | `>= 40,960 B` |
| `h1-wifi-off` (`HB.md:131`) | 关闭 Wi-Fi，启动 6 s | — | `80,776` | — | `>= 81,920 B` (`adr0011`, FAIL) |
| **`c7v-idf` 实测日志** (`c7_boot3.log:55`, `c7_ota.log:79`) | BT 模式 `boot done` | `~53,862` (`52.6 KB`) | — | — | `>= 40,960 B` |
| **`c7v-idf` 从 `hb-doc` 增量推算** | Wi-Fi + 1 客户端 | **推算 `~72–74 KB`** | **推算 `~55–57 KB`** | — | `>= 40,960 B` |
| **用户 CM680 实机反馈** | Wi-Fi + 1 PC 连 Web | **`40.x KB`** | **`23.x KB`** | 未测 | **已低于 `40,960 B` 红线** |

---

## 5. 为何 CM680 + Wi-Fi 下实测只有 `40.x KB` free / `23.x KB` min？

这里必须把**“已证实的 17 KB 瞬时峰值谷底（`free − min`）”**与**“相对 `c7v-idf` 推算多出的 ~30 KB 基线缺口”**严格分开：

### 5.1 为什么 `min` 比 `free` 低 ~17 KB（`40.x KB -> 23.x KB`）——已证实机制，非泄漏、非 CM680 特有

1. **与 `HB.md` 实测峰值差完全一致**：`HB.md:128,130` 中，`h1-idf` 的 `free − min = 74,484 − 55,932 = 18,552 B`（`18.1 KB`），`hb-doc` 的 `free − min = 86,836 − 70,180 = 16,656 B`（`16.3 KB`）。用户看到的 `40.x KB − 23.x KB ≈ 17 KB` 正好落在这个固定瞬时谷底区间。
2. **瞬时谷底的主要来源（启动与建链并发峰）**：
   - **Wi-Fi 动态 RX/TX 与 lwIP pbuf 突发**：`ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=12` + `DYNAMIC_RX_BUFFER_NUM=12`（每包 ~1.6 KB）+ lwIP `TCP_SND_BUF=5760` / `WND=5760`。电脑连上 SoftAP 后打开首页时，浏览器会并行发起 `GET /`（传输 `18,475 B` `index.html.gz`）、`/favicon.ico`、系统联网/门户探测（`/generate_204`、`/ncsi.txt`、`/connecttest.txt` 等）以及 `/ws` WebSocket 升级，短时触发多个 TCP PCB、`pbuf` 和 Wi-Fi 动态发送缓冲同时在途。
   - **启动任务 `main` 未退出前叠加 Wi-Fi/httpd 初始化**：`WebModule::init()` 在 `BootFlow.cpp` 的 `app_main()` 中执行，`main` 任务栈（`4,096 B` + TCB）直到 `BootFlow.cpp:624` 返回后才释放，这意味着 Wi-Fi + httpd 启动瞬间比稳态多占约 `4.4 KB`。
   - **httpd 请求暂存与启动期短暂分配**：`httpd_req_aux.scratch` 初始按 `max_uri_len=512 B` 分配并按头长度 `realloc` 到 `1024 B`；另有 `BootFlow` / `nvs_flash_init` / `esp_wifi_init` 初始化期临时对象。
3. **排除了 CM680 特有泄漏与 Web 稳态泄漏**：
   - 检查 `GnssModuleProbe.cpp:641`、`GnssModule.cpp`、`GnssBaseConfig.cpp`、`SettingsBackend.cpp`、`LegacyMigration.cpp`：CM680 分支**没有任何额外堆分配或大缓冲**，与 UM980/F9P 共用完全相同的 `BootBuffers`（`gnss.demux` / `rtcm.pipe` / `gnss.app_ring` / `gnss.latest` / UART2）。
   - 检查 `WebModule.cpp` / `WebServer.cpp` / `WebOta.cpp`：运行期 WS 收发全部复用 `BootArena` 预先分配的 `web.frames`（`16,576 B`）和 `web.ota`（`2,048 B`），发送采用 `httpd_ws_send_frame_async`（`WebServer.cpp:633`，不分配 `async_transfer_t` 和 `EventGroup`），无每帧堆分配；且 `Free memory` 稳定在 `40.x KB` 而非持续下跌，符合“瞬时峰拉低 `min`”而非持续泄漏。

### 5.2 为何基线 `free` 停在 `40.x KB`（而非 `hb-doc` 的 `86.8 KB` 或推算的 `~72 KB`）

1. **代码已证实的永久占用（解释了为何比理想上限少 ~40+ KB，存在于所有 `c7v-idf` Wi-Fi 构建）**：
   - **最大单笔浪费 — Bluedroid Host 未释放 (`23,048 B`)**：`BootFlow.cpp:210` 仅调用 `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)`，导致 `_bt_data` (`4,576 B`) + `_bt_bss` (`18,472 B`) = `23,048 B` 在 Wi-Fi 模式完全白白占着静态 DRAM。
   - **从 `hb-doc` 到 `c7v-idf` 新增的确认开销 (~13–14 KB)**：静态 DRAM `+3,000 B`（`95,627 -> 98,627 B`）、`BootBuffers` 新增 `web.ota` (`2,048 B`) + `task.oled` (`3,072 B`) + `oled.fb` (`1,024 B`) = `+6,144 B`、C6 挂载 32 扇区 `spiffs` NVS 分区约 `+3.2–3.8 KB`、W7 强制门户 UDP/URI 与 OL1 I2C 约 `+0.6–1.0 KB`。
   - **常驻开发/调试缓冲**：`web.frames` `16,576 B` + `RTK_CONSOLE_UART_TX_BUF` `8,192 B` + `Trace/Log` (`task.trace_out`+`trace.ring`+`log.ring`) `11,264 B` + `rtcm.pipe`（Wi-Fi 模式未用但仍分配）`4,096 B` + `coredump` `908 B`。
2. **相对推算值 `~72 KB` 仍多出的 ~30 KB 差异——待上板核实假设**（当前无该 CM680 实机 `sys mem` 串口输出，不能武断定因）：
   - **假设 A（最可能）**：机台上烧录的构建与当前 `builds/c7v-idf` 不完全一致（例如未启用 `sdkconfig.defaults` 的 H1c 调优项导致 `STATIC_RX=10`/WPA3/IPv6 回弹、或开启了额外诊断/堆调试选项）。
   - **假设 B**：该板在 `stepEarlyConfig` 触发了 `withNvsBootScratch`（`BootFlow.cpp:155-162` 仅在 NVS 恢复时借用 `2,560 B`，正常会立即归还；主要影响 `min` 不影响稳态 `free`），或者电脑端（Windows/macOS NCSI + 浏览器多连接 + 后台重传）在观察瞬间仍占着多路 TCP/Wi-Fi 动态发送缓冲。
   - **假设 C**：`hb-doc`（`86.8 KB`）与当前 `c7v-idf` 之间除了我们统计到的模块变化，IDF v6.1 Wi-Fi/netif/NVS 或 `spiffs` 内实际条目数较多导致 NVS `HashListBlock` 与 Wi-Fi/lwIP 池实占高于估算（`spiffs` 若大量历史页含条目，每页额外挂 `HashListBlock` `128 B` × 块数）。
   - **现场 10 秒定位指令**：通过 2 Mbaud 串口采集开机 `heap_init` 与 `boot done` 日志，并执行 `sys mem`、`sys heap`、`sys tasks`、`radio`，对比 `internal_8bit total` 是否为 `236 KB` 即可立判是“配置/版本差异”还是“运行期某模块实占超预期”。

---

## 6. 按收益排序的优化建议（不实施，仅供评估）

| 优先级 | 优化项 | 预计节省 (B) | 风险 | 需修改位置（Kconfig / 代码） |
| :---: | --- | ---: | --- | --- |
| **1** | **Wi-Fi 模式改为释放全部 BT 内存**（Controller + Bluedroid Host） | **`+23,048 B`**（静态 DRAM 归还堆） | **低**（Wi-Fi 模式切回 BT 必重启；`BootFlow.cpp:193` 已禁止调用 `esp_bt_*`） | `main/app/BootFlow.cpp:210`：`esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` → `esp_bt_mem_release(ESP_BT_MODE_BTDM)` |
| **2** | **按需关闭/精简 Console**（仅缩 TX 缓冲或启用无控制台/精简档位） | **`+6,144 B`**（缩 TX 缓冲 `8192→2048`）或 **`~13–14 KB`**（`NONE` 档位） | **低**（缩 TX 缓冲仅影响长打印阻塞）；**中**（`NONE` 档失串口 CLI） | `sdkconfig.defaults:64`：`CONFIG_RTK_CONSOLE_UART_TX_BUF=2048`；或 `CONFIG_RTK_CONSOLE_PROFILE_NONE=y` (`main/Kconfig.projbuild`) |
| **3** | **生产构建关闭运行时 Trace 环**（或缩小 `log.ring`） | **`+11,264 B`**（全关）或 **`+2,048 B`**（`log.ring` 减半） | **中**（发布版失去异步 `trace` 诊断与 `debug` 命令组） | `main/Kconfig.projbuild:55`：`CONFIG_RTK_TRACE_ENABLE=n`（移除 `task.trace_out` 3072 + `trace.ring` 4096 + `log.ring` 4096）；或 `CONFIG_RTK_LOG_RING_BYTES=2048` |
| **4** | **缩减 `web.frames` 帧池容量**（`16 → 8` 或 `10` 帧） | **`+8,288 B`**（8 帧）或 **`+6,216 B`**（10 帧） | **低–中**（须配合 `web_load.py` 验证 2–3 客户端并发与 `kTxReservedCtrlSlots` 不丢帧） | `main/modules/web/WebModule.hpp:35-40`：调整 `kRxMailboxCap` / `kTxMailboxCap` / `kScratchFrames`（每帧 `1,036 B`） |
| **5** | **Wi-Fi 模式跳过分配未使用的 `rtcm.pipe`** | **`+4,096 B`** | **极低**（`RtcmModule` 在 `module_list.inc:25` 为 `kBtOnly`，须保证 `GnssModule` 允许未绑定 `rtcmPipe`） | `main/app/BootBuffers.cpp:137-140` 与 `main/modules/gnss/GnssModule.cpp:118` |
| **6** | **按 BT/Wi-Fi 实测余栈下调任务栈与中断栈**（依据 `HB.md:133` HWM：`link` 留 `2,416`、`io` 留 `1,744`、`console` 留 `2,212`） | **`+3,168 B` ~ `+4,192 B`** | **中**（须在 CM680 + Wi-Fi + OTA + 2 客户端下重测 `sys tasks` 余栈） | `main/AppContext.hpp:82,84`（`4096→3072`）；`sdkconfig` `RTK_CONSOLE_STACK_SIZE=3072`；`FREERTOS_ISR_STACKSIZE=1536`（`+1,120 B`） |
| **7** | **缩减 `gnss.app_ring` / 复用 `web.ota` 2 KB 缓冲** | **`+2,048 B` ~ `+4,096 B`** | **低–中**（`web.ota` 按需借用或上传时再申请需防碎片；`app_ring` 需查 `highWaterBytes`） | `CONFIG_RTK_GNSS_APP_RING_BYTES=2048`（`main/Kconfig.projbuild`）；`main/app/BootBuffers.cpp:169` / `WebOtaTransfer.hpp:14` |
| **8** | **Wi-Fi / lwIP / Coredump 细调（降低突发峰值 `min`）** | **`+1,000 B` ~ `+5,000 B`**（稳态+峰值） | **低–中**（关闭 Coredump 失事后转储；缩 `DYNAMIC_RX/TX` 需复测吞吐） | `sdkconfig.defaults:10` (`ESP_COREDUMP_ENABLE_TO_NONE=y`, `+908 B`)；`ESP_WIFI_MGMT_SBUF_NUM=16`、`DYNAMIC_RX/TX_BUFFER_NUM=8`、`RX_BA_WIN=4` (`HB.md:84-87`)；`LWIP_MAX_ACTIVE_TCP=8` |
| **9** | **合并/延迟卸载 `spiffs` NVS 分区**（仅当历史兼容不再需要独立 32 扇区 NVS 时） | **`~2,700 B` ~ `3,500 B`** | **高**（`partitions.csv` 改动会破坏 `ADR-0022` 与 V3.1 OTA 兼容性） | `components/nvs_store/NvsStore.c:365` / `partitions.csv:7` |

> **综合预期**：仅实施**高确定性、低风险**的第 **1 + 4（10 帧）+ 5 + 2（TX 2048）** 项，即可在不关闭 Trace、不改分区表、不裁剪核心功能的前提下稳定增加 **`~39.5 KB`** 可用堆，使 `c7v-idf` Wi-Fi 稳态空闲与 `min free` 全部回到远高于 `40,960 B` 门槛的安全区间。
