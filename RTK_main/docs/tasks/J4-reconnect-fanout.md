# J4 bt_reconnect + FanoutRing + app_fw 测试 + 报警器测试固件自动重连（BATCH-J，ADR-0007）

执行方：pi + antigravity/gemini-3.8-flash。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0007-app-services-split.md`（重点 §3.5、§3.6、§5、§9），
`components/bt_reconnect/include/bt_reconnect/reconnect.hpp`、`components/app_fw/include/app/*.hpp`、`components/app_fw/CMakeLists.txt`，
以及 `test_apps/rtk_spp_peer/main/peer.c`、`test_apps/rtk_spp_peer/CMakeLists.txt`。

## 交付
1. `components/bt_reconnect/`：`CMakeLists.txt`（同 `components/app_fw/CMakeLists.txt` 的 IDF/host 双模式与纯组件告警集）、`src/reconnect.cpp`，实现头文件注释中的全部语义。
2. `test/bt_reconnect/`（新建 host CMake 工程，普通与 ASan/UBSan）：逐条覆盖头文件“语义”列表：快速窗口 1 s 固定重试；窗口外 1/2/4/8/16/30 s 封顶；抖动范围（固定种子 1000 次采样全部落在 ±jitter% 内且不全相同；jitter=0 时精确）；在线 ≥10 s 掉线复位、<10 s 掉线不复位；SCN 缓存与连续 2 次失败后重新 SDP；attempt 超时；stop/start；改目标清缓存；非预期事件忽略；u32 时间回绕（now 从 0xFFFF_F000 起跑）；configure 非法参数。
3. `components/app_fw/src/fanout_ring.cpp`（并加入 `components/app_fw/CMakeLists.txt` 的 `_app_fw_srcs`）：实现 `fanout_ring.hpp` 全部语义。
4. `test/app_fw/`（新建 host CMake 工程，普通与 ASan/UBSan）：`Slot`（写读、版本号、LockOps 调用次数与配对）、`LockGuard`、`StoreKey::valid`/`unique_keys`（含 constexpr `static_assert` 用例）、`FanoutRing`（单/多读者、最慢读者被覆盖时整条丢弃并计数、attach 晚于写入只读新数据、detach、记录长度边界、`kNoSpace` 输出缓冲不足不消费、u32 位置回绕）。
5. `test_apps/rtk_spp_peer`：接入 `bt_reconnect`（CMake `EXTRA_COMPONENT_DIRS` 指向 `../../components/bt_reconnect`）；`connect MAC` 设置目标并写入 NVS（namespace `peer`，key `target`），上电读到目标即自动开始重连；新增命令 `reconnect stop|start|status`（status 打印状态、`next_attempt_in`、`Stats`）；去掉固定 2 s 重试。现有命令语义不变。时间与随机种子由调用方注入（`esp_timer_get_time`、`esp_random`）。
6. 报告 `docs/reports/J4.md`。

## 要求
- 纯组件（bt_reconnect、app_fw）不得包含任何 IDF/FreeRTOS 头；无堆、无异常、无虚函数。
- peer 固件是 C（`peer.c`）：通过一个小的 C++ 桥接文件 `test_apps/rtk_spp_peer/main/reconnect_bridge.{h,cpp}`（C 接口）使用 `btrc::Reconnector`，不得把 peer.c 整体改成 C++。
- 源文件头 `Generated-by: pi + antigravity/gemini-3.8-flash`，人工负责人 `UNKNOWN（待人工审查签字）`。

## 自检（报告逐项 PASS/FAIL 并给出命令）
1. `test/bt_reconnect`、`test/app_fw` 普通与 ASan/UBSan 构建，ctest 全部通过。
2. `idf.py -C test_apps/rtk_spp_peer -B build-hil-peer build`：0 warning。
3. `. ~/esp/esp-idf/export.sh >/dev/null && idf.py -B build-j4 -D SDKCONFIG=build-j4/sdkconfig build`（主工程）：0 warning。
4. clang-format dry-run 无违规；`sha256sum -c docs/frozen.sha256` 全部 OK。

## 并行约束
J1、J2a、J4 同时运行：主工程只能用 `-B build-j4 -D SDKCONFIG=build-j4/sdkconfig` 构建（不得用默认 `build/` 与根目录 `sdkconfig`，它们属于 J1）；host 构建目录也必须带 `build-j4` 前缀或放在自己的测试目录下。

## 禁止
修改冻结文件（AGENTS.md §4，含所有 include 头）；修改 `main/**`、`tools/**`、`components/rtk_os/**`（J1）、`components/rtk_core/**`、`test/services/**`（J2a）、`components/app_fw/src/result.cpp`；
烧录、monitor、连接串口、联网安装、git commit。完成后最后一行输出 `J4 DONE` 或 `J4 BLOCKED: <原因>`。
