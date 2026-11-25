# RTK ↔ 服务器 MQTT 围栏与远程配置协议 v0.1（提案）

状态：**提案**（2026-11-25，固件侧起草，待服务器/App 同事评审）。评审通过后写 ADR 并实施。
需求来源：`docs/requirements/P2-nextgen-customer.md`（REQ-004/005/006/008/009/010/014/017/018/019）。
相关：旧版 `MQTT平台指令交互.doc`（上行 GGA 遥测、下行 `warning`）、旧版蓝牙 `P2本地电子围栏专用协议指令-20260707.docx`（0x60/0x61/0x62）、
ADR-0018（设置服务：MQTT 远程配置是其调用方之一）、ADR-0015（4G：UART 32/33，GPIO26 使能）。

> 本文只定义**设备 ↔ 服务器**线路协议。主数据归属、授权模型、对客户的服务器 API 由服务器/App 同事负责（§9 待定项）。

## 1. 传输

- MQTT 3.1.1，由 **EC600 模块内置 MQTT/SSL 协议栈**承载（AT `QMT*`/`QSSLCFG`），ESP32 侧不运行 MQTT/TLS 栈（理由见 §10）。
- TLS 1.2（模块支持则允许 1.3），校验服务器证书；CA 证书存模块文件系统，可远程更新（REQ-014/015，更新通道待 §9）。
- 认证：ClientID + 用户名 + 密码（兼容旧版）；X.509 客户端证书为可选项，待服务器确认。
- QoS：命令与应答 **QoS 1**；周期遥测 QoS 0。不使用 retain（`dev/online` 遗嘱除外）。
- 单条消息上限：暂定 **≤ 1536 B**（含 JSON 包装）。待确认 EC600 `AT+QMTPUB`/接收缓冲上限与 broker 上限后修订（§9）。
- 所有字符串 UTF-8；数值用 JSON number，坐标在二进制记录中为 IEEE-754 double 小端。

## 2. 主题

`<SN>` 为设备序列号（旧版 `client_id`）。

| 方向 | 主题 | 用途 | QoS |
|---|---|---|---|
| 设备→服务器 | `v1/devices/me/telemetry` | **旧版兼容**遥测 `{"gga","warn_status","client_id"}`，v0.1 增加可选字段（§6.1） | 0 |
| 服务器→设备 | `v1/devices/<SN>` | **旧版兼容** `{"params":{"warning":"1|2","warning_precision":"4.0"}}`，语义不变 | 1 |
| 服务器→设备 | `p2/<SN>/req` | 新请求（§3） | 1 |
| 设备→服务器 | `p2/<SN>/resp` | 对 `req` 的应答 | 1 |
| 设备→服务器 | `p2/<SN>/evt` | 主动事件（围栏进出、报警、网络恢复） | 1 |
| 设备→服务器 | `p2/<SN>/online` | 遗嘱/上线：`{"online":true|false}`，retain | 1 |

设备只订阅 `v1/devices/<SN>` 与 `p2/<SN>/req`。

## 3. 请求/应答包装

请求：
```json
{"v":1,"id":1234,"op":"fence.put","args":{ ... }}
```
应答：
```json
{"v":1,"id":1234,"code":0,"msg":"ok","data":{ ... }}
```

- `v`：协议主版本；设备不支持时回 `code=2`。
- `id`：u32，服务器生成，单设备内单调递增（回绕允许）。
- **幂等**：设备缓存最近 8 个 `id` 的应答；重复 `id` 直接重发缓存应答，不重复执行。服务器超时 10 s 未收到应答可用**同一 `id`** 重发，最多 3 次。
- 设备同一时刻只执行一个写请求；执行中收到另一写请求回 `code=5`，读请求照常处理。
- 未知 `op` 回 `code=2`；JSON 解析失败且取不到 `id` 时丢弃并计数，不应答。

### 3.1 结果码

| code | 名称 | 含义 |
|---|---|---|
| 0 | OK | 成功 |
| 1 | BAD_REQUEST | JSON/字段缺失/类型错误/base64 错 |
| 2 | UNSUPPORTED | `v` 或 `op` 不支持 |
| 3 | OUT_OF_RANGE | slot、点数、类型值越界 |
| 4 | CRC_MISMATCH | 记录 CRC 不符 |
| 5 | BUSY | 有写请求或事务在执行 |
| 6 | STORAGE_FAIL | 落盘失败（已回滚，旧数据不变） |
| 7 | NOT_AUTHORIZED | 配置已锁定且调用方无权限（REQ-019，规则见 §9） |
| 8 | TXN_INVALID | 事务号不存在、已过期或已提交 |
| 9 | VERSION_CONFLICT | `if_ver` 与设备当前版本不符 |

## 4. 围栏数据模型

- 容量：**10 个槽位**（slot 0–9）× 每个 **≤ 40 点**（REQ-004）。点数上限由固件常量决定，`dev.caps` 上报。
- 每个槽位带 `ver`（u32，服务器给出）与 `crc`（CRC-32/IEEE，覆盖整条记录）。
- 整套围栏带 `set_ver`（u32），任一槽位变更后由服务器给新值；设备只保存、不自行生成。

### 4.1 围栏记录（二进制，兼容蓝牙 0x61）

头 24 B，与蓝牙 0x61 偏移 0–21 **完全相同**，原 2 B 保留位启用；后接 `point_num × 16 B`。40 点 = 664 B，base64 后 888 B。

| 偏移 | 字段 | 类型 | 说明 |
|---|---|---|---|
| 0 | name | char[16] | 名称，不足补 0 |
| 16 | enable | u8 | 0 关 / 1 开 |
| 17 | led_warn | u8 | 0/1 |
| 18 | beep_warn | u8 | 0/1 |
| 19 | motor_warn | u8 | 0/1 |
| 20 | warn_type | u8 | 1 区域内报警 / 2 区域外报警 |
| 21 | point_num | u8 | 3 – 40 |
| 22 | fence_type | u8 | **新**：围栏类型 0–15（REQ-009），0 = 默认；旧 App 写 0 |
| 23 | reserved | u8 | 置 0 |
| 24+16n | lat | double | 度，WGS-84 |
| 32+16n | lon | double | 度，WGS-84 |

> 注：新固件 `phone_protocol.md` 帧 payload ≤ 256 B，40 点记录超限；蓝牙侧下发大围栏需分片或放宽上限，另议。

### 4.2 报警档案（按围栏类型，REQ-009/010）

每个 `fence_type` 对应一条报警档案；`warn_type` 满足且该类型档案 `mode≠0` 才报警。

| 字段 | 取值 |
|---|---|
| `mode` | 0 不报警 / 1 声+光 / 2 仅声 / 3 仅光 |
| `sound` | 0–N（≥ 2 种音型，N 由 `dev.caps` 上报） |
| `blink_ms` | 0 = 常亮；100–2000 闪烁周期 |
| `motor` | 0/1 |

记录内的 led/beep/motor 位与档案取**与**：记录关闭的输出不因档案打开。

## 5. 操作（`op`）

| op | args | data | 说明 |
|---|---|---|---|
| `dev.caps` | — | `{"slots":10,"max_points":40,"types":16,"sounds":2,"proto":[1]}` | 能力查询 |
| `fence.list` | — | `{"set_ver":7,"items":[{"slot":0,"name":"A","ver":3,"crc":…,"enable":1,"type":0,"points":12}]}` | 只列已用槽位 |
| `fence.get` | `{"slot":0}` | `{"slot":0,"ver":3,"crc":…,"rec":"<base64>"}` | |
| `fence.put` | `{"slot":0,"ver":4,"crc":…,"rec":"<base64>","if_ver":3?,"txn":…?,"set_ver":8?}` | `{"slot":0,"ver":4}` | 无 `txn`：单槽原子替换并立即生效；有 `txn`：暂存 |
| `fence.del` | `{"slot":0,"if_ver":4?,"set_ver":9?}` | — | |
| `fence.patch` | `{"slot":0,"enable":1?,"type":2?,"warn_type":1?,"set_ver":10?}` | `{"slot":0,"ver":…}` | 小消息改属性（REQ-009），不重传坐标；`ver` 由设备 +1 |
| `fence.sync.begin` | `{"txn":55,"count":10}` | — | 开始整套替换，60 s 无进展自动放弃 |
| `fence.sync.commit` | `{"txn":55,"set_ver":11}` | `{"set_ver":11}` | 暂存区校验通过后**一次性**替换全部槽位；未在事务中下发的槽位清空 |
| `fence.sync.abort` | `{"txn":55}` | — | |
| `alarm.profile.get` | `{"type":0}` | 档案 | |
| `alarm.profile.set` | `{"type":0,"mode":1,"sound":0,"blink_ms":500,"motor":0}` | — | |
| `dev.status` | — | `{"net":…,"fix":…,"set_ver":…,"alarm":…}` | |

- 写操作全部**先写暂存、校验 CRC/范围、再原子提交**；失败回 `code=6` 且旧数据保持（掉电安全）。
- 整套同步：`begin` → 逐槽 `put(txn)` → `commit`。中途断网：事务过期，设备保持旧围栏。
- `set_ver` 由服务器给出；设备上线后在 `evt` 中报告当前 `set_ver`，服务器发现不一致再触发同步。

## 6. 上行

### 6.1 遥测（旧版主题，新增可选字段）

```json
{"gga":"$GPGGA,...","warn_status":"0","client_id":"1823P0001","fence_set_ver":11,"net_rtk":1}
```

- `gga` 仅在有定位时填写；无定位时省略该字段（同时满足 REQ-017 对 RTK 服务的要求，NTRIP 侧另行门控）。

### 6.2 事件 `p2/<SN>/evt`

```json
{"v":1,"ev":"fence","slot":0,"state":"enter|exit","alarm":1,"t":"2026-11-25T08:00:00Z"}
{"v":1,"ev":"alarm","src":"fence|warning|precision","on":1}
{"v":1,"ev":"boot","set_ver":11,"fw":"x.y.z"}
{"v":1,"ev":"net","rtk":"ok|lost","since_s":120}
```

- 围栏事件在状态变化时发 1 次（QoS 1），不周期重复。
- 设备端判定频率跟随 GNSS 定位输出，目标从越界定位到报警输出 ≤ 1 s（REQ-004）。

## 7. 兼容

- 旧版遥测与 `warning` 命令**线路格式不变**，旧服务器可继续工作。
- 旧版"1 分钟未收到服务器信息即报警"行为保留与否待服务器确认（§9）。
- 围栏记录与蓝牙 0x61 共用格式，App 与服务器可共用编解码。

## 8. 资源预估（设备侧）

- 围栏存储：10 × 664 B + 元数据 ≈ 6.8 KB，NVS blob 或专用分区（ADR 决定），另需同等大小暂存区用于整套同步。
- 消息缓冲：1.5 KB 收 + 1.5 KB 发，静态分配。

## 9. 待服务器/App 确认

1. broker 单条消息上限；EC600 `QMTPUB`/接收缓冲上限（固件侧查 AT 手册）→ 修订 §1 上限。
2. 认证方式（密码 / X.509）、设备凭据下发方式。
3. 授权模型：首次配置后哪一方有权修改（REQ-019），`code=7` 的判定依据。
4. 围栏主数据在服务器还是 App；冲突时以谁为准（`if_ver`/`set_ver` 机制两种都能支持）。
5. CA 证书更新通道（MQTT 下发 / HTTP 拉取 / App 蓝牙）。
6. 是否保留旧版"1 分钟无下行即报警"。
7. `fence_type` 取值的业务含义。

## 10. 实现边界（设计说明）

- **不在 ESP32 上跑 esp-mqtt/esp-tls**：目标为 ESP32（无 PSRAM），同时运行经典蓝牙 + Wi-Fi SoftAP + httpd；
  关 Wi-Fi 构建实测最小空闲堆仅 80,776 B（HB 报告）。mbedTLS 每条 TLS 会话约需数十 KB（`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384`），
  MQTTS + NTRIPS 两条会话放不下；PPP + lwIP 还要额外开销。
- 因此 MQTT/HTTP/NTRIP 的 TCP/TLS 均用 EC600 内置栈，固件维护：EC600 AT 驱动、链路状态机（上电 → 注网 → PDP → 各连接分级重连，REQ-006）、
  本协议的编解码与围栏存储、经 ADR-0018 设置服务的远程配置调用。
- 风险：TLS 版本与密码套件取决于 EC600 模块固件版本，需实测确认 TLS 1.2/1.3 支持与证书校验行为。
