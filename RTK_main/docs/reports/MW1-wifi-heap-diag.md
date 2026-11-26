# MW1 Wi-Fi 堆诊断与内存回收

基线 `8dedf5a`，分支 `feat/mw1-wifi-heap`。目标为经典 ESP32，使用本机 ESP-IDF v6.1 和 DEV 控制台档。

## 改动与契约

| 文件 | 改动 |
| --- | --- |
| `main/modules/web/WebDiagnostics.{cpp,hpp}`、`WebLogic.{cpp,hpp}`、`WebModule.{cpp,hpp}`、`WebServer.cpp` | 周期状态新增 `heap_largest`、`heap_int_free`、`heap_int_min`。复用 `cmd` 提供显式 `diag`（两种能力堆及 BootArena）与 `diag.tasks`（任务栈）。帧池 4 RX + 8 TX + 4 工作槽缩到 2 RX + 4 TX + 4 工作槽。 |
| `main/web/ui/app.js`、`tools/web_mock/web_mock.py`、`components/web_proto/src/Request.cpp`、`docs/web/ws-protocol.md` | Device 卡显示最大连续空闲块，模拟状态和协议文档同步。旧字段与请求格式兼容，协议仍为 v1。 |
| `main/app/BootFlow.cpp`、`RadioMode.hpp`、`main/modules/system/SystemModule.cpp` | Wi-Fi 启动在 BootArena 分配前调用 `esp_bt_mem_release(ESP_BT_MODE_BTDM)`，失败沿用降级路径。SPP 入口追加射频模式检查。 |
| `sdkconfig.defaults` | UART0 TX 从 8,192 B 降到 2,048 B。 |
| `tools/main_host_check/{BootAllocCheck,WebModuleCheck,WebServerCheck}.cpp`、`CMakeLists.txt`、`web_shim/esp_heap_caps.h`、`web_shim/freertos/freertos_debug.h` | 覆盖新增状态字段的差分、诊断请求和回包、帧池满载、控制帧预留、三客户端轮换及 Wi-Fi 的 RTCM 管道。 |

显式请求将完整堆统计、台账遍历和任务栈填充区扫描移出周期推送。两种诊断各回一帧，保留 1,024 B 帧限制。任务表上限 16 个已知 Wi-Fi 常驻任务名，当前存在的条目用 `uxTaskGetStackHighWaterMark` 采样。栈容量来自 IDF 对齐后栈首尾。短命任务（如启动任务 `main`）或未列名的 SDK 新任务可能只反映在 `task_count` 中。暂停调度只覆盖指定任务的快照读取，句柄寿命由 Wi-Fi 启停顺序保证。

4 个 TX 槽中预留 2 个供 ACK 和后续 config/bye，周期状态和诊断成功帧共用另外 2 个槽。积压时已有控制帧暂存与重调路径继续生效，各客户端轮换使用周期状态配额。RX 满时返回同 ID 的 `busy`。

## 蓝牙和 UART 源码核对

- `stepRadio` 早于 SPP 工作区绑定、系统启动及执行器启动。Wi-Fi 模式跳过 SPP 绑定、启动、停止、启动摘要取址及 `kBtOnly` 模块。`SystemModule::startBt` 自身也检查模式。公共 Debug 启动路径安装的 SPP trace 指针 `s_spp_trace` 及其锁位于应用的普通 DRAM，链接图确认两者在回收区之外。切回蓝牙走重启。
- `SystemModule::fillIdentity` 的 `esp_read_mac(..., ESP_MAC_BT)` 走 ESP-IDF `esp_hw_support/mac_addr.c`，从硬件 MAC 和独立表生成地址。早期 NVS 加载、网页白名单及下次配对请求使用应用配置项。网页恢复出厂的 `bt_config.conf`、`bt_config` 清理走 `NvsStore.c` 的通用 NVS namespace 擦除。上述路径均无需 Bluedroid 配置 API。
- IDF `components/bt/controller/esp32/bt.c` 的 BTDM 释放先处理控制器 ROM 区，再将 `_bt_bss`、`_bt_data` 及对应 controller 区归还能力堆。链接图中的两段 Bluedroid 静态区共 23,048 B（4,576 + 18,472）。
- `ConsoleUart.cpp` 仍使用 `uart_write_bytes`。IDF `components/esp_driver_uart/src/uart.c` 的 `uart_tx_all` 获取 TX 锁后循环分块，环满时等待 ISR 腾出空间，环发送等待值为 `portMAX_DELAY`，有效写调用返回原请求长度。缩环影响等待时间。实机字节完整性和上游 trace/log 环的丢弃计数须分别验证。

## 资源对比

`idf.py size` 和 `.bin` 大小，单位 B。同一构建档的 A-only 镜像用于在整机上建立带诊断的回收前基线。

| 项目 | `8dedf5a` | A-only | A+B | A+B 相对基线 |
| --- | ---: | ---: | ---: | ---: |
| Flash Code | 1,144,724 | 1,146,180 | 1,146,504 | +1,780 |
| Flash Data | 275,820 | 276,156 | 276,140 | +320 |
| IRAM | 116,139 | 116,139 | 116,139 | 0 |
| 链接 DRAM | 98,627 | 98,675 | 98,675 | +48 |
| 总镜像估计 | 1,589,998 | 1,591,838 | 1,592,146 | +2,148 |
| OTA `.bin` | 1,590,112 | 1,591,952 | 1,592,256 | +2,144 |

链接 DRAM 增量包含状态快照。单镜像在运行时做射频选择，蓝牙静态段仍在镜像中。Wi-Fi 启动预计归还其中 23,048 B，扣除 48 B 链接增量后，Wi-Fi 模式的常驻静态 DRAM 预计净少 23,000 B。再计入帧池 6,216 B 和 UART TX 6,144 B，空闲堆预计净增约 35,360 B。分配器头部、对齐和网络瞬时负载需要实测。

P2 v1.41 的正常 Wi-Fi 模式 BootArena 请求量，按 `BootBuffers.cpp` 和两次构建的结构容量计算，单位 B。

| 分区 | 回收前 | 回收后 |
| --- | ---: | ---: |
| `task.link` | 4,096 | 4,096 |
| `task.io` | 4,096 | 4,096 |
| `task.trace_out` | 3,072 | 3,072 |
| `trace.ring` | 4,096 | 4,096 |
| `log.ring` | 4,096 | 4,096 |
| `gnss.demux` | 4,096 | 4,096 |
| `rtcm.pipe` | 4,096 | 4,096 |
| `gnss.app_ring` | 4,096 | 4,096 |
| `gnss.latest` | 6,144 | 6,144 |
| `web.frames`（每槽 1,036 B） | 16,576 | 10,360 |
| `web.ota` | 2,048 | 2,048 |
| `task.oled` | 3,072 | 3,072 |
| `oled.fb` | 1,024 | 1,024 |
| 合计 | 60,608 | 54,392 |

整机 allocator 的 `actual` 和峰值堆尚未读取。任务卡引用的 41,996–43,712 B free 和 29,436 B min 是 8454efc 历史样本。当前有约 23 KB 的静态蓝牙占用证据，其余缺口的运行期来源待比较 A-only 与 A+B 诊断。

## HIL 步骤

1. 使用同一 P2 v1.41 + CM680、同一客户端和网页负载。由用户通过 Wi-Fi OTA 页面上传 `builds/mw1-diag-before/rtk_main.bin`，重连设备热点，确认 `GET /ota/info` 的运行槽通过验证。已有基线固件时可先记录旧 `status.dev`。
2. 进入 `http://192.168.10.12/`，用一个 WebSocket 采集连续 60 s 的状态。待 `hello/config/status` 后逐条发送下列诊断请求，收到同 ID 的成功帧后再发下一条。遇到 `busy` 时待 TX 排空后重试。记录各任务的栈和余栈、BootArena 每项 `req/actual`、两个能力堆组和 `status.dev` 的对应数据。

   ```js
   const ws = new WebSocket("ws://192.168.10.12/ws");
   const frames = [];
   ws.addEventListener("message", ({data}) => {
     const frame = JSON.parse(data);
     if (["status", "diag", "diag.tasks", "ack"].includes(frame.t)) frames.push(frame);
   });
   // 初始状态到达后分两次执行，各自等待同 id 应答
   ws.send(JSON.stringify({t: "cmd", id: 1, name: "diag"}));
   ws.send(JSON.stringify({t: "cmd", id: 2, name: "diag.tasks"}));
   ```

3. OTA 上传 `builds/mw1-idf/rtk_main.bin`，同条件重复采集。比较相同稳态时段 `heap_free`（目标至少 +25 KB）、`heap_min`、`heap_largest`、`heap_int_free`、`heap_int_min`、任务余栈和台账。确认 `web.frames.req=10360`、`rtcm.pipe.req=4096`，检查 `task_count` 与 `tasks` 条目的差异。
4. 通过三条 WS 连接确认握手和状态轮换。用 `tools/hil/web_load.py --host 192.168.10.12 --duration 60` 另跑负载，检查设置应答和断开后恢复。执行网页退出并验证重启后 SPP 恢复连接。同机 OTA 前后保留浏览器样本与 `/ota/info` 结果。
5. UART 字节级验证在另有 UART0 接入的开发板进行。两版分别连续执行 `sys tasks`，用 `debug trace on all` 产生持续 `trace_out` 输出，捕获超过 2 KiB 的输出并检查整行、命令末尾 `OK`、阻塞时长及 `debug list` 的上游环丢弃计数。整机的 Type-C 无 UART0。

## 验证记录

- `FAIL` 完整门禁 `tools/ci/run_gates.sh --prefix mw1 --base 8dedf5a` 共 42 项 PASS、1 项 FAIL，见 `builds/mw1-logs/final-gates.log`。G1 唯一差异是任务明确要求的 `sdkconfig.defaults` TX 缓冲值。按 AGENTS.md，`docs/frozen.sha256` 由 Architect 验收后重算。
- `PASS` G2/G2b 的 14 组普通主机和 14 组 ASan/UBSan 测试，其中 `main_host_check` 两个变体各通过 205 项。初次沙箱运行遇到 LeakSanitizer 的线程检查错误，最终在沙箱外运行并保留泄漏检测。
- `PASS` G3 的 7 项 ESP32 固件配置、G3b 的 ESP32 与 ESP32-C3 测试固件，均无编译警告。默认构建生成的 `sdkconfig.h` 与基线只差 UART TX 缓冲值。
- `PASS` G5 格式、G5b clang-tidy（162 个文件，0 发现）、G6 函数注释（缓存中的 lizard，15 个函数全部合格）、G7b 堆分配及 G7c 无控制台依赖检查。
- `PASS` 基线与 A-only 的 `idf.py build`、`idf.py size`。日志在 `builds/mw1-baseline/` 与 `builds/mw1-diag-before/`。
- `PASS` 最终 `idf.py -B builds/mw1-idf size`，日志为 `builds/mw1-logs/mw1-idf.size.log`。
- `PASS` `node --check main/web/ui/app.js`、Python `ast.parse` 检查 Web mock、`python3 -I tools/hil/web_load.py --selftest`、`git diff --check`。
- `NOT_RUN` 整机 OTA/HIL、串口捕获。当前无整机诊断样本。
