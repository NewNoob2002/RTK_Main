# `tools/hil/spp_rtcm_mix.py` 使用说明（ADR-0006）

用于验证 ESP32 经典蓝牙 SPP 单条字节流同时承载 **裸 RTCM3 帧** 与 **产品协议帧（`AA 44 18`）** 时的正确性、抗溢出能力与产品往返时延（RTT）。主机以 BlueZ RFCOMM 扮演 `PHONE` 角色连接设备，并通过 UART0 控制台（115200 bps）在测试前后核对板端 `rtcm:` 与 `stats:` 计数。

## 1. 环境与前置条件

- **Python 环境**：Python 3 标准库 + `pyserial`（使用 ESP-IDF 自带 Python 环境，无需额外安装依赖）：
  ```sh
  ~/.espressif/python_env/idf6.1_py3.14_env/bin/python tools/hil/spp_rtcm_mix.py --help
  ```
- **主机蓝牙**：Linux BlueZ，Python 需支持 `socket.AF_BLUETOOTH` / `socket.BTPROTO_RFCOMM`。测试主机蓝牙 MAC **不得**位于设备报警器白名单内，确保设备将其识别为 `PHONE` 角色。
- **串口独占**：运行前请关闭 `idf.py monitor`、minicom 等占用 `/dev/ttyACM0` 的工具；若串口被占用，脚本会直接报错提示并退出。

## 2. 常用命令

1. **无板离线自检**（CRC-24Q `"123456789" -> 0xCDE703`、RTCM3 帧生成、产品帧重组匹配、控制台正则解析与判定逻辑）：
   ```sh
   python3 tools/hil/spp_rtcm_mix.py --selftest
   ```
2. **RTCM-only 历元模式**（1 Hz 真实基站混合历元：`1005` 19 B + `1074/1084/1094/1124` 各 100–400 B + `1230` 8 B）：
   ```sh
   ~/.espressif/python_env/idf6.1_py3.14_env/bin/python tools/hil/spp_rtcm_mix.py \
       --addr F8:B3:B7:25:D8:B2 --serial /dev/ttyACM0 --product off --epoch-hz 1 --duration 15
   ```
3. **RTCM 突发 + 产品请求混合负载**（`--burst` 每历元一次性写出整组，模拟 NTRIP 突发；`--product cycle` 交错发 `0x07` SET/QUERY 与 `0x01` QUERY）：
   ```sh
   ~/.espressif/python_env/idf6.1_py3.14_env/bin/python tools/hil/spp_rtcm_mix.py \
       --addr F8:B3:B7:25:D8:B2 --serial /dev/ttyACM0 --epoch-hz 1 --burst --product cycle --duration 20
   ```
4. **恒定速率模式**（按 `--rtcm-rate` 均匀发送）：
   ```sh
   ~/.espressif/python_env/idf6.1_py3.14_env/bin/python tools/hil/spp_rtcm_mix.py \
       --addr F8:B3:B7:25:D8:B2 --serial /dev/ttyACM0 --rtcm-rate 5000 --product cycle --duration 15
   ```
5. **吞吐与溢出拐点扫描（`--scan`）**：依次以 `2k / 5k / 10k / 20k / 40k B/s` 各跑 `--duration` 秒，输出吞吐/丢弃汇总表与 JSON 摘要：
   ```sh
   ~/.espressif/python_env/idf6.1_py3.14_env/bin/python tools/hil/spp_rtcm_mix.py \
       --addr F8:B3:B7:25:D8:B2 --serial /dev/ttyACM0 --scan --duration 10 --product cycle --json-out scan.json
   ```

## 3. 主要 CLI 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--addr` | `F8:B3:B7:25:D8:B2` | 目标板经典蓝牙 MAC 地址 |
| `--channel` | `1` | RFCOMM 通道号 |
| `--serial` | `/dev/ttyACM0` | 控制台串口设备；传 `none` 可跳过串口比对 |
| `--baud` | `115200` | 串口波特率 |
| `--duration` | `10.0` | 单轮（或 `--scan` 每档）持续秒数 |
| `--tail-wait` | `1.5` | 发送结束后至少等待排空的秒数（无论是否有串口或产品负载，再读状态与关闭蓝牙） |
| `--epoch-hz` | `1.0` | 历元频率 Hz（未指定 `--rtcm-rate` 时生效） |
| `--rtcm-rate` | `None` | 目标 RTCM 均匀发送速率（B/s）；覆盖历元模式 |
| `--burst` | `False` | 历元模式下每历元一次性 `sendall` 整组 RTCM 帧 |
| `--product` | `cycle` | `off`（纯 RTCM）或 `cycle`（交错 `0x07` SET/QUERY + `0x01` QUERY） |
| `--product-hz` | `20.0` | 产品请求发送频率 Hz |
| `--product-inflight` | `8` | 最大在途未应答产品请求数 |
| `--scan` | `False` | 依次跑 `2k / 5k / 10k / 20k / 40k B/s` 扫描 |
| `--seed` | 当前时间 | 随机载荷种子，便于复现 |
| `--json-out` | `None` | 将完整 JSON 摘要写入指定路径 |
| `--selftest` | `False` | 无板离线自检（不打开蓝牙或串口） |

## 4. 核对与判定规则

1. **测试前**：若启用串口，先发送 `rtcm reset\r`（等待 `OK`）清零板端 RTCM 统计，再发送 `status rtcm\r` 读取基线 `rtcm:` 与 `stats:` 行。若启用 `--product cycle`，在清零前先发送一次 `0x01` 验证 `PHONE` 角色并学习定值应答。
2. **测试后**：无论是否启用串口或产品负载，发送结束后均至少等待 `--tail-wait`（默认 `1.5 s`）让主机蓝牙发送队列与板端接收缓冲区充分排空，再执行串口 `status rtcm\r` 采集或关闭蓝牙会话（关闭时先 `shutdown(SHUT_WR)` 短暂等待再完全关闭）；若启用串口，解析板端：
   - `rtcm: frames=.. bytes=.. crc32=0x........ last_msg=.. rejected_role=.. unknown_bytes=.. product_rejects=.. rtcm_rejects=..`
   - `stats: frames_rx=.. replies_tx=.. write_errors=.. pushes_tx=.. disconnects=.. rx_dropped=..`
3. **判定（`verdict`）**：
   - `PASS`：`rx_dropped` 增量为 `0`，板端与发送端 `rtcm.frames` / `rtcm.bytes` / `rtcm.crc32` 完全一致，产品 `lost == 0`、`content_err == 0`、`host_crc_bad == 0` 且 `rejected_role == 0`。
   - `OVERFLOW_DROPPED`：板端 `rx_dropped` 增量 `> 0`（RX StreamBuffer 溢出丢字节），此时 `rtcm` 计数与 `crc32` 不匹配属于预期现象；用于定位流控/缓冲区溢出拐点（此时产品请求仍应尽量 0 丢失）。
   - `FAIL`：未发生 `rx_dropped` 却出现 RTCM 计数/`crc32` 不一致、产品请求丢失或内容错误、或出现 `rejected_role != 0`。
   - 退出码：无 `FAIL` 返回 `0`，存在 `FAIL` 返回 `1`，连接/串口/参数错误返回 `2`。
