# W3 交叉审查

审查基线为 `eaf1415`。W3 为 ESP-IDF 项目的纯 C++20 协议组件。本次对照 `docs/web/ws-protocol.md` v1、`docs/tasks/W3-web-proto.md` 和 `AGENTS.md`，检查解析、输出、键表、主机测试及 CI 接线。审查结论为暂缓合入，先修复两项 block。

## block

1. **有效帧会因未知键的数量被拒绝**。请求合法字段后追加 10 个互不重复的小写标量扩展字段，帧长仍远低于 1024 B。第 13 个成员使 `rememberName()` 返回失败，整帧得到 `bad_json`，违反协议 §1 的未知键忽略规则。用帧长给重复检查定界，保留重复键拒绝语义，并覆盖第 12、13 个成员的边界。定位 `components/web_proto/src/Request.cpp:10`、`components/web_proto/src/Request.cpp:291`、`docs/web/ws-protocol.md:11`。
2. **MAC 和 SN 校验失败后会改写上一份规范值**。如果调用者复用此前成功的 `CanonicalValue`，输入末尾才出现非法字符时，前缀字节已写入，而 `kind` 和 `size` 仍沿用上一份结果。这违反公开的「失败时保持原内容」契约。先完整校验或暂存在局部结果，通过后一次提交。用合法值预填输出，再用末尾非法的 MAC 和 SN 验证所有字段都保持原值。定位 `components/web_proto/src/KeyTable.cpp:145`、`components/web_proto/src/KeyTable.cpp:177`、`components/web_proto/include/web_proto/KeyTable.hpp:77`。

## should

3. **超长帧保留上一条请求的视图**。成功解析后复用同一个 `Request` 处理 1025 B 帧时，长度检查直接返回，旧 `id` 和视图仍在。复用或释放原帧后，这些视图也可能失效。公开接口承诺所有失败都会清空输出。统一失败路径，并把 1025 B 用例改成先填充同一输出对象再断言错误与清空。定位 `components/web_proto/src/Request.cpp:538`、`components/web_proto/include/web_proto/Request.hpp:52`、`test/web_proto/test_web_proto_request.cpp:72`。
4. **未知整数字段绕过 int32 上限，负 ID 的前导零会被误分类**。未知标量值 `2147483648` 或 `-2147483649` 目前会被成功跳过，尽管协议 §1 将所有整数限制为 int32。ID `-01` 在检查分隔符前就返回 `bad_value`，与 `01` 的 `bad_json` 不一致。先检查完整数字语法，再按字段范围分类。补 int32 两端和两侧越界、负前导零及未知字段整数边界。定位 `components/web_proto/src/Request.cpp:184`、`components/web_proto/src/Request.cpp:246`、`components/web_proto/src/Request.cpp:355`、`docs/web/ws-protocol.md:9`。
5. **键名没有按 v1 字符集校验，重复检测使用转义前拼写**。`parseFrame()` 用通用字符串扫描读取成员名，再按原文比较。它会接受空键和大写键。含斜线的未知键以原字符和 JSON 转义形式各出现一次时，也会绕过重复检查。按 §1 对成员名执行 ASCII 字符集检查，随后保留原文比较。补非法成员名、转义成员名和截断转义用例。字符串值目前接受标准短转义、拒绝 Unicode 转义，这与 §1 给设备规定的最小支持范围相容。帧内原始非 ASCII 字节未验证 UTF-8，需在 W3 校验，或在 W5 集成契约明确由 WebSocket 层预先校验。定位 `components/web_proto/src/Request.cpp:83`、`components/web_proto/src/Request.cpp:291`、`components/web_proto/src/Request.cpp:313`、`docs/web/ws-protocol.md:8`。
6. **字段视图的所有权偏离任务卡，公开注释缺少输入寿命约束**。未转义字段借用输入帧，转义字段借用 `Request` 内部 1024 B 缓冲。任务卡要求指向输入缓冲，而头文件把 `Request` 原地存活写成主要条件。W5 若在解析后复用接收缓冲，未转义字段会读到新内容。请 Architect 确认是否接受双重存储，并明确接收缓冲须保持存活且不变、再次解析会使旧视图失效、异步提交前须复制有效载荷。用一个混合了原始和转义字段的成功帧检查两个视图的实际归属，保留当前禁止复制和移动的保护。定位 `docs/tasks/W3-web-proto.md:11`、`components/web_proto/include/web_proto/Request.hpp:21`、`components/web_proto/include/web_proto/Request.hpp:42`、`components/web_proto/src/Request.cpp:388`。
7. **键值和输出边界测试还缺任务卡要求的逐键覆盖**。键表 14 项、周期、布尔值、MAC、SN 与协议 §4 对齐，四个星座键带可选标记，SN 带解锁标记。现有用例只给 GGA 合法周期、GST 非法周期，只给 BDS 合法布尔值，只给白名单 add 非法 MAC。给每个键各加一条合法、一条非法用例，避免个别表项接错验证器时测试仍通过。周期校验当前将字符串 `"-0"` 当作 `"0"` 接受，mock 会拒绝，建议按 §4 的列举值统一。Writer 的外部缓冲原子性成立，建议再以哨兵检查内部暂存溢出，以及转义后连末尾右花括号计入的 1024 B 成功、1025 B 失败两种情况。定位 `components/web_proto/include/web_proto/KeyTable.hpp:34`、`components/web_proto/src/KeyTable.cpp:51`、`test/web_proto/test_web_proto_key_table.cpp:74`、`test/web_proto/test_web_proto_writer.cpp:159`。
8. **部分公开 API 的 Doxygen 标签与调用约束不完整**。生产函数已有中文 `@brief`，G6 检出的 51 个函数均通过。人工核对发现 `parseRequest`、`validateValue` 和 `finish` 的输出参数仍使用 `@param`，项目 §6.2 要求 `@param[out]`。Writer 只有 `clear()` 的逐函数注释明确单任务使用。错误码头文件的文件头同时写了「非 ISR 安全」和「ISR 中也可调用」。统一公开契约并补齐输出参数、线程和 ISR 说明。定位 `components/web_proto/include/web_proto/Request.hpp:52`、`components/web_proto/include/web_proto/KeyTable.hpp:77`、`components/web_proto/include/web_proto/Writer.hpp:29`、`components/web_proto/include/web_proto/Writer.hpp:97`、`components/web_proto/include/web_proto/Error.hpp:2`、`docs/CODING-STYLE.md:431`。

## suggest

9. **用已校验的区间直接构造视图**。主机静态库的 `Request.cpp.o` 引用了 `std::__throw_out_of_range_fmt`，来源与两处 `string_view::substr` 的检查路径一致。当前索引由解析器推进，未发现可达的越界异常，既有 IDF 产物也通过 G7b。使用已经证明有界的指针和长度构造视图，可去掉主机构建的这个异常辅助符号。定位 `components/web_proto/src/Request.cpp:437`、`components/web_proto/src/Request.cpp:475`。

## 核验

| 项目 | 结果 |
| --- | --- |
| 普通及 ASan/UBSan 主机测试 | PASS。直接运行 `builds/w3-web_proto/` 与 `builds/w3-web_proto-asan/` 下的三支现有测试二进制，两组各 32 项通过。沙盒内 LeakSanitizer 退出失败，经授权在沙盒外运行后完整通过。 |
| 格式、冻结文件、Doxygen | PASS。对全部 W3 C++ 源和头运行 `clang-format --style=file --dry-run --Werror`，运行 `sha256sum -c --quiet docs/frozen.sha256` 及 `python3 -I tools/doc_check/doc_check.py --base eaf1415`。 |
| 依赖和堆检查 | PASS。组件只依赖标准 C++ 头和纯 `fw/Result.hpp`，CMake 同时接入 IDF 与 host 严格告警。以既有 `builds/w3-idf` 及本机 Xtensa nm 运行 `tools/g7b_check/g7b_check.py`，117 个自有对象通过，包含 `web_proto`。 |
| CI 接线 | PASS（静态审查）。`tools/ci/run_gates.sh:85` 将新套件加入普通及 ASan 路径，`tools/tidy_check/tidy_check.py:239` 收录四个生产源文件。当前 G5 的 `git ls-files` 只覆盖已跟踪文件，本次直接检查了未跟踪文件。 |
| 全量门禁、重新构建、硬件与 W5 集成 | NOT_RUN。复用了已有构建产物以遵守只读审查范围。既有 G5b 日志通过，当前工作树尚无 W5 生产调用方。 |

解析器已覆盖 1024/1025 B、ID 0/1/65535/65536、常规重复键、嵌套和浮点的基本用例。输出器先检查关闭状态、内部失败状态和目标容量，再拷出完整字节，未发现半帧写入路径。
