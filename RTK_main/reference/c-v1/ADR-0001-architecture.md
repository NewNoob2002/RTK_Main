# ADR-0001 RTK_main 架构（阶段 1：蓝牙 + 手机/报警器通信）

- 状态：**Approved 2026-11-10（用户批准，默认 CONCURRENT，PHONE_PREEMPT 为降级）**；消息 ID 0x40–0x44 暂用，待确认不与 App 厂商冲突
- Profile：`mcu-rtos`（ESP32 + FreeRTOS/ESP-IDF），C 语言，BARR-C:2018
- 评审深度：Full（新固件、SDK 从 Arduino 迁到原生 IDF v6.1）

## 1. 上下文

| 项 | 内容 | 类别 / 来源 |
| --- | --- | --- |
| 芯片 | ESP32 rev ≥ 3.1，8 MB Flash，PSRAM 未知 | 事实：E1-5.4/sdkconfig |
| SDK | ESP-IDF v6.1 | 事实：`~/esp/esp-idf` `git describe` |
| 旧固件 | E1-5.4：Arduino-esp32 3.2 + SparkFun RTK 派生，SPP 从机，私有帧协议 | 事实：源码 |
| 手机协议 | 20 B 头 + payload + CRC-32，见 `docs/protocol/phone_protocol.md` | 事实：逆向 5.4 源码 |
| 报警器 | 原版 ESP32，经典蓝牙 SPP 主动连接 RTK，电池供电带开关，WebServer 配置 RTK MAC | 用户需求（会话记录） |
| 并发会话 | IDF 原生 SPP 服务端支持多会话：`ESP_SPP_MAX_SESSION = BTA_JV_MAX_RFC_SR_SESSION = MAX_BD_CONNECTIONS` | 事实：IDF v6.1 `btc_spp.h:18`、`bta_jv_api.h:63`、`bt_target.h:2028` |
| ESP32 同时做两个 piconet 的从机（scatternet） | `UNKNOWN`，需实测（HIL-3） | 假设 |
| 5.4 已知缺陷 | 解析器每轮重建（分包丢帧）；设备名带 `\n`；设置命令无长度检查；Arduino BluetoothSerial 只允许一个客户端 | 事实：源码 |

## 2. 拓扑

```
            ┌──────────── main（app，组合根）──────────────┐
            │ app_main: store_init → 读 SN/白名单 → link/cmd 初始化 │
            │ link_task (core1, prio 10): 唯一拥有 bt_spp 读写      │
            │ console (UART0 REPL): 测试命令                         │
            └───────┬───────────────┬──────────────┬─────────┘
                    │               │              │
        components/bt_spp     components/store   纯 C 模块（host 可测）
        (driver: Bluedroid    (driver: NVS)      proto ← cmd → link
         SPP 多会话)                               └→ alarm
```

数据流（接收）：Bluedroid 回调 → 会话 StreamBuffer → 事件队列 → link_task → `proto_parser_feed`（每会话一个常驻解析器）→ `link_mgr_on_frame`（角色判定）→ `cmd_disp_handle` → `bt_spp_write`。

周期（link_task 内）：每 100 ms `link_mgr_tick`；每 `ALARM_PUSH_PERIOD_MS` 向所有 ALARM 会话推送 0x40。

## 3. 备选方案与评估

| 维度 | A：原生 IDF SPP 多会话（手机与报警器并存） | B：SPP 单会话 + 手机优先抢占 | C：ESP-NOW 连报警器 |
| --- | --- | --- | --- |
| 硬件效率 | Good：同一 BR/EDR 控制器，不开 WiFi | Good | Trade-off：需开 WiFi，堆占用增加 |
| 复杂度 | Trade-off：需要会话/角色管理 | Good | Trade-off：两套无线栈 + 配对 |
| 可维护性 | Good：纯 C 策略模块可 host 测试 | Good | Trade-off |
| 实时性 | Unknown：两条 ACL 分时，需实测手机吞吐 | Good | Good |
| 健壮性 | Unknown：scatternet 稳定性未测 | Trade-off：报警器在线时手机需抢占 | Good |
| 与报警器方案匹配 | Good：报警器已选 SPP 主机 | Good | Problem：报警器方案需重做 |
| 结论 | **Proceed（默认）** | **作为降级策略保留**：`link_policy_t` 一行切换 | Reject（本阶段） |

决策：实现 A，同时在 `link_mgr` 中实现 B 的策略（`LINK_POLICY_PHONE_PREEMPT`），由 Kconfig/编译期常量选择。HIL-3 若证明 scatternet 不稳定，切换到 B，不改代码结构。

## 4. 接口（冻结）

| 模块 | 头文件 | 职责 | 失败语义 |
| --- | --- | --- | --- |
| proto | `components/proto/include/proto_frame.h` | CRC、组帧、逐字节解析 | 返回码 + 错误计数器，永不越界 |
| link | `components/link/include/link_mgr.h` | 会话角色、白名单、配对窗口、超时 | 通过 `link_actions_t` 要求断开 |
| alarm | `components/alarm/include/alarm_msg.h` | 0x40/0x41 编解码、报警状态源（测试覆盖 > 围栏） | 非法输入返回 0/false |
| cmd | `components/cmd/include/cmd_disp.h` | 按角色分发命令、生成应答 | `CMD_RES_DENIED/NO_REPLY` |
| bt_spp | `components/bt_spp/include/bt_spp.h` | IDF SPP 封装，回调只拷贝数据 | `esp_err_t`，初始化失败逆序释放 |
| store | `components/store/include/store_nvs.h` | SN、白名单持久化 | `esp_err_t` |

## 5. 任务与资源预算

| 任务 | 核 | 优先级 | 栈 | 说明 |
| --- | --- | --- | --- | --- |
| Bluedroid BTC/BTU | 0 | IDF 默认 | IDF 默认 | `CONFIG_BT_BLUEDROID_PINNED_TO_CORE_0` |
| `link_task` | 1 | 10 | 6144 B（实测后收紧到高水位 ×1.5） | 静态创建 `xTaskCreateStaticPinnedToCore` |
| console REPL | 任意 | IDF 默认 | 4096 B | `esp_console_new_repl_uart` |

| 资源 | 目标 | 测量方法 |
| --- | --- | --- |
| Flash（app） | ≤ 1.2 MB（槽 1.625 MB） | `idf.py size` |
| 空闲堆（启动后、两会话连接时） | ≥ 80 KB | 日志 `esp_get_free_heap_size`/`esp_get_minimum_free_heap_size`，HIL |
| link_task 栈余量 | ≥ 30 % | `uxTaskGetStackHighWaterMark`，HIL |
| 应答时延（手机查询 → 应答） | ≤ 100 ms | HIL 抓包/日志时间戳，`Unknown` 待测 |

## 6. 预留（本阶段不实现，只保留位置）

| 功能 | 5.4 来源 | 预留方式 |
| --- | --- | --- |
| GNSS UM980（UART2：RX16/TX17，电源 25） | `GNSS_UM980.cpp` | `cmd_dev_info_t` 占位；0x04/0x05/0x06 返回 5.4 占位值 |
| 电台（UART：RX32/TX33，电源 4，配置 5） | `Radio.cpp` | 0x14 配置只存 RAM |
| 电池/充电（I2C SDA21/SCL22，充电状态 35） | `Battery.cpp` | `battery_pct = 0xFF` |
| LED（27/14/13/15/2）、按键（39/12）、电源保持 26 | `LED.cpp`、`Begin.cpp` | 无 |
| SD 日志（SPI 18/19/23） | `NVM.cpp` | 0x15 配置只存 RAM |
| 电子围栏 | 无（App 侧） | `alarm_src_set_fence()` 钩子；0x43 回 `02 02` |
| OTA | 分区表已含 ota_0/ota_1 | 无 |

**WARNING**：引脚 26（`pin_mcu_on_power`）在 5.4 中用于电源保持。本阶段固件不驱动它，若硬件需要上电后拉高才能保持供电，设备会在松开按键后断电。该行为 `UNKNOWN`，HIL 前须向硬件负责人确认；必要时作为单独任务加入（需授权）。

## 7. 验证计划

见 `docs/acceptance.md`。

## 8. 决策

**Proceed**，用户已于 2026-11-10 批准。
