# P3a 记录：GNSS 驱动基类与厂家驱动拆分（ADR-0012）

日期：2026-11-16。分支 `feat/gnss`，基线 `4cf1320`（本任务未 commit、未切分支）。执行者：pi + workbuddy/deepseek-v4.1-flash。

## 结论

**P3a PASS（软件门禁）**。黄金向量在未修改的基线代码上生成并经新实现逐字节比对通过；`gnss` 组件由「Dialect + dialects/」重构为「Receiver 基类 + drivers/」；IDF 三种配置、11 个 host 测试工程、堆门禁、clang-tidy、clang-format、冻结清单全部通过。上板与串口验证 NOT_RUN（AGENTS.md §6 未授权）。

任务分解与 ADR-0012 的阶段划分不同之处：本卡只做驱动拆分与黄金向量，`build_output`、配置读回、main 的帧标签与 `GnssOutput` 归类仍留给 P3b/P3c（见 §5）。

## 1. 黄金向量（第一步：改代码之前）

目的：重构前先把基线行为固化成数据，重构后逐字节比对，保证「行为不变」可被机器验证，而不是靠人工审阅方言与驱动的对应关系。

| 项 | 内容 |
|---|---|
| 生成来源 | `git archive 4cf1320 \| tar -x -C builds/p3a-golden/base`（未修改的基线树） |
| 生成器 | `test/gnss/golden/GoldenGen.cpp` + `GoldenData.hpp`（拷入基线树，仅借助基线 `gnss::Dialect` 导出数据） |
| 命令 | `cmake -S builds/p3a-golden/base/test/gnss -B builds/p3a-golden/gen -DRTK_GNSS_DIALECT_COMNAV=ON && cmake --build builds/p3a-golden/gen --target golden_gen && ./builds/p3a-golden/gen/golden_gen test/gnss/golden` |
| 覆盖 | 型号 × rate_hz{1,2,5,10,20,3} × gsv{off,on} × port{1,2,3} × port_baud{0,115200,460800}；外加 `build_probe`、`match_probe`、`classify_reply`、`caps` 的定点样本 |

| 文件 | rover 组合 | 步骤 | 探测样本 | 应答样本 | 能力位 | 行数 |
|---|---|---|---|---|---|---|
| `test/gnss/golden/GoldenUnicore.inc` | 324 | 2754 | 5 | 7 | 4 | 3131 |
| `test/gnss/golden/GoldenUblox.inc` | 108 | 288 | 4 | 5 | 2 | 444 |
| `test/gnss/golden/GoldenComnav.inc` | 108 | 666 | 4 | 5 | 2 | 822 |

比对器 `test/gnss/golden/GoldenCheck.hpp` 返回「第一个不一致项的下标」（`kAllMatch = SIZE_MAX`），调用方再用 Unity 断言，因此三个驱动的测试文件共用同一份比较逻辑。

生成器本身踩到并修正的两个坑（与产品代码无关，记录以便复现）：

1. 二进制帧必须按 `char[N]` 保留长度（`golden::Bytes`），用 `std::string_view` 存字面量会在第一个 `0x00` 处被 `strlen` 截断（UBX 帧只剩 4 字节）。
2. `std::array<T,N> x{ {e}, ... }` 需要内层再包一层花括号，否则 GCC 报 `too many initializers`。

## 2. 文件映射

| 基线 | 现状 | 行数（旧 → 新） | 说明 |
|---|---|---|---|
| `include/gnss/Dialect.hpp` | 删除 | 129 → — | 抽象基类改名并重写契约 |
| — | `include/gnss/Receiver.hpp` | — → 179 | `Ack`/`OutputPlan`/`Receiver`；`CmdBuf`/`ConfigStep`/`RoverConfig`/`ProbeResult`/`kMaxCommand`/`kStepTimeoutMs`/`kVersionLen` 原样迁移 |
| — | `include/gnss/NullReceiver.hpp` | — → 91 | 未探测到硬件时的替身，全部操作「无动作」 |
| — | `include/gnss/DriverList.inc` | — → 14 | X-macro 行表，Registry 与测试共用 |
| `include/gnss/dialects/{Unicore,Ublox,Comnav}.hpp` | `include/gnss/drivers/{Unicore,Ublox,Comnav}Receiver.hpp` | 36/29/32 → 64/45/52 | 驱动头只导出 `const Receiver &x_receiver()` 与私有解码器 |
| `src/dialects/Unicore.cpp` | `src/drivers/UnicoreReceiver.cpp` | 547 → 640 | 纯机械搬迁：自由函数（`probe_cmd`/`probe_match`/`script_step`/`reply_ack`/`model_caps`）进匿名命名空间，新增 4 个 override |
| `src/dialects/Ublox.cpp` | `src/drivers/UbloxReceiver.cpp` | 269 → 331 | 同上，额外 override `frame_tag`（UBX 标签） |
| `src/dialects/Comnav.cpp` | `src/drivers/ComnavReceiver.cpp` | 331 → 383 | 同上，无额外 override |
| — | `src/Receiver.cpp` | — → 102 | 基类默认实现：输出掩码、`build_output = kNotSupported`、通用 NMEA 归类、帧标签、读回桩 |
| — | `src/FrameTag.hpp` | — → 80 | 帧标签/命令名截取的私有工具（`copy_word`/`format_tag`/`starts_with`） |
| `src/Registry.cpp` | 改 | 102 → 156 | `enabled_receivers()` 按 `DriverList.inc` 展开；`receiver_for()`、`family_name`/`family_from_u8`/`model_from_u8` |
| `src/Controller.cpp` | 改 | 442 → 449 | 类型换成 `Receiver`，状态机逻辑未动；新增 `receiver()` 访问器 |
| `include/gnss/Controller.hpp` | 改 | 169 → 175 | `Dialect* → const Receiver*`，`init()` 接 `std::span<const Receiver *const>` |
| `include/gnss/Types.hpp` | 改 | 109 → 143 | 新增 `OutputMsg`/`kOutputMsgCount` 与 `family_name`/`family_from_u8`/`model_from_u8` 声明 |
| `Kconfig` | 改 | 17 → 19 | `RTK_GNSS_DIALECT_*` → `RTK_GNSS_DRV_*`（默认 y/y/n 不变） |
| `CMakeLists.txt`（gnss） | 改 | 53 → 55 | 按 `CONFIG_RTK_GNSS_DRV_<D>` 选择驱动源文件；`REQUIRES stream_demux app_fw` |
| `test/gnss/TestGnssDialect{Unicore,Ublox,Comnav}.cpp` | `test/gnss/TestGnss{Unicore,Ublox,Comnav}Receiver.cpp` | 311/239/… → 352/260/193 | 原用例全部保留并追加黄金向量比对 |
| — | `test/gnss/TestGnssNullReceiver.cpp` | — → 70 | 空实现逐个接口 |
| — | `test/gnss/TestGnssClassify.cpp` | — → 183 | 归类、帧标签、输出掩码 |
| `main/modules/gnss/GnssModule.cpp` | 改 | — | 解码器表改由 `enabled_receivers()` 汇总；`det.dialect->family` → `det.receiver->family()`；提示值校验改用 `family_from_u8`/`model_from_u8`（`nullopt → kInvalidArg`） |
| `main/modules/gnss/GnssCmds.cpp` | 改 | — | 删掉本地 `family_name`，改调 `gnss::family_name` |
| `main/modules/debug/TraceText.cpp` | 改 | — | `gnss_family_text` 只保留越界保护，名称取自组件 |
| `docs/frozen.sha256` | 改 | — | 删 4 个 Dialect 条目、加 6 个新冻结头、重算 `Types.hpp`/`Controller.hpp` |

`rg -n 'RTK_GNSS_DIALECT'`（排除 `docs/`、`reference/`、`builds/`）无匹配；`sdkconfig.defaults` 与 `test_apps/*/sdkconfig.defaults` 基线中从未列过这些符号，因此无需改动（Kconfig 默认值已覆盖）。

## 3. 命令与结果

每个 shell 先 `. ~/esp/esp-idf/export.sh`。所有构建目录在 `builds/p3a*`。

| # | 命令 | 结果 | 证据 |
|---|---|---|---|
| 1 | `idf.py -B builds/p3a-base build`（基线树） | PASS | Flash Code 540028 / Flash Data 144232 / IRAM 86187 / DRAM 63807 / image 791110 |
| 2 | `ctest --test-dir builds/p3a-gnss-base` | PASS | 5 个目标 36 例（controller 9、nmea 9、registry 4、unicore 9、ublox 5） |
| 3 | 黄金向量生成（§1 命令） | PASS | 324/108/108 组，2754/288/666 步 |
| 4 | `idf.py -B builds/p3a build` | PASS | 0 warning；Flash Code 542460 / Flash Data 144504 / IRAM 86187 / DRAM 63855 / image 793830 |
| 5 | `idf.py -B builds/p3a-off -D SDKCONFIG=…/p3a-off/sdkconfig build`（`CONFIG_RTK_GNSS_ENABLE` 关） | PASS | `Project build complete`，0 warning |
| 6 | `idf.py -B builds/p3a-comnav -D SDKCONFIG=…/p3a-comnav/sdkconfig build`（`CONFIG_RTK_GNSS_DRV_COMNAV=y`） | PASS | Flash Code 544756 / Flash Data 144712 / DRAM 63863 / IRAM 86187 / image 796334 |
| 7 | `ctest --test-dir builds/p3a-gnss`（ComNav 关） | PASS | 7/7 目标，54 例（基线 36） |
| 8 | `ctest --test-dir builds/p3a-gnss-comnav`（ComNav 开） | PASS | 8/8 目标，65 例（基线 42：36 + comnav 6） |
| 9 | `ctest --test-dir builds/p3a-gnss-asan`（SANITIZE + ComNav 开） | PASS | 8/8 |
| 10 | `ctest --test-dir builds/p3a-gnss-asan-off`（SANITIZE，ComNav 关） | PASS | 7/7 |
| 11 | `ctest --test-dir builds/p3a-host` / `builds/p3a-host-plain` | PASS | 各 7/7（含 ASan/UBSan） |
| 12 | `ctest --test-dir builds/p3a-{app_fw,stream_demux,bt_reconnect,services,cli}` | PASS | 6/4/1/6/3 |
| 13 | `ctest --test-dir builds/p3a-mhc` / `p3a-mhc-asan` / `p3a-t13` | PASS | 44 / 44 / 43 |
| 14 | `python3 -I tools/g7b_check/g7b_check.py --build builds/p3a` | PASS | 自有目标 70 个；堆白名单 1 个源文件放行 0 个堆符号；ELF 异常/RTTI 仅 6 个白名单符号 |
| 15 | `python3 -I tools/tidy_check/tidy_check.py --host-build builds/p3a-host-plain --idf-build builds/p3a --test-app-build builds/p3a-tapp` | PASS | 文件 30 个，发现 0 条（G5b PASS） |
| 16 | `clang-format --style=file --dry-run --Werror`（全部改动的 .cpp/.hpp） | PASS | rc 0 |
| 17 | `sha256sum -c docs/frozen.sha256` | PASS | 180 条全部 OK |
| 18 | `rg -n 'RTK_GNSS_DIALECT'`（排除 docs/reference/builds） | PASS | 无输出 |
| 19 | 上板、串口、`idf.py flash/monitor` | NOT_RUN | AGENTS.md §6 禁止 |

新增/增长的用例（Unity 计数，ComNav 开）：unicore 9 → 14、ublox 5 → 10、comnav 6 → 11、registry 4 → 6、null_receiver 2、classify 4；`controller` 9、`nmea` 9 未变。

## 4. 体积

| 项 | 基线 | P3a（ComNav 关） | 差 | P3a（ComNav 开） |
|---|---|---|---|---|
| Flash Code | 540028 | 542460 | +2432 | 544756 |
| Flash Data | 144232 | 144504 | +272 | 144712 |
| IRAM | 86187 | 86187 | 0 | 86187 |
| DRAM | 63807 | 63855 | +48 | 63863 |
| image | 791110 | 793830 | +2720 | 796334 |

ADR-0012 §5 预估「虚表 +1–2 KB flash、+12 B DRAM」。实测 flash 增量略高：除 3 份虚表与 RTTI-free 的 `final` 驱动类外，还包含 `Receiver.cpp` 的默认实现（`classify_frame`/`frame_tag`/`output_mask`）、`FrameTag.hpp` 的文件作用域工具与 Registry 的常量表；DRAM +48 B 是 3 个 `constinit` 静态驱动对象（每个一个 vptr 加对齐填充，非 §5 假设的 12 B，因为 COMNAV 类在关掉时也留有对齐占位——见 §6 第 2 条）。IRAM 无变化说明没有新增 IRAM 驻留代码（驱动仍在 flash 执行）。

## 5. 与契约的偏离

1. **`test/gnss/CMakeLists.txt` 保留 `golden_gen` 目标但加了 `if(EXISTS …/Dialect.hpp)` 守卫**。基线方言删除后该目标自然不再构建，生成器源码留在仓库里作为数据来源的凭证。卡未规定生成器去留，此处选择「保留源码、不构建」。
2. **`GoldenData.hpp` 的 `Bytes` 类型**。卡只说「黄金数据 + 比对」，实现上必须用自带长度的类型（见 §1 的坑 1）；这属于测试侧细节，不影响驱动契约。
3. **`Receiver` 的删除拷贝/移动声明放在 public 段**。ADR-0012 §2.2 只要求「拷贝/移动删除」，未规定访问段；放在 public 是 G5b（`modernize-use-equals-delete`）的要求。
4. **`GnssModule::apply_out_to_receiver()` 只做了成员名适配**（`det.dialect->family` → `det.receiver->family()`），逻辑与 P3a 前一致。按卡 §1.2 该函数属于 P3b，本卡不重写它。
5. **`main` 的 `describe_frame()` 未删除**（卡 §1.4 明确 main 的帧标签属 P3b）。`gnss::Receiver::frame_tag` 在组件内已实现且有等价性用例，两者暂时并存，P3b 收敛为一处。
6. **`family_name` 在 `main` 的重复实现已删**（`GnssCmds.cpp`、`TraceText.cpp` 改调组件），与卡 §1.4 一致；`TraceText` 保留一层越界保护以维持「越界返回 `?`」的既有输出。

## 6. 未解决与未验证

1. **上板行为 NOT_RUN**。探测波特率顺序、真实模块的应答时序、PTK 等只在 host 上以黄金向量验证，硬件未验证。
2. **DRAM +48 B 与 ADR §5 的 12 B 预估不符**，原因是三个 `constinit` 静态驱动对象各占一个 vptr 加编译器对齐，`COMNAV` 关闭时 `registry` 仍保留对齐空洞。若后续需要精确控制，可在 P3c 用 `-Wpadded` 复核，本卡不做（属体积优化，非行为变更）。
3. **clangd/LSP 在 `components/gnss/include/gnss/*.hpp` 上持续报「找不到 `demux/Decoder.hpp`」「未声明的 `gnss`」，与实际编译无关**：LSP 没有读到 `builds/p3a/compile_commands.json`（本任务按卡要求把 IDF 构建目录放在 `builds/p3a*`，而 `.clangd` 默认指向 `build/`）。实际 IDF 与 host 构建、G5b 均通过；这是本仓库既有的 LSP 路径约定问题，未在本卡范围内修改 `.clangd`。
4. **`gnss::Receiver::build_output` 目前一律 `kNotSupported`**（P3b），因此 `main` 的输出订阅在这些驱动下暂不会真正下发命令；这是卡规定的阶段边界，不是缺陷。
5. **`test_apps/rtk_pure` 仅为满足 G5b 的编译数据库而构建**（`builds/p3a-tapp`），未在目标板上运行。

## 7. ADR 不一致记录（未修改 ADR）

- ADR-0012 §2.2 说 `name()` 返回 `"unicore"` 等短名，而 `demux::Decoder::name` 用的是 `"UnicoreHash"` 之类；实现在 `frame_tag` 里按解码器名分支，与 `name()` 无关，两套命名并存但语义不冲突。
- ADR-0012 §3 把 `include/gnss/drivers/*.hpp` 列为冻结头，但驱动的私有解码器符号（`gnss::unicore::kResponseDecoder`、`gnss::comnav::kPromptDecoder`）必须由这些头导出才能被测试引用；已按冻结头实现，无契约冲突。
- ADR-0012 §2.3 的探测顺序（NVS 提示优先，其次 `enabled_receivers()` × 波特率）与基线 `Controller` 一致，未改动。

P3a DONE

## 8. 审查方验收（Claude，2026-11-16）

- 黄金向量独立复现：在干净的 `git archive 4cf1320` 树上重新构建 `golden_gen`（ComNav ON），三份 `.inc` 与提交文件 `cmp` 逐字节一致。比对器同时校验命令字节、expect_reply、timeout_ms、switch_baud 与脚本长度（多一步即失败）。
- 复跑：`idf.py -B builds/p3a build` 0 warning；test/gnss ComNav OFF/ON × 普通/ASan 共 4 组全过（7/7/8/8）；tools/main_host_check 44/44；`sha256sum -c docs/frozen.sha256` 0 失败；改动的 C++ 文件 clang-format 0 差异。
- HIL（UM980 R4.10Build11833，/dev/ttyACM0，builds/p3a 烧录）：冷启动 `family unicore / model UM980 / baud 115200`，`ctrl probes=1 steps=8 nacks=0 to=0 reprobes=0 fail=0`（NVS 提示一次命中）；15 s 内 `nmea gga=rmc=gsa=gsv=15 bad=0`。
- 未覆盖：无 NVS 提示的全表冷探测在板上未执行（控制台没有清除 gnss 提示的命令，未擦 NVS 以免丢 SN/白名单）；该路径由 TestGnssController 覆盖。F9P/K803 仅主机测试。
