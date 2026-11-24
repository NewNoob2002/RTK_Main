# O2：网页“固件升级”卡片与离线 mock

- 依据：**`docs/adr/ADR-0019-web-ota.md` §2.3（HTTP 契约，字节级不得改）、§2.8（页面要求）**；ADR-0017 §2.4（页面风格与 24 KB gzip 预算）；`AGENTS.md`。
- 基线：分支 `feat/ota-base`。分支 `feat/o2-ota-ui`，工作树 `../RTK_Main-o2`（仓库根）。
- 执行者：AGY。评审：Codex。执行者不提交、不烧录、不连串口。
- 只改：`main/web/ui/**`、`tools/web_mock/**`、报告 `docs/reports/O2-ota-ui.md`。不改固件 C/C++ 代码（O1 负责）。

## 1 范围

1. **页面**（`main/web/ui/app.js`、`app.css`，必要时 `index.html`），沿用现有风格与 `h()`/`confirmBtn()` 等辅助函数：
   - 新卡片“固件升级”，放在系统分组附近；进入页面时 `GET /ota/info`，显示当前版本、运行分区、状态、另一槽版本（`null` 显示“无”）。
     `/ota/info` 失败（如旧固件 404）时卡片显示“设备不支持网页升级”，不影响其他卡片。
   - 选择文件（`accept=".bin"`）后用 `FileReader`/`Blob.slice(0, 288)` 在浏览器端解析：偏移 0 字节 = `0xE9`；偏移 32 起 `esp_app_desc_t`：
     `magic_word` u32 小端 = `0xABCD5432`、`version` = 偏移 48 起 32 B、`project_name` = 偏移 80 起 32 B（均以 `\0` 截断）。
     magic 不符或 `project_name` ≠ `/ota/info.project` → 提示“不是本设备固件”，不允许上传；文件大小 > `max` → 提示过大。
   - 显示“当前 X → 新 Y”；Y 与 X 相同提示“版本相同”，字符串比较无法判断新旧，不做降级判断，只在确认框里写明“将替换当前固件”。
     确认后 `XMLHttpRequest` `POST /ota`，`Content-Type: application/octet-stream`，体 = 文件本身；`upload.onprogress` 显示百分比进度条。
   - 上传期间禁用其他设置控件与本卡按钮（WS 断开重连逻辑不得在此期间弹“退出”类提示）。
   - 应答：`ok:true` → “升级完成，设备将在约 N 秒后重启进入蓝牙模式，热点会断开”；失败按 `err` 显示中文：
     `busy` 设备忙/正在关机，`state` 新固件尚未确认（启动后约 30 s 再试），`length`/`too_large`/`bad_image`/`validate`/`timeout`/`flash`/`aborted`
     各一句；网络错误（XHR `onerror`）显示“连接中断，设备仍运行原固件”。
2. **`tools/web_mock`**：实现 `GET /ota/info` 与 `POST /ota`（按 §2.3 返回），命令行参数注入：`--ota-err <码>`（返回指定错误）、
   `--ota-slow-kbps <n>`（限速读取以观察进度条）、`--ota-max <字节>`；同样校验 magic/project_name；成功后 mock 模拟“重启”断开 WS。
3. 预算：`python3 -I tools/web_mock/pack.py` gzip ≤ 24 KB（现 13.7 KB），报告新大小。

## 2 不做

- 固件端任何代码、`main/CMakeLists.txt`、HTTP 契约改动（发现契约问题停下写进报告）。

## 3 测试（执行者）

- `python3 -I tools/web_mock/pack.py` 通过；用 mock 在浏览器（或 `curl` + 无头脚本）走通：正常升级、每种 `err`、非本工程文件、超大文件、慢速进度。
- 若 `tools/web_mock` 已有自测，补 OTA 用例并全过；`python3 -I -m py_compile tools/web_mock/*.py`。
- 报告 `docs/reports/O2-ota-ui.md`：改动文件、gzip 大小、验证步骤与结果（PASS/FAIL/NOT_RUN）、截图描述或文字说明、偏离与未决问题。
