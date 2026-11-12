# G1 分流器引擎（BATCH-G，ADR-0004）

执行方：pi + codex/gpt-6.1-sol。先完整阅读 `AGENTS.md`、`docs/adr/ADR-0004-stream-demux.md`（重点 §2、§3）、`components/rtk_demux/include/rtk_demux.h`。

## 交付
- `components/rtk_demux/rtk_demux.c`：实现 `rtk_demux.h` 全部函数，严格遵循 ADR-0004 §3 参考语义与不变量 I1–I4。
- 自测（可选、不交付）只能放在 `test/demux_dev/g1/`，不得改动 `test/demux/`（G3 的黑盒测试工程）与 `test/host/`。
- 报告 `docs/reports/G1.md`。

## 要求
- 纯 C11：只包含 `rtk_demux.h` 与 `<string.h>`；无 malloc、无 VLA、无递归、无浮点、无 printf；栈占用 O(1)（不得在栈上开 buf_size 级数组）。
- AGENTS.md §2 的 C 文件规则（BARR-C 安全核心、`s_` 静态、switch default、窄化注释、无 goto）。
- 回放重扫必须原地完成（`memmove`），注意：回放过程中解码器可能在 p < len 处得到 FRAME；回调中 `buf` 必须保持该帧内容不变直到回调返回。
- 防重入：回调期间对同一实例调用 `feed/abort/init` 返回 `RTK_DEMUX_ERR_REENTRY`，且不破坏状态。
- 未知字节：连续的未知字节合并后回调，帧回调前先冲刷积压的未知字节；来自调用方输入的未知字节可直接用输入指针回调，来自 buf 的未知字节用 buf 指针回调（注意回调后再 memmove）。
- `need` 契约违规（0 或超 max_frame）按 REJECT 处理并计入该解码器 rejects；超 buf_size 另计 overflows。
- 性能：空闲态每字节一次位图查表；累积态按 need 整段 memcpy，每段只调用一次 step。
- 源文件头：`Generated-by: pi + codex/gpt-6.1-sol`，人工负责人 `UNKNOWN（待人工审查签字）`。

## 自检命令（报告逐项 PASS/FAIL）
```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wcast-align \
    -Wimplicit-fallthrough -Wundef -Wswitch-default -Wvla -Wstrict-prototypes -Wmissing-prototypes \
    -Icomponents/rtk_demux/include -c components/rtk_demux/rtk_demux.c -o /dev/null
gcc -std=c11 -m32 ...同上...
arm-none-eabi-gcc -std=c11 -mcpu=cortex-m0plus -mthumb -Os ...同上...
```
G3 的 `test/demux` 就绪后，跑它的普通与 ASan/UBSan 构建并在报告中给结果（失败时区分是引擎缺陷还是测试缺陷，有争议写证据，不要改 G3 的测试）。

## 禁止
修改任何冻结文件（AGENTS.md §4，含本 ADR 和三个新头文件）、`rtk_demux_decoders.c`（G2）、`test/demux/`（G3）；烧录、联网安装、git commit。
