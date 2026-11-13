# K3 实施下达（2026-11-13）

用户已批准实施 K3（docs/tasks/K3-rtcm-rx-path.md），基线提交 a26ac33（K2）。卡中“本轮仅修订文档 / 实施与冻结契约定稿另行下达”由本文件取代：现在实施。

- 冻结授权：`components/spp_server/include/spp_server.h`（RX level / wake hook / 统计快照，K2 已批接口上追加）、`components/link_core/include/core/trace_codes.hpp`（仅追加）、ADR-0007 / ADR-0010 中 RTCM 批次通路说明段落。只更新这些文件在 `docs/frozen.sha256` 的条目，最终 `sha256sum -c docs/frozen.sha256` 全 OK。其余冻结文件（rtos、demux 公共头、test/host/**）不得改。
- 格式：clang-format 是唯一格式门禁（AGENTS.md:94），不要运行 astyle。
- `tools/hil/spp_stress.py` 已有 `WAIT_SLICE_S` 分片等待修复（Python 3.14 长 Condition.wait 会饿死 RX 线程），新增 upwin 模式必须沿用分片等待。
- 现状基线（K2 HIL）：up256 无窗口洪泛丢 15–27 帧；win16/win32×3000 0 丢；下行 36.7–42.1 KiB/s。K3 不得回退这些。
- 构建：idf 主工程可用 `build/` 或 `builds/k3`；其它构建只放 `builds/<name>/`。
- 不 commit、不连串口/烧录（HIL 由审查方执行），不安装/联网。完成后写 `docs/reports/K3.md` 并停下。
