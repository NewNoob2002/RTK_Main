# W1：射频模式框架（按启动选择 BT / Wi-Fi 配置模式）

依据：ADR-0017 §2.1、§2.2；W0 报告 `docs/reports/W0.md`（预算是否通过以 Architect 验收为准）。
基线：main（含 W0 结论提交：`sdkconfig.defaults` 已开 SoftAP 与 HTTPD WS）。W0 探针代码在 worktree `RTK_Main-w0`（分支 `spike/w0-wifi-budget`，未合入），`main/app/W0Radio.cpp` 可参考其 BTDM 释放与 SoftAP 写法。分支：`feat/w1-radio-mode`（worktree）。
实现：codex（`--approve-for-me`）；交叉评审：omp。Architect（Claude）复跑门禁、P2 台架验收、合入。
本卡**不做** SoftAP/httpd（W2）；`kWifiConfig` 模式下只是“蓝牙不起、BT 内存释放、GNSS 照常、控制台照常”。

## 1 目标

1. `main/app/RadioMode.hpp/.cpp`：`enum class RadioMode : uint8_t { kBluetooth, kWifiConfig }`、
   `enum class RadioReason : uint8_t { kConsole, kPhone, kWebExit, kIdle, kUi }`；
   - `requestRadioMode(RadioMode, RadioReason)`：写 `RTC_NOINIT_ATTR` 记录 `{magic, mode, reason, crc8/16}` 后
     `esp_restart()`（不返回）；
   - `consumeRadioRequest()`：读记录、**先清除再返回**；magic/crc 不对或复位原因是上电/掉电（`esp_reset_reason()`
     为 `ESP_RST_POWERON`/`ESP_RST_BROWNOUT`）一律返回 `kBluetooth`；
   - 记录编解码与校验放成纯函数，供 host 测试。
2. 新启动步骤 `radio`（`main/app/BootStepList.inc`，`earlyConfig` 之后、`contexts` 之前），结果存
   `AppContext::radioMode`；`kWifiConfig` 时调用 `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)`，失败按 kDegrade。
   启动横幅/`status sys` 显示 `radio=bt|wifi` 与 reason。
3. 模块启用条件：`main/module_list.inc` 的 `MODULE_DEF` 增加一列 `enable`（取值 `kAlways` / `kBtOnly` / `kWifiOnly`），
   alloc/bind/init/start/poll 全部按该列跳过，**不在模块内部判断模式**。`bt_link` 与 `rtcm` 设 `kBtOnly`，其余 `kAlways`。
   注意：kLink 执行器的等待函数现在依赖 `bt_link.wait`（`main/app/Executors.cpp`），`kWifiConfig` 下要换成不依赖
   bt_link 的等待（按 roundBudget 定时醒或命令队列），并说明理由。
4. 依赖 bt_link 的其他路径（`status` 里的 link/session 统计、`bt` 控制台命令、factory 里的配对/白名单）在
   `kWifiConfig` 下返回明确的 `unavailable in wifi mode`，不能空指针、不能断言。
5. 控制台 `radio`（DEV 档；PRODUCTION/NONE 不注册，沿用 ADR-0016 的门控表）：`radio` 显示当前模式；
   `radio wifi|bt` 打印 OK 后等 UART 发完再 `requestRadioMode`。
6. Kconfig `RTK_WIFI_CONFIG_ENABLE`（默认 y；n 时 `radio` 步骤恒为 kBluetooth、控制台命令不注册），
   新增门禁 `G3-idf-wifi-off`（按 `run_gates.sh` 里 G3-idf-trace-off 的写法）。
7. 空闲回落计时器留给 W2（那时才有“终端连接”事件），本卡只提供 `requestRadioMode` 接口。

## 2 测试

- host：`tools/main_host_check` 新增 `RadioModeCheck`：记录编解码、crc 错、magic 错、上电复位忽略、读后清除；
  `BootStepsCheck` 更新步骤数与顺序；模块启用列在两种模式下的启用集合（编译期表检查即可）。
- 门禁：`tools/ci/run_gates.sh` 全 PASS，含新增 `G3-idf-wifi-off`；镜像增量 > 4 KB 需说明。
- HIL（Architect 在 P2 上做，开发者只写步骤）：`radio wifi` → 重启后 `radio=wifi`、无 BT 广播、GNSS running、
  空闲堆比 bt 模式多（记录数值）；bt 模式负载测试：手机连接持续下发 RTCM 10 min，`sys mem` 的 min ≥ 40 KB（ADR-0017 §3）；`radio bt` → 恢复；wifi 模式下拔电再上电 → bt；`bt`/`factory pair` 在 wifi 模式下提示不可用。

## 3 交付

- 报告 `docs/reports/W1.md`：改动文件、接口、门禁结果、镜像大小、HIL 步骤。
- 不 commit；不改冻结文件（`sdkconfig.defaults` 如需改，列出具体行，由 Architect 提交）；结束打印 `W1 DONE`。
