# QA 验收报告：BATCH-C（T10–T12，C++ 版）

验收方：Claude（独立于实现方 omp/DeepSeek-V4.1-Flash）。日期 2026-11-10。所有检查都在独立构建目录（/tmp/rtk-*）重跑。

| ID | 结果 | 证据 |
| --- | --- | --- |
| G1 冻结文件 | PASS | `sha256sum -c` 全部 OK；只新增了预期文件（组件 .cpp/CMakeLists、main、reports） |
| G2 host 测试 | PASS | 6/6 可执行文件，74 用例 0 失败 |
| G2b ASan/UBSan | PASS | 6/6，无 sanitizer 报告 |
| G3 目标构建 | PASS | 干净构建，0 warning；image 137,281 B（rtk_bt/os/store/main 仍为骨架） |
| G5 clang-format | PASS | 全部自有 .cpp/.hpp 0 处差异。astyle_py：NOT_RUN（需要联网授权） |
| G5b clang-tidy | PASS（修正后） | 实现 .cpp 0 发现。3 条发现在 Architect 的冻结头文件中（见下），已修正 |
| G6 分层 | PASS | rtk_proto/rtk_core 无 IDF/FreeRTOS 头 |
| G7 禁用特性 | PASS | 自有源码中没有堆、异常、RTTI、`std::function`、会分配的容器、`volatile` |
| G7b C++ 运行时 | NOT_RUN | 待 BATCH-D 链接蓝牙栈后检查 |
| G8 代码审查 | PASS（Low 2 条） | 见下 |

## 发现

1. **Medium（Architect 自身）**：冻结头文件 `command_dispatcher.hpp` 与 `link_service.hpp` 删除了拷贝，却没有声明析构和移动（C.21 rule of five）；`DeviceInfo::com_baud` 默认值 115200 是魔数。已修正：补全五个特殊成员，新增 `kDefaultComBaud`，行为不变；重新运行 G2、G5b 均通过，并重算 `docs/frozen.sha256`。
2. **Low（实现方）**：`command_dispatcher.cpp` 中配置块拷贝和 0x44 查询应答使用了带长度检查的下标循环，没有经过 `ByteWriter`/`ByteReader`。逐处核对，全部有界，属于风格偏离，不阻塞。
3. **Low（实现方）**：`LinkService::apply` 处理重复句柄 open（LinkManager 规则 1）时会释放该句柄已有的解析器槽，而 LinkManager 中的会话保留到关闭事件到来。随后的 disconnect 会触发关闭事件，状态会重新一致，不阻塞。

## 审查要点（通过）

- `ByteWriter/Reader`：始终保持 `pos_ ≤ size`，宽度先检查后写入，满足“失败不部分写入”。
- `FrameParser::feed`：设置 `expect_` 前已检查上限，`buf_` 下标始终 < kMaxFrame；同步与重同步规则与契约一致。
- `LinkService::on_data`：当前句柄被断开后立即返回，不使用已释放的槽；错误计数在释放槽位时累加到 `retired_*`。

## 结论

BATCH-C：**PASS（软件证据）**。硬件证据全部 NOT_RUN。最终签字由人完成。
