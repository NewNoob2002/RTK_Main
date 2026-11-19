# Q1 GGA 质量门控（2026-11-19 Architect）

依据：用户 2026-11-19 要求。NTRIP caster 依赖客户端上报的 GGA 判断在线与位置（VRS/就近基站选择），部分 caster 对质量不达标（无解、卫星数过少）的 GGA 会封禁账号。增加可配置、可保存的门控开关：质量不达标时暂停向蓝牙手机（以及将来的 4G NTRIP 客户端）发送 GGA，质量恢复后自动放行。

前置：P1（连接即推 NMEA、GGA 优先保留）已合入。分支 `feat/q1-gga-gate`，基线为 P1 合入后的 main。

## 1 行为

1. 门控对象只是 **GGA**（caster 只需要 GGA）；GST/RMC/GSA/GSV 照常推送，app 仍能显示卫星与位置。
2. 判定：解析每条 GGA 的定位质量字段与使用卫星数。**不达标** = 质量 0（无解）或 卫星数 < `min_sats`。字段为空或解析失败按不达标处理。
3. 迟滞：处于阻断时，连续 3 条达标 GGA 后才放行；处于放行时，1 条不达标即阻断（宁可少发，避免把坏 GGA 交给 caster）。
4. 阻断期间丢弃 GGA，不伪造、不缓存、不补发；放行后从下一条真实 GGA 开始。
5. 开关默认 **关闭**；`min_sats` 默认 8，范围 0–40。两项保存在 NVS（StoreCatalog 去抖），重启后生效保持；开关改动立即生效。
6. 门控作用于所有 GGA 出口：当前为蓝牙 PHONE 会话的直传与 0x04 query 应答（阻断时 query 回 `02 02`）；为将来 4G NTRIP 客户端留同一判定接口（例如 `bool ggaAllowed()`），本卡不实现 4G。
7. 计数进 `status gnss`：`gga_gate=on/off`、`blocked=yes/no`、`gga_blocked`（累计被拦条数）、`gate_trans`（阻断/放行切换次数）。

## 2 管理入口

1. **控制台**：`gnss gate [on|off] [min_sats <n>]`，无参数打印当前值；DEV/PRODUCTION 白名单按现有规则（PRODUCTION 只读查询）。
2. **Wi-Fi 网页**：`docs/web/ws-protocol.md` 的配置对象增加 `gga_gate: {enabled: bool, min_sats: int}`，网页加开关与数字框（英文 UI，与现有控件同风格）；校验失败按现有错误码返回。
3. **Configure app**：消息 ID 与格式由用户的 app 团队定义，本卡**不实现**手机协议消息；在 `PhoneCompatService` 处留 TODO 注释（写明等待 app 团队 ID）。

## 3 约束与测试

- 无堆、静态缓冲；纯判定逻辑放在 gnss 或 link_core 纯组件内（不含 IDF 头），可主机测试。
- 测试：质量 0 / 卫星不足 / 空字段 → 阻断；连续 3 条达标才放行；开关关闭时全部放行；0x04 query 阻断时回 `02 02`；NVS 回读；ws 配置校验（越界 min_sats 拒绝）。
- 门禁 `tools/ci/run_gates.sh --base <P1 合入后的 main> --prefix q1`；报告 `docs/reports/Q1.md`，列 §文档待改（phone_protocol.md、ws-protocol.md、acceptance.md）；打印 `Q1 DONE`。不烧录、不提交。
