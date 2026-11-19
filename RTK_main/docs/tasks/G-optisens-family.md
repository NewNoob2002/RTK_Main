# G-optisens：CM680 归入 Optisens 家族

- 基线：main `7d3c803`（G-baud460k 已合并）
- 分支：`feat/gnss-optisens-q1`（与 Q1 同一实施者、同一工作树，先做本卡再做 Q1）
- 构建前缀：`builds/gq-*`
- 关联：ADR-0012 §2.8/§2.9（本卡追加 §2.9 修订 B）

## 1 背景

CM680（P2 v1.41 实板，固件 `CM680_SDK.1.2.5a_GNSS.1.1.9`）由 Optisens 生产（手册封面 “Optisens《CM6XX 系列高精度模组产品数据接口协议》”），
命令集与 Unicore N4 平台兼容。G-CM680 为省事把它挂在 `Family::kUnicore` 下，`gnss status` 显示 `family unicore`，与实际厂商不符。
用户要求：CM680 的家族显示与存储为 **optisens**。

## 2 要求

1. **枚举追加**：`components/gnss/include/gnss/Types.hpp` 的 `Family` 末尾追加 `kOptisens`（值 4）。不得插队、不得改已有值（NVS 兼容）。
   本卡授权修改该冻结头；如确需新增 `gnss/drivers/OptisensReceiver.hpp` 或改 `gnss/drivers/UnicoreReceiver.hpp`（例如把 `family()` 交给叶子类），同样授权，
   报告中逐文件列出理由。不得修改其它冻结文件。Architect 合并时重算 `docs/frozen.sha256`，G1 预期 FAIL 仅限上述文件。
2. **驱动**：CM680 使用独立叶子类（如 `Cm680Receiver final`），`family()` 返回 `kOptisens`，`name()` 为 `"optisens"`；
   命令、应答解析、能力位、波特率切换、CRC 处理全部复用 Unicore 家族实现，**禁止复制代码**（继承或组合 `UnicoreReceiver`）。
3. **注册与查询**：`familyOf(kCm680) == kOptisens`；`familyName(kOptisens) == "optisens"`；`familyFromU8(4)` 返回 `kOptisens`；
   `receiverFor(kOptisens)` 能找到该驱动；Kconfig/CMake 驱动开关沿用 Unicore 的（CM680 随 Unicore 驱动一起启用，不新增开关，除非现有结构要求，报告说明）。
4. **探测**：
   - 无缓存冷启动：探测次数与当前一致（`VERSIONA` 一次命中 CM680），不能因新家族多一轮探测。
   - 缓存 `family=4 model=6` 时首候选即命中。
   - 旧缓存 `family=1 model=6`（现网已存）：可以首候选落空一次再命中，但必须正确识别为 CM680/optisens，并把缓存更新为 `family=4`；不得卡死或报 fail。
5. **行为不变**：所有按 `Family::kUnicore` 判断的地方改为对 CM680 保持现有行为。已知位置（实施者须全量 `rg 'Family::kUnicore'` 复核）：
   - `main/modules/gnss/GnssModulePpp.cpp:61,114`（PPP；CM680 当前不支持，`supportsPpp` 仍只对 UM98x 为真）
   - `main/modules/bt_link/BtLinkModule.cpp:109`
   - `main/modules/gnss/GnssBaseConfig.cpp:515`（基站 RTCM 家族判定：CM680 维持与 Unicore 相同的可用报文集）
   - `components/gnss/src/PppSequence.cpp`、`components/gnss/src/drivers/UnicoreReceiver.cpp` 中 `kCm680` 分支
   - `test/gnss/golden/GoldenGen.cpp`、`main/modules/storage/StoreCatalog.cpp`（`config gnss` 显示 `4 (optisens)`）
   优先改为能力位/型号判断，而非再加一个家族比较；无法用能力表达的保留家族比较并注释原因。
6. **测试**：更新 `test/gnss/TestGnssRegistry.cpp`（familyName/familyFromU8/receiverFor 覆盖 kOptisens）、CM680 相关驱动与 main_host_check 用例；
   新增旧缓存 `family=1 model=6` 迁移用例与新缓存首候选命中用例。golden 文件若变化，报告说明差异仅为家族名。

## 3 验收

- `tools/ci/run_gates.sh --base 7d3c803 --prefix gq` 除 G1（仅授权文件）外全 PASS。
- 报告 `docs/reports/G-optisens-family.md`：改动清单、全部 `kUnicore` 判断点处置表、探测时序说明、门禁结果。
- HIL 由 Architect 执行：`gnss status` 显示 `family optisens`；旧缓存板子一次重启后 `config gnss` 显示 `family = 4 (optisens)`；
  460800 切换、20 Hz、基站 RTCM 不回退。
