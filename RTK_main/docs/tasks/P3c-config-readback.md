# P3c：Unicore `$CONFIG` 配置读回（ADR-0012 §2.7）

状态：ISSUED 2026-11-17，基线 `feat/gnss` T0 提交（见 `git log -1 -- docs/tasks/P3c-config-readback.md`）。
实施：pi + workbuddy/deepseek-v4.1-flash；审查/验收：Claude。

先完整阅读：`AGENTS.md`、`docs/CODING-STYLE.md`（§5.1 日志、§6.2 注释）、`docs/adr/ADR-0012-gnss-receiver-drivers.md`
（重点 §2.7 及其“数据通路”补充、§2.8）、`docs/adr/ADR-0009-trace-console.md` §2.12（日志样式），
再读 `components/gnss/**`、`components/demux/**`、`test/gnss/**`、`main/modules/gnss/**`。

## 0 已由 Claude 写好的冻结接口（T0，不得改动其签名/布局）

- `gnss/Controller.hpp`：`State::kReadback`（末尾追加）、`ControllerStats::readback_lines` / `readback_seq`、
  `[[nodiscard]] bool request_readback(uint32_t now_ms) noexcept`（doc 即契约）。`Controller.cpp` 里目前是返回 false 的桩。
- `gnss/Receiver.hpp`：`build_readback(CmdBuf&)`、`match_readback(dec, frame, out)`（早已存在，基类默认不支持）。
- `gnss/drivers/UnicoreReceiver.hpp`：`kResponseDecoder` 注释已改为接受 `$command,` 与 `$CONFIG,`。
- `main/def/GnssDef.hpp`：`kGnssReadbackBytes = 1536`、`struct GnssReadback`（seq/lines/len/supported/truncated/busy）。
- `main/def/GnssSubDef.hpp`：`GnssSubVerb::kReadback = 5`。
- `main/data/AppData.hpp`：`fw::Slot<GnssReadback> gnss_readback`、`std::array<char, kGnssReadbackBytes> gnss_readback_text`
  （交接规则见成员注释；Slot 上限 512 B，文本不得放进 Slot）。

## 1 要做

### 1.1 应答解码器（`src/drivers/UnicoreReceiver.cpp`，`response_first_byte` / `response_step` / `explain_response`）
- 第 2 字节起接受 `"command,"` **或** `"CONFIG,"` 两种前缀（逐字节判定，任一前缀仍可能时返回 kNeedMore）；
  之后的规则（可打印 ASCII、不含 `$`/`*`、`*hh` XOR、`\r\n`、`kResponseMaxFrame` 上限）完全不变，`$CONFIG` 的 XOR 规则同 `$command`。
- `explain_response` 的拒收原因文字同步（如 `byte 1 is 'X', expected 'c' or 'C'`）。
- 现有 `$command` 黄金向量字节不许变。

### 1.2 Unicore 驱动读回
- `build_readback`：写 `"CONFIG\r\n"`，返回 true。
- `match_readback`：仅当 `&dec == &kResponseDecoder` 且帧以 `$CONFIG,` 开头时命中；把 `$CONFIG,` 之后、`*` 之前的正文写入 out
  （超长截断到 out.size()），返回长度；否则 0。`$command,CONFIG,response: OK*..` 不算一行（返回 0），但仍按现有规则被分类。
- 新增 protected static 成员时在头里加一行注释声明（只允许在 `UnicoreReceiver` 的 `protected:` 段新增，其余冻结部分不动）。
- `Um982Receiver` 继承即可，不覆盖。u-blox / ComNav 不实现（保持基类默认）。

### 1.3 Controller（`src/Controller.cpp`）
- `request_readback(now)`：仅 `state()==kRunning` 且 `receiver().build_readback(cmd)` 为 true 时接受：保存命令、进入 kReadback、
  `readback_lines=0`、截止时间 = now+1000（静默）与 now+5000（总）；否则返回 false，不改状态。
- 下一次 `poll()` 写出命令（沿用现有写命令通道/统计，与 kSend 同风格）。
- `on_frame()` 在 kReadback 时：`receiver().match_readback(...)` > 0 → `readback_lines++`，静默截止 = now+1000（不超过总截止）。
  其他帧照常处理（App 报文分类等不受影响）；kReadback 期间不触发 kRunning 的静默重探测。
- `poll()` 静默或总截止到期 → 回 kRunning，`readback_seq++`。常量放 Controller.cpp 匿名命名空间（`kReadbackIdleMs`、`kReadbackTotalMs`）。
- 用 `uint32_t` 回绕安全的比较（照抄文件里已有的写法）。

### 1.4 GnssModule（`main/modules/gnss/`）
- `handle_frame`：在 `controller().on_frame()` **之前**记下 `state()==kReadback`，是则调用 `receiver().match_readback()` 取一行，
  追加 `行 + '\n'` 到 `ctx().data.gnss_readback_text`；放不下则不写该行并置 truncated；lines 只要命中就 +1。
- 子命令 `GnssSubVerb::kReadback`：`request_readback()` 成功 → 清空 len/lines/truncated 本地累计；失败时立即发布一次
  `GnssReadback{seq+1, 0, 0, supported, false, busy}`：驱动 `build_readback` 失败 → supported=false；否则（状态不对）→
  supported=true、busy=true。
- 检测到 `controller().stats().readback_seq` 变化（kReadback→kRunning）时一次 `gnss_readback.write()` 发布结论（supported=true）。
- 日志（ADR-0009 §2.12，标签 `<cfg>`）：开始 `FW_LOGI(kGnss, "<cfg> TX \"CONFIG\"")`；结束 `FW_LOGI(kGnss, "<cfg> Readback done, %u lines%s", ...)`；
  每行 D 级 `"<cfg> RX %s"`。不新增 TraceTag 码（若必须新增，在 GnssSubTrace 末尾追加并补 TraceText 文本）。

### 1.5 控制台 `gnss config`（`main/modules/gnss/GnssCmds.cpp`）
- 参照 `cmd_gnss_out` 与 `submit_gnss_and_wait`：记下当前 `gnss_readback` seq → 投递 `kReadback` → 轮询等 seq 变化（≤ 6000 ms）。
- 结论：`!supported` → `ERR not supported`；`busy` → `ERR busy`；否则逐字节打印 text（按 len），
  再打印 `lines=<n>`（截断时加 ` truncated`）与 `OK`。
  超时 → `ERR timeout`。
- 登记命令表 + help 文本；控制台输出遵守客户可见极简风格（无调试噪声）。

### 1.6 注释
- 新增/改动的函数按 CODING-STYLE §6.2 写 doxygen（英文标签、中文正文）；头里已有声明注释的，.cpp 定义处不重复。

## 2 测试
`test/gnss`（登记 CMake）：
1. 解码器：`$CONFIG,...*hh\r\n` 接受；坏校验、`$CONFIX`、`$CONF` 截断（kNeedMore）、超 `kResponseMaxFrame`、含 `$` 正文拒收；
   `$command` 原用例全过。
2. `StaticDemux<nmea::kDecoder, unicore::kResponseDecoder>` 同时挂载：喂一段真实形状的 UM980 读回（≥ 20 行 `$CONFIG,...`
   夹 `$GNGGA`、`$command,CONFIG,response: OK*..`），逐字节与 4096 B 一次性两种喂法，`$CONFIG` 全部由 UnicoreResp 交付、
   NMEA 帧由 nmea 交付、`unknown`/丢弃字节为 0；帧跨喂入边界（每种切点）结果一致。
3. 驱动：`build_readback` 字节 == `"CONFIG\r\n"`；`match_readback` 正文提取、`$command` 返回 0、非 UnicoreResp 解码器返回 0、out 过小截断；
   u-blox/ComNav/NullReceiver `build_readback` false。
4. Controller：非 kRunning 拒绝；接受后 poll 写出 CONFIG；每行重置 1 s 静默；无行 1 s 后结束；持续有行 5 s 总上限结束；
   结束后 kRunning、`readback_seq`+1、`readback_lines` 正确；u-blox 驱动拒绝。
5. 若 main_host_check 能覆盖 GnssModule 文本拼接/截断，就加一例；不能则在报告里说明由 HIL 覆盖。

## 3 不做
- 不做读回结果与期望配置的自动比对/修复（ADR-0012 §2.7 第一阶段）。
- 不改 u-blox / ComNav 读回；不改 NmeaParser；不改黄金向量 `.inc` 期望字节（可新增向量）。

## 4 验收门禁（输出摘要贴进报告）
1. `test/gnss`：默认与全部驱动开启 × 普通 / ASan+UBSan（`-fno-sanitize=vptr`）全部通过。
2. `test/demux`（若存在）、`tools/main_host_check`、`tools/t13_host_check`、`test/services` 普通 + ASan 通过。
3. `. ~/esp/esp-idf/export.sh && idf.py -B builds/p3c build` 0 warning；记录 bin 大小与基线差值。
4. `sha256sum -c docs/frozen.sha256`：只允许 `UnicoreReceiver.hpp`（若新增 protected 声明）失败，其余全部 OK。
5. `clang-format --style=file --dry-run --Werror` 改动文件通过。
6. `python3 tools/doc_check/doc_check.py --base HEAD` 0 不合格。

## 5 交付
- 报告 `docs/reports/P3c.md`：改动清单、新增用例、门禁输出、bin 差值、HIL 待验项（UM980 `gnss config` 列出全部行、无 `unknown bytes dropped`）。
- 不要 commit，由 Claude 审查后提交。不要烧录/动硬件。
