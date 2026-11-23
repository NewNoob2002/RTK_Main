# RTK_Main

RTK 接收机主控平台固件，面向多个产品：启动时先识别产品与硬件版本，再按该产品的配置表驱动引脚和模块。运行在经典 ESP32 上，管理 GNSS 接收机，并通过经典蓝牙 SPP 与手机 App、报警器通信：
转发 NMEA，接收 RTCM 差分数据并直通给接收机，处理设备配置。另有 Wi-Fi 网页配置模式。

本仓库主体是 [`RTK_main/`](RTK_main/)。[`ExternalSpeak/`](#externalspeak) 是某一产品外接功放的独立测试固件，不参与主固件构建。

## 目录

```text
RTK_Main/
├── RTK_main/                 主固件（ESP-IDF 工程）
│   ├── main/                 应用层：启动流程、板型、各功能模块、网页资源
│   ├── components/           可复用组件（下表）
│   ├── configs/              构建档位叠加文件（production / console-none / none-wifi）
│   ├── test/                 主机单元测试（Unity + CMake）
│   ├── test_apps/            板上测试工程（rtk_pure、rtk_spp_peer）
│   ├── tools/                门禁脚本、HIL 工具、网页离线 mock
│   ├── docs/                 ADR、编码规范、验收标准、协议、进度
│   └── reference/            只读参考实现，不参与构建
├── ExternalSpeak/            外接功放 I2S 测试固件（独立工程）
└── .github/workflows/        CI 门禁
```

## 硬件

| 项 | 值 |
| --- | --- |
| 芯片 | ESP32 经典款（Xtensa LX6 双核），rev ≥ 3.1，8 MB Flash，不依赖 PSRAM |
| 已适配产品 | P2 v1.41（原理图 `RTK_main/docs/hardware/P2/schematic/p2v1.41.pdf`），为首个适配产品；E1 只作测试板。新产品在 `BoardProfile.cpp` 配置表登记，产品级差异另立 ADR |
| 产品识别 | GPIO35 上的 ID 分压，启动第一步读取并选择产品/版本配置，未知 ID 只进控制台（ADR-0015） |
| GNSS | UART2 RX16/TX17；支持 Unicore UM980/UM982、u-blox、ComNav、Optisens CM680，启动时自动探测 |
| 蓝牙 | Bluedroid 经典蓝牙 SPP，最多 2 个会话；设备名 `<产品>-<SN>`，如 `P2-B5F2` |
| 控制台 | UART0，2 Mbaud |

引脚和板级事实以 `RTK_main/AGENTS.md` §1 与 `RTK_main/main/board/BoardProfile.cpp` 为准。

## 功能

- **数据通路**：`stream_demux` 把蓝牙字节流分成产品帧与 RTCM；RTCM 直接送接收机，NMEA 经 FanoutRing 分发给各会话（ADR-0004～0006、0011）。
- **GNSS 管理**：探测接收机型号与波特率，下发输出计划，保存配置，切换工作模式（流动站 / 基站自测 / 固定基站），输出基站 RTCM（ADR-0010、0012、0014）。
- **手机与报警器协议**：`docs/protocol/` 下的帧协议，兼容旧版手机 App（P1）。
- **设置服务**：手机、控制台、网页共用一套设置注册表，经执行器异步命令总线处理（ADR-0018）。
- **Wi-Fi 网页配置**：蓝牙与 Wi-Fi 按启动互斥。手机发 0x28 或控制台 `radio wifi` 后重启进入 SoftAP（SSID 与蓝牙名相同，`192.168.10.12`），带强制门户；网页协议见 `docs/web/ws-protocol.md`（ADR-0017）。
- **控制台**：DEV / PRODUCTION / NONE 三档，按构建选择（ADR-0016）；支持追踪与分级日志（ADR-0009）。
- **存储**：用户 NVS 与 `nvs_fac` 出厂分区（SN、板卡信息，`freset` 不清除），coredump 分区，`freset` / `factory erase`（ADR-0014）。

## 组件

| 组件 | 语言 | 作用 |
| --- | --- | --- |
| `stream_demux` | C++ | 通用字节流分流器与解码器（NMEA、RTCM3、UBX、Unicore 二进制/哈希），不依赖 ESP |
| `device_proto` | C 核心 + C++ 外观 | 手机/报警器帧编解码 |
| `link_core` | C++ | 链路服务、命令路由、各业务服务 |
| `app_fw` | C++ | 应用框架：执行器、数据中心、FanoutRing、日志 |
| `rtos` | C++ | FreeRTOS 封装与启动内存池 |
| `gnss` | C++ | 接收机驱动类层次与控制器 |
| `settings` | C++ | 设置注册表与设置服务 |
| `web_proto` | C++ | 网页 WebSocket JSON 协议 |
| `cli` | C++ | 控制台 |
| `bt_reconnect` | C++ | 通用重连状态机 |
| `spp_server` | C 驱动 + C++ 适配 | 经典蓝牙 SPP 多会话 |
| `nvs_store` | C | NVS 与出厂分区存储 |

依赖方向见 `RTK_main/AGENTS.md` §3。

## 构建

环境：ESP-IDF v6.1（`~/esp/esp-idf`），GCC 15.2。除 `idf.py` 默认的 `build/` 外，构建目录一律放在 `RTK_main/builds/<名称>/`。

```sh
cd RTK_main
. ~/esp/esp-idf/export.sh
idf.py -B builds/dev build                       # 默认 DEV 档

# 发布档：叠加 configs/ 下的文件，并用单独的 sdkconfig
idf.py -B builds/prod -DSDKCONFIG=builds/prod/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;configs/production.defaults" build
```

| 叠加文件 | 档位 |
| --- | --- |
| `configs/production.defaults` | PRODUCTION 控制台，关闭追踪 |
| `configs/console-none.defaults` | 无控制台，关闭追踪 |
| `configs/none-wifi.defaults` | 无控制台 + Wi-Fi 网页配置 |

分区表 `partitions.csv`：两个 3 MB OTA 应用槽、`nvs`、`nvs_fac`、`coredump`。

## 测试与门禁

```sh
cd RTK_main
tools/ci/run_gates.sh               # 本地一键门禁，合入 main 前运行
tools/ci/run_gates.sh --skip-idf    # 只跑主机检查
```

门禁项（G1–G8）及通过标准见 `RTK_main/docs/acceptance.md`，包括：主机单元测试（含 ASan/UBSan）、0 warning 目标构建、
esp32/esp32c3 双架构构建、clang-format、clang-tidy、C++ 运行时检查（`tools/g7b_check`）、文档检查（`tools/doc_check`）。
GitHub Actions（`.github/workflows/gates.yml`）分主机和 ESP-IDF 两个作业运行同一脚本。

各主机测试目录可单独构建（Unity 取自 ESP-IDF 自带副本），例如：

```sh
cmake -S test/host -B builds/qa-host && cmake --build builds/qa-host && ctest --test-dir builds/qa-host
```

硬件在环工具（SPP 压测、RTCM 混合流、手机设置、网页负载）见 `RTK_main/tools/hil/README.md`。
网页可用 `tools/web_mock/web_mock.py` 离线调试。烧录与串口操作须经授权（`AGENTS.md` §6）。

## 文档

| 文档 | 内容 |
| --- | --- |
| `RTK_main/AGENTS.md` | 工程事实、组件依赖、冻结文件、命令、代理协作规则 |
| `RTK_main/docs/PROGRESS.md` | 已完成系列、进行中项目、待补 HIL |
| `RTK_main/docs/adr/` | 架构决定记录 ADR-0002～0018（ADR-0001 在 `reference/c-v1/`） |
| `RTK_main/docs/CODING-STYLE.md` | 编码规范 |
| `RTK_main/docs/acceptance.md` | 门禁与 HIL 验收标准 |
| `RTK_main/docs/protocol/` | 手机、报警器、测量协议 |
| `RTK_main/docs/web/ws-protocol.md` | 网页 WebSocket 协议 |
| `RTK_main/docs/tasks/` | 进行中的任务卡 |
| `RTK_main/docs/reports/` | 仍被引用的实施报告与裁决证据 |
| `RTK_main/docs/frozen.sha256` | 冻结文件哈希，`sha256sum -c docs/frozen.sha256` 校验 |

`reference/c-v1/` 是早期 C 版实现，作为事实来源保留；`reference/cpp-v3-dispatcher/` 是退役的 CommandDispatcher，
`test/services` 的 golden 等价测试仍以它为基准。

## 开发流程

新功能不直接在 `main` 上开发：每张任务卡一个特性分支（或 worktree），由实现代理完成后交叉评审，
Architect 复跑门禁并在受影响的产品板上做台架验收后合入。任务卡与报告格式见 `RTK_main/AGENTS.md` §6、§7。

## ExternalSpeak

`ExternalSpeak/` 是某一产品外接功放（MAX98357A 类 I2S 功放）的声压测试固件，独立 ESP-IDF 工程，与主固件无代码依赖。

| 信号 | GPIO |
| --- | --- |
| LRC / WS | 25 |
| BCLK | 26 |
| DIN | 22 |
| SD（功放使能） | 21 |

上电默认静音。串口 115200 baud，单字母命令调频率、音量、波形与开关，命令表见 `ExternalSpeak/main/main.c` 文件头。

```sh
cd ExternalSpeak
. ~/esp/esp-idf/export.sh
idf.py build
```
