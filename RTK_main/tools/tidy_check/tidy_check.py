#!/usr/bin/env python3
"""G5b clang-tidy 门禁：覆盖全部自有源文件（components/**、main/**）与测试工程。

规则来源（2026-11-11 用户授权修订，2026-11-17 Q3 扩到全部自有源）：
- C++ 文件：根 .clang-tidy，按目录继承 main/、components/spp_server|rtos|cli/、test/host/ 下的 .clang-tidy；
- C 文件：.clang-tidy-c（--config-file 会屏蔽目录配置，因此 C 侧的分区排除项在本脚本 C_EXTRA_CHECKS 中给出，
  与对应目录 .clang-tidy 的理由一致）。
ESP 侧翻译单元用 idf.py 生成的 compile_commands.json，按 .clangd 的做法去掉 clang 不认识的 xtensa GCC
选项、补齐工具链系统头，生成临时编译数据库后解析。本机 clang 必须支持 xtensa 目标。
gnss 是纯组件，IDF 只启用树内默认驱动，ComNav 不在 ESP 数据库里；因此 src/**/*.cpp 用 test/gnss
全驱动构建的编译数据库（host 语言标准与配置），保证每个驱动都被分析到。
纯组件（device_proto/link_core/app_fw/bt_reconnect/stream_demux）若在 host 数据库里已有条目就用它
（同一份源、同一配置、能完整解析），否则回落到 ESP 数据库。
覆盖率自检：components/**、main/** 和 test/nvs_store/** 下全部 .c/.cpp 必须有 job（排除组件内 test/ 与生成物），
或登记在 EXCLUDED 表（附理由），否则退出码 2。

用法（先 . ~/esp/esp-idf/export.sh，并已构建 builds/host、idf.py build 与 test/gnss）：
  python3 -I tools/tidy_check/tidy_check.py [--host-build builds/host] [--idf-build build]
      [--gnss-build builds/gnss] [--nvs-build builds/qa-nvs-store]
      [--test-app-build builds/rtk_pure-esp32] [--jobs N]
退出码：0 PASS；1 有发现；2 工具或输入错误（含覆盖率不全）。
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

# 覆盖率自检的显式排除项：路径 -> 理由。只放确实不参与任何构建的自有源文件。
EXCLUDED = {
    "main/modules/_template/TemplateCmds.cpp":
        "模块模板：module_list.inc 未注册，ESP 与 host 编译数据库均无条目",
    "main/modules/_template/TemplateModule.cpp":
        "模块模板：module_list.inc 未注册，ESP 与 host 编译数据库均无条目",
}

# 自有源文件根：覆盖率自检与 job 构造共用。
OWNED_ROOTS = ("components", "main", "test/nvs_store")
# 自有源文件中不参与产品构建的子目录段（组件内测试、生成物）。
SKIP_SEGMENTS = ("test", "golden")

# 冻结的 test/host/CMakeLists.txt 不编译 device_proto/DemuxFrame.cpp（该文件由 t13_host_check 目标链接），
# 因此它在 host 编译数据库里没有条目；按 job 构造规则回落到 ESP 数据库（该文件属于 device_proto 组件，
# 在 idf.py 构建中有条目），不再手工补包含路径。


def sources(*globs):
    out = []
    for g in globs:
        out.extend(sorted(ROOT.glob(g)))
    return out


def owned_sources():
    """列举覆盖根下的 C/C++ 文件（组件内 test/ 与生成物除外）。"""
    out = []
    for base in OWNED_ROOTS:
        for p in sorted((ROOT / base).rglob("*")):
            if p.suffix not in (".c", ".cpp") or not p.is_file():
                continue
            if any(part in SKIP_SEGMENTS for part in p.relative_to(ROOT / base).parts[:-1]):
                continue
            out.append(p)
    return out


def db_files(build):
    """读取 CMake 编译数据库中的源文件集合（绝对路径）。"""
    path = Path(build) / "compile_commands.json"
    if not path.is_file():
        raise RuntimeError(f"缺少 {path}")
    return {str(Path(e["file"]).resolve()) for e in json.loads(path.read_text())}


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
    ap.add_argument("--gnss-build", default="builds/gnss",
                    help="test/gnss 全驱动构建目录（需要 compile_commands.json）")
    ap.add_argument("--nvs-build", default="builds/qa-nvs-store",
                    help="test/nvs_store 主机构建目录（需要 compile_commands.json）")
    ap.add_argument("--test-app-build", default="builds/rtk_pure-esp32",
                    help="T15 测试应用的 esp32 构建目录；不存在时跳过并提示")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    if shutil.which("clang-tidy") is None:
        print("ERROR: 找不到 clang-tidy", file=sys.stderr)
        return 2
    host_db = ROOT / args.host_build
    gnss_db = ROOT / args.gnss_build
    nvs_db = ROOT / args.nvs_build
    host_cc = host_db / "compile_commands.json"
    if not host_cc.is_file():
        print(f"ERROR: 缺少 {host_db}/compile_commands.json", file=sys.stderr)
        return 2
    try:
        host_files = db_files(host_db)
        gnss_files = db_files(gnss_db)
        nvs_files = db_files(nvs_db)
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

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

        def pick_db(path):
            """gnss 用全驱动 host 数据库；纯组件优先 host 数据库，缺失时用 ESP 数据库。"""
            if str(path.resolve()) in gnss_files:
                return gnss_db
            if str(path.resolve()) in host_files:
                return host_db
            return esp_db

        esp_like = {esp_db} if app_db is None else {esp_db, app_db}

        def add(path, is_c, extra_checks=(), db=None):
            if path.relative_to(ROOT).as_posix() in EXCLUDED:
                return
            chosen = db or pick_db(path)
            extra_args = ESP_EXTRA_ARGS if chosen in esp_like else ()
            jobs.append((path, chosen, is_c, extra_checks or None, extra_args))

        # host 测试与纯组件：host 数据库条目优先（同一份源、同一配置、能完整解析）。
        for p in sources("test/host/*.cpp", "test/host/*.c"):
            add(p, p.suffix == ".c", C_EXTRA_CHECKS["host_test"] if p.suffix == ".c" else (),
                db=host_db if str(p.resolve()) in host_files else esp_db)
        for p in sources("test/nvs_store/**/*.c", "test/nvs_store/**/*.cpp"):
            add(p, p.suffix == ".c", C_EXTRA_CHECKS["host_test"] if p.suffix == ".c" else (),
                db=nvs_db)
        for p in sources("components/device_proto/*.cpp", "components/device_proto/*.c",
                         "components/link_core/*.cpp", "components/link_core/services/*.cpp",
                         "components/app_fw/src/*.cpp", "components/bt_reconnect/src/*.cpp",
                         "components/web_proto/src/*.cpp",
                         "components/stream_demux/src/**/*.cpp"):
            add(p, p.suffix == ".c")
        # gnss：test/gnss 全驱动数据库，包含 IDF 未启用的 ComNav。
        for p in sources("components/gnss/src/**/*.cpp"):
            add(p, False, db=gnss_db)
        # ESP 侧其余自有源：main（递归）、cli、spp_server、rtos、nvs_store。
        for p in sources("main/**/*.cpp", "components/cli/src/*.cpp",
                         "components/spp_server/*.cpp", "components/rtos/*.cpp"):
            add(p, False, db=esp_db)
        for p in sources("components/spp_server/*.c", "components/nvs_store/*.c"):
            add(p, True, C_EXTRA_CHECKS["esp"], db=esp_db)
        if app_db is not None:
            for p in sources("test_apps/rtk_pure/main/*.cpp"):
                add(p, False, db=app_db)
            for p in sources("test_apps/rtk_pure/main/*.c"):
                add(p, True, C_EXTRA_CHECKS["app_test"], db=app_db)

        files_by_db = {host_db: host_files, gnss_db: gnss_files, nvs_db: nvs_files, esp_db: esp_files,
                       app_db: app_files}
        missing = [str(j[0].relative_to(ROOT)) for j in jobs
                   if str(j[0].resolve()) not in files_by_db[j[1]]]
        if missing:
            print(f"ERROR: 编译数据库中缺少：{', '.join(missing)}", file=sys.stderr)
            return 2

        covered = {str(j[0].resolve()) for j in jobs}
        uncovered = []
        for p in owned_sources():
            rel = p.relative_to(ROOT).as_posix()
            if str(p.resolve()) in covered:
                continue
            if rel in EXCLUDED:
                print(f"NOTE: 排除 {rel}：{EXCLUDED[rel]}")
                continue
            uncovered.append(rel)
        if uncovered:
            print(f"ERROR: tidy 覆盖不全（未纳入且无排除理由）：{', '.join(uncovered)}", file=sys.stderr)
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
