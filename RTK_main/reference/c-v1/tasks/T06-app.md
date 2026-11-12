# T06 应用组合根、链路任务与测试控制台

```yaml
task: T06-app
observable_outcome: >
  idf.py build 0 warning；idf.py size 中 app ≤ 1.2 MB；
  main/ 中实现链路任务与控制台命令，满足下列行为（由 HIL 验证）。
frozen_inputs: [全部公共头, docs/adr/ADR-0001-architecture.md §2 §5, docs/protocol/phone_protocol.md]
owner: omp/DeepSeek-V4.1-Flash
outputs: [main/rtk_main.c, main/app_link.c, main/app_link.h, main/app_cons.c, main/app_cons.h, main/CMakeLists.txt, docs/reports/T06.md]
affected_checks: [idf.py build, idf.py size, barrc check.sh main/*.c main/*.h, host ctest 回归]
stop_conditions: [需要修改任何冻结头文件]
hardware_gap: HIL-1..HIL-6 全部需要实板
```

启动顺序（`app_main`）：
1. `store_init()`；失败记录错误并继续（白名单为空、SN 用默认）。
2. 读 SN；不存在时用本机 BT MAC 后 2 字节生成 `"%02X%02X"`。蓝牙名 = `"E1-Lite-" + SN`，无换行。
3. 读白名单 → `link_mgr_init(&g_link, APP_LINK_POLICY, ...)`；`APP_LINK_POLICY` 默认 `LINK_POLICY_CONCURRENT`，用 `main/Kconfig.projbuild` 的 choice `RTK_LINK_POLICY` 选择。
4. `alarm_src_init`、`cmd_disp_init`，填 `info.sn`、`info.fw_ver = APP_VERSION_STR`。
5. `bt_spp_init(name)`；失败则记录错误，不创建链路任务，控制台仍启动。
6. `xTaskCreateStaticPinnedToCore(app_link_task, "link", 6144, ..., 10, ..., 1)`。
7. 启动控制台。

`app_link_task` 循环（`for (;;)`）：
- `bt_spp_poll_event(&evt, 20)`：
  - OPEN → `link_mgr_on_open`；拒绝/踢出的句柄逐个 `bt_spp_disconnect`；为接受的会话 `proto_parser_init`（解析器数组与会话一一对应，按 handle 查找）。
  - CLOSE → `link_mgr_on_close`，释放解析器槽。
  - DATA → 循环 `bt_spp_read` 到 0，逐字节 `proto_parser_feed`；每帧：`link_mgr_on_frame`（执行断开动作）→ 角色非 NONE 时 `cmd_disp_handle` → REPLY 时 `bt_spp_write(handle, buf, len, 200)`；`b_whitelist_changed` 或角色由 PHONE/PENDING 变为 ALARM 时用 `link_mgr_whitelist_get` + `store_set_alarms` 持久化；`b_heartbeat` 时记录报警器电量与状态（供控制台显示）。
  - RX_DROP → 计数。
- 每 100 ms `link_mgr_tick`，执行断开动作。
- 每 `ALARM_PUSH_PERIOD_MS`：对 `link_mgr_handles(ALARM)` 的每个句柄 `cmd_build_status_push` + `bt_spp_write`，seq 自增。
- 时间：`(uint32_t)(esp_timer_get_time() / 1000)`，注释说明回绕约 49.7 天且所有比较使用无符号差值。
- link_mgr/cmd_ctx/alarm_src 只在链路任务内访问；控制台通过 `xQueueCreateStatic` 的命令队列把请求投递给链路任务（单一所有者），不得直接改共享结构。

控制台命令（`esp_console`，UART0 115200）：
| 命令 | 作用 |
| --- | --- |
| `status` | 打印会话表（handle、角色、MAC）、白名单、配对状态、报警状态、解析错误计数、空闲堆与最小空闲堆、link 任务栈高水位 |
| `alarm set <state 0-2> <level 0-3>` / `alarm clear` | 测试覆盖（经队列） |
| `pair` | 打开配对窗口 |
| `wl clear` | 清空白名单并持久化 |
| `sn <str>` | 写 SN（重启后生效） |
| `policy` | 打印当前链路策略 |
