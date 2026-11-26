# OL8 — OLED 打磨：WiFi 启动 Logo 卡顿、网页 OTA 进度、超长文本滚动

- 分支/工作树：`feat/ol8-oled-polish`，`../RTK_Main-ol8`，基线为本卡提交
- 门禁：`tools/ci/run_gates.sh --prefix ol8 --base c189c80`（需 `export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env`）
- 报告：`docs/reports/OL8-oled-polish.md`；完成时输出 `OL8-FINISHED`
- 来源：用户 2026-11-26 三条反馈（整机 P2 v1.41 + CM680）

## A 蓝牙→WiFi 切换复位后首帧（产品信息 Logo）卡顿
现象：BT 模式切到 WiFi 模式时设备复位，自检页第一帧（带产品信息的 Logo 逐列展开，`kLogoRevealMs` 450 ms，`main/modules/oled/OledUi.hpp:155`，渲染 `main/modules/oled/OledRender.cpp:493 renderSystemCheckLogo`）画到一半停顿再继续；冷启动 BT 模式顺滑。
1. **先测量再改**：用现有 `OledPerf`（`main/modules/oled/OledPerf.hpp`）或临时 trace 记录 WiFi 启动与 BT 启动下 Logo 阶段每帧间隔、I2C flush 耗时、OLED 任务被抢占情况；对照 `main/app/BootFlow.cpp` 时序（`stepRadio` :195 `esp_bt_mem_release`、WiFi/httpd 初始化、`markOledLaunched` :382）。报告给出证据定位根因（假设：WiFi 初始化/`esp_bt_mem_release`/NVS 读写与 Logo 同期占用 CPU 或长关中断）。
2. 按根因修复，优先级：调整启动时序（例如 Logo 展开完成后或 OLED 首帧后再启动 WiFi/httpd）> 任务优先级/核分配微调 > 其他。不得延长总启动到 WiFi 可用的时间超过 500 ms。
3. 验收：WiFi 启动 Logo 阶段最大帧间隔 ≤ 2× 正常帧间隔（报告给出前后数值）。

## B 网页 OTA 时 OLED 显示进度与提示
现状：OLED 无 OTA 状态；网页 OTA 写入路径 `main/modules/web/WebOta.cpp`（`esp_ota_begin` :210 / `esp_ota_write` :224 / `esp_ota_end` :236），httpd 任务执行。
1. WebOta 向共享数据发布 OTA 进度（建议 `s_app.data` 新增小快照：阶段 idle/receiving/verifying/success/failed、已收字节、总字节、失败码 `UploadCode`）；单写者（httpd），OLED 只读；无堆分配，写频率限流（≥ 1% 或 ≥ 200 ms 才更新）。
2. OLED 新增全屏 OTA 页（不进 dock）：
   - 接收中：标题 `OTA UPDATING`、进度条 + 百分比、`xxx/yyy KB`、提示 `DO NOT POWER OFF`；
   - 校验/写尾：`VERIFYING`；
   - 成功：`OTA OK` + `REBOOTING...`，与现有重启横幅 / USB 终帧逻辑协调（`kRebootMs` 1500 ms，`main/modules/web/WebOtaTransfer.hpp`）；
   - 失败/超时/中止：`OTA FAILED` + 简短原因（按 `UploadCode` 映射短词），3 s 后回到原页面。
3. OTA 进行中：短按/双击不切页、不弹关机确认；**8 s 强制关机保留**（`kForcePowerOffHoldMs`）；长按关机走 PW1 流程时 OLED 关机页优先于 OTA 页。
4. 页面覆盖所有模态（WiFi/配对/关机确认对话框在 OTA 开始时关闭）。

## C 超长文本滚动（跑马灯）
现象：已写 SN 的设备 WiFi 模式下 SoftAP SSID（如 `P2-1826M0056`）在 WiFi 子页（`main/modules/oled/OledRender.cpp:1180 renderCommWifi`）被截断；`kSubRowValBytes` 16 B、`kMaxLineChars` 21。
1. 实现通用的单行跑马灯绘制：文本像素宽超过其框宽才滚动；起点停 1 s → ~30 px/s 左移 → 末端停 1 s → 回到起点循环；裁剪在框内，不覆盖图标/边框。可复用 WouoUI `WouoUI_CanvasDrawSlideStr`（`components/third_party/wououi/WouoUI_graph.c`），若不满足裁剪需求自写，说明理由。
2. 排查所有可能超框的文本并接入：子页行值（SSID、IP、设备名、SN/PN、版本号等）、对话框正文（OL6 SoftAP 详情）、表盘状态文本；报告列出清单与是否接入。必要时扩大对应字符串缓冲以保留完整文本（不截断数据，只在绘制时裁剪）。
3. 帧率：仅在滚动文本所在页可见时驱动动画（OL5 帧节拍，30 fps 上限），离开页面或熄屏时停止，空闲功耗不变。

## 约束
- 无堆分配；DRAM 增量 ≤ 256 B，IRAM 0；遵守 ADR-0018 线程/单写者模型。
- 不改冻结协议头；网页 UI 可不改（OLED 侧功能）。
- 模拟器（OL4/OL5 引入的 host 模拟/截帧工具）补 OTA 页与滚动截图，报告附图或说明。

## 验收
- 门禁全绿；host 测试新增：OTA 快照状态机与 OLED 页切换（含失败 3 s 返回、OTA 中按键屏蔽、关机优先）、跑马灯位置时间函数（短文本不滚、长文本停顿/循环/裁剪）。新增用例在基线上失败。
- 报告：A 的前后帧间隔测量、资源前后（`idf.py size`）、C 的接入清单。
- HIL（用户，整机无串口，WiFi OTA 升级）：① BT→WiFi 切换 Logo 顺滑；② 网页 OTA 全程 OLED 进度/成功重启；拔网或刷错镜像显示失败后返回；③ WiFi 子页 SSID 完整滚动显示。
