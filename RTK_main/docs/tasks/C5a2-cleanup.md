# C5a2 C5a 遗留清理

依据：`docs/reports/C5a.md` 末尾「Architect 验收」的遗留项；ADR-0013（长度/复杂度）、ADR-0009 §2.12（日志风格）、ADR-0014 §2.6。
基线：`2f78b0e`。实施者：agy（gemini-4-hydrogen-high）；Architect（Claude）验收。只做下列 6 项，行为除第 1、5 项外不变。

## 1 `gnss out rtcm` 按接收机族显示

- 1033 只对 Unicore 下发，1230 只对 u-blox 下发（`GnssBaseConfig.cpp` kSlot1033/kSlot1230）。`gnss out rtcm`（及 `gnss base` 中的 RTCM 列表）只显示当前已探测接收机族会下发的那条；未探测到接收机时两条都显示并标注适用族（如 `1033 (Unicore)`）。
- 对不适用的报文执行 `gnss out rtcm 1230 <hz>`（在 Unicore 上）返回 `ERR 1230 is not supported by this receiver`，不写 NVS。
- NVS 布局与缺省值不变。

## 2 gnss.base 保存移到 kIo

- `GnssCmdsMode.cpp` `persistBaseKeys` 目前在 console 线程里直接 `writeBaseConfig` + `storage.saveKeys`。改为与 `gnss out`/`gnss rate`（`GnssModuleCmds.cpp:161/207`）一致：console 打包命令 → `Executor::call` → GnssModule 在 kIo 写内存并 `saveKeys`，结果经 `CommandResult` 返回。新命令字接在 `GnssSubVerb::kMode = 9` 之后，并扩展 `GnssModuleCmds.cpp` 已有的 static_assert，保证与 `GnssMaintenanceVerb` 不重叠。
- 参数编码放进 `fw::Command::args`；超出 args 容量时说明分包方式（例如按字段分别提交）。

## 3 gnss.base load/save 表驱动

- `GnssModuleMode.cpp:140-500` 的 20 个 `loadBase*`/`saveBase*` 结构几乎一致。改成一张描述表（字段指针/偏移、宽度、范围、缺省），保留 Storage 回调签名不变，由 1–2 个通用函数 + 表实现。NVS 键名、编码字节、范围校验、无效值回退缺省的行为必须逐字节不变（用现有主机测试 + 新增往返测试证明）。

## 4 BaseRtcmRing 分段拷贝

- `main/data/BaseRtcmRing.hpp:58`、`:90` 在锁内逐字节拷贝。改为最多两段 `std::memcpy`（处理回绕），锁持有时间缩短；语义（整帧写入、满则整帧丢弃并计数）不变。主机测试覆盖回绕边界（写到末尾跨界、读跨界、恰好满）。

## 5 探测日志二进制显示

- `GnssModuleProbe.cpp:309-330` 把下发命令原文按 `%.*s` 打印；u-blox UBX 是二进制，日志出现乱码。改为：全部可打印 ASCII 时保持现有 `TX "..."`；否则打印十六进制，例 `<probe> TX UBX 0x06 0x8A, 9 bytes: B5 62 06 8A ...`（最多前 16 字节，超出加 `...`）。遵守 ADR-0009 §2.12 句子风格。
- 主机测试或 golden 覆盖一条 ASCII、一条二进制。

## 6 命令入口测试

- C5a 的撞号 bug 是因为主机测试绕过了 `GnssModule::onCommand` 分流。新增主机测试：用 console 侧打包函数生成的 `fw::Command` 经 `onCommand`（或抽出的纯分流函数）分派，断言 `gnss mode base`、`gnss save`、`gnss freset`、`gnss baud`、`gnss out`、`gnss config` 及第 2 项新命令各自落到正确处理分支。

## 7 不做

survey-in 实际完成、基站 RTCM 经蓝牙到手机的外场测试（已在 `docs/reports/C5a.md` 列为待外场 HIL）；smartBase（C5b）。

## 8 门禁与交付

- idf `builds/c5a2` 0 warning；全部主机套件普通 + ASan（main_host_check、t13、services、app_fw、host、gnss 默认与 all-drivers、nvs_store）；G5b tidy（含 `--nvs-build`、`--test-app-build builds/q4-rtk_pure-esp32`）0；G7b `--build builds/c5a2`；clang-format；doc_check `--base 2f78b0e` 0；golden 除第 5 项外不变；新增/改动函数 ≤60 行、认知复杂度 ≤25；frozen 不应变化（不要重算 frozen.sha256）。
- 禁止：`uvx`/系统 pip、堆分配、修改无关代码、git 提交。
- 报告 `docs/reports/C5a2.md`：每项改动、取舍、门禁实测摘要、bin 大小变化。最后一行单独打印 `C5a2 DONE` 并停止。
