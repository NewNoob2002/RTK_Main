# 检查自有目标文件的堆使用白名单与全 ELF 的异常/RTTI 门禁。
# 由构建/验收进程调用，可阻塞等待 nm；不适用于任务回调或 ISR。
r"""G7b C++ 运行时门禁：自有目标文件分级堆规则 + 全 ELF 精确白名单。

背景：IDF nvs_flash 的 nvs_encrypted_partition.cpp 无条件编译，引用
operator new[](size_t, const std::nothrow_t&)，链接器因此从 libstdc++ 拉入
new_opvnt.o → eh_catch.o（2 个异常符号）与 new_opv.o → new_op.o → bad_alloc.o
（4 个 RTTI 符号）。证据见 docs/reports/T14-REVIEW.md「G7b 根因」。
本门禁只放行这 6 个符号，且要求 map 中的拉入链与上述一致；其他任何异常/RTTI
符号、链路变化、自有目标文件中的异常/RTTI 符号都判 FAIL。
自有目标文件中的堆符号按源文件白名单放行，未登记或无法映射源文件时判 FAIL。
自有目标文件不得引用 __cxa_guard_*：该符号只由函数内非平凡初始化的静态变量产生（CPP-009），
出现即说明回到了动态初始化；改用文件作用域 constinit/常量初始化。

自有目标文件范围：main/ 与 components/ 下每个含 CMakeLists.txt 的目录（运行时列举，Q3 起）。
唯一规则内例外（docs/acceptance.md G7b）——删除析构中的 operator delete：
虚析构会让编译器生成删除析构（Itanium ABI D0，符号 _ZN...D0Ev），其中含一条 delete 路径；
对象从不经 new 创建时该函数不会执行，因此不算真实分配。判定依据是重定位表：
operator delete（_ZdlPv*）的每一条重定位都必须位于删除析构的代码段，否则照旧按白名单裁决。
段名来自 -ffunction-sections（IDF 默认开启）；只有 .text 的目标文件不豁免。
Xtensa GCC 把调用目标地址放进独立的字面量池段（.literal.<组签名>；.text.<D0Ev> 内另有
R_XTENSA_ASM_EXPAND 重定位引用该池），而字面量池段与 .text.<...>D0Ev 同属一个 COMDAT 组；
因此“位于删除析构” = 段名匹配 ^\.text\..*D0Ev$ 或与某个这样的段同组。_Zda*（数组删除）不适用本例外。

用法：python3 -I tools/g7b_check/g7b_check.py [--build build] [--nm xtensa-esp32-elf-nm]
      [--objdump xtensa-esp32-elf-objdump] [--readelf xtensa-esp32-elf-readelf]
      [--allowlist tools/g7b_check/heap_allowlist.txt]
退出码：0 PASS；1 FAIL；2 工具或输入错误。
"""

import argparse
import json
import re
import shlex
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
# 函数内静态的线程安全初始化守卫（Itanium ABI C++ ABI §3.3.2）；未反修饰名，无参数重载。
GUARD_RE = re.compile(r"^__cxa_guard_(?:acquire|release|abort)$")
# 删除析构例外（acceptance G7b）：只限 operator delete，不含数组删除 _Zda*。
DELETE_OP_RE = re.compile(r"^_ZdlPv")
# 删除析构的代码段；-ffunction-sections 下每个函数一段，段名带反修饰前的符号名。
DTOR_TEXT_RE = re.compile(r"^\.text\..*D0Ev$")

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

ROOT = Path(__file__).resolve().parents[2]


def own_component_dirs():
    """列举自有目标文件所在目录：main/ 与 components/ 下每个含 CMakeLists.txt 的目录。"""
    dirs = ["main"]
    dirs += sorted(p.name for p in (ROOT / "components").iterdir() if (p / "CMakeLists.txt").is_file())
    return dirs


def read_heap_allowlist(path):
    """读取仓库相对源文件路径与档位/理由，拒绝通配、越界和无说明条目。"""
    allowed = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        source, marker, reason = line.partition("#")
        source = source.strip()
        if not source:
            continue
        source_path = Path(source)
        if (not marker or not reason.strip() or source_path.is_absolute()
                or source_path.as_posix() != source or ".." in source_path.parts or not source_path.parts
                or source_path.parts[0] not in ("main", "components")
                or source_path.suffix not in (".c", ".cpp", ".cc", ".cxx")
                or not (ROOT / source_path).resolve().is_relative_to(ROOT)
                or not (ROOT / source_path).is_file() or source in allowed):
            raise ValueError(f"{path}:{number}: 需要唯一的仓库相对源文件路径 + # 档位与理由")
        allowed[source] = reason.strip()
    return allowed


def object_sources(build):
    """按 CMake 编译数据库关联目标文件与源文件，避免同名源文件误放行。"""
    sources = {}
    try:
        entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise RuntimeError(f"无法读取编译数据库 {build / 'compile_commands.json'}：{exc}") from exc
    for entry in entries:
        directory = Path(entry["directory"])
        source = (directory / entry["file"]).resolve()
        if not source.is_relative_to(ROOT):
            continue
        output = entry.get("output")
        if output is None:
            command = entry.get("arguments")
            if command is None:
                command = shlex.split(entry["command"])
            if "-o" not in command or command.index("-o") + 1 >= len(command):
                raise ValueError(f"编译数据库缺少 -o 输出路径：{source}")
            output = command[command.index("-o") + 1]
        obj = (directory / output).resolve()
        relative_source = source.relative_to(ROOT).as_posix()
        if obj in sources and sources[obj] != relative_source:
            raise ValueError(f"编译数据库中目标文件对应多个源文件：{obj}")
        sources[obj] = relative_source
    return sources


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


def reloc_sections(objdump, obj):
    """返回 {符号: [所在段名…]}；objdump -r 按“RELOCATION RECORDS FOR [段]”分段输出。"""
    proc = subprocess.run([objdump, "-r", str(obj)], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{objdump} -r {obj} 失败：{proc.stderr.strip()}")
    relocs = {}
    section = None
    for line in proc.stdout.splitlines():
        header = re.match(r"^RELOCATION RECORDS FOR \[(.+)\]:$", line)
        if header:
            section = header.group(1)
            continue
        if section is None:
            continue
        # "偏移 类型 值"；值可能带 +0x… 加数，符号名本身不含空格。
        parts = line.split()
        if len(parts) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            relocs.setdefault(parts[2].split("+")[0], []).append(section)
    return relocs


def dtor_group_sections(readelf, obj):
    """返回与任一 .text.*D0Ev 段同属一个 COMDAT 组的段名集合（Xtensa 字面量池段的归属依据）。"""
    proc = subprocess.run([readelf, "-g", str(obj)], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{readelf} -g {obj} 失败：{proc.stderr.strip()}")
    groups = []
    current = None
    for line in proc.stdout.splitlines():
        if line.startswith("COMDAT group section"):
            current = []
            groups.append(current)
            continue
        member = re.match(r"^\s*\[\s*\d+\]\s+(\S+)\s*$", line)
        if current is not None and member:
            current.append(member.group(1))
    names = set()
    for group in groups:
        if any(DTOR_TEXT_RE.match(name) for name in group):
            names.update(group)
    return names


def deleting_dtor_exempt(objdump, readelf, obj, symbol):
    """判断 symbol 的每条重定位是否都位于删除析构范围内；返回删除析构函数名或 None。"""
    sections = reloc_sections(objdump, obj).get(symbol)
    if not sections:
        return None
    group = dtor_group_sections(readelf, obj)
    if not all(DTOR_TEXT_RE.match(name) or name in group for name in sections):
        return None
    dtors = sorted(name[len(".text."):] for name in group if DTOR_TEXT_RE.match(name))
    return ", ".join(dtors) if dtors else None


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
    ap.add_argument("--objdump", default="xtensa-esp32-elf-objdump", help="objdump 可执行文件（读重定位表）")
    ap.add_argument("--readelf", default="xtensa-esp32-elf-readelf", help="readelf 可执行文件（读 COMDAT 组）")
    ap.add_argument("--allowlist", type=Path, default=ROOT / "tools/g7b_check/heap_allowlist.txt",
                    help="堆白名单文件（条目均相对仓库根目录）")
    args = ap.parse_args()

    build = Path(args.build)
    elf, map_path = build / "rtk_main.elf", build / "rtk_main.map"
    nm = shutil.which(args.nm)
    objdump = shutil.which(args.objdump)
    readelf = shutil.which(args.readelf)
    if nm is None or objdump is None or readelf is None or not elf.is_file() or not map_path.is_file():
        print(f"ERROR: 需要 {args.nm}、{args.objdump}、{args.readelf}、{elf}、{map_path}"
              "（先 export.sh 并构建）", file=sys.stderr)
        return 2

    failures = []
    allowed_heap_hits = []
    exempt_delete_hits = []
    guard_refs = 0
    own_dirs = own_component_dirs()
    print(f"自有组件目录：{', '.join(own_dirs)}")
    try:
        allowlist = read_heap_allowlist(args.allowlist)
        sources = None
        # 1. 自有目标文件：异常/RTTI 为 0；堆符号仅按精确源文件路径放行。
        own_objs = sorted(p for d in own_dirs for p in (build / "esp-idf" / d).rglob("*.obj"))
        if not own_objs:
            print(f"ERROR: {build}/esp-idf 下没有自有目标文件", file=sys.stderr)
            return 2
        for obj in own_objs:
            for _, name in run_nm(nm, ["-C", str(obj)]):
                if is_runtime(name):
                    failures.append(f"自有目标文件含异常/RTTI 符号：{obj.relative_to(build)}: {name}")
            raw_symbols = run_nm(nm, [str(obj)])
            # 未定义引用（U）才需要守卫；本仓库不允许任何形式，故按引用计数判定。
            guard_hits = sorted({name for kind, name in raw_symbols if kind == "U" and GUARD_RE.match(name)})
            heap_hits = sorted({name for _, name in raw_symbols if HEAP_RAW_RE.match(name)})
            if not guard_hits and not heap_hits:
                continue
            if sources is None:
                sources = object_sources(build)
            source = sources.get(obj.resolve())
            for name in guard_hits:
                guard_refs += 1
                failures.append(f"自有目标文件引用 __cxa_guard（函数内静态需常量初始化）："
                                f"{obj.relative_to(build)}: {name}（源文件：{source or 'UNKNOWN'}）")
            for name in heap_hits:
                if DELETE_OP_RE.match(name):
                    dtor = deleting_dtor_exempt(objdump, readelf, obj, name)
                    if dtor is not None:
                        exempt_delete_hits.append(f"{source or 'UNKNOWN'}: {dtor}")
                        continue
                if source in allowlist:
                    allowed_heap_hits.append(f"{source}: {name} # {allowlist[source]}")
                else:
                    failures.append(f"自有目标文件含堆符号：{obj.relative_to(build)}: {name}"
                                    f"（源文件：{source or 'UNKNOWN'}，未登记白名单）")

        # 1b. 守卫（CPP-009）：覆盖编译数据库中映射到仓库源码的全部目标文件。
        # 自有目录已由 main/ 与 components/* 列举（Q3），这里仍扫全部数据库映射对象，
        # 覆盖 ESP 侧未编译、但出现在 host/测试数据库里的自有源文件。
        sources = object_sources(build) if sources is None else sources
        scanned = {obj.resolve() for obj in own_objs}
        build_abs = build.resolve()
        for obj, source in sorted(sources.items()):
            if obj in scanned or not obj.is_file():
                continue
            for kind, name in run_nm(nm, [str(obj)]):
                if kind == "U" and GUARD_RE.match(name):
                    guard_refs += 1
                    failures.append(f"自有目标文件引用 __cxa_guard（函数内静态需常量初始化）："
                                    f"{obj.relative_to(build_abs)}: {name}（源文件：{source}）")

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
    except (RuntimeError, OSError, ValueError, KeyError, TypeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    print(f"自有目标文件：{len(own_objs)} 个")
    print(f"堆白名单：{len(allowlist)} 个源文件，放行 {len(allowed_heap_hits)} 个堆符号")
    print(f"自有目标文件 __cxa_guard 未定义引用：{guard_refs} 个")
    print(f"删除析构豁免的 operator delete：{len(exempt_delete_hits)} 处")
    for hit in exempt_delete_hits:
        print(f"  deleting-dtor exempt: {hit}")
    for hit in allowed_heap_hits:
        print(f"  allowed heap: {hit}")
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
