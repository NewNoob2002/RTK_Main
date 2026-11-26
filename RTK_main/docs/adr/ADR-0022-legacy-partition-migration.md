# ADR-0022：分区表与旧固件统一、旧 EEPROM 配置迁移

状态：**已批准**（2026-11-26，用户批准；§6 Q1–Q4 已答复）。由 C6/C7 任务卡实施（§4）。修订 ADR-0014 §2.1（分区表）、§2.2（数据归属）、§2.7（恢复出厂）、§3（后果）。
关联：ADR-0014（运行期配置）、ADR-0019（网页 OTA 与回滚）、ADR-0021（4G，APN/NTRIP 键）、`docs/requirements/P2-nextgen-customer.md`（围栏 WP-FENCE）、
`docs/reports/P2-legacy-gap.md` §持久化存储。

## 1 背景

- 外场 P2 跑的是旧固件，两条线：Arduino 版（`P2 V2.7` 源码树，客户报的 `2.4.0.9` 就在其 `build/` 里，Arduino core 基于 IDF **v4.4.7**，
  bootloader 为 4MB/DIO/80m）和 IDF 版 `P2_V3.1_IDF`（IDF 5.5.5）。两者分区表**逐字节相同**（解自 `P2_V2.7.ino.partitions.bin` 与
  `P2_V3.1_IDF/partitions.csv`）：

  | 名称 | 类型 | 偏移 | 大小 |
  | --- | --- | --- | --- |
  | nvs | data/nvs | 0x9000 | 0x5000（20 KB） |
  | otadata | data/ota | 0xE000 | 0x2000 |
  | app0 / app1 | app ota_0 / ota_1 | 0x10000 / 0x1F0000 | 0x1E0000 |
  | spiffs | data/spiffs | 0x3D0000 | 0x20000（128 KB，旧固件未使用） |
  | coredump | data/coredump | 0x3F0000 | 0x10000 |

- 旧固件所有配置存在 NVS 命名空间 `eeprom`、键 `eeprom` 的**单个 blob**（Arduino `EEPROM` 库/仿制的 `xyz_eeprom.c`），
  V2.7 长 2280 B（`EEPROM.begin(2024+256)`），V3.1 长 2560 B（`eeprom_size 512*5`），**字段地址两版相同**（§2.5）。
- 现行 ADR-0014 §2.1 的分区表（nvs 0xA000/0x9000、phy_init、otadata 0x14000、ota_0 0x20000、nvs_fac、fence）与旧表重叠且错位：
  1. 旧 NVS 数据无法读取，首次启动 `nvs_flash_init()` 会把错位残留当损坏页擦掉；
  2. OTA 改不了分区表，旧固件的网页/蓝牙 OTA（Arduino `Update`）无法升级到新固件，外场只能串口整片烧录。
- 用户 2026-11-26 决定：分区表与旧固件统一；**旧配置全部迁移**；E1-5.4 是测试板，不考虑其兼容；Arduino bootloader 同步测试。

## 2 决定

### 2.1 分区表：完全沿用旧表

新固件的 `partitions.csv` 与 §1 表**逐项相同**（名称、类型、子类型、偏移、大小都不变），出厂新板与外场 OTA 升级的旧板用同一张表。

- 删除 `phy_init`（保持 `CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION=n`，PHY 校准仍存 NVS `phy` 命名空间）、`nvs_fac`、`fence` 三个分区。
- `spiffs` 分区**名称与子类型不改**（OTA 升级的旧板分区表改不了，两类设备必须一致），由固件当作 NVS 使用，下称 **cfg 区**。
- app 槽 1,966,080 B；当前镜像 1,586,176 B（MT1），余量约 370 KB。`ota_0` 偏移由 0x20000 改为 0x10000。
- 不在运行期改写 0x8000 的分区表（掉电即变砖，见 §5）。

### 2.2 两个 NVS 实例的分工

| 实例 | 位置 | 内容 | 初始化 |
| --- | --- | --- | --- |
| 系统 NVS | `nvs` 0x9000，20 KB | IDF 自用（PHY 校准、Wi-Fi、蓝牙绑定 `bt_config`）；旧 `eeprom/eeprom` blob **只读保留**；SN/PN 备份副本 `facbk/*` | `nvs_flash_init()` |
| cfg NVS | `spiffs` 0x3D0000，128 KB | 出厂 `fac/*`（SN、PN、预留板卡信息/注册码）；用户配置 `gnss`、`rtk`、`link`、`dbg`、`boot`、`net`、`ntrip` …；围栏 `fence`；迁移状态 `mig` | `nvs_flash_init_partition_ptr()`，分区指针由 `esp_partition_find_first(DATA, SPIFFS, "spiffs")` 取得 |

- cfg 区是**一个**完整 NVS 实例，不在分区内再切多个实例；各类数据靠命名空间区分。由 C6 在 IDF 6.1 上验证 `nvs_flash_init_partition_ptr`
  对 spiffs 子类型分区可用、`nvs_open_from_partition("spiffs", …)` 能按标签打开（官方公开接口，不需手工构造 `esp_partition_t`）。
- **SN/PN 双副本**（替代 ADR-0014 的独立 `nvs_fac` 分区）：`fac/sn`、`fac/pn` 在 cfg 区，`facbk/sn`、`facbk/pn` 在系统 NVS。
  启动时两处比较：一处缺失或损坏 → 用另一处补写；两处都有但不同 → 以 cfg 区为准并打 W。`factory sn` 同时写两处。
- 擦除策略（保留 ADR-0014“绝不随手丢 SN”的约束）：
  - 系统 NVS 遇 `NO_FREE_PAGES` / `NEW_VERSION_FOUND`：先尝试只读拷出 `facbk/*` 与 `eeprom/eeprom`，擦除后写回，再打 E；拷不出就照擦并打 E。
    IDF 6.1 冷启动初始化失败时不会登记该实例，旧 blob 无法通过 NVS API 拷出，擦除后会丢失并跳过 C7 迁移，SN/PN 则由 cfg 回填。
    旧固件 IDF 4.4.7 与 IDF 6.1 使用相同的 NVS 页格式版本，正常 OTA 升级不会触发此路径，只有页满或损坏等分区异常才会触发。
  - cfg NVS 遇同样错误：擦除 cfg 区后由 `facbk/*` 恢复 `fac/*`，用户配置回默认，打 E。
  - 两者都不会去擦对方。
- 围栏改存 cfg 区 `fence` 命名空间：每个槽一个 blob（0x61 记录格式，24+16n B，40 点约 664 B），10 槽约 6.6 KB；NVS 单键写入原子，
  不再需要 ADR-0014 的 2×16 KB 双槽轮换。格式细节仍由 WP-FENCE 定。
- 容量估算：系统 NVS 20 KB（5 页，1 页留作 GC）装 PHY 约 2 KB + Wi-Fi 约 2 KB + 蓝牙绑定约 1 KB + 旧 blob 2.5 KB + 备份 < 0.1 KB，
  余量约 8 KB；cfg 区 128 KB（32 页）远大于现有键（约 4 KB）+ 围栏 6.6 KB。C6 用 `nvs_get_stats` 实测记录。

### 2.3 恢复出厂（修订 ADR-0014 §2.7）

- `freset`：对 cfg 区的用户命名空间（`gnss`、`rtk`、`link`、`dbg`、`boot`、`net`、`ntrip`、`fence`）逐个 `nvs_erase_all`；
  删除蓝牙绑定（`esp_bt_gap_remove_bond_device` 逐个，或 `bt_config` 命名空间清空）与 Wi-Fi 配置；**保留** `fac`、`facbk`、`mig`、
  旧 `eeprom` blob、PHY 校准、coredump。不再整片 `nvs_flash_erase()`。
- `factory erase`（需解锁）：再清 `fac`、`facbk`，仍保留旧 blob 与 `mig`（防止重复迁移把旧配置又灌回来）。

### 2.4 迁移流程

在 `stepEarlyConfig`（ADR-0014 §2.3）两个 NVS 实例初始化之后、读用户配置之前执行，单次、幂等：

1. `mig/done` 已存在 → 跳过。
2. 系统 NVS 无 `eeprom/eeprom` → 写 `mig/done = none`，跳过（新板）。
3. 读整个 blob 到启动期临时缓冲（≤2560 B，迁移结束即释放或复用，不新增常驻 RAM；C7 实测记录）。长度只接受 2280 或 2560，其余打 W 并写
   `mig/done = bad_len`。
4. 按 §2.5 逐字段解析、校验；**目标键已存在则不覆盖**（新固件上已改过的配置优先）；非法字段跳过、保持默认，并计数。
5. 写入后 `nvs_commit`，读回校验；全部成功写 `mig/done = v1`、`mig/src_len`、`mig/stats`（迁移/跳过/非法计数），打一行 I
   `Legacy config migrated: N fields, M skipped, K invalid`；中途失败不写 `mig/done`，下次启动重试（已写的键因“不覆盖”规则不会被改坏）。
6. 旧 blob 不删：回刷旧固件时旧配置仍在（但不会包含升级后的修改）；后续阶段的功能（§2.6）也从这份 blob 再解析各自的字段。

### 2.5 字段映射（地址为 blob 内偏移，V2.7 与 V3.1 相同）

字符串字段：取到首个 `\0` 为止，长度上限为字段宽度；全 `0xFF`、空串或含控制字符视为“未设置”。数值字段：`0xFF`/`0xFFFFFFFF`
（从未写过）视为未设置。

**第一阶段（C7）迁移，新固件已有对应键：**

> 2026-11-26 修订（用户决定）：C7 **只迁移 SN、PN**。星座、NMEA 频率、截止角、SBAS、JSON 服务器、ThingsBoard
> 不迁移，升级后用新固件默认值，由用户在 nav App 重新配置。原因：旧固件对 0/0xFF 的语义与新键不一致
> （F9P 线频率 0 实为“保持 1 Hz 输出”，星座全 0 实为“从未配置”），迁移易造成升级后无输出；这些都不是生产配置。
> 下表除 SN/PN 两行外仅作旧地址记录。

| 旧字段 | 地址/宽度 | 新键 | 换算与校验 |
| --- | --- | --- | --- |
| SN | 200 / 16 | `fac/sn` + `facbk/sn` | 可见 ASCII；与 `factory.sn` 同一校验 |
| PN | 216 / 16 | `fac/pn` + `facbk/pn`（现 `rtk/pn` 迁入 `fac`） | 原文保存，不解析（§2.7） |
| GPS/BDS/GLO/GAL 开关 | 300–303 / 各 1 | `gnss.const.gps/bds/glo/gal` | 0/1；其他值未设置 |
| GGA/GSA/GSV/GST/RMC 频率 | 304–308 / 各 1 | `gnss.out.gga/gsa/gsv/gst/rmc` | 旧值：正数 = Hz，0 = 关（旧固件把负值当 0）。换算为周期 ms = 1000/Hz；按当前 `gnss.rate` 对齐，不能整除的取最近合法值并计入“非法” |
| 截止高度角 | 310 / int32 | `gnss.cutoff` | 1–90；旧固件 ≤0 用 15 |
| SBAS | 314 / 1 | 卫星设置的 sbas 位（0x24 路径 `rtk/sat_cfg`） | 0/1；F9P 无 SBAS 能力时按能力掩码清零 |

**第二阶段，随对应功能迁移（同一份 blob 再解析，`mig` 下各自记 `mig/net`、`mig/fence` 等标记）：**

| 旧字段 | 地址/宽度 | 新键 | 随哪张卡 |
| --- | --- | --- | --- |
| APN、APN 用户名、APN 密码 | 0/32、32/16、48/16 | `net.apn`、`net.apn_user`、`net.apn_pass`（ADR-0021 新增） | 4G N1 |
| 内置 NTRIP 开关 | 444 / 1 | `ntrip.enabled` | 4G N2 |
| NTRIP 域名、端口、用户名、密码、挂载点、IP | 64/32、96/int32、100/16、116/16、132/16、445/16 | `ntrip.host`、`ntrip.port`、`ntrip.user`、`ntrip.pass`、`ntrip.mount`（域名为空时用 IP 作 host） | 4G N2；同时取代 `rtk/ntrip_cfg` 原始帧 |
| JSON 服务器地址、端口 | 320/32、352/int32 | 原样存 `rtk/json_srv` | 现在即可（C7），功能按 ADR-0021 只预留 |
| ThingsBoard 开关、client_id、用户名、密码、主机、端口 | 356/1、360/16、376/16、392/16、408/32、440/int32 | 原样存 `rtk/tb_cfg` | 现在即可（C7），以后由 N3 MQTT 取代 |
| 围栏：数量、存在表 | 512 / 1、513+i / 1 | `fence/*` | WP-FENCE |
| 围栏 i（i<5）：id | 2228+i / 1 | 同上 | WP-FENCE |
| 名称、启用、告警类型、LED、蜂鸣、马达、顶点数 | 518+342i / 16，534–539+342i / 各 1 | 同上；`fence_type` 旧数据无，取默认 | WP-FENCE |
| 顶点 j（j<20）经度、纬度 | 540+342i+16j / double，+8 / double | 同上；经度 ±180、纬度 ±90 校验，顶点数 3–20 | WP-FENCE |

- 不在旧 blob 里的配置（天线高、基站模式、蓝牙白名单、Wi-Fi、OLED）不迁移，用新固件默认值。
- 迁移代码对每个版本独立的地址表放在一个只读常量表里，主机测试用 V2.7 与 V3.1 两份真实 blob 样本（从真机读出）做金样。

### 2.6 升级路径与 bootloader

- 外场旧板（全部是 V2.x Arduino 线，§6 Q1）：用旧固件自己的**网页 OTA**（V2.7 `Form.ino`，§6 Q2）写新固件到另一个 app 槽 → 重启进入新固件 → §2.4 迁移。
  bootloader **不更新**（运行期改写 bootloader 掉电即变砖，见 §5）。
- 因此新 app 要能被 Arduino（IDF 4.4.7）和 IDF 5.5.5 两种旧 bootloader 引导，必须真机验证（§4 HIL）：
  1. 镜像头 flash 参数一致（4MB/DIO/80m；新固件 sdkconfig 选 QIO，实际镜像头写的是 DIO，C6 确认）；
  2. 旧 bootloader 能识别 IDF 6.1 镜像的芯片版本字段（`min_chip_rev_full`/`max_chip_rev_full`）与 app 描述；
  3. 旧 bootloader 的 otadata 格式与 IDF 6.1 `esp_ota_*` 互认（新固件之后的 OTA、回刷旧槽都要能切换）。
- **回滚能力取决于 bootloader**：Arduino bootloader 未开 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，首次从旧固件升到新固件、以及之后
  新固件之间的 OTA 都**没有自动回滚**；ADR-0019 §2.5 的“确认有效/回滚”只在串口整片烧录了新 bootloader 的板子上生效。
  补救（§6 Q4）：新增“手动切回另一槽”。发布版关闭终端控制台，且 Type-C 未接 ESP32 串口，客户用不到控制台，所以**主入口是 Wi-Fi 配置网页的按钮**
  （ADR-0019 OTA 页，`POST /ota/rollback`，另一槽有合法镜像时才可用，二次确认后设为启动分区并重启）；`ota rollback` 控制台命令仅供开发/产线调试。
- 新出厂板串口整片烧录新 bootloader + 统一分区表 + 新 app，具备回滚。
- 回刷旧固件：旧固件读到的仍是未改动的旧 blob，可正常启动。需验证 IDF 6.1 写过的系统 NVS 页在 IDF 4.4.7 / 5.5.1 的 `nvs_flash_init`
  下**不触发** `NEW_VERSION_FOUND`（否则旧固件会整片擦 NVS，旧配置和蓝牙绑定都丢）。

### 2.7 产品识别：硬件探测为主，PN 管选配

- 产品板型继续由 GPIO35 ADC 识别（早于读 NVS，决定引脚与电源初始化）；GNSS 板卡由运行期协议探测（现状，优于旧固件按 `PN[4..5]` 查表）。
- PN 按《PN-SN编码规则》解析：`[0-1]` 产品类别、`[2-3]` 机型类、`[4-5]` 板卡（15 = F9P、19 = CM680、16 = 980 …）、`[6-7]` 产品（03 = P2）、
  后 5 位选配（倾斜、电台、4G、通讯、显示）。用途：对外上报型号、开放探测不到的选配（倾斜等）。
- PN 与探测结果不一致（如 PN 写 F9P、探测到 CM680）→ 按硬件运行，打 W，`sys status` 与手机 0x08 应答附“PN 不符”标志，便于产线发现填错。
- PN 解析与选配开关属于后续卡（随 IMU/倾斜立项），本 ADR 只规定原则；迁移阶段 PN 原文保存。

## 3 后果

- 外场旧板可直接用旧固件的 OTA 升级到新固件，配置自动迁移，不需返厂或串口。
- 现有开发/测试板（含 E1-5.4、P2 v1.41 测试板）需整片烧录一次统一分区表，原 0xA000 nvs、`nvs_fac` 里的数据丢弃（测试板，用户已同意不考虑）。
- `nvs_fac` 独立分区取消，SN 保护改为“双 NVS 实例双副本 + 不互相擦除”；保护强度相当（两份同时损坏才丢）。
- 系统 NVS 只有 20 KB，Wi-Fi/蓝牙绑定多了会挤；新固件自有数据全部放 cfg 区，系统 NVS 只增加约 0.1 KB 备份。
- 经旧 bootloader 升级的板子没有自动回滚，新固件发布前的回归必须覆盖“旧 bootloader + 新 app”。
- ADR-0014 §2.1/§2.2/§2.7/§3 与 §4 C1 的分区相关描述由本 ADR 取代；`partitions.csv`、`components/nvs_store/NvsStore.c`、
  `main/modules/storage/StoreKeys.hpp`、`docs/reports/P2-legacy-gap.md` §持久化存储随 C6/C7 更新。

## 4 实施计划

| 卡 | 内容 | 依赖 |
| --- | --- | --- |
| C6 | 统一分区表；cfg NVS 实例（`nvs_store` 双实例 API）；现有键迁到 cfg 区；SN/PN 双副本；擦除策略；`freset`/`factory erase` 按命名空间；`nvs_get_stats` 记录；网页手动切槽 `POST /ota/rollback`（+ 调试用 `ota rollback`） | — |
| C7 | §2.4 迁移框架 + §2.5 SN/PN（2026-11-26 修订）；V2.7/V3.1 真机 blob 金样主机测试 | C6 |
| 后续 | 第二阶段字段随 4G N1/N2、WP-FENCE 迁移 | C7 |

HIL（C7 验收，以 Arduino V2.x 线为主；V3.1 IDF 线外场没有，只做一次 bootloader 引导冒烟）：

1. 整片烧录旧固件（Arduino V2.7 的 `P2_V2.7.ino.bootloader.bin` + 分区表 + app）；用旧 App 写 SN/PN（其余字段不迁移，可不写）。
2. 串口读出 0x9000 nvs 存档（作为主机测试金样）。
3. 用旧固件自身的网页 OTA 写入新固件 → 重启：确认旧 bootloader 能引导、`Legacy config migrated` 计数正确、`config get` 各键值与旧配置一致、
   SN/PN 两副本一致、蓝牙绑定保留。
4. 新固件网页 OTA 到另一版本新固件，确认 otadata 切换正常；再切回旧固件槽，确认旧固件启动且配置仍在、NVS 未被整片擦除。
5. `freset` 后确认 `fac`/`facbk`/`mig`/旧 blob 保留、用户配置回默认、不会再次迁移。

## 5 被否决的方案

- **LittleFS/SPIFFS 存 ini 配置文件**：SPIFFS 非掉电安全且 IDF 已不推荐；LittleFS 需“临时文件 + rename”才原子，每个打开文件还要读写缓存，
  无 PSRAM 时最小空闲堆仅 58,624 B（Wi-Fi 构建），偏紧；且 ADR-0014/0018 的设置注册表、补偿写入、主机测试都基于按键存储，换存储层改动大。
  可读性需求用“从注册表导出 ini/JSON 下载”满足，不改存储层。
- **在 spiffs 区内切出多个 NVS 实例**（手工构造 `esp_partition_t` 或 `esp_partition_register_external`）：非常规用法，后者还会与已登记的 spiffs 分区重叠被拒；
  单实例 + 命名空间已能满足归属划分，SN 保护由双副本解决。
- **运行期改写 0x8000 分区表或 bootloader**：掉电即变砖，且外场无串口救援手段。
- **保持现行分区表、写只读 NVS 页解析器读 0x9000 残留**：仍然无法用旧固件 OTA 升级（app 偏移与分区表不同），外场必须串口整片烧录。

## 6 开放问题

用户 2026-11-26 答复：

1. 外场设备是否有 V3.x IDF 线 → **全部是 V2.x Arduino 线**。迁移与 HIL 以 Arduino bootloader + V2.7 blob（2280 B）为主；2560 B 长度仍接受。
2. 外场 OTA 入口 → **网页 OTA**。蓝牙 OTA 不作为升级路径验证。
3. JSON/ThingsBoard 原始帧 → **第一阶段（C7）就迁**。（2026-11-26 用户改为不迁移，见 §2.5 修订。）
4. 手动切槽 → **增加**。发布版关闭终端控制台，Type-C 也未接 ESP32 串口，因此做成网页按钮（§2.6），控制台命令只作调试。
