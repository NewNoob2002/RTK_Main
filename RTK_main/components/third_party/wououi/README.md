# WouoUI-PageVersion (Vendored Subset)

- **Upstream**: https://github.com/Sheep118/WouoUI-PageVersion
- **Commit**: `82b985a` (`修正 CMakeLists.txt 文件中的路径注释`)
- **License**: Mozilla Public License Version 2.0 (see [LICENSE](LICENSE))
- **Memory model**: Pure C, static storage only (verified zero `malloc`/`calloc`/`realloc`/`free` calls across `Csource/` and enforced by `tools/g7b_check/g7b_check.py`).

## Included Files (OL1 Minimal Subset)

- `LICENSE` — Verbatim MPL-2.0 license text from upstream.
- `WouoUI_common.h` — Unmodified from upstream `Csource/WouoUI_common.h`.
- `WouoUI_font.h`, `WouoUI_font.c` — Unmodified from upstream `Csource/`.
- `WouoUI_graph.h`, `WouoUI_graph.c` — Unmodified from upstream `Csource/`.
- `WouoUI_conf.h` — Derived from upstream `Csource/WouoUI_conf.h` with minimal marked modifications:
  1. Disabled `WOUOUI_LOG_ENABLE` (prevents `printf` dependency on real-time/firmware builds).
  2. Set `SOFTWARE_DYNAMIC_REFRESH` to `0` (`HARDWARE_DYNAMIC_REFRESH` remains `0`; dirty-page detection is handled via per-page 64-bit hashes in the SH1106 driver without a second 1 KB shadow framebuffer).

Unused page/window state machines (`WouoUI.c`, `WouoUI_page.c`, `WouoUI_win.c`, `WouoUI_anim.c`, `WouoUI_msg.c`, `WouoUI_user.c`) are omitted in OL1 per ADR-0020.
