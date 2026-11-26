# OL8 OLED 打磨

分支 `feat/ol8-oled-polish`，门禁基线 `c189c80`。目标为 ESP32 Xtensa、ESP-IDF 6.1，代码与尺寸复核使用同一套构建工具和默认配置。实施依据为任务卡、项目 `AGENTS.md` 与 ADR-0018。当前工作树中无 `RTK_main/CLAUDE.md`。

## A · Wi‑Fi 启动 Logo

改代码前核对了启动步骤和 OLED 任务入口，并用现有 `FramePacer` 建立 host 基线。SH1106 全量发送以每页 135 B 加地址、9 位含 ACK、400 kHz 计算，模型使用约 25 ms 总线时间。固定 BT 释放时刻为初帧后 34 ms，分别注入 80、150、250 ms 的 Wi‑Fi 启动屏障延迟。在可运行后的额外抢占设为 0 的条件下，原顺序的最大帧间隔为 80、150、250 ms，延后首帧和时钟后的模型均为 34 ms。记录在 `builds/ol8-baseline/logo_timing.log`。

原流程在 `Executor::init` 内按模块表先执行 `OledModule::init`，写出 Logo 初帧，随后同步执行 `WebModule::init` 的 netif、httpd、DNS 和 SoftAP 启动。OLED 任务要到 `Executor::start` 才创建，再等待 IO 步骤和启动屏障。这段空窗没有可调度的 OLED 刷新任务，已展开的列停住，放行后墙钟继续推进造成跳变。`esp_bt_mem_release` 和早期 NVS 在 OLED 初始化之前。原流程还在放行后清零 NVS 启动失败计数，可能与 OLED 首次刷新重叠。板上 IRQ、调度抢占和 Flash 暂停时长待 HIL。

Wi‑Fi 模式在面板探测成功后保持清屏状态，放行后由 OLED 任务同时起算 Logo 时钟和生成首帧。Wi‑Fi 启动失败计数写入在放行前结束。AP 与 HTTPD 在屏障之前启动。同步路径省去屏障前的额外 Logo 刷写。网页业务全部放行的时间仍需按 500 ms 上限做整机对照。

在基线和变更上执行相同 host 场景，运行真实 `OledModule` 和 SH1106 脏页刷写，I2C mock 每传输字节计 23 µs 并计地址字节。Wi‑Fi 注入 150 ms 的同步启动耗时。统计窗口为各实现的 450 ms 展开期，排除随后的 Logo 停留。已写帧经过脏页过滤，写屏间隔与任务唤醒分别统计。

| 运行模式 | 实现 | 放行前已写帧 | 展开期写帧数 | OLED 任务唤醒数 | 最大写屏间隔 | 峰值 I2C flush |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| BT | 基线 / OL8 | 1 | 9 | 9 | 34 ms | 25.024 ms |
| Wi‑Fi | 基线 | 1 | 4 | 5 | 175 ms | 25.024 ms |
| Wi‑Fi | OL8 | 0 | 12 | 14 | 66 ms | 25.024 ms |

OL8 host 达到 ≤ 2 × 34 ms。两个日志分别为 `builds/ol8-baseline/host-logo.log` 与 `builds/ol8-logo.log`。

## B · 网页 OTA

`WebOta.cpp` 的 httpd 回调独占写入 `DataCenter::otaProgress`。通过 `Slot` 一次发布阶段、总长、已收字节、结果码和时间，释放锁后唤醒只读的 OLED。接收进度跨整数 1% 或间隔达到 200 ms 才发布。校验前和终态立即发布。网页业务设置依 ADR-0018 走 Settings Service 与执行器。OTA 沿用原 HTTP 契约及电源动作仲裁。

`OledUi` 用全屏页覆盖原页面与弹窗，清掉待消歧按键和关机确认。失败、中断、超时均显示对应短词，到 3 s 恢复原页面。物理按键的 8 s 强制关机由 PowerModule 受理。PW1 实际进入关机时，关机页先于 OTA 页，USB 和电池终帧按 I2C 刷出结果确认。上传成功保留 `OTA OK / REBOOTING...`，沿用 1500 ms 重启窗口与 OLED I2C 静默握手。

## C · 跑马灯

使用时间驱动的像素偏移与 WouoUI 单行字形裁剪。WouoUI 自带 `SlideStr` 根据绘制次数推进，无法在事件驱动的刷新、重绘和过渡节拍之间维护 1 s 停留与恒定速度。新实现固定起止各停留 1 s，左移 30 px/s，显示尾部后循环。滚动字段离开前景、被遮挡或熄屏时停止。停留期只设置下一个截止点，移动时交给现有 OL5 节拍。短文本按原对齐方式绘制，无滚动唤醒。

| 位置 | 接入范围 |
| --- | --- |
| COMM | SSID、IPv4、BT 对端摘要及其他行值。使用完整 32 B SSID 字段，绘制区避开图标、标签和选框。 |
| DEV | 固件版本、SN、PN、设备名、运行时间及左卡产品/硬件版本。新增 PN 行读取既有定长快照，其余列表沿用统一导航。 |
| POS / PWR | 经纬度、高程、HDOP/龄期、电压、充电状态等行值与左卡。 |
| 表盘 | 右侧 UTC、精度、卫星数及 GNSS 状态摘要。顶部状态词最多 5 个字符，大号状态词框为 60 px，顶部状态词框为 34 px，现有词在各自字体下完整显示。 |
| 对话框 | 配对设备名和 Wi‑Fi 确认正文接入同一裁剪器。当前 OL6 SoftAP 详情位于 COMM 子页，Wi‑Fi 确认弹窗正文为 `BT WILL DISCONNECT`。 |
| 自检 / 充电待机 | 自检 GNSS 型号与固件版本组合、待机 FW/HW 页脚。Logo 版本行随短时开机过渡裁剪，完整固件版本可在 DEV 查阅。 |

## 主机与模拟器回归

- 新增真实 OLED 启动测试、OTA 快照状态及限流测试、OLED 覆盖/按键/失败回页/关机优先测试，以及跑马灯时间函数与逐像素裁剪测试。Wi‑Fi 启动回归在原基线超过 68 ms。用新增接口和相同测试夹具重编原 WebOta、OledUi、OledRender 行为也能触发 B/C 断言。
- `tools/oled_sim/scripts/ol8.txt` 覆盖长 SSID 起点、末端、循环和 OTA 四阶段，并验证失败后恢复 COMM 页。起点和回环的 PBM 逐字节一致，与末端 PBM 有像素差异。
- 截图为 128×64 PBM，可用模拟器脚本重建。SSID [起点](../../tools/oled_sim/golden/ssid-start.pbm)、[末端](../../tools/oled_sim/golden/ssid-end.pbm)、[回环](../../tools/oled_sim/golden/ssid-loop.pbm)。OTA [接收](../../tools/oled_sim/golden/ota-receiving.pbm)、[校验](../../tools/oled_sim/golden/ota-verifying.pbm)、[成功](../../tools/oled_sim/golden/ota-success.pbm)、[失败](../../tools/oled_sim/golden/ota-failed.pbm)。

## 门禁与体积

```sh
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
tools/ci/run_gates.sh --prefix ol8 --base c189c80
idf.py -B builds/ol8-idf size
```

| 检查 | 结果 |
| --- | --- |
| G1 冻结输入 | PASS |
| G2 / G2b 普通与 ASan/UBSan 主机套件，包括完整 GNSS 驱动和 OLED 模拟器 | PASS，28 项 |
| G3 / G3b ESP-IDF 构建，包括 Wi‑Fi/OTA/TRACE 关闭、量产、NONE+Wi‑Fi 和纯组件双目标 | PASS，9 项 |
| G5 / G5b / G6 格式、clang-tidy、函数文档 | PASS |
| G7b / G7c 堆白名单与 NONE+Wi‑Fi 控制台依赖 | PASS |

完整门禁 **43 PASS / 0 FAIL**，记录在 `builds/ol8-gates.log` 与 `builds/ol8-logs/`。初次沙箱内的 LeakSanitizer 受 ptrace 限制而退出，日志保留在 `builds/ol8-sandbox-logs/`。最终经审批在沙箱外重跑原命令，保留全部 sanitizer 检查。函数文档门禁使用本机已缓存的 lizard 1.26.1。

| ESP32 默认配置 | 基线 | OL8 | 增量 |
| --- | ---: | ---: | ---: |
| 静态 DRAM | 98,675 B | 98,803 B | +128 B |
| IRAM | 116,139 B | 116,139 B | 0 B |
| Flash Code | 1,146,796 B | 1,149,284 B | +2,488 B |
| Flash Data | 276,156 B | 276,348 B | +192 B |
| IDF image size | 1,592,454 B | 1,595,262 B | +2,808 B |
| OTA `.bin` | 1,592,576 B | 1,595,376 B | +2,800 B |

基线与变更尺寸日志为 `builds/ol8-baseline/build.log` 与 `builds/ol8-logs/size.log`。新增自有代码无堆分配。Xtensa 函数入口栈帧中，`refreshOnce` 为 496 → 656 B，`collectViewModel` 为 672 → 816 B，`renderUi` 为 144 → 160 B，`handleOtaPost` 为 496 → 528 B。记录在对应构建目录的 `stack-entries.log`。运行期栈高水位列入 HIL。

## 待 HIL

整机 P2 v1.41 + CM680 无可用串口，以下为 **NOT_RUN**。

1. 冷启动 BT 与 BT→Wi‑Fi 复位分别记录 Logo 帧间隔、I2C flush 和刷新任务调度。确认 Wi‑Fi 最大间隔不超过实测正常节拍的两倍。分别记录复位到 SoftAP 可见、关联成功及网页首个业务响应的耗时，验证增量 ≤ 500 ms。
2. 用网页上传合法镜像，验证接收、校验、成功及重启。分别拔网、上传错误镜像，确认失败原因和 3 s 回页。上传期间试短按、双击、8 s 保持强制关机，核对 PW1 页面与 USB 终帧。
3. 用写过 SN 的设备检查 COMM 页 SSID、IP 与 DEV 页长版本。确认离页和熄屏后停止跑马灯，并测 OLED/httpd 运行期栈余量。
