# I1 蓝牙单流分流集成（BATCH-I，ADR-0006）

执行方：pi + codex/gpt-6.1-sol。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0006-bt-rtcm-integration.md`、ADR-0004 §7、ADR-0005，
以及 `components/stream_demux/include/demux/*.hpp`、`components/rtk_proto/include/rtk/proto/demux_frame.hpp`、
`main/app_link.{hpp,cpp}`、`main/app_console.cpp`、`components/rtk_core/include/rtk/core/link_service.hpp`。

## 交付
- `main/app_link.cpp`：按 ADR-0006 §1 改 `SessionEntry`/`session_record`/`session_forget`/`drain_rx`；会话 Sink 为 `final : demux::Sink`。
  保留现有注释里的调度契约（每轮至多一个产品帧、256 B/20 ms 预算、64 B 预读跨轮保留）。
- 新 `main/app_rtcm.hpp` / `app_rtcm.cpp`：`RtcmForwarder`（无堆、无锁，只由链路任务调用），ADR-0006 §1.4 的统计。
- `main/app_link.hpp` `StatusSnapshot` 增加 ADR-0006 §2 字段；`Command` 增加 `rtcm reset`；`app_console.cpp` 增加输出行与命令（格式严格按 §2）。
- `main/CMakeLists.txt`：`REQUIRES`/`PRIV_REQUIRES` 增加 `stream_demux`，SRCS 增加 `app_rtcm.cpp`。
- `tools/main_host_check`：CMake 增加 stream_demux 与 `demux_frame.cpp` 源；新增用例：同一 DATA 中“产品请求帧 + 2 条 RTCM + 垃圾 + 产品请求帧”，
  断言两次应答、RTCM 计数/字节/crc32 与期望一致、unknown_bytes 正确、产品帧仍每轮一个；非 PHONE 角色会话的 RTCM 计入 rejected_role。
- 报告 `docs/reports/I1.md`。

## 要求
- AGENTS.md 全部 C++ 规则（无异常/RTTI/堆/std::function，constinit 全局，[[nodiscard]] 结果必须处理，Status/Error 不得静默丢弃——确需忽略写原因）。
- `Demux::init` 返回值必须检查；失败时该会话不喂分流器并 ESP_LOGE。
- 源文件头 `Generated-by: pi + codex/gpt-6.1-sol`，人工负责人 `UNKNOWN（待人工审查签字）`；修改已有文件时在其 Generated-by 行追加本工具。

## 自检（报告逐项 PASS/FAIL 并给出命令）
1. `. ~/esp/esp-idf/export.sh && idf.py build`：0 warning。
2. `tools/main_host_check` 构建并 ctest（两种策略）。
3. `test/host`、`test/stream_demux` 回归；`sha256sum -c docs/frozen.sha256`。
4. clang-format（`.clang-format`）对改动文件 dry-run 无违规。

## 禁止
修改冻结文件（AGENTS.md §4；本批 `rtk_bt_spp.h`、`components/rtk_bt/Kconfig` 已由架构方改好）、`components/rtk_core/**`、`components/stream_demux/**`、
`tools/hil/**`（I2）；烧录、联网安装、git commit。完成后最后一行输出 `I1 DONE` 或 `I1 BLOCKED: <原因>`。
