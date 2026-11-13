#!/usr/bin/env python3
"""G5b clang-tidy 门禁：覆盖纯组件、host 测试与 ESP-IDF 依赖代码。

规则来源（2026-11-11 用户授权修订）：
- C++ 文件：根 .clang-tidy，按目录继承 main/、components/spp_server|rtos/、test/host/ 下的 .clang-tidy；
- C 文件：.clang-tidy-c（--config-file 会屏蔽目录配置，因此 C 侧的分区排除项在本脚本 C_EXTRA_CHECKS 中给出，
  与对应目录 .clang-tidy 的理由一致）。
ESP 侧翻译单元用 idf.py 生成的 compile_commands.json，按 .clangd 的做法去掉 clang 不认识的 xtensa GCC
选项、补齐工具链系统头，生成临时编译数据库后解析。本机 clang 必须支持 xtensa 目标。

用法（先 . ~/esp/esp-idf/export.sh，并已构建 builds/host 与 idf.py build）：
  python3 -I tools/tidy_check/tidy_check.py [--host-build builds/host] [--idf-build build]
      [--test-app-build builds/rtk_pure-esp32] [--jobs N]
退出码：0 PASS；1 有发现；2 工具或输入错误。
"""

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# 与 .clangd 中 ESP 段的 Remove 列表一致：xtensa GCC 专有、clang 不认识的选项。
# -mlongcalls 来自 IDF 的 @toolchain/{c,cxx}flags 响应文件，因此先展开响应文件再过滤。
GCC_ONLY_FLAGS = ("-mno-target-align", "-fstrict-volatile-bitfields", "-fno-tree-switch-conversion",
                  "-fzero-init-padding-bits", "-fno-malloc-dce", "-mlongcalls", "-freorder-blocks", "-fno-shrink-wrap",
                  "-specs=")
# IDF esp_libc 的 sys/reent.h 以 -I（非 -isystem）引入，其空结构体触发 clang 的 C/C++ 兼容诊断，与自有代码无关。
# IDF 组件 CMake 还会加入 GCC 专有告警（如 -Wno-old-style-declaration），clang 对未知告警选项报错，一并忽略。
ESP_EXTRA_ARGS = ("-Wno-extern-c-compat", "-Wno-unknown-warning-option")

# C 文件的分区排除项（理由见 test/host/.clang-tidy 与 main/.clang-tidy）。
C_EXTRA_CHECKS = {
    "host_test": "-readability-magic-numbers,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling",
    # ESP 侧 C 驱动：IDF 宏中的有符号位运算；insecureAPI 要求 C11 Annex K（memcpy_s），picolibc 不提供。
    "esp": "-bugprone-signed-bitwise,-hicpp-signed-bitwise,"
           "-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling",
    # 目标端测试应用的 C 文件：两者之和（理由见 test_apps/rtk_pure/.clang-tidy）。
    "app_test": "-readability-magic-numbers,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,"
                "-bugprone-signed-bitwise,-hicpp-signed-bitwise",
}

FINDING_RE = re.compile(r"^(.+?):(\d+):(\d+): (warning|error): (.*)$")

# 冻结的 test/host/CMakeLists.txt 不编译 device_proto/demux_frame.cpp（该文件由 t13_host_check 目标链接），
# 因此它在 host 编译数据库里没有条目，clang-tidy 会退化成无参数解析并误报 demux/decoder.hpp 找不到。
# 这里按 host_pure 的实际包含路径与语言标准补齐；用绝对路径，因为数据库的 directory 是构建目录。
HOST_FALLBACK_INCLUDES = ("app_fw", "device_proto", "link_core", "stream_demux")
HOST_FALLBACK_ARGS = ("-std=gnu++20",
                     *(f"-I{ROOT / 'components' / name / 'include'}" for name in HOST_FALLBACK_INCLUDES))


def sources(*globs):
    out = []
    for g in globs:
        out.extend(sorted(ROOT.glob(g)))
    return out


def toolchain_isystem(compiler):
    """由编译器路径推出 xtensa 工具链系统头目录（与 .clangd 中的 -isystem 列表同构）。"""
    root = Path(compiler).resolve().parent.parent
    gcc_dirs = sorted((root / "lib" / "gcc" / "xtensa-esp-elf").glob("*"))
    cxx_dirs = sorted((root / "xtensa-esp-elf" / "include" / "c++").glob("*"))
    if not gcc_dirs or not cxx_dirs:
        raise RuntimeError(f"无法从 {compiler} 推出工具链头文件目录")
    gcc, cxx = gcc_dirs[-1], cxx_dirs[-1]
    dirs = [root / "picolibc" / "include", cxx, cxx / "xtensa-esp-elf" / "esp32", cxx / "backward",
            gcc / "include", gcc / "include-fixed", root / "xtensa-esp-elf" / "include"]
    return [f"-isystem{d}" for d in dirs if d.is_dir()]


def expand_response_files(args):
    """展开 GCC 风格的 @file 参数（IDF 用它传工具链选项）；clang-tidy 不对其中的选项做过滤。"""
    out = []
    for a in args:
        rsp = Path(a[1:].strip('"')) if a.startswith("@") else None
        if rsp is not None and rsp.is_file():
            out.extend(shlex.split(rsp.read_text()))
        else:
            out.append(a)
    return out


def make_esp_db(build_dir, out_dir):
    src = Path(build_dir) / "compile_commands.json"
    if not src.is_file():
        raise RuntimeError(f"缺少 {src}，先执行 idf.py -B {build_dir} build")
    entries = json.loads(src.read_text())
    isystem = None
    for e in entries:
        args = e.pop("arguments", None) or shlex.split(e.pop("command"))
        if isystem is None:
            isystem = toolchain_isystem(shutil.which(args[0]) or args[0])
        args = [a for a in expand_response_files(args) if not a.startswith(GCC_ONLY_FLAGS)]
        e["arguments"] = [args[0], "--target=xtensa-esp32-elf", *isystem, *args[1:]]
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    (Path(out_dir) / "compile_commands.json").write_text(json.dumps(entries))
    return {str(Path(e["file"]).resolve()) for e in entries}


def run_one(job):
    path, db, is_c, extra_checks, extra_args = job
    cmd = ["clang-tidy", "-p", str(db), "--quiet"]
    if is_c:
        cmd.append(f"--config-file={ROOT / '.clang-tidy-c'}")
    if extra_checks:
        cmd.append(f"--checks={extra_checks}")
    cmd += [f"--extra-arg={a}" for a in extra_args]
    cmd.append(str(path))
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, check=False)
    findings = []
    for line in (proc.stdout + proc.stderr).splitlines():
        m = FINDING_RE.match(line)
        if m and str(ROOT) in str(Path(m.group(1)).resolve()):
            findings.append(line.replace(str(ROOT) + os.sep, ""))
    # 有 error 级输出但未解析出发现（如编译数据库缺项）时按工具错误处理。
    tool_error = (proc.returncode != 0 and not findings) or "Found compiler error" in proc.stdout + proc.stderr
    return path, sorted(set(findings)), tool_error, proc.stderr[-800:]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host-build", default="builds/host")
    ap.add_argument("--idf-build", default="build")
    ap.add_argument("--test-app-build", default="builds/rtk_pure-esp32",
                    help="T15 测试应用的 esp32 构建目录；不存在时跳过并提示")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    if shutil.which("clang-tidy") is None:
        print("ERROR: 找不到 clang-tidy", file=sys.stderr)
        return 2
    host_db = ROOT / args.host_build
    host_cc = host_db / "compile_commands.json"
    if not host_cc.is_file():
        print(f"ERROR: 缺少 {host_db}/compile_commands.json", file=sys.stderr)
        return 2
    host_files = {str(Path(e["file"]).resolve()) for e in json.loads(host_cc.read_text())}

    jobs = []
    with tempfile.TemporaryDirectory(prefix="rtk-tidy-") as tmp:
        try:
            esp_db = Path(tmp) / "esp"
            esp_files = make_esp_db(ROOT / args.idf_build, esp_db)
            app_db, app_files = None, set()
            app_build = ROOT / args.test_app_build
            if (app_build / "compile_commands.json").is_file():
                app_db = Path(tmp) / "app"
                app_files = make_esp_db(app_build, app_db)
            else:
                print(f"NOTE: 未找到 {app_build}，跳过 test_apps/rtk_pure")
        except RuntimeError as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 2

        for p in sources("components/device_proto/*.cpp", "components/link_core/*.cpp", "test/host/*.cpp"):
            fallback = () if str(p.resolve()) in host_files else HOST_FALLBACK_ARGS
            jobs.append((p, host_db, False, None, fallback))
        for p in sources("components/device_proto/*.c"):
            jobs.append((p, host_db, True, None, ()))
        for p in sources("test/host/*.c"):
            jobs.append((p, host_db, True, C_EXTRA_CHECKS["host_test"], ()))
        for p in sources("main/*.cpp", "components/spp_server/*.cpp", "components/rtos/*.cpp"):
            jobs.append((p, esp_db, False, None, ESP_EXTRA_ARGS))
        for p in sources("components/spp_server/*.c", "components/nvs_store/*.c"):
            jobs.append((p, esp_db, True, C_EXTRA_CHECKS["esp"], ESP_EXTRA_ARGS))
        if app_db is not None:
            for p in sources("test_apps/rtk_pure/main/*.cpp"):
                jobs.append((p, app_db, False, None, ESP_EXTRA_ARGS))
            for p in sources("test_apps/rtk_pure/main/*.c"):
                jobs.append((p, app_db, True, C_EXTRA_CHECKS["app_test"], ESP_EXTRA_ARGS))

        missing = [str(j[0].relative_to(ROOT)) for j in jobs
                   if j[1] in (esp_db, app_db) and str(j[0].resolve()) not in (esp_files | app_files)]
        if missing:
            print(f"ERROR: 编译数据库中缺少：{', '.join(missing)}", file=sys.stderr)
            return 2

        total, rc = 0, 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for path, findings, tool_error, err in pool.map(run_one, jobs):
                rel = path.relative_to(ROOT)
                if tool_error:
                    print(f"ERROR: {rel}: clang-tidy 失败\n{err}", file=sys.stderr)
                    rc = 2
                    continue
                print(f"{len(findings):4d}  {rel}")
                for f in findings:
                    print(f"      {f}")
                total += len(findings)

    print(f"文件 {len(jobs)} 个，发现 {total} 条")
    if rc:
        return rc
    print("G5b PASS" if total == 0 else "G5b FAIL")
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
