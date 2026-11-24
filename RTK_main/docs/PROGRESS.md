# 开发进度与遗留事项

截至 2026-11-23（`main`，另有未合入分支见末尾）。各系列的设计依据见 `docs/adr/`，验收标准见 `docs/acceptance.md`。

已完成卡片的任务卡与实施报告已于 2026-11-23 清理出工作树。需要原文时用 `git log --all -- docs/tasks/<卡>.md`
或 `git show d6db196:RTK_main/docs/reports/<卡>.md` 取回。仍被 ADR、工具或验收引用的报告保留在 `docs/reports/`。

## 1. 已完成系列

| 系列 | 内容 | ADR | 关键提交 |
| --- | --- | --- | --- |
| T10–T16 | 工程骨架；手机/报警器帧协议（C 核心 + C++ 外观）；链路核心、OS 封装、NVS；经典蓝牙 SPP 多会话驱动与重连；应用框架与控制台 | 0002、0003 | `d1780c2`～`989c860` |
| G、H | 蓝牙字节流分流器：先 C 版（G1–G3），后按 ADR-0005 全部改写为 C++ `stream_demux` | 0004、0005 | `ca265a8` |
| I | 产品帧与 RTCM 走同一条蓝牙流 | 0006 | — |
| J | 执行器、数据中心、命令路由与服务拆分（旧 CommandDispatcher 退役，留作 golden 基准） | 0007 | — |
| R0–R8a | 代码组织：`main/` 分目录、模块类化、命名空间、doxygen、SPP 拆分、模块模板、组件改名、步骤表启动 | 0008 | `719580f`～`7850095` |
| T1a–T4、L0 | 追踪环、控制台 2 Mbaud 与命令分组、文本日志通道、组件日志级别 | 0009 | `bd613e5`、`ce8d3e6`、`27ceb60` |
| G-A、G-B | GNSS 组件（NMEA、Unicore/u-blox/ComNav 方言、控制器）与主程序集成 | 0010 | `6e2b27e`、`4ed9076` |
| K1–K5 | 数据通路与内存：typed FanoutRing、SPP 非阻塞发送、RTCM 直通、GNSS App 环、启动内存池 | 0011 | `5fa2861`～`1e03c15` |
| P3a–P3c | 接收机驱动类层次、输出计划闭环、型号子类、`$CONFIG` 回读 | 0012 | `c745093`～`90cff22` |
| Q1–Q4 | 编码技能符合性审计、修正、门禁覆盖扩大到全部自有源文件 | — | `52571fd`、`d436a2f` |
| S1–S5 | 可读性：lowerCamelCase 改名、函数 ≤ 60 行、拆分长文件、注释 | 0013 | `2bad7da`～`d0d3dff` |
| C1–C5a2 | 运行时配置：`nvs_fac` 出厂分区与 coredump、3 MB 应用槽、GNSS 配置持久化、早期配置与启动失败回退、`freset`/配对窗口、工作模式（流动站/基站自测/固定） | 0014 | `e400e74`～`abf1178` |
| B1、G-CM680 | 板型识别（GPIO35 ID 分压）与 P2 电源保持；CM680 接收机 | 0015、0012 | `a2deda2`、`9b405bf` |
| D1 | 控制台构建档位 DEV / PRODUCTION / NONE | 0016 | `b4d5488`、`3efe097` |
| N1、P1 | 设备名 `<产品>-<SN>`；旧手机协议兼容 | — | `61027b5`、`2729a52` |
| G-baud、G-optisens、Q1 门控 | 探测后切 460800；只在首次保存启动波特率；Optisens 家族；GGA 质量门控 | 0012 | `f07bee3`、`c94fcef` |
| W0–W8 | Wi-Fi 网页配置：按启动互斥的射频模式、SoftAP + httpd/WS、`web_proto`、网页前端与离线 mock、强制门户、页面重做 | 0017 | `4023131`～`f66389e` |
| H1、HB | 按构建计算堆预算，按官方文档调整 Wi-Fi/lwIP 内存 | 0017 | `37aafb4` |
| SV1–SV3 | 执行器异步命令总线；设置服务；手机与控制台改走设置服务；0x28 手机进入网页配置 | 0018 | `1105c35`、`a74bf94`、`5bd560f`、`489e2f7` |
| B2 | P2 电源键长按关机、关机序列（保存 → 静默 → 断电）、SGM41513 I2C USB 检测与充电待机；2026-11-23 P2 v1.41 主路径 HIL PASS（报告 `docs/reports/B2-power-key.md` §9） | 0015 | `612ff8a`、`65c2863`、合入 `00f73c2` |

## 2. 进行中与未开始

| 项 | 状态 |
| --- | --- |
| B3 充电芯片探测、GPIO27 USB 检测、电池 ADC、低电关机 | 已合入 main（2026-11-24）。新 F9P P2 v1.41 HIL 通过：GPIO27 插拔、电池 ADC +30 mV、App 电量一致、无芯片时只报一行 W（报告 `docs/reports/B3-charger-usb-battery.md` §8） |
| U1 ZED-F9P 驱动（VALSET、NMEA 静默、星座/截止角、RTCM 帧边界下发） | 已合入 main（2026-11-24）。P2 v1.41 + ZED-F9P HPG 1.12 HIL：探测/SN、无计划外 NMEA、控制台/手机 0x06/网页改星座与截止角并掉电保持、三入口拒绝关 GPS 均通过；RTK 固定解保持未测（报告 `docs/reports/U1-f9p-driver.md` §10） |
| U2 ZED-F9P 定位频率/输出周期、NMEA 高精度 | 已合入 main（2026-11-24）。F9P 1/2/5 Hz + 整数倍周期规则（控制台/设置/网页/驱动四处），改频率周期自动对齐并持久化，回滚恢复 CFG-RATE-MEAS，不重放星座；HIGHPREC/SVNUMBERING 带 3 键 NAK 降级。室内 HIL 通过；室外项未测（报告 `docs/reports/U2-ublox-rate.md` §8） |
| OL1 SH1106 OLED（共享 I2C0、WouoUI graph/font 子集、只读状态页、短按翻页、关机提示） | 已合入 main（2026-11-24）。P2 v1.41 HIL：显示方向、开机页→状态页、短按翻页、关机提示均正常，`i2c_errors=0`，oled 栈 HWM 1,088/3,072 B。用户反馈关机提示一闪而过、开机无过渡、页面单调 → OL2 |
| O1–O3 网页 OTA | 已合入 main（2026-11-24）。P2 v1.41 HIL 通过：升级 ~35 s、30 s 自动确认、错误码、并发 150 s 408、中止后 other=null、断 Wi-Fi、上传中长按电源键中止 |
| C5b smartBase | 用户 2026-11-18 暂停 |
| C5c 其他输出去向 | 未开始，等用户确定去向种类 |
| 4G | 未立项，需新 ADR，必须走 ADR-0018 的服务与总线 |
| P2 OLED 表盘与子菜单（OL2） | 进行中：表盘 + 子菜单、开机自检、关机进度、30 fps 动画；按键方案 A（短按下一项、双击进入/返回） |

## 3. 待补的硬件验证（HIL）

- C5a：外场基站自测与基站 RTCM → 手机（见 `docs/reports/C5a.md` 末尾）。
- G-CM680：RTCM 注入未测；所有 RTCM 共用最后一个周期、`nmea bad` 每 5 s 加 1 的原因未查明、`unlog com1` 返回 ERROR。
- W0：CM680 星座 MASK/UNMASK 未验证，`gnss.const.*` 暂不支持。
- B1：E1 强制档与未知 ID 路径未测。
- B3：REG02/REG03 写入需在 CM680 P2 v1.41（有 SGM41513）补测；<10% 低电关机未实测；新 F9P 板 I2C 只见 0x3C，充电芯片位置待原理图确认。
- U1：室外 + 基站下，接收差分期间修改截止角/星座，确认 RTK Fixed 不中断（未测）；接入差分后计划外 NMEA 计数 `ign=4` 原文未抓。
- U2：室外有星时确认 GGA 经纬度 7 位小数；SVNUMBERING 下真实卫星 `nmea.bad=0`；5 Hz 四星座 RTK + RTCM 满载下 VALSET 不超时、SPP 吞吐正常（均未测）。RU2-02：改频率时 rate/out 两键分开保存，部分失败会让 NVS 频率与周期不一致，待改为一次原子保存。
- 门禁：`G2b-t13_host_check-asan` 的 `t13_disconnect_reuse_across_slots` 在高负载下偶发失败（测试同步竞态：`fake_wait_disconnect_returns` 早于 worker `spp_server_ctrl_settle_locked` 结算，见 `docs/reports/T13F-pairing-order.md`），单独重跑通过；待另开卡修正测试同步。
- B2：固件卡死时 10 s 硬关断 NOT_RUN；3V3/GPIO4/U3 波形未用仪表测；SGM41513 REG00–REG0B 位核对、`senseOnMv` 与硬关断时长校准、注入类项（按键毛刺、U3 30 s 未掉电回退、BT 拥塞、NVS 错误）未测；BT 关机时 SPP CLOSE 日志晚于 `device powered down` 约 0.49 s。
- HB：关闭 Wi-Fi 的构建最小空闲堆 80,776 B，低于 81,920 B 预算，用户已接受（该构建不发布）。
- 多张实现卡（C1–C4、P1、N1、G-baud*、Q1 门控、W1）的报告只有实现方结论 NOT_RUN，此后的整机 HIL 没有逐卡单独记录。
