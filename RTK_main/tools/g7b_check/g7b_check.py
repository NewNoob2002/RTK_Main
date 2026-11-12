#!/usr/bin/env python3
"""G7b C++ 运行时门禁：自有目标文件零符号 + 全 ELF 精确白名单。

Generated-by: Claude Opus 5.5（Claude Code）
人工负责人：UNKNOWN（待人工审查签字）

背景：IDF nvs_flash 的 nvs_encrypted_partition.cpp 无条件编译，引用
operator new[](size_t, const std::nothrow_t&)，链接器因此从 libstdc++ 拉入
new_opvnt.o → eh_catch.o（2 个异常符号）与 new_opv.o → new_op.o → bad_alloc.o
（4 个 RTTI 符号）。证据见 docs/reports/T14-REVIEW.md「G7b 根因」。
本门禁只放行这 6 个符号，且要求 map 中的拉入链与上述一致；其他任何异常/RTTI
符号、链路变化、自有目标文件中的异常/RTTI/堆符号都判 FAIL。

用法：python3 -I tools/g7b_check/g7b_check.py [--build build] [--nm xtensa-esp32-elf-nm]
退出码：0 PASS；1 FAIL；2 工具或输入错误。
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

# 与 cpp_cost.py 的 exceptions、rtti 类别保持一致（匹配 nm -C 反修饰名）。
EXCEPTIONS_RE = re.compile(r"^(?:__cxa_throw|__cxa_begin_catch|__cxa_end_catch|__cxa_allocate_exception"
                           r"|__gxx_personality_v0|_Unwind_RaiseException|_Unwind_Resume)$")
RTTI_RE = re.compile(r"^typeinfo (?:name )?for ")
# 自有目标文件的堆检查：沿用 acceptance.md G7b 的原始（未反修饰）名模式。
HEAP_RAW_RE = re.compile(r"^(?:_Znw|_Zna|_Zdl|_Zda|malloc$|free$|calloc$|realloc$|_malloc_r$|_free_r$)")

# 全 ELF 白名单：只允许以下 6 个符号存在（可以更少，不能更多）。
ALLOWED_ELF_SYMBOLS = frozenset({
    "__cxa_begin_catch",
    "__cxa_end_catch",
    "typeinfo for std::bad_alloc",
    "typeinfo for std::exception",
    "typeinfo name for std::bad_alloc",
    "typeinfo name for std::exception",
})

# 必须出现在 map「Archive member included」段中的拉入边：成员 -> (引用方后缀, 引用符号)。
# 白名单符号存在时，这条链必须完整成立，否则说明来源已变化，需要重新裁决。
REQUIRED_EDGES = {
    "libstdc++.a(new_opvnt.o)": ("libnvs_flash.a(nvs_encrypted_partition.cpp.obj)", "_ZnajRKSt9nothrow_t"),
    "libstdc++.a(eh_catch.o)": ("libstdc++.a(new_opvnt.o)", "__cxa_begin_catch"),
    "libstdc++.a(new_opv.o)": ("libstdc++.a(new_opvnt.o)", "_Znaj"),
    "libstdc++.a(new_op.o)": ("libstdc++.a(new_opv.o)", "_Znwj"),
    "libstdc++.a(bad_alloc.o)": ("libstdc++.a(new_op.o)", "_ZTVSt9bad_alloc"),
    "libstdc++.a(eh_exception.o)": ("libstdc++.a(bad_alloc.o)", "_ZNSt9exceptionD2Ev"),
}

OWN_COMPONENT_DIRS = ("main", "rtk_proto", "rtk_core", "rtk_os", "rtk_bt", "rtk_store")


def run_nm(nm, args):
    """运行 nm，返回 (类型, 名字) 列表；名字可能含空格（反修饰名）。"""
    proc = subprocess.run([nm, *args], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{nm} {' '.join(args)} 失败：{proc.stderr.strip()}")
    out = []
    for line in proc.stdout.splitlines():
        # 定义符号 "addr T name"；未定义符号 "         U name"。反修饰名可含空格，只切前两段。
        m = re.match(r"^\s*(?:[0-9a-fA-F]+\s+)?([A-Za-z?-])\s+(.+)$", line)
        if m:
            out.append((m.group(1), m.group(2).strip()))
    return out


def is_runtime(name):
    return bool(EXCEPTIONS_RE.match(name) or RTTI_RE.match(name))


def short_member(path):
    """把 map 中的长路径缩成 'libX.a(member)' 或 'file.obj'。"""
    m = re.search(r"([^/\s]+\.a\([^)]+\))$", path)
    return m.group(1) if m else Path(path).name


def parse_archive_edges(map_path):
    """解析 GNU ld map 第一段：成员 -> (引用方, 符号)。"""
    text = map_path.read_text(errors="replace").splitlines()
    if not text or not text[0].startswith("Archive member included"):
        raise RuntimeError(f"{map_path} 不是预期的 GNU ld map")
    edges = {}
    pending = None
    for line in text[1:]:
        if line.startswith(("Allocating common symbols", "Discarded input sections", "Memory Configuration")):
            break
        if not line.strip():
            continue
        ref = re.match(r"^\s+(\S+) \((.+)\)\s*$", line)
        if line[0].isspace() and ref and pending:
            edges[pending] = (short_member(ref.group(1)), ref.group(2))
            pending = None
            continue
        same = re.match(r"^(\S+)\s+(\S+) \((.+)\)\s*$", line)
        if same:
            edges[short_member(same.group(1))] = (short_member(same.group(2)), same.group(3))
            pending = None
        else:
            pending = short_member(line.strip())
    return edges


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build", help="idf.py 构建目录（默认 build）")
    ap.add_argument("--nm", default="xtensa-esp32-elf-nm", help="nm 可执行文件")
    args = ap.parse_args()

    build = Path(args.build)
    elf, map_path = build / "rtk_main.elf", build / "rtk_main.map"
    nm = shutil.which(args.nm)
    if nm is None or not elf.is_file() or not map_path.is_file():
        print(f"ERROR: 需要 {args.nm}、{elf}、{map_path}（先 export.sh 并构建）", file=sys.stderr)
        return 2

    failures = []
    try:
        # 1. 自有目标文件：异常/RTTI 与堆符号（定义或引用）一律为 0。
        own_objs = sorted(p for d in OWN_COMPONENT_DIRS for p in (build / "esp-idf" / d).rglob("*.obj"))
        if not own_objs:
            print(f"ERROR: {build}/esp-idf 下没有自有目标文件", file=sys.stderr)
            return 2
        for obj in own_objs:
            for _, name in run_nm(nm, ["-C", str(obj)]):
                if is_runtime(name):
                    failures.append(f"自有目标文件含异常/RTTI 符号：{obj.relative_to(build)}: {name}")
            for _, name in run_nm(nm, [str(obj)]):
                if HEAP_RAW_RE.match(name):
                    failures.append(f"自有目标文件含堆符号：{obj.relative_to(build)}: {name}")

        # 2. 全 ELF：异常/RTTI 符号必须是白名单子集。
        elf_hits = {name for _, name in run_nm(nm, ["-C", "--defined-only", str(elf)]) if is_runtime(name)}
        elf_hits |= {name for _, name in run_nm(nm, ["-C", "-u", str(elf)]) if is_runtime(name)}
        for name in sorted(elf_hits - ALLOWED_ELF_SYMBOLS):
            failures.append(f"ELF 含白名单外的异常/RTTI 符号：{name}")

        # 3. 白名单符号存在时，拉入链必须与裁决时一致。
        if elf_hits & ALLOWED_ELF_SYMBOLS:
            edges = parse_archive_edges(map_path)
            for member, (want_ref, want_sym) in REQUIRED_EDGES.items():
                got = edges.get(member)
                if got != (want_ref, want_sym):
                    failures.append(f"拉入链不符：{member} 期望由 {want_ref} ({want_sym}) 引入，实际 {got}")
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print(f"自有目标文件：{len(own_objs)} 个")
    print(f"ELF 异常/RTTI 符号（白名单内）：{len(elf_hits & ALLOWED_ELF_SYMBOLS)}/{len(ALLOWED_ELF_SYMBOLS)}")
    for name in sorted(elf_hits & ALLOWED_ELF_SYMBOLS):
        print(f"  allowed: {name}")
    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print("G7b PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
