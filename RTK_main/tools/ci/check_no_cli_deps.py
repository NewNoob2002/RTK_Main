#!/usr/bin/env python3
"""Verify NONE + Wi-Fi build links web/phone/settings without CLI/REPL dependencies (ADR-0018 SV3).

Checks:
1. sdkconfig enables CONFIG_RTK_CONSOLE_PROFILE_NONE=y and CONFIG_RTK_WIFI_CONFIG_ENABLE=y.
2. Object files for web, phone, and settings have zero undefined references to cli/console symbols.
3. Final ELF links WebModule, PhoneCompatService, and settings::Service, and excludes REPL/console
   command tables and esp_console/linenoise symbols.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


FORBIDDEN_OBJ_RE = re.compile(
    r"(?:^|[^A-Za-z0-9_])("
    r"cli::|"
    r"cli::outPrintf\(|"
    r"esp_console_|"
    r"linenoise|"
    r"app::initConsole|"
    r"app::runConsole|"
    r"app::gatedCommand|"
    r"app::submitSetting\(|"
    r"app::settingsErrorText\(|"
    r"app::[a-zA-Z0-9_]*Cmds\("
    r")"
)

REQUIRED_ELF_PATTERNS = [
    ("WebModule", re.compile(r"\bapp::WebModule::poll\(")),
    ("PhoneCompatService", re.compile(r"\bcore::services::PhoneCompatService::table\(")),
    ("settings::Service", re.compile(r"\bsettings::Service::set\(")),
]

FORBIDDEN_ELF_RE = re.compile(
    r"(?:^|[^A-Za-z0-9_])("
    r"cli::init\(|"
    r"cli::run\(|"
    r"cli::add\(|"
    r"cli::addVerbs\(|"
    r"cli::dispatch\(|"
    r"cli::fail\(|"
    r"cli::outPrintf\(|"
    r"esp_console_init\b|"
    r"esp_console_cmd_register\b|"
    r"linenoise\b|"
    r"app::initConsole\(|"
    r"app::runConsole\(|"
    r"app::gatedCommand\(|"
    r"app::submitSetting\(|"
    r"app::settingsErrorText\(|"
    r"app::configCmds\(|"
    r"app::gnssCmds\(|"
    r"app::factoryCmds\(|"
    r"app::linkCmds\(|"
    r"app::systemCmds\(|"
    r"app::rtcmCmds\(|"
    r"app::debugCmds\(|"
    r"app::factoryResetCmds\(|"
    r"::kConfigCmds\b|"
    r"::kGnssCmds\b|"
    r"::kGnssVerbs\b|"
    r"::kFactoryCmds\b|"
    r"::kFactoryVerbs\b|"
    r"::kLinkCmds\b|"
    r"::kBtVerbs\b|"
    r"::kSystemCmds\b|"
    r"::kStatusVerbs\b|"
    r"::kRtcmCmds\b|"
    r"::kDebugCmds\b|"
    r"::kRadioVerbs\b|"
    r"::kFactoryResetCmds\b"
    r")"
)


def find_nm(explicit: str | None) -> str:
    if explicit:
        return explicit
    for cand in ("xtensa-esp32-elf-nm", "nm"):
        found = shutil.which(cand)
        if found:
            return found
    tools = sorted(glob.glob(os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32-elf-nm")))
    if tools:
        return tools[-1]
    raise RuntimeError("xtensa-esp32-elf-nm not found in PATH or ~/.espressif/tools")


def run_nm(nm_bin: str, args: list[str]) -> str:
    proc = subprocess.run([nm_bin, *args], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"nm failed ({proc.returncode}): {proc.stderr.strip()}")
    return proc.stdout


def check_sdkconfig(build_dir: Path) -> list[str]:
    errors: list[str] = []
    sdkconfig = build_dir / "sdkconfig"
    if not sdkconfig.is_file():
        return [f"missing sdkconfig: {sdkconfig}"]
    text = sdkconfig.read_text(encoding="utf-8", errors="replace")
    for flag in ("CONFIG_RTK_CONSOLE_PROFILE_NONE=y", "CONFIG_RTK_WIFI_CONFIG_ENABLE=y"):
        if f"\n{flag}\n" not in f"\n{text}\n":
            errors.append(f"sdkconfig missing {flag}")
    return errors


def collect_group_objs(build_dir: Path) -> tuple[dict[str, list[Path]], list[str]]:
    idf_dir = build_dir / "esp-idf"
    missing: list[str] = []

    bt_link_dir = idf_dir / "main" / "CMakeFiles" / "__idf_main.dir" / "modules" / "bt_link"
    phone_explicit = [
        bt_link_dir / name
        for name in (
            "BtLinkModule.cpp.obj",
            "BtLinkSessions.cpp.obj",
            "BtLinkGnss.cpp.obj",
            "BtPairing.cpp.obj",
        )
    ]
    phone_objs: list[Path] = []
    for path in phone_explicit:
        if path.is_file():
            phone_objs.append(path)
        else:
            missing.append(f"[phone] missing required object file: {path}")

    backend_obj = (
        idf_dir / "main" / "CMakeFiles" / "__idf_main.dir" / "app" / "SettingsBackend.cpp.obj"
    )
    settings_objs = sorted((idf_dir / "settings").rglob("*.obj"))
    if backend_obj.is_file():
        settings_objs.append(backend_obj)
    else:
        missing.append(f"[settings] missing required object file: {backend_obj}")

    groups = {
        "web": (
            sorted((idf_dir / "main" / "CMakeFiles" / "__idf_main.dir" / "modules" / "web").glob("*.obj"))
            + sorted((idf_dir / "web_proto").rglob("*.obj"))
        ),
        "phone": phone_objs + sorted((idf_dir / "link_core").rglob("*.obj")),
        "settings": settings_objs,
    }
    return groups, missing


def check_objects(nm_bin: str, build_dir: Path) -> list[str]:
    groups, errors = collect_group_objs(build_dir)
    for group, objs in groups.items():
        if not objs:
            errors.append(f"no object files found for group '{group}' under {build_dir}")
            continue
        out = run_nm(nm_bin, ["-C", "-u", *[str(p) for p in objs]])
        cur_file = str(objs[0])
        for line in out.splitlines():
            if line.endswith(":"):
                cur_file = line[:-1]
                continue
            if FORBIDDEN_OBJ_RE.search(line):
                errors.append(f"[{group}] forbidden CLI/console undefined symbol in {cur_file}: {line.strip()}")
    return errors


def check_elf(nm_bin: str, build_dir: Path) -> list[str]:
    errors: list[str] = []
    elf = build_dir / "rtk_main.elf"
    if not elf.is_file():
        return [f"missing ELF: {elf}"]
    out = run_nm(nm_bin, ["-C", "--defined-only", str(elf)])
    for label, pat in REQUIRED_ELF_PATTERNS:
        if not pat.search(out):
            errors.append(f"ELF missing required {label} symbol ({pat.pattern})")
    for line in out.splitlines():
        if FORBIDDEN_ELF_RE.search(line):
            errors.append(f"ELF linked forbidden REPL/console symbol: {line.strip()}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Check NONE+Wi-Fi build has no CLI/REPL dependencies in web/phone/settings.")
    parser.add_argument("--build", required=True, help="Path to NONE+Wi-Fi IDF build directory")
    parser.add_argument("--nm", default=None, help="Optional path to xtensa-esp32-elf-nm")
    args = parser.parse_args()

    build_dir = Path(args.build)
    try:
        nm_bin = find_nm(args.nm)
    except RuntimeError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    errors = []
    errors.extend(check_sdkconfig(build_dir))
    errors.extend(check_objects(nm_bin, build_dir))
    errors.extend(check_elf(nm_bin, build_dir))

    if errors:
        for err in errors:
            print(f"FAIL: {err}", file=sys.stderr)
        return 1

    print(f"PASS: NONE+Wi-Fi build ({build_dir}) links web/phone/settings cleanly without CLI/REPL dependencies")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
