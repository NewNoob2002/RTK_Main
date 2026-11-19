# N1 设备名按产品：`<product>-<SN>`

依据：用户 2026-11-19 要求（BT/Wi-Fi 名不再用 E1-Lite，固件已识别产品）；ADR-0015 §板型档案。基线：main 56d022c。实现：AGY（Gemini 4 Neon）；Architect（Claude）验收、更新冻结文档与 AGENTS.md、提交。

## 1 目标
1. `app::board::Profile` 增加 `const char *product`（P2 v1.41 → `"P2"`，E1 DevKit → `"E1"`，unknown → `"RTK"`）。
2. `main/modules/system/SystemModule.cpp` `buildDeviceName()` 输出 `<product>-<SN>`；SN 未写 NVS 时仍用 BT MAC 后 2 字节 `%02X%02X`（现有 `deriveSnFromMac`）。长度仍受 `kDeviceNameMax` 限制，超长返回 false。
3. 手机协议设备信息的 product 字段（`components/link_core/services/DeviceService.cpp:214` `"E1-Lite"`）改为由 main 注入的产品名（不得让 link_core 依赖 main/board；用已有 setter 或新增 setter），board 字段同理改为当前 GNSS 型号或保持现状并在报告说明。
4. Wi-Fi SoftAP SSID 沿用设备名（ADR-0017，无需改代码，仅确认 W2 取同一字符串）。
5. `tools/web_mock/web_mock.py` 示例名改为 `P2-A1B2`。
6. host 测试：BootStepsCheck/SystemModule 相关 main_host_check 覆盖 P2/E1/unknown 三种名字及 SN 为空、MAC 回退、超长。
7. 不改 docs/**（冻结）与 AGENTS.md；需改的文字列在报告 §文档待改，由 Architect 改。

## 2 测试
`tools/ci/run_gates.sh --base 56d022c --prefix n1` 全部 PASS；HIL 由 Architect（P2 显示 `P2-B5F2`）。

## 3 交付
报告 docs/reports/N1.md；不提交；打印 `N1 DONE`。
