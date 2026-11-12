# R2 全部 main 源码与主机回归的 tidy 基线对比脚本。
# 仅 host 离线运行；先运行 R2-check.py 并配置 builds/r2-base-mh，不修改固件或检查规则。
import concurrent.futures
import importlib.util
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("tidy_check", ROOT / "tools/tidy_check/tidy_check.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载现有 tidy_check 工具")
TIDY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TIDY)
PATTERN = re.compile(r"^(.*?):\d+:\d+: (?:error|warning): (.*)$")
# 原规则写死 RTK_main 路径，无法涵盖快照目录；两边统一扩大头文件范围，不减少检查。
HEADER_FILTER = r".*/(components|main|test)/.*\.(hpp|hh|hxx)$"


def run(job):
    label, source_root, source, db = job
    command = ["clang-tidy", str(source), "-p", str(db), f"--header-filter={HEADER_FILTER}"]
    command += [f"--extra-arg={arg}" for arg in TIDY.ESP_EXTRA_ARGS]
    proc = subprocess.run(command, cwd=source_root, capture_output=True, text=True, check=False)
    findings = []
    for line in (proc.stdout + proc.stderr).splitlines():
        match = PATTERN.match(line)
        if match:
            path = Path(match[1])
            if not path.is_absolute():
                path = source_root / path
            if path.resolve().is_relative_to(source_root):
                findings.append((str(path.resolve().relative_to(source_root)), match[2]))
    if proc.returncode != 0 and not findings:
        raise RuntimeError(f"{source}: clang-tidy 失败且没有可比对的项目诊断\n{proc.stderr}")
    return label, source.relative_to(source_root), findings, proc.returncode


jobs = []
for label, source_root, db, host_db in [
    ("BEFORE", ROOT / "builds/r2-base-src", ROOT / "builds/r2-base-clang-db", ROOT / "builds/r2-base-mh"),
    ("AFTER", ROOT, ROOT / "builds/r2-clang-db", ROOT / "builds/r2-mh"),
]:
    jobs += [(label, source_root, source, db) for source in sorted((source_root / "main").rglob("*.cpp"))]
    jobs.append((label, source_root, source_root / "tools/main_host_check/main_host_check.cpp", host_db))
sets = {label: set() for label in ["BEFORE", "AFTER"]}
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
    for label, source, findings, code in pool.map(run, jobs):
        print(label, source, "findings=", len(findings), "exit=", code)
        sets[label].update(findings)
for label in sets:
    print(label, "UNIQUE=", len(sets[label]))
new = sets["AFTER"] - sets["BEFORE"]
for finding in sorted(new):
    print("NEW", finding)
print("NEW_UNIQUE=", len(new))
for finding in sorted(sets["AFTER"]):
    print("EXISTING" if finding in sets["BEFORE"] else "NEW", finding)
raise SystemExit(1 if new else 0)
