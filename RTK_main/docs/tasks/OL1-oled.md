# OL1：P2 OLED（SH1106）驱动 + WouoUI 只读状态页

- 分支：`feat/ol1-oled`（基线 main `3bbd6d2`）。与 U2（`feat/u2-ublox-rate`，改 gnss 驱动/GnssModule/SettingsBackend）并行：**本卡不得改 `components/gnss/`、`main/modules/gnss/`、`main/settings/` 下的文件**。
- 依据：ADR-0015（板级档案，`Feature::kOled`）、ADR-0017 §7（OLED 菜单延后）、`docs/reports/P2-legacy-gap.md` §1.5 与硬件表（SH1106 128×64，I2C0 `0x3C`，电源 `GPIO12` 置高；旧实现 `main/src/xyz_oled.c`、`main/user_src/user_oled.c`）、原理图 `docs/hardware/P2/schematic/p2v1.41.pdf`。
- UI 库：用户已选 WouoUI-PageVersion（MPL-2.0，纯 C、无 malloc），本地副本 `/tmp/claude-1000/-home-gtc-Desktop-workspace-ESP32-PROJ-RTK-Main/66413007-6f88-48b3-b94e-525f98fe6b70/scratchpad/wouo/repo`（`Csource/`）。

## 目标

P2 开机后 OLED 亮屏，显示开机画面与只读状态页；单键短按翻页；长按关机行为不变，关机前屏幕显示关机提示并断电。

## 范围

1. **ADR 草稿**：写 `docs/adr/ADR-0020-oled-display.md`（状态 Proposed），定：模块/任务归属、I2C 总线共享、刷新率与优先级、页面清单、按键语义（短按=下一页；长按=关机，不变）、内存/体积预算、MPL-2.0 合规（vendored 文件保持原许可头，修改处标注，单独目录 + LICENSE）。由 Architect 审核后再定稿。
2. **I2C 总线共享**：现在 `main/board/Charger.cpp:146` 独占创建总线。改为板级单一总线所有者（创建一次，Charger 与 OLED 各自 `i2c_master_bus_add_device`）；B3 行为（0x1A 探测、REG02/REG03 写入、无芯片只报一行 W）不得回归。
3. **SH1106 驱动**：IDF `i2c_master`，400 kHz；仅当 `boardProfile` 含 `Feature::kOled` 才拉高 `GPIO12` 并初始化；面板无应答 → 一行 W 日志并禁用显示，不影响其他模块。SH1106 列偏移 2（132 列 RAM）。全帧缓冲 1 KB 静态分配，按页（8 page）写出。
4. **WouoUI 集成**：vendored 到 `components/third_party/wououi/`（或项目既有第三方目录约定），只编译所需文件；HAL 接到上面的驱动；不得引入堆分配（若库内部有 malloc 须报告并替换为静态）。
5. **页面（只读）**：开机画面（产品名 + `RTK_FW_VERSION`）；状态页（GNSS 状态/卫星数/解状态、电量%与充电、BT/Wi-Fi 模式）；设备信息页（SN、hwVer、fwVer）。数据从现有数据总线/快照读取，不新增跨模块写接口。充电待机（USB 供电开机）只显示电量/充电画面。
6. **按键**：消费 `PowerModule.cpp:688` 的 `KeyEvent::kShort`（经事件/总线通知 UI，不在 PowerModule 里直接画屏）。
7. **关机**：关机序列切断电源前显示关机提示，并关闭面板与 `GPIO12`；不得延长现有关机时序超出 ADR-0015 规定。
8. **调度**：低优先级任务，刷新 ≤ 5 Hz，空闲时不刷新（内容无变化不写 I2C）；不得阻塞充电读数与按键检测。
9. 控制台：`oled` 状态命令（是否存在、当前页、帧数、I2C 错误计数），遵循 ADR-0016 档位。

不在本卡：设置菜单/动作项、OTA 进度页、电子围栏页、动画调优、其他产品（E1）。

## 测试

- 主机测试：页面格式化（数字、截断、无 GNSS/无充电芯片时的占位）、翻页状态机、"内容未变不刷新"逻辑。
- 现有 B3 / power 相关主机检查全部通过（总线改造不回归）。
- 每项修复配修复前会失败的测试。

## 门禁与报告

- 全量门禁：`ASAN_OPTIONS=detect_leaks=0 bash tools/ci/run_gates.sh --prefix ol1 --base 3bbd6d2`（t13 ASan 偶发超时已知，单独重跑确认）。
- 与干净基线比较 rtk_main.bin、IRAM、DRAM、最小空闲堆（若可测）；WouoUI 体积单列。
- 报告 `docs/reports/OL1-oled.md`：改动、设计取舍、MPL 合规清单、测试映射、门禁、体积、HIL 步骤（Architect 执行）。
- 不提交、不烧录、不连串口。
