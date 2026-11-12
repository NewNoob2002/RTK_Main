# POWER-HOLD 独立复审

日期：2026-11-11。复审者：Codex，独立 QA 角色。人工负责人：UNKNOWN（待人工审查签字）。Profile：esp-idf，ESP32 / GPIO26。范围仅为用户补充授权后的 `main/app_main.cpp` 电源保持初始化和 `main/CMakeLists.txt` 的 `esp_driver_gpio` 依赖。

## 建议结论

**PASS（软件复审）**。未发现阻塞缺陷：先将 GPIO26 输出锁存器预置高，再配置输出；两步任一步失败均记录错误并返回，不继续 NVS/蓝牙/链路/控制台初始化。真实 `app_main.cpp` 的 host 替身回归 3/3 通过。此结论不表示已确认电池供电自保持、上电瞬态或实际引脚电压，硬件结果仍为 NOT_RUN。

原 TIDY-FIX 复审与其哈希对应电源保持改动前快照，本报告不覆盖或修改其结论。当前两文件哈希单独保存于 `logs/POWER-HOLD-REVIEW/sources.sha256`。

## 溯源与 SDK 调用核对

- 旧固件 `../../E1-5.4/main/main.cpp:90` 明确 `pin_mcu_on_power = 26`；`../../E1-5.4/main/src/Begin.cpp:81-82` 使用 OUTPUT、写入 1。与用户确认的电池开机保持需求相符。未推测原理图或其他引脚。
- 本地 IDF `components/esp_driver_gpio/src/gpio.c:249` 的 `gpio_set_level` 先校验可输出引脚，再经 `gpio_hal_set_level` 写硬件锁存器。ESP32 `components/esp_hal_gpio/esp32/include/hal/gpio_ll.h:504` 对 level=1、pin<32 写 `out_w1ts`，不要求已启用输出。
- `gpio_config`（同 gpio.c:351）按掩码只操作 GPIO26：切换 RTC pad 到数字 GPIO、关闭输入/开漏、启用输出并选择 GPIO 功能、关闭上下拉和中断。`gpio_output_enable`（:226）与 ESP32 `gpio_ll_output_enable`（gpio_ll.h:403）写输出使能，不将 OUT 锁存器改回 0。因此 preload→config 在该 SDK 路径上保留高输出值，避免主动先输出 0。
- `components/soc/esp32/include/soc/soc_caps.h:194` 的可输出掩码包含 GPIO26。移位使用 `1ULL`，位号 26 在 64 位范围内。
- `main/CMakeLists.txt` 显式依赖 `esp_driver_gpio`；没有将硬件依赖引入纯协议/业务组件。原有初始化顺序在电源保持成功后继续。

一个非阻塞溯源细节：初读源码注释的 `src/Begin.cpp` 是旧 `main/` 内相对路径，完整工程路径为 `E1-5.4/main/src/Begin.cpp`；已向实现者反馈，未自行修改。

## 独立执行证据

先用 CodeGraph 定位并读取当前 `app_main`，再核对上述 SDK 和旧固件原文。没有执行硬件命令、串口连接、安装、下载或 git 写操作。

| 命令 / 检查 | 状态 | 结果 |
|---|---|---|
| `python3 -I docs/reports/logs/POWER-HOLD-REVIEW/run_smoke.py` | PASS | 编译真实 `main/app_main.cpp`，`g++ -std=c++20 -Wall -Wextra -Werror`，3 场景通过；完整命令及输出见 `logs/POWER-HOLD-REVIEW/smoke.log` |
| 成功路径 | PASS | `LCISWNBTH`：set_level → config → store_init → get_sn → load_whitelist → build_name → bt_init → link_start → console。替身同时检查 GPIO26、level=1、单引脚掩码、OUTPUT、无上下拉/中断 |
| set_level 失败 | PASS | 仅 `L`；不调用 config 或后续初始化 |
| config 失败 | PASS | 仅 `LC`；不继续 NVS/蓝牙/链路/控制台 |
| `clang-format --style=file --dry-run --Werror main/app_main.cpp` | PASS | 退出 0；`logs/POWER-HOLD-REVIEW/format.log` |
| `sha256sum main/app_main.cpp main/CMakeLists.txt` | PASS | 生成本次源码快照 |
| IDF 完整构建、G5b、G7b | NOT_RUN（本子任务） | 主任务正在独立执行 `build-power-hold` 及相关门禁，结果由主任务报告记录，不冒用其他执行者结果 |
| GPIO26 实测电压、纯电池自保持、启动电源时序 | NOT_RUN | 本复审子任务禁止硬件/串口动作；host 替身不能验证物理电路 |

替身脚本是本次复审的一次性可复跑证据，使用现成 T13 host 的基本 SDK/FreeRTOS 头，另在 `/tmp` 生成三份最小 GPIO/MAC/reset 头和替身。被测启动函数来自真实源码，未抽取或重写其分支。未新增泛用测试框架，未修改冻结 host 工程。替身验证调用顺序和错误短路，不执行真实 GPIO 驱动；实际锁存器行为的证据来自本地 IDF 源码审查。

## 偏离与未解决项

本复审仅新增报告、一次性替身脚本及日志，未修改产品源码或冻结输入。脚本使用实际 `Generated-by: Codex` 标记，没有冒用 DeepSeek 实现者署名；人工负责人仍为 UNKNOWN。硬件事实仅引用旧固件与用户确认，未把 host 通过解释为上板通过。电池保持及启动窗口仍需主任务 HIL 验证。
