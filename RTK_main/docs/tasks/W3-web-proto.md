# W3：`web_proto` 纯组件（WebSocket JSON 解析/输出、键表）

依据：ADR-0017 §2.5；协议 `docs/web/ws-protocol.md` v1。
基线：main。分支：`feat/w3-web-proto`（worktree）。与 W1、W4 并行。
实现：omp；交叉评审：codex。Architect（Claude）复跑门禁、验收、合入。

## 1 目标

1. 新组件 `components/web_proto`：**不含 IDF/FreeRTOS 头**（与 `stream_demux` 同级的纯组件，写进 AGENTS.md 依赖表由 Architect 负责），
   不分配堆、不抛异常、C++20，接口放 `include/web_proto/`。
2. 请求解析：输入 `std::span<const char>`（≤ 1024 B）→ `Request{type(kSet/kCmd/kUnlock), id, key, value/name/token}`，
   键值以 `std::string_view` 指向输入缓冲；拒绝：非对象、超长、重复键、缺字段、`id` 越界、未知 `t`、嵌套、浮点。
   错误返回 §5 的错误码枚举。
3. 输出器：写入调用方给的定长缓冲（`std::span<char>`），支持对象/一层嵌套/字符串数组/int32/bool，自动转义 `"` `\`
   与控制字符；缓冲不足返回失败且不输出半截 JSON。
4. 键表：`KeySpec{name, kind(kEnum/kMac/kSn/kBool), allowed values, needsUnlock}` 的常量表与查表、值校验函数，
   覆盖协议 §4 全部键（星座键带“可选”标记）。校验通过后产出与键无关的规范值（整数或字节串）；
   **转成 `fw::Command` 不在本卡**（W5 在 main 里做，因为动词定义在 `main/def/`）。
5. `fw::Result` → 错误码映射函数（`web_proto` 可依赖 `app_fw` 的纯头 `fw/Result.hpp`）。

## 2 测试

- `test/web_proto/`：解析正反例（含协议 §6 示例、每种拒绝原因、边界 1024/1025 B、id 0/1/65535/65536、转义）、
  输出器缓冲不足、键表每键合法/非法值；加入 `tools/ci/run_gates.sh` 的 `HOST_DIRS`。
- 门禁：`tools/ci/run_gates.sh` 全部 PASS（plain + ASan、G5 格式、G5b tidy、G6 doc_check、G7b）。

## 3 交付

- 报告 `docs/reports/W3.md`：接口摘要、测试清单、门禁结果、代码行数。
- 不 commit，不改冻结文件（AGENTS.md 依赖表由 Architect 改）；结束打印 `W3 DONE`。
