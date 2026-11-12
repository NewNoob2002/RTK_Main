# C++ v2 草稿（已被 ADR-0003 取代）

- `spp_server.hpp`、`settings_store.hpp`：ADR-0002 的冻结接口。ADR-0003 把 SPP 驱动与 NVS 改为 C 实现，这两个头作废。
- `spp_server.cpp`：DeepSeek 在 BATCH-D/T13 中途被停下时的未完成草稿（611 行，未经验收）。
- 只读参考，不参与构建，不得修改。
