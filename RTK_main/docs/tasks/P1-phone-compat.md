# P1 手机协议兼容：0x24 天线高 / 0x25 / 0x29 / 0x33 PPP

依据：2026-11-19 P2 台架抓包（app 对 0x24/0x25/0x33 各重发 3 次超时，日志 RTK_Main-w1/RTK_main/builds/w1a-hil/phone-trace.log）；旧固件 P2 V2.7 `meaasge.ino:436-619`、`UM980.ino:95-130`；用户决定（2026-11-19）：天线高掉电保存；PPP 按旧固件真实配置 CM680。
基线：main 56d022c。实现：Codex；Architect（Claude）评审、改 docs/protocol/phone_protocol.md（冻结）、HIL、提交。

## 1 目标（报文格式沿用现有 link_core services 的 query/set 应答类型与头部）
1. **0x24 天线高**：query → 应答 16 B 负载，[0..7] = 天线高 IEEE754 double 小端（单位 m），其余 0；set → 负载 [0..7] 为 double，校验有限且 0 ≤ h ≤ 10 m（越界回失败），写 NVS 新键（StoreKeys/StoreCatalog，新增 StorageVerb 或复用配置写路径，去抖规则同现有键），应答 set-OK（2 B `01 01`）。不启动倾斜对准（无 IMU 驱动），报告注明。默认 0.0。
2. **0x25 惯导数据**：应答 set-res 类型、2 B 全 0（与旧固件一致）。
3. **0x29 BASEINFO**：应答 set-OK `01 01`。
4. **0x33 PPP**：query → 4 B 负载 [0]=ppp_enable(0/1)；set → 负载 [0]，持久化 NVS，经 fw::Command 交 gnss 模块（kIo）执行：开 = `CONFIG SBAS DISABLE`、`CONFIG SIGNALGROUP 2`、`CONFIG PPP ENABLE E6-HAS`、`CONFIG PPP CONVERGE 15 25`、`SAVECONFIG`；关 = `CONFIG PPP DISABLE`、`SAVECONFIG`（旧固件关时不恢复 SBAS，保持一致）。每条等 `$command,...,response: OK`，任一 ERROR/超时 → 应答失败并 FW_LOGW；仅 Unicore 家族（UM98x/CM680）支持，其它型号应答失败。gnss 模块重新配置（上电/重探测）后按 NVS 值重放 PPP 配置。
5. 0x28/0x30/0x34/0x36 维持不回复。
6. 不改 docs/**、AGENTS.md、components/*/include/**（冻结）；若必须改公共头，列在报告 §冻结待改，停止并说明。
7. host 测试：services 测试覆盖 0x24 query/set/越界/NaN、0x25、0x29、0x33 query/set；gnss 测试覆盖 PPP 命令序列与 NACK；main_host_check 覆盖 NVS 回读。

## 2 测试
`tools/ci/run_gates.sh --base 56d022c --prefix p1` 全 PASS；镜像增量 >4 KB 说明。HIL 由 Architect：app 连接无超时；天线高重启后回读；PPP 开/关 CM680 回 OK（CM680 协议文档未列 CONFIG PPP，若 NACK 由 Architect 决定）。

## 3 交付
报告 docs/reports/P1.md（含 §冻结待改、§文档待改）；不提交、不烧录；打印 `P1 DONE`。

## 4 Revision A（2026-11-19 Architect，HIL 结论 + 用户决定）

HIL：CM680 对 `CONFIG SIGNALGROUP/CONFIG PPP *` 全部回 `PARSING FAILD GRAMMAR ERROR`，仅 `CONFIG SBAS DISABLE` OK。app 对 0x04/0x05/0x21/0x22/0x23 收到我们的 query-res(0x01) 后仍每 3 s 重发 3 次（日志 builds/p1-hil/phone-trace2.log）。用户：使用 Survey app 与 Configure(Nav) app；补全其余旧固件消息，暂不能实现的做占位。参考旧固件 `P2_V2.7/meaasge.ino`（路径见 P1 依据）逐条对齐。

1. **按 app 类型应答**：旧固件按头部 sender 字节区分：`0x00` = Nav/Configure app，其它（报警器 `0x03` 除外，保持现有 ALARM 角色）= Survey app。每帧取 sender 得到 AppKind，传给服务（不改变会话角色判定）。
2. **0x04/0x21/0x22**：Nav → query-res 最近一句原文（现行为）；Survey → set-res(0x03) `01 01`，并保持订阅语义（interval≠0 开启直传）。**0x23** 两类都 set-res `01 01`。**0x05**：Survey → set-res 2 B 全 0；Nav → query-res 2 B 全 0。interval 订阅逻辑不变。
3. **PPP**：Capabilities 新增“支持 PPP”判定（UM98x 支持，CM680 不支持；放在 gnss 驱动能力或 Registry，不改公共头则用私有表）。不支持时 0x33 set 不发送任何命令，直接回 `02 02`；query 回 PPP=0。UM98x 保持原脚本。
4. **延迟应答正规化**：去掉 LinkService.cpp 中 `s_phone_link/s_phone_service` 全局与 `bindPhoneCompat`；在 link_core 公共接口中加入通用“延迟应答”机制（例如 DispatchResult::kDeferred + 由服务持有 credit/handle，LinkService::poll 发送）。**Architect 授权修改 `components/link_core/include/**`**，报告 §冻结待改 列出文件，Architect 重算 frozen.sha256（G1 届时会 FAIL，可接受，报告注明）。
5. **补全旧固件消息**（真实 = 有实际效果；占位 = 按旧固件长度回固定/回显内容）：
   | ID | 应答 | 级别 |
   | --- | --- | --- |
   | 0x06 Survey 截止角/星座 | query 12 B：[1]=截止角 [4]GPS [5]GLO [6]BDS [7]GAL [8]SBAS；set 持久化 NVS 并经 kIo 下发 `MASK <deg>`、`MASK/UNMASK GPS/GLO/BDS/GAL`、`CONFIG SBAS ENABLE AUTO/DISABLE`，逐条等 OK，失败回 `02 02`；重探测后重放。Nav 维持现占位 | 真实 |
   | 0x08 Nav SN/PN | query 48 B [0..15]SN [16..31]PN；set 写 SN（复用现有 SN 存储/kSaveSn）与 PN（新 NVS 键），回 `01 01`；设备名随 N1 规则，重启生效 | 真实 |
   | 0x09 4G 信息 | 按旧固件长度回全 0 | 占位 |
   | 0x0A Nav GSV | query-res 最近 GSV 原文（无缓存 `02 02`） | 真实 |
   | 0x11 挂载点列表 | `[0]=1 [1]=0`（0 个），共 4 B | 占位 |
   | 0x12 NTRIP | query 112 B 回显已存配置；set 存 RAM+NVS 回 `01 01`；无 4G，不连接 | 占位（存储真实） |
   | 0x13 | query-res 132 B 全 0 | 占位 |
   | 0x1D OTA | 回 `02 02` | 占位 |
   | 0x1E 恢复/重启 | set：[0]≠0 → 复用现有 FactoryReset 流程（含 GNSS freset 若现有流程有）；[3]≠0 → 重启；先回 `01 01` 再执行（延迟 ≥200 ms 让应答发出） | 真实 |
   | 0x26 对准 | query 4 B 全 0；set 回 `01 01` | 占位 |
   | 0x27 | set-res 2 B 全 0 | 占位 |
   | 0x32 Nav JSON 服务器 | query 42 B 回显；set 存储回 `01 01` | 占位（存储真实） |
   | 0x33 Nav Thingsboard | query 86 B 回显；set 存储回 `01 01`（Survey 仍为 PPP） | 占位（存储真实） |
   | 0x60/0x61/0x62 电子围栏 | 按旧固件格式：0x60 query `[0]=0`；0x61/0x62 set 回 `02 02`、query 回 `02 02` | 占位 |
   读旧固件确定每条的应答 type（query-res/set-res）与长度，与旧固件不一致处在报告逐条说明。0x28/0x30/0x34/0x36 仍不回复。
6. 无堆、静态缓冲；NVS 新键沿用 StoreCatalog 去抖。Flash 增量 > 8 KB 说明来源。
7. 测试：services 覆盖每个新 ID × 两种 AppKind 的 type/长度/负载；PPP 不支持分支不发命令；0x06 命令序列与 NACK；延迟应答在断开时释放 credit；main_host_check 覆盖新 NVS 键回读。
8. 门禁 `tools/ci/run_gates.sh --base 56d022c --prefix p1a`（G1 因公共头改动 FAIL 可接受，其它全 PASS）；在 docs/reports/P1.md 追加 “Revision A” 节；同步更新报告 §文档待改（列出 phone_protocol.md 需改的每一行）；打印 `P1 DONE`。

## 5 Revision B（2026-11-19 Architect，HIL 结论 + 用户决定）

HIL：Survey app 首次连接时发 0x04/0x21/0x22/0x23 订阅，NMEA 正常推送、NTRIP RTCM 正常回灌（199 帧 28 KB，drop 0）。设备断电重启后 app 自动重连，只发 0x03（每 16 s），不再发订阅 → 设备不推 NMEA。旧固件 `GNSS.ino:312-336`：蓝牙已连接且 `bt_connect_type != Connect_Nav_APP` 时，无条件把 GGA/GSA/GSV/GST/RMC/ZDA/VTG/GLL 原文推给手机。用户要求：连接即推 NMEA。**关键约束**：NTRIP caster 依赖手机转发的设备 GGA 判断在线与有效性（卫星数等不达标会封号），所以 GGA 必须连续、按模块原样、不得伪造或丢弃。

1. **连接即推**：PHONE 会话建立（SPP 连接）即为该会话开启默认 NMEA 直传，掩码 = 当前接收机支持的 GGA、GST、RMC、GSA、GSV（按 supportedMask 交集，不新增模块输出），周期沿用模块输出（不节流）。无需等待任何帧。
2. **Configure(Nav) app 关闭**：该会话首个 `sender == 0x00` 的帧到达时，若默认直传仍开启且 app 未显式订阅，关闭默认直传（Nav 只用 query）。之后 Nav 显式订阅仍按原逻辑生效。ALARM 会话不推。
3. **显式订阅**：Survey 的 0x04/0x21/0x22/0x23/0x05 带 interval 的请求仍按现有语义修改掩码；`interval=0` 的 query 不得关闭默认直传（只回应答）。断开时 resetSession 清零，下次连接重新按第 1 条开启。
4. **GGA 不丢**：app_ring 满（本次 HIL hwm=4096 B 触顶）时，优先保留 GGA：丢弃顺序 GSV → GSA → GST → RMC，GGA 最后。若环形缓冲结构做不到按类型丢弃，改为：写入 GGA 前若空间不足先丢最旧的非 GGA 记录；或在报告说明替代方案与证据。新增计数 `gga_drop` 进 `status gnss`。
5. **不改** sdkconfig.defaults（Architect 自己改 BT HCI 日志级别）。
6. 测试：会话建立即有 GGA 推送（无任何请求帧）；Nav 首帧关闭默认推送；Survey interval=0 query 不关闭；断开后重连恢复；环满时 GGA 保留且 `gga_drop`=0、其它类型先丢。
7. 门禁 `tools/ci/run_gates.sh --base 56d022c --prefix p1b`（G1 仍只有 4 个授权头 FAIL）；docs/reports/P1.md 追加 “Revision B” 节（含 §文档待改 增补）；打印 `P1 DONE`。
