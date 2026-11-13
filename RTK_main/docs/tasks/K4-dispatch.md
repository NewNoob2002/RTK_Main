# K4 实施下达（2026-11-13）

用户已批准实施 K4（docs/tasks/K4-gnss-app-ring.md），基线提交 0127fd1（K3）。卡中“本轮仅修订文档 / 实施与冻结契约定稿另行下达”由本文件取代：现在实施。

- 冻结授权（仅卡 §3 所列）：
  - `components/stream_demux/include/demux/decoders/unicore_hash.hpp`：最大帧 1024→4096（含校验/行尾），须有测试证明缓冲约束；
  - 新增 `components/link_core/include/core/services/gnss_service.hpp`，完成后加入 `docs/frozen.sha256`；
  - `docs/adr/ADR-0010-gnss.md` §2.9、`docs/tasks/G-C-phone-subscription.md`、`docs/protocol/phone_protocol.md`：按卡 §3 修订。
  - `command_router.hpp`、`device_service.hpp` 等其它冻结头不预授权：确需修改时停下，在报告中列出精确字段与理由，不要自行改。rtos、device_proto、gnss 公共头、test/host 不得改。
  - 只更新上述文件在 `docs/frozen.sha256` 的条目，最终 `sha256sum -c docs/frozen.sha256` 全 OK。
- 格式：clang-format 是唯一格式门禁（AGENTS.md:94），不要运行 astyle。
- 基线（K3 HIL）：up256 洪泛 0 丢；upwin 3072B 10 min 230k 帧 0 丢（94–99 KiB/s）；win16/win32×3000 0 丢，下行 37.8–43.5 KiB/s；DRAM 108,359 / 124,580 B；heap min 98.2 KB。K4 不得回退这些；负载下内部 heap min ≥ 81920、最大块 ≥ 32768。
- 新增/修改 HIL 脚本须沿用 `tools/hil/spp_stress.py` 的 `WAIT_SLICE_S` 分片等待（Python 3.14 长 Condition.wait 会饿死 RX 线程），输出 `flush=True`。
- 构建：idf 主工程可用 `build/` 或 `builds/k4`；其它构建只放 `builds/<name>/`。
- 不 commit、不连串口/烧录（HIL 由审查方执行），不安装/联网。完成后写 `docs/reports/K4.md`（含 HIL 计划的可执行命令）并停下。
