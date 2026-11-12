# C 版分流器 v1（归档，只读，不参与构建）

BATCH-G（ADR-0004）的纯 C11 实现：引擎 `rtk_demux.c`（pi + codex/gpt-6.1-sol）、解码器（pi + deepseek-v4.1-flash）、
黑盒测试 `test/demux`（pi + gemini-3.8-flash，28 用例，普通与 ASan/UBSan 全通过）。G4 验收全部门禁通过（2026-11-11）。
用户决定改为 C++ 重写（ADR-0005），本目录保留作 C++ 版的参考实现与测试向量来源。已知缺口：首次 init 读取未初始化的
in_callback（G1 报告“阻塞项 1”），C++ 版用 constexpr 构造的默认成员初值消除。
