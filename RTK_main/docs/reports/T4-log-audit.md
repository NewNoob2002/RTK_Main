# T4 日志通道审计（ADR-0009 §2.11）

审计范围：`components/**` 与 `main/modules/**` 中既有的诊断输出方式（`ESP_LOG*`、`fw::trace`）与
ADR-0009 的规则差距。统计为调用点数量（`grep -c` 计数），不是“行数”。

| 组件 / 模块 | 已有日志 | 缺口 | 本次补齐 | 备注 |
| --- | --- | --- | --- | --- |
| `components/spp_server`（C 驱动 + 适配器） | `ESP_LOG*` 42 处、trace 事件 3 类（经 `spp_server_set_trace_hook`） | 驱动层 `ESP_LOG` 位于蓝牙回调与发送路径，违反 ADR-0009「回调/发送任务不新增 ESP_LOG」 | 无（未改） | 既有调用早于本 ADR；迁移需要重排回调内的格式化缓冲，超出 T4 卡片范围，列为后续任务。trace 事件已覆盖会话开关、写失败、配对窗口 |
| `components/link_core` | trace 事件 38 处（会话、路由、去重、限速、编码） | 无（事件已覆盖状态与错误） | 无 | 纯组件，禁用 IDF 头，只能 `fw::trace` |
| `components/device_proto` | 无 | 无（纯协议核心；拒绝原因由分流器 `explain()` 生成） | 无 | C 核心与 C++ 外观都不允许依赖 IDF 头 |
| `components/rtos` | `ESP_LOG*` 5 处、trace 事件 4 类（执行器超时/投递失败） | 栈溢出/看门狗路径的文本只有 `ESP_LOG` | 无 | 执行器已把可恢复失败转成 trace 事件 |
| `components/nvs_store` | `ESP_LOG*` 3 处 | 无实质缺口（启动期一次性失败，速率非实时） | 无 | 只依赖 `nvs_flash`、`log` |
| `components/cli` | `ESP_LOG*` 12 处 | 无（控制台输出走 `cli::out_*`，`ESP_LOG` 只用于驱动初始化失败） | 无 | 保持可被其他产品复用 |
| `components/bt_reconnect` | 无 | 无（纯状态机，事件由 main 侧 trace 记录） | 无 | 禁止依赖 IDF 头 |
| `components/gnss` | 无 | 无（纯解析/控制器，不依赖 app_fw） | 无 | 诊断在 `main/modules/gnss` 侧上报 |
| `main/modules/gnss` | trace 事件 9 类（`GnssTrace`）14 处调用 | 逐帧解码结果、拒绝原因、控制器下发命令文本、状态迁移文本、波特率切换、探测命中、未知字节无人类可读输出 | **`fw::tlog` 10 处 + `tlog_hex` 3 处 + `DemuxSink::on_reject` → `handle_reject`**：`GNSS <tag>, N bytes`、`GNSS <decoder> rejected N bytes: <explain>`（仅 `last`）、`GNSS unknown N bytes dropped`、`GNSS <model|probe> <- "<cmd>"`、`GNSS state A -> B`、`GNSS detected <model> at <baud>`、`GNSS baud N (was M)` | 实时路径保持 `trace_gnss`；文本走 io 执行器（非实时） |
| `main/modules/bt_link` | `ESP_LOG*` 7 处、trace 事件 5 类 | `ESP_LOGE` 出现在分流投递与回调相关路径，违反 ADR-0009 | 无（未改） | 迁移需把错误文本改成 trace 的 `reason/result` 字段；列为后续任务 |
| `main/modules/rtcm` | `ESP_LOG*` 2 处 | 批次截断只有 `ESP_LOGE`，无 trace 事件 | 无（未改） | 需要新增 `RtcmTrace` 事件码，属契约变更，T4 不改 |
| `main/modules/storage` | `ESP_LOG*` 8 处、trace 事件 6 类 | 白名单装载失败仅有 `ESP_LOGW` | 无（未改） | 同上，新增事件码需要同步 `trace_text` 表 |
| `main/modules/system` | `ESP_LOG*` 4 处 | 无 trace 覆盖 | 无（未改） | 启动期一次性输出 |
| `main/modules/debug` | trace 输出 28 处（格式化、命令、统计） | — | **新增文本日志通道**：`fw::LogRing` + 两个环的按时间归并输出、hex 转储、丢弃计数、`debug on <cat...> [hex]`、`dbg/hex` 键 | 低优先级任务输出，全部 `cli::out_*` 整行写 UART |

## 结论

1. 实时路径（链路任务、发送任务、蓝牙回调）本次没有新增任何 `ESP_LOG`/`printf`，文本诊断统一经
   `fw::tlog` → `fw::LogRing` → DebugModule 低优先级任务输出。
2. 既有 `ESP_LOG` 集中在 `spp_server`、`bt_link`、`storage`、`rtcm`、`system`，其中违反 ADR-0009 的
   是回调/发送路径上的调用；本次未改，原因见上表备注（需要新增 trace 事件码或重排回调缓冲，
   属契约与结构调整，超出 T4 卡片范围）。
3. `device_proto`、`link_core`、`bt_reconnect`、`gnss`、`stream_demux`、`app_fw` 仍然不含 IDF 头，
   纯组件约束未被本次改动破坏。
