# 网页配置 WebSocket 协议 v1

依据：ADR-0017 §2.5、§2.6。W3（设备端解析，`components/web_proto`）、W4（网页与 `tools/web_mock`）、W5（集成）共用本文；
改动须由 Architect 更新本文并同步三方。

## 1 通道

- `ws://192.168.4.1/ws`，文本帧，UTF-8，每帧一个 JSON 对象，单帧 ≤ 1024 B（超长：设备关闭连接，码 1009）。
- JSON 子集：对象一层，外加 `status`/`config` 里**一层**嵌套对象；值只有字符串、整数（int32）、`true/false`、
  以及字符串数组（只用于 `bt.whitelist`）。不用浮点、`null`、转义以外的 Unicode（设备端只需支持 `\"` `\\`）。
- 键名固定为小写 ASCII + `.` `_`；未知键：设备忽略（向前兼容），网页忽略。

## 2 设备 → 浏览器

| `t` | 时机 | 字段 |
|---|---|---|
| `hello` | 连接后第一帧 | `proto`(=1)、`board`、`fw`、`bt_name`、`console`（dev/production/none）、`factory`（是否已解锁，bool） |
| `config` | `hello` 之后、任一 `set` 成功之后 | 见 §4 全部可改键的当前值（扁平：`"gnss.rate":10`） |
| `status` | 每 1 s；只带有变化的组 | `dev:{uptime_s,heap_free,heap_min,reset}`、`gnss:{state,model,fw,fix,sats,hacc_mm,hz}`、`bt:{whitelist:[...]}` |
| `ack` | 每条请求一个 | `id`（回显）、`ok`、失败时 `err`（§5） |
| `bye` | 设备即将重启 | `reason`（`exit`/`idle`/`freset`） |

`gnss.state` 取值：`off probing configuring running failed maintenance`；`gnss.fix`：`none single dgps float fixed`。
数值不可得时省略该字段。

## 3 浏览器 → 设备

| `t` | 字段 | 说明 |
|---|---|---|
| `set` | `id`、`key`、`value`（字符串） | 改一个键；一次只处理一条，`ack` 之后再发下一条 |
| `cmd` | `id`、`name` | `save`（GNSS 配置写模块 flash）、`exit`（回蓝牙模式重启）、`freset`（需解锁） |
| `unlock` | `id`、`token` | 工厂解锁，口令与控制台 `factory unlock` 相同；连接断开即失效 |

`id`：1..65535 的整数，浏览器自增。

## 4 可改键（v1）

| 键 | 值 | 对应现有路径 | 备注 |
|---|---|---|---|
| `gnss.rate` | `1` `2` `5` `10` `20` | `GnssVerb::kSetRate` | |
| `gnss.out.gga` `gnss.out.gsa` `gnss.out.gsv` `gnss.out.gst` `gnss.out.rmc` | 周期毫秒：`0`(关) `50` `100` `200` `500` `1000` | `GnssSubVerb::kSetOut` | 接收机不支持的组合由模块拒绝 |
| `gnss.const.gps` `gnss.const.bds` `gnss.const.glo` `gnss.const.gal` | `0` `1` | W0 验证 CM680 `mask` 后新增动词 | W0 不通过则 v1 不提供 |
| `bt.wl.add` `bt.wl.del` | MAC `AA:BB:CC:DD:EE:FF` | `LinkVerb::kWhitelistAdd/Del` | 只写不读，结果见 `status.bt.whitelist` |
| `bt.pair_next` | `0` `1` | 新：下次蓝牙启动时开配对窗口 | W5 定实现 |
| `factory.sn` | 1–16 个 `[0-9A-Za-z-]` | 同控制台 `factory sn` | 需解锁 |

## 5 错误码 `err`

`bad_json` `too_long` `unknown_key` `bad_value` `locked` `busy` `unsupported` `failed` `timeout`
（`busy`/`unsupported`/`failed`/`timeout` 对应模块返回的 `fw::Result`；映射表在 W3 的 `web_proto` 中）。

## 6 示例

```
→ {"t":"set","id":7,"key":"gnss.rate","value":"10"}
← {"t":"ack","id":7,"ok":true}
← {"t":"config","gnss.rate":10,"gnss.out.gga":100,...}
→ {"t":"set","id":8,"key":"factory.sn","value":"P2A0001"}
← {"t":"ack","id":8,"ok":false,"err":"locked"}
```
