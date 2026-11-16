# P3a：gnss 组件驱动化——Receiver 基类与厂家驱动（ADR-0012 §2.1–§2.3、§4）

状态：ISSUED 2026-11-16，基线 `feat/gnss` 上 ADR-0012 提交。实施：pi + workbuddy/deepseek-v4.1-flash；审查/验收：Claude。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`（尤其规则 1–4、§3.1 PascalCase）、`docs/adr/ADR-0012-gnss-receiver-drivers.md`（本卡依据，冲突时以 ADR 为准）、`docs/adr/ADR-0010-gnss.md` §2.1–§2.4/§2.8/§3，
再读 `components/gnss/**`、`test/gnss/**`、`main/modules/gnss/{GnssModule,GnssCmds}.cpp`、`main/modules/gnss/GnssModule.hpp`、`components/stream_demux/include/demux/Decoder.hpp`。

## 0 基线事实

- 方言 = `gnss::Dialect` 函数指针表（`components/gnss/include/gnss/Dialect.hpp`），三个实例在 `gnss/dialects/{Unicore,Ublox,Comnav}.hpp` / `src/dialects/*.cpp`；`Registry.cpp` 提供 `enabled_dialects()` / `dialect_for()` / `model_name()` / `family_of()`。
- `Controller::init(std::span<const Dialect *const>, bauds, ...)`，内部持有 `const Dialect *`。
- Kconfig / 主机 CMake option 名：`RTK_GNSS_DIALECT_{UNICORE,UBLOX,COMNAV}`（默认 y/y/n）。`components/gnss/CMakeLists.txt` 按此选源文件。
- main 中直接用到方言的只有 `GnssModule.cpp` 与 `GnssCmds.cpp`。
- gnss 组件目前不依赖 app_fw。

## 1 要做

### 1.1 先做黄金向量（**第一步，改任何 gnss 源码之前**）

在 `test/gnss/golden/` 新增一个主机小程序 `GoldenGen.cpp`（加入 test/gnss CMake，作为独立可执行文件，不进 ctest），用**当前未修改的** `Dialect` 接口导出 `test/gnss/golden/Golden<Family>.inc`（三个家族都要，ComNav 用 `-DRTK_GNSS_DIALECT_COMNAV=ON` 构建导出）：

- `build_probe` 的字节；
- `rover_step`：对每个 `Model`（该家族的全部型号）× `rate_hz ∈ {1,2,5,10,20,3}` × `gsv ∈ {false,true}` × `port ∈ {1,2,3}` × `port_baud ∈ {0,115200,460800}`，逐步导出 `cmd` 字节、`expect_reply`、`timeout_ms`、`switch_baud`，直到返回 false；
- `match_probe`、`match_reply`：用现有 test/gnss 里各方言测试的应答样例（含 NACK、不相关帧、截断帧），导出判定结果与 `ProbeResult`；
- `caps(model)`。

格式为 C++ 聚合初始化的数据表（`.inc`），可直接被新测试 `#include`。导出后**先提交给我看（在报告里列文件与条目数）**，不要因为新实现对不上就改黄金文件。

### 1.2 接口（冻结头，按 ADR-0012 §2.2 原样落地）

- 新增 `components/gnss/include/gnss/Receiver.hpp`：`Ack`、`OutputPlan`、`Receiver` 抽象类；`CmdBuf`/`ConfigStep`/`RoverConfig`/`ProbeResult`/`kMaxCommand`/`kStepTimeoutMs`/`kVersionLen` 从 `Dialect.hpp` 原样移入。
  - 本卡实现：身份、探测、`rover_step`、`classify_reply`、`caps`、`frame_tag`、`classify_frame`、`output_mask` 的默认与各驱动实现。
  - `build_output` 只给基类默认（返回 `fw::Status::kNotSupported`），各驱动**本卡不覆盖**（P3b 做）。
  - `build_readback` / `match_readback` 只给基类默认（false / 0），P3c 做。
  - 析构 `protected`、非虚、`= default`；构造 `constexpr`；删除拷贝/移动。所有成员 `const noexcept`。
- `Types.hpp` 追加：`OutputMsg`（值见 ADR §2.2）、`kOutputMsgCount`、`family_name(Family)`、`family_from_u8(uint8_t) -> std::optional<Family>`、`model_from_u8(uint8_t) -> std::optional<Model>`。`model_name()`/`family_of()` 移到 `Types.hpp` 声明（实现可留在 `Registry.cpp`）。
- 新增 `gnss/NullReceiver.hpp`：`class NullReceiver final : public Receiver`，全部返回失败/空；提供 `const Receiver &null_receiver() noexcept`。
- 新增 `gnss/drivers/{UnicoreReceiver,UbloxReceiver,ComnavReceiver}.hpp` + `src/drivers/*.cpp`：逻辑从对应 `src/dialects/*.cpp` 搬入，**行为逐字节不变**（`rover_step` 仍含 GST/SATSINFOA/HEADINGA，P3b 才移出）。每个驱动文件导出 `const Receiver &unicore_receiver() noexcept` 之类的访问函数，实例为文件内 `constinit` 静态对象。
- 新增 `gnss/DriverList.inc`（X-macro，格式见 ADR §2.3），`Registry.cpp` 用它生成 `enabled_receivers() -> std::span<const Receiver *const>`、`receiver_for(Family) -> const Receiver *`（未启用返回 nullptr）。
- `Controller.hpp`：`Dialect` → `Receiver`（`init(std::span<const Receiver *const>, ...)`、`receiver()` 访问器返回 `const Receiver &`，未探测到时为 `null_receiver()`）。状态机逻辑不改，只换类型与调用形式。
- 删除 `gnss/Dialect.hpp`、`gnss/dialects/*.hpp`、`src/dialects/*.cpp`。
- 名称：`gnss::Reply` → `gnss::Ack`（含所有使用处）。
- `classify_frame` 默认实现：NMEA 帧忽略 talker 按语句类型识别 GGA/RMC/GSA/GSV/GST；Unicore 覆盖并追加 `#SATSINFOA,` → `kSatsInfo`、`#HEADINGA,` → `kHeading`（与 `main/data/GnssOutput.cpp:97` `classify_gnss_sentence()` 现有判定**等价**，主机测试对比两者结果；main 侧替换留给 P3b）。
- `frame_tag` 默认实现与 `GnssModule.cpp` `describe_frame()` 现有标签逻辑等价（NMEA 句名、RTCM 类型号），Unicore 覆盖 `#XXXX`、`$command`，u-blox 覆盖 `UBX cc-ii`。

### 1.3 构建配置

- Kconfig `RTK_GNSS_DIALECT_*` → `RTK_GNSS_DRV_*`（默认不变）；`components/gnss/CMakeLists.txt`（IDF 与主机分支）、`test/gnss/CMakeLists.txt` option、仓库内 `sdkconfig.defaults*`、`test_apps/*/sdkconfig.defaults*` 中凡出现的同步改名（`rg -n 'RTK_GNSS_DIALECT' .` 结果须为空，`docs/` 历史文档与 `reference/` 除外）。
- gnss 组件 `REQUIRES stream_demux app_fw`（主机分支 `target_link_libraries(gnss PUBLIC stream_demux app_fw)`，按 test/gnss 现有方式找到 app_fw 源）。

### 1.4 main 的最小适配（只做机械替换）

- `GnssModule.cpp` / `GnssCmds.cpp` / `GnssModule.hpp`：`Dialect` → `Receiver`、`enabled_dialects` → `enabled_receivers`、`dialect->xxx(...)` → `receiver.xxx(...)`、`Reply` → `Ack`。
- `GnssCmds.cpp:84-91` 与 `main/modules/debug/TraceText.cpp:389-396` 的家族名 switch 改调 `gnss::family_name()`；`GnssModule.cpp:997,1019` 的 NVS 读回上界改用 `family_from_u8`/`model_from_u8`。
- **不做**：`apply_out_to_receiver`、`infer_probe_family`、帧标签、`GnssOutput` 分类的替换，这些是 P3b。

### 1.5 测试

- 把 `TestGnssDialect{Unicore,Ublox,Comnav}.cpp` 改为 `TestGnss{Unicore,Ublox,Comnav}Receiver.cpp`：原有用例全部保留（用例数只增不减），另加“对黄金向量逐条比较”的用例（所有组合）。
- 新增：`NullReceiver` 全接口、`DriverList` 过滤（关掉某驱动后 `enabled_receivers` 与 `receiver_for` 正确）、`family_from_u8/model_from_u8` 边界（最大合法值、+1、0xFF）、`family_name` 全枚举、`classify_frame` 与 `frame_tag` 等价性（多 talker、畸形帧）、`build_output` 默认返回 `kNotSupported`。
- `TestGnssController.cpp`：类型替换后原用例全部通过。
- 主机套件以 ComNav ON 与 OFF 各跑一次。

## 2 门禁（写入 `docs/reports/P3a.md`，逐项 PASS/FAIL/NOT_RUN 并附命令与关键输出）

1. IDF：`idf.py -B builds/p3a build` 与 `builds/p3a-off`（`CONFIG_RTK_GNSS_ENABLE=n`）、`builds/p3a-comnav`（`CONFIG_RTK_GNSS_DRV_COMNAV=y`）均 0 warning；报告给出与基线（`builds/v4` 或在 1548de8 上重建的基线）的 DRAM/IRAM/flash 差值并解释。
2. 主机：test/gnss（普通 + `-DSANITIZE=ON`，ComNav ON/OFF）、test/host、test/app_fw、test/stream_demux、test/bt_reconnect、test/services、test/cli、tools/main_host_check（含 ASan）、tools/t13_host_check 全部通过，用例数不减。
3. `python3 -I tools/g7b_check/g7b_check.py --build builds/p3a` PASS；G5b `tools/tidy_check/tidy_check.py` 0 新增；clang-format（唯一格式门禁，禁止 astyle）。
4. `docs/frozen.sha256`：删除 Dialect/dialects 条目，加入 ADR-0012 §3 新冻结文件，修订文件重算；`sha256sum -c docs/frozen.sha256` 全 OK。
5. 报告列出：黄金向量文件与条目数；新旧文件对照与行数；所有 vtable 带来的 flash 增量；任何与 ADR-0012 不一致处（不自行改 ADR）。

报告末行 `P3a DONE` 或 `P3a BLOCKED: <原因>`。

## 3 禁止

- 改协议帧、NVS 命名空间与键、蓝牙名、执行器参数；改变任何发给模块的字节或应答判定（黄金向量为准）。
- 修改黄金向量去迁就新代码；改动本卡未列出的冻结文件；改 `test/host`、`reference/`、`AGENTS.md`、ADR 文档。
- 使用 `new`/堆、RTTI、异常；驱动内保存可变状态。
- 烧录、打开串口、联网、安装软件、git commit/切分支。构建目录只用 `builds/p3a*`。
- 提交 trailer 由我补：`Generated-by: pi + workbuddy/deepseek-v4.1-flash`。
