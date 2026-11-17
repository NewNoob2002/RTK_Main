# S3：减少间接层——GNSS 驱动表改为 constinit 表，转发包装审计（ADR-0013 §2.3）

状态：ISSUED 2026-11-17，基线 `feat/gnss` 当前 HEAD（T0 = 本卡所在提交，S2 已完成）。
实施：omp + workbuddy/deepseek-v4.1-flash；审查/验收：Claude。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`、`docs/adr/ADR-0013-readability.md` §2.3、
`docs/adr/ADR-0012-gnss-receiver-drivers.md` §2.3/§2.8，再读 `components/gnss/**`、`test/gnss/**`、`main/module_list.inc`。

## 0 背景

现在加一个 GNSS 驱动或找到某个驱动实例，要跨 4 处：`gnss/DriverList.inc`（X-macro）→ `Registry.cpp` 展开 →
`src/DriverInstances.hpp`（extern 声明 + 开关归一化）→ 驱动 .cpp 里的实例和访问器（`unicoreReceiver()` 等）。
DriverList.inc 只生成一种产物（地址表），按 ADR-0013 §2.3 不该用 X-macro。

## 1 要做

### 1.1 GNSS 驱动表（本卡授权修改 `components/gnss/include/gnss/**` 中下列文件）
- 删除 `components/gnss/include/gnss/DriverList.inc`。
- `components/gnss/src/Registry.cpp`：`kReceivers` 直接写成
  ```cpp
  constinit const std::array<const Receiver *, kEnabledCount> kReceivers{
  #if GNSS_ENABLE_UNICORE
      &detail::kUm980Receiver,
  #endif
      ...
  };
  ```
  顺序（= 探测优先级）不变：Unicore → u-blox → ComNav；表上方一行注释说明“顺序即探测优先级，新增驱动在此加一行”。
- 删除公共访问器 `unicoreReceiver()`、`um982Receiver()`、`ubloxReceiver()`、`comnavReceiver()`（3 个驱动头的声明 +
  驱动 .cpp 的定义）。仍需要具体实例的代码改用：`receiverFor(Family::kX)`（基准叶子），UM982 用
  `receiverFor(Family::kUnicore)->forModel(Model::kUm982)`。测试里可写一个文件内的小助手函数，但不要放进组件公共头。
- `components/gnss/src/DriverInstances.hpp` 保留（开关归一化 + extern 声明），更新文件头注释（不再提 DriverList.inc）。
- `Receiver.hpp:183` 等注释里对 DriverList.inc 的引用改为 `Registry.cpp` 的 `kReceivers`。
- `ComnavReceiver` 只在全驱动构建（`test/gnss` 的 all-drivers 配置）里编译，必须在该配置下验证。

### 1.2 module_list.inc 文件头
`main/module_list.inc` 保留 X-macro（4 种产物）。文件头补一段“展开点”清单，列全每个 `#include "module_list.inc"` 的位置
（`main/AppContext.hpp` 3 处、`main/app/Executors.cpp` 1 处，以实际 rg 结果为准）及每处生成什么。

### 1.3 转发包装审计（全仓自有 C++ 代码：components/**、main/**，不含 test/tools/test_apps）
- 找出“函数体只有一条语句、只把参数原样转给另一个函数/方法”的函数（含成员函数、内联函数、static 函数）。
- 报告表格逐条列出：位置、包装→目标、分类：
  - **A 保留**：C ABI 边界（`extern "C"`）、测试接缝、虚函数覆盖/接口实现、公开 API 的稳定门面（跨组件调用方 ≥ 2）、
    `constexpr`/类型适配（实际做了类型转换或加了 `noexcept`/`[[nodiscard]]` 语义的不算纯转发）；
  - **B 删除**：同组件内部、调用点 ≤ 3 处、删了不会改公共头的纯转发——本卡直接删除，调用点改为直接调用目标；
  - **C 待定**：其余（尤其是改公共头的），只列出理由，不改。
- A 类若缺少说明是哪种边界的注释，补一行注释。

## 2 不做
- 不改行为、日志文本、控制台输出、协议字节；不改 `makeOps`/`Slot`/`FanoutRing` 等 ADR-0013 §2.3 点名保留的设计。
- 不改 docs/adr、AGENTS.md、CODING-STYLE（Claude 验收后同步）；不重算 `docs/frozen.sha256`；不提交。
- 不新增依赖、不用 uvx。

## 3 门禁（全部在 `builds/s3*` 下跑，日志放 `builds/s3-logs/`）
1. 所有主机测试库普通 + ASan 全过（同 S2-B4 的 10 个库，含 test/gnss 默认与 all-drivers）。
2. `idf.py -B builds/s3 build` 0 warning；关闭 GNSS 的配置（参照 builds/s2-b4-nognss）0 warning；
   `test_apps/rtk_pure` esp32 + esp32c3、`test_apps/rtk_spp_peer` esp32 编译通过。
3. 黄金字节：`test/gnss/golden/*.inc` 空 diff。
4. G7b PASS；clang-format 0 违规；tidy G5b 只允许剩 `SppServerTx.c:422` 一条既有 function-size。
5. `rtk_main.bin` 大小与 S2（800240 B）比较并在报告写出差值；访问器删除后预计略减，增加须说明原因。
6. `rg -n "DriverList|unicoreReceiver|ubloxReceiver|comnavReceiver|um982Receiver" --glob '!docs/**' --glob '!builds/**'` 为空。
7. frozen：`sha256sum -c docs/frozen.sha256` 只允许 gnss 3 个驱动头、`Receiver.hpp`、`DriverList.inc`（已删）以及 B 类删除涉及的头失败，报告逐条列出。

## 4 交付
报告 `docs/reports/S3.md`：改动文件清单、1.3 审计表（A/B/C 计数与逐条）、门禁结果、bin 差值、frozen 失败清单、遗留问题。
完成后在终端打印 `S3 DONE`。
