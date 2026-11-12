# I2 上板测试工具：蓝牙 RTCM + 产品请求混合负载（BATCH-I，ADR-0006）

执行方：pi + antigravity/gemini-3.8-flash。先阅读 `AGENTS.md`、`docs/adr/ADR-0006-bt-rtcm-integration.md`（重点 §2 控制台格式、§4 门禁）、
`docs/protocol/phone_protocol.md` §1–§3（产品帧格式）、ADR-0004 §4（RTCM3 帧格式、CRC-24Q）。
参考现有压测脚本（只读，可复制其中逻辑）：`/tmp/claude-1000/-home-gtc-Desktop-workspace-ESP32-PROJ-P2-RTK/84006cc8-9758-45f1-a393-77c17b49f97b/scratchpad/spp_stress.py`
（RFCOMM 连接、产品帧组帧、0x07 设置/查询回显、0x01 状态查询；msg_id = 命令号，无序号，按顺序匹配应答）。

## 交付
`tools/hil/spp_rtcm_mix.py`（Python 3 标准库 + pyserial；不得 pip install，pyserial 用 ESP-IDF 的 python 环境即可）与 `tools/hil/README.md`，报告 `docs/reports/I2.md`。

## 功能
- 连接：`--addr F8:B3:B7:25:D8:B2 --channel 1`，`socket.AF_BLUETOOTH/BTPROTO_RFCOMM`。主机作为 PHONE 角色（非白名单地址）。
- RTCM 生成：合法 RTCM3 帧（D3、保留位 0、10 bit 长度、载荷首 12 bit 为消息号、CRC-24Q 自己实现并用 "123456789"→0xCDE703 自检）；
  消息号/载荷长度按真实基站混合：1005(19 B)、1074/1084/1094/1124（各 100–400 B 随机）、1230(8 B)，每“历元”一组，`--epoch-hz`；
  可选 `--rtcm-rate BYTES_PER_S` 均匀发送模式；`--burst` 每历元一次性写出整组（模拟 NTRIP 突发）。
- 产品负载（`--product off|cycle`）：与 RTCM 交错发送 0x07 set/query 回显与 0x01 查询，单独接收线程按顺序匹配应答，统计 RTT p50/p99、丢失、内容错。
- 发送端累计：RTCM 帧数、字节数、对全部已发 RTCM 线路帧按发送顺序的 zlib crc32。
- 板端计数：`--serial /dev/ttyACM0`（115200）。测试前发 `rtcm reset\r` 并读 `status`，测试后（等待 1 s 排空）再读 `status`，用正则解析
  ADR-0006 §2 的 `rtcm:` 行与已有 `stats:` 行（rx_dropped、frames_rx、replies_tx）。串口被占用时给出清晰错误。
- 判定输出一个 JSON 摘要 + 人类可读表：`rtcm_frames/bytes/crc32 match`、`rx_dropped`、产品 RTT/丢失、实际吞吐（B/s）。
  有 rx_dropped 时说明 crc32 不匹配是预期的（溢出丢字节），否则不匹配即 FAIL。
- `--scan`：依次以 `--rtcm-rate` 2k/5k/10k/20k/40k B/s 各跑 `--duration`，输出吞吐/丢弃表。
- 无板时可用 `--selftest`：只运行 CRC-24Q/帧生成/解析正则的离线自检。

## 要求与禁止
- 代码风格清晰、有中文注释说明原因；文件头 `Generated-by: pi + antigravity/gemini-3.8-flash`，人工负责人 `UNKNOWN（待人工审查签字）`。
- 你没有硬件权限：只做 `--selftest` 与 `python -m py_compile`，不得连接蓝牙或打开串口、不得烧录、联网安装或 git commit。
- 不得修改 `tools/hil/` 以外的文件（报告除外）。完成后最后一行输出 `I2 DONE` 或 `I2 BLOCKED: <原因>`。
