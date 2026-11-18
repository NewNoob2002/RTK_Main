# ADR-0014：运行期配置、出厂数据分区与恢复出厂

状态：已批准（2026-11-18）。决定来自用户（2026-11-17）；标 **〔提议〕** 的细节由 Claude 补充，用户 2026-11-18 全部同意。由 C 系列任务卡实施（C1–C5，见 §4）
关联：ADR-0010 §2.4（流动站配置，本 ADR 修订）与 §2.10（NVS 目录）、ADR-0011（启动与内存）、ADR-0007（蓝牙会话）、
`docs/protocol/alarm_protocol.md`（报警器协议，暂停中，不在本 ADR 范围）

> 命名：仓库里 `R0`–`R8a` 已用于 2026-11 前的重构卡，本系列用 **C**（Config）编号，避免与历史卡和报告重名。

## 1 背景

现在的持久化只有 SN、报警器白名单、`gnss.config`（rate/gsv/out）、`gnss.cache`（探测提示）和 `debug`，都在 `nvs` 分区的
`rtk`/`gnss`/`debug` 命名空间里：

- 生产数据（SN）和用户数据在同一分区。`components/nvs_store/NvsStore.c:21` `nvs_store_init()` 遇到 `NO_FREE_PAGES` /
  `NEW_VERSION_FOUND` 会**整片擦除** `nvs`，SN 跟着丢。
- 接收机每次启动重新写 RAM 配置（ADR-0010 §2.4），用户改过的波特率、工作模式、基站参数无处保存。
- 没有面向客户的恢复出厂；`factory wl-clear` 只清白名单且要解锁。
- 启动日志对存储几乎不出声，加载失败被吞掉（`StorageModule::loadKeyTables`）。
- 蓝牙对任何配对请求自动接受（IO cap NONE、自动确认），无授权窗口。

目标：做到 SparkFun RTK 那种“改了就记住、重启还在”，同时不丢出厂数据。

## 2 决定

### 2.1 分区表（8 MB flash）

| 名称 | 类型 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- | --- |
| nvs | data/nvs | 0xA000 | 0x9000 | 用户数据，`freset` 清除 |
| phy_init | data/phy | 0x13000 | 0x1000 | 保留 |
| otadata | data/ota | 0x14000 | 0x2000 | 保留（本阶段无 OTA，ADR-0002 §9） |
| ota_0 / ota_1 | app | 0x20000 / 0x320000 | 0x300000 | 由 1.625 MB 放大到 3 MB（用户 2026-11-18：模块确认 8 MB 且后续沿用） |
| **nvs_fac** | data/nvs | 0x620000 | 0x6000 | **新增**：出厂数据，`freset` 不碰 |
| **coredump** | data/coredump | 0x630000 | 0x10000 | **新增**：崩溃转储到 flash，`freset` 不碰 |

- 删除未使用的 `spiffs`（原 0x360000 起 0x19000）。0x640000–0x7FFFFF（1.75 MB）留空，暂不上 littlefs。
- nvs/phy_init/otadata 不动：老板子的用户数据（白名单、蓝牙绑定）升级后仍在。
- 启用 `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`；启动时若有转储，打一行 W `Core dump found`；读取命令〔提议〕`sys coredump`。

### 2.2 数据归属

| 归属 | 分区/命名空间 | 内容 | 谁能写 |
| --- | --- | --- | --- |
| 出厂数据 | `nvs_fac` / `fac` | SN、板卡信息、校准、注册码（**只预留槽位**，格式待定） | `factory ...`（需解锁） |
| 用户配置 | `nvs` / `gnss`、`link`、`dbg`、`boot` … | 接收机设置、蓝牙策略、诊断开关 | 用户命令、手机 App |
| 用户数据 | `nvs` / `rtk` | 报警器白名单、蓝牙绑定（IDF `bt_config`） | 运行期 |
| 缓存 | `nvs` / `gnss` 的 cache 键 | 上次探测的家族/型号/波特率 | 固件自动 |

- `nvs_store_init` 只允许在 `nvs`（用户分区）上做擦除重试，**绝不擦 `nvs_fac`**；`nvs_fac` 初始化失败 → 启动继续，
  SN 回退到 MAC 派生名并打 E。
- SN 迁移（C1）：启动时 `fac/sn` 不存在而 `rtk/sn` 存在 → 复制到 `fac/sn`，校验读回一致后删除 `rtk/sn`；任一步失败保留旧值并下次重试。
- 任务栈大小**不可配置**（编译期常量）。任务优先级与核心也**不可配置**（用户 2026-11-18）。
- `config set` 只放在 factory 命令集里（解锁后可用），普通用户用各模块自己的命令（如 `gnss out`）。

### 2.3 启动顺序：两段加载

1. banner 之后、分配之前新增 `stepEarlyConfig`：初始化两个 NVS 分区，读 `boot.*`、`gnss.port.*`、`dbg.*` 到 `BootConfig`，逐项校验，
   越界值用缺省并打 W。
2. 启动失败计数 `boot.fails`：进 `stepEarlyConfig` 时 +1，`boot done` 时清 0；连续 ≥3 次〔提议：阈值 3〕 → 本次忽略用户配置用缺省启动，
   打 W `Boot failed N times, using default settings`（不擦 NVS，用户可 `freset`）。
3. 其余键表仍在 `stepStorage` 加载。
4. 存储启动日志（SparkFun 风格，每类一句 I）：NVS 用量（`nvs_get_stats`）、SN、白名单条数、每组 `N saved, M default`、
   gnss.cache 名称。逐键明细只在 `RTK_BOOT_VERBOSE` 或该组有加载失败时打印；每个失败键单独一行 W。
   `config` 命令和启动明细共用同一个键值格式化函数。

### 2.4 接收机配置持久化（修订 ADR-0010 §2.4）

- **NVS 是唯一事实来源**；启动仍按 NVS 内容把配置写进接收机 RAM（保持“上电即确定”）。
- **SAVECONFIG**：用户改动**成功应用到接收机之后**自动发一次 `SAVECONFIG`（u-blox：CFG-VALSET 写 BBR/Flash 层）；
  另有手动 `gnss save`。启动流程本身**不**发 SAVECONFIG（不因每次上电磨损接收机 flash）。
- 持久化的接收机设置：主机口波特率、接收机端口号、各报文速率、工作模式（rover / base / smartbase）、基站参数（§2.6）。
- `gnss freset`：向接收机发出厂复位（Unicore `FRESET`；u-blox CFG-CFG clear），**同时**把 NVS 的 gnss 用户键恢复缺省，再重新探测。

### 2.5 统一输出命令 `gnss out`（单位 Hz）

```
gnss out                       # 显示当前表
gnss out gga 1                 # 1 Hz；0 或 off 关闭
gnss out gsv 0.2               # <1 Hz 只接受 1/n：0.5/0.2/0.1 …
gnss out rtcm 1005 0.1         # 基站 RTCM 报文（§2.6）
gnss out rtcm msm 7            # MSM4 或 MSM7 整组
gnss out default               # 恢复缺省
```

- `gnss rate` 保留为 `gnss out` 定位类报文的快捷方式，帮助文本指向 `gnss out`。
- NVS 与蓝牙协议里的 int8 编码不变（正数 = Hz，负数 = 周期秒数；`GnssService::intervalToPeriodMs`），只改控制台解析和显示。

### 2.6 工作模式与基站

- 模式：`rover`（缺省）/ `base` / `smartbase`，〔提议〕`gnss mode <...>` 设置并持久化（`gnss.base` 组）。
- 基站 RTCM 报文集合用户可选：1005/1006/1033、MSM4 或 MSM7（1074/1084/1094/1124 … 或 1077/1087/1097/1127 …）、1230、星历；
  存 `gnss.base`。输出去向（sink 枚举）第一阶段只有蓝牙 SPP。
- 基站坐标：手工输入或测量（survey-in）。
- **smartBase**：最多 4 个已记录站点；开机定位后若与某站点距离 ≤1 m，自动以该坐标启动基站；都不匹配 → 运行配置的缺省模式。
  控制台或手机 App 可随时切换。

### 2.7 恢复出厂与诊断开关

- `freset`（客户可用，需二次确认，〔提议〕`freset yes`）：清 `nvs` 中的用户配置、报警器白名单、蓝牙绑定、gnss 缓存；保留 `nvs_fac`、
  `phy_init`、`coredump`；完成后重启。
- `factory erase`：需 `factory unlock`，额外清 `nvs_fac`（产线返修用）。
- 诊断开关（`dbg.*`）在早期加载，决定启动期 trace 类别与 `RTK_BOOT_VERBOSE` 等，`freset` 恢复缺省。

### 2.8 蓝牙配对授权（C4）

- 设备**始终可被发现**；配对窗口外的配对请求一律拒绝，**白名单设备除外**。
- 窗口由用户手动打开（〔提议〕控制台 `bt pair open [s]` / 手机 App），**首次开机也不自动开窗口**。
- 窗口内可选“逐个确认”模式；配对成功后可选择加入白名单（角色 phone/alarm）。

## 3 后果

- 分区表变更需要整片烧录（含分区表）；已出货板子上的 SN 靠 §2.2 迁移保住，`spiffs` 无数据可丢。
- coredump 分区占 64 KB，换来现场崩溃可追。
- 多一步早期加载，启动时间预计增加 <20 ms（C1 实测记录）。
- ADR-0010 §2.4 “不 SAVECONFIG”改为“仅用户改动后 SAVECONFIG”。

## 4 实施计划（C 系列）

| 卡 | 内容 | 依赖 |
| --- | --- | --- |
| C1 | 分区表 + coredump；`nvs_fac` 与 SN 迁移；`nvs_store_init` 只擦用户分区；存储启动日志（§2.3-4）；`config` 共用格式化 | — |
| C2 | 接收机配置持久化（波特率/端口/模式键）、自动 SAVECONFIG + `gnss save`、`gnss freset`、统一 `gnss out`（Hz） | C1 |
| C3 | `stepEarlyConfig` 两段加载、`BootConfig` 校验、启动失败计数回退 | C1 |
| C4 | `freset` / `factory erase`、诊断开关、蓝牙配对窗口 | C1、C3 |
| C5a | 工作模式与基站 RTCM 报文集合 | C2 |
| C5b | smartBase（4 站点、1 m） | C5a |
| C5c | 其他输出去向（sink） | C5a，待用户再定 |

每张卡：主机测试覆盖新逻辑，IDF 构建 0 warning，G5b/G7b/format/doc_check 全过，上板 HIL；分区或 NVS 布局变化的卡附“老板子升级”实测
（先烧旧固件写入 SN 和白名单，再烧新固件确认迁移）。

## 5 开放问题


- 注册码格式与校验方式（只预留 `fac/reg` 槽位）。
- 板卡信息/校准数据的具体字段（C1 只建 `fac` 命名空间与 SN）。
- C5c 的 sink 种类（NTRIP、UART 透传…）。
