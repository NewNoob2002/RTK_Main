# G-optisens-family 实现报告

- 任务卡：`docs/tasks/G-optisens-family.md`
- 分支：`feat/gnss-optisens-q1`（基线 `215851d`，构建前缀 `builds/gq-*`）
- 结论：实现完成，除授权冻结头触发的 `G1-frozen` 外，全部自动门禁（34/35）`PASS`；HIL 待 Architect 在实板执行（`NOT_RUN`）。

## 1. 改动清单

### 1.1 授权修改的冻结文件（`G1-frozen` 预期 FAIL）

| 文件 | 改动内容与理由 |
| --- | --- |
| `components/gnss/include/gnss/Types.hpp` | 在 `gnss::Family` 末尾追加 `kOptisens`（值 4），保持既有 NVS 数值兼容；同步更新 `Model::kCm680`、`familyName()` 与 `familyFromU8()` 注释。任务卡 §2.1 明确授权。 |
| `components/gnss/include/gnss/drivers/UnicoreReceiver.hpp` | 新增 `class Cm680Receiver final : public UnicoreReceiver` 声明（覆盖 `family()`、`name()`、`forModel()`），完全复用 `UnicoreReceiver` 命令/应答/波特率/CRC 实现，不复制代码且无需新增独立头文件与 Kconfig 开关。任务卡 §2.1 明确授权。 |

其余冻结文件零改动（`builds/gq-logs/G1-frozen.log` 仅报告以上 2 个文件校验和变更）。

### 1.2 非冻结文件改动

| 文件 | 改动摘要 |
| --- | --- |
| `components/gnss/src/DriverInstances.hpp` | 在 `GNSS_ENABLE_UNICORE` 下新增 `extern const Cm680Receiver kCm680Receiver;` 声明。 |
| `components/gnss/src/drivers/UnicoreReceiver.cpp` | 定义 `constinit const Cm680Receiver kCm680Receiver{}`；`UnicoreReceiver::forModel(kCm680)` 与 `Um982Receiver::forModel(kCm680)` 返回 `kCm680Receiver`；实现 `Cm680Receiver::family()`（`kOptisens`）、`name()`（`"optisens"`）、`forModel()`（`kCm680` 返回 `*this`，其余交回 `kUm980Receiver.forModel(model)`）。 |
| `components/gnss/src/Registry.cpp` | `familyOf(kCm680) -> kOptisens`；`familyName(kOptisens) -> "optisens"`；`familyFromU8(4) -> kOptisens`；`receiverFor(kOptisens)` 在 `GNSS_ENABLE_UNICORE` 下返回 `&detail::kCm680Receiver`；`kReceivers`（`enabledReceivers()`）保持不增加新槽位，避免冷启动多跑一轮 `VERSIONA`。 |
| `components/gnss/src/ControllerProbe.cpp` | `hintReceiver()` 在缓存为 `Family::kOptisens` 时从 `receivers` 找到 `Family::kUnicore` 并经 `forModel(Model::kCm680)` 映射到 `Cm680Receiver`，保证新缓存 `(4, 6)` 首候选即命中。 |
| `components/gnss/private_include/PppSequence.hpp` | 新增 `supportsSatMask(Model)` 声明，按型号判定 0x06 截止角/星座开关支持能力。 |
| `components/gnss/src/PppSequence.cpp` | 实现 `supportsSatMask(Model)`（`kUm980`/`kUm981`/`kUm982`/`kCm680` 为 `true`，其余为 `false`）；`supportsPpp(Model)` 保持仅对 UM98x 为 `true`。 |
| `main/modules/gnss/GnssModulePpp.cpp` | 第 61、114 行改用 `::gnss::supportsSatMask(controller().detected().probe.model)`，消除对 `Family::kUnicore` 的硬编码。 |
| `main/modules/bt_link/BtLinkModule.cpp` | 第 109 行 `phoneStartSatConfig()` 改用 `::gnss::supportsSatMask(status.model)`。 |
| `main/modules/gnss/GnssBaseConfig.cpp` | 第 515 行 `isBaseRtcmSupportedForFamily()` 对 RTCM 1033 放行 `Family::kOptisens`（并注释原因：此函数仅有家族参数，无型号上下文）。 |
| `test/gnss/TestGnssRegistry.cpp` | 更新 `familyOf(kCm680)`、`familyName(kOptisens)`、`familyFromU8(4/5)`、`receiverFor(kOptisens)` 断言及禁用开关分支检查。 |
| `test/gnss/TestGnssUnicoreReceiver.cpp` | 更新 `testMatchCm680()` 验证 `Cm680Receiver` 实例、`family()`/`name()` 与 `forModel()` 互转；扩展 `testSupportsPpp()` 覆盖 `supportsSatMask()`；新增 `testCm680ProbeHints()` 覆盖无缓存、新缓存 `(4, 6)`、旧缓存 `(1, 6)` 三种探测场景。 |
| `tools/main_host_check/GnssModeCheck.cpp` | `testBaseRtcmFamilyFilter()` 补充 `Family::kOptisens` 对 1033/1230 与后缀标签的校验。 |
| `tools/main_host_check/GnssResetCheck.cpp` | 更新 CM680 家族断言（`kOptisens` / NVS `family=4`）；在 `testGnssTargetBaudPolicy()` 新增用例 7（旧缓存 `family=1, model=6` 迁移写回 `family=4`）与用例 8（新缓存 `family=4, model=6` 首候选 1 次命中）。 |
| `tools/main_host_check/GnssStreamCheck.cpp` | 第 395 行 CM680 测试状态改为 `st.family = ::gnss::Family::kOptisens`。 |

说明：
- **Kconfig / CMake 开关**：`Cm680Receiver` 随 `RTK_GNSS_DRV_UNICORE` / `CONFIG_RTK_GNSS_DRV_UNICORE` 启用，未新增驱动开关（符合任务卡 §2.3）。
- **`main/modules/storage/StoreCatalog.cpp`**：`gnssCacheValueName()` 已统一调用 `gnss::familyFromU8()` 与 `gnss::familyName()`，无需改代码即可在 `config gnss` 中显示 `4 (optisens)`。
- **Golden 产物**：`test/gnss/golden/GoldenGen.cpp` 为 ADR-0010 归档生成器（依赖已移除的 `Dialect.hpp`，不参与构建），且只遍历到 `Model::kK803`，`GoldenUnicore.inc` 不受影响，无差异。

## 2. 全部 `Family::kUnicore` 判断点处置表

基于全仓 `rg -n 'Family::kUnicore' components/ main/ test/ tools/ test_apps/` 复核：

| 位置 | 原用途 | 处置方式 |
| --- | --- | --- |
| `components/gnss/src/Registry.cpp:77` | `familyOf(Model::kCm680)` 返回 `kUnicore` | 改为返回 `Family::kOptisens` |
| `components/gnss/src/Registry.cpp:91,107-108` | `familyName` / `familyFromU8` | 保留 `kUnicore` 分支，新增 `kOptisens` 分支 |
| `components/gnss/src/drivers/UnicoreReceiver.cpp:89` | `UnicoreReceiver::family()` | 保留（UM980/UM981/UM982 仍为 `kUnicore`）；由 `Cm680Receiver::family()` 覆写返回 `kOptisens` |
| `main/modules/gnss/GnssModulePpp.cpp:61,114` | 0x06 截止角/星座命令下发与重启重放准入 | **改为型号能力判定** `::gnss::supportsSatMask(model)`（UM98x + CM680 为真；PPP 仍经 `supportsPpp(model)` 仅 UM98x 为真） |
| `main/modules/bt_link/BtLinkModule.cpp:109` | 手机 0x06 卫星配置投递准入 | **改为型号能力判定** `::gnss::supportsSatMask(status.model)` |
| `main/modules/gnss/GnssBaseConfig.cpp:515` | 基站 RTCM 1033 按家族过滤 | 放行 `Family::kOptisens` 并加注释（控制台/配置过滤接口仅接收 `gnss::Family`，无型号参数） |
| `components/gnss/src/ControllerProbe.cpp:42`（新增） | `hintReceiver()` 解析 `kOptisens` 缓存 | 在 `receivers` 查 `kUnicore` 并经 `forModel(kCm680)` 映射，避免重复探测槽位 |
| `test/gnss/TestGnssRegistry.cpp` | 家族枚举与注册表断言 | 保留 `kUnicore` 用例并新增 `kOptisens` 覆盖 |
| `test/gnss/TestGnssUnicoreReceiver.cpp` | UM980/UM982/CM680 驱动测试 | 保留 UM982 `kUnicore` 断言，CM680 改为 `kOptisens` |
| `test/gnss/TestGnssReadback.cpp`、`TestGnssClassify.cpp`、`TestGnssController.cpp` | Unicore 基类读回/分类/控制器桩测试 | 保持不变（测试对象为 UM98x 或测试桩） |
| `test/gnss/golden/GoldenGen.cpp:201,302` | ADR-0010 归档黄金生成器（不编译，仅遍历至 `kK803`） | 保持不变，无 golden 变化 |
| `tools/main_host_check/GnssModeCheck.cpp:37,361-363` | 桩驱动与基站 RTCM 过滤测试 | 保留 `kUnicore` 断言并新增 `kOptisens` 断言 |
| `tools/main_host_check/GnssResetCheck.cpp:117` | 预设 UM980 缓存的辅助启动函数 | 保留（用于 UM980 用例）；CM680 用例与新增用例 7/8 覆盖 `kOptisens` 与旧缓存迁移 |
| `tools/main_host_check/GnssStreamCheck.cpp:395` | CM680 模拟状态快照 | 改为 `st.family = ::gnss::Family::kOptisens` |

## 3. 探测时序说明

1. **无缓存冷启动（`hint = {kUnknown, kUnknown, 0}`）**：
   - `enabledReceivers()` 表长不变（不单独为 `Cm680Receiver` 占位）。
   - 第 1 个候选即 `kUm980Receiver` @ `bauds[0]`（115200），发送一次 `VERSIONA\r\n`。
   - CM680 回 `#VERSIONA,...;"CM680",...` → `UnicoreReceiver::matchProbe()` 返回 `Model::kCm680` → `Controller::onFrame()` 调用 `forModel(Model::kCm680)` 切换至 `detail::kCm680Receiver`（`family() == kOptisens`，`name() == "optisens"`）。
   - 探测总次数与改造前完全一致（1 次命中），不会因新增家族多跑一轮探测。
2. **新缓存（`family = 4, model = 6, cache_baud = 460800`）**：
   - `hintReceiver()` 识别 `hint.family == Family::kOptisens`，从 `receivers` 找到 `kUm980Receiver` 并通过 `forModel(Model::kCm680)` 返回 `&detail::kCm680Receiver`。
   - 候选 0 直接使用 `kCm680Receiver` @ `460800` 发送 `VERSIONA\r\n`，1 次命中（`probesSent == 1`）。
3. **旧缓存迁移（现网已存 `family = 1, model = 6, cache_baud = 460800`）**：
   - `hintReceiver()` 命中 `kUm980Receiver`，候选 0 发送 `VERSIONA\r\n` → `matchProbe()` 识别 `Model::kCm680` → `forModel(Model::kCm680)` 切换至 `kCm680Receiver`（`family = 4`）。
   - 进入 `kRunning` 后，`GnssModule::persistDetectedHint()` 检测到 `detFamily (4) != hint_.family (1)`，自动将 NVS 缓存更新并写回 `family = 4`（`optisens`），不卡死、不报 fail。

## 4. 门禁与验证结果

执行命令：
```sh
export IDF_PATH=$HOME/esp/esp-idf-v6.1 IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf6.1_py3.13_env
. "$IDF_PATH/export.sh" >/dev/null
tools/ci/run_gates.sh --base 215851d --prefix gq
```

| 门禁 | 结果 | 备注 |
| --- | --- | --- |
| `G1-frozen` | `FAIL`（预期） | 仅授权的 2 个冻结头变动：`components/gnss/include/gnss/Types.hpp`、`components/gnss/include/gnss/drivers/UnicoreReceiver.hpp`（待 Architect 合并时重算 `docs/frozen.sha256`） |
| `G2-host` / `G2b-host-asan` | `PASS` | — |
| `G2-app_fw` / `G2b-app_fw-asan` | `PASS` | — |
| `G2-services` / `G2b-services-asan` | `PASS` | — |
| `G2-stream_demux` / `G2b-stream_demux-asan` | `PASS` | — |
| `G2-bt_reconnect` / `G2b-bt_reconnect-asan` | `PASS` | — |
| `G2-cli` / `G2b-cli-asan` | `PASS` | — |
| `G2-gnss` / `G2b-gnss-asan` | `PASS` | 含 `testCm680ProbeHints`、Optisens 注册表测试 |
| `G2-gnss-all` / `G2b-gnss-all-asan` | `PASS` | 全驱动（ComNav + u-blox + Unicore/Optisens） |
| `G2-nvs_store` / `G2b-nvs_store-asan` | `PASS` | — |
| `G2-web_proto` / `G2b-web_proto-asan` | `PASS` | — |
| `G2-main_host_check` / `G2b-main_host_check-asan` | `PASS` | 含旧缓存 `(1,6)->(4,6)` 迁移与新缓存首候选命中 |
| `G2-t13_host_check` / `G2b-t13_host_check-asan` | `PASS` | — |
| `G5-format` | `PASS` | `clang-format --style=file` 全通过 |
| `G6-doc_check` | `PASS` | 新增/改动函数 Doxygen 100% 合格 |
| `G3-idf` | `PASS` | `rtk_main.bin` = 879568 B，0 warning |
| `G3-idf-trace-off` | `PASS` | 0 warning |
| `G3-idf-production` | `PASS` | 0 warning |
| `G3-idf-console-none` | `PASS` | 0 warning |
| `G3b-rtk_pure-esp32` / `esp32c3` | `PASS` | 0 warning |
| `G5b-tidy` | `PASS` | 0 warning / 0 error |
| `G7b-runtime` | `PASS` | 堆白名单、无异常/RTTI/`__cxa_guard_*` |
| HIL（P2 v1.41 实板） | `NOT_RUN` | 按授权边界不烧录、不连接 `/dev/ttyACM0`，交 Architect 验证 `gnss status`、`config gnss` 缓存迁移、460800/20 Hz/基站 RTCM |

## 5. 偏离与未解决问题

- 无契约偏离；无阻塞问题。
- 待 Architect 合并时重算 `docs/frozen.sha256` 并执行实板 HIL 验收。
