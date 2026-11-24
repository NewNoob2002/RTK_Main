#!/usr/bin/env python3
"""函数注释门禁（docs/CODING-STYLE.md §6.2）：每个函数都要有 doxygen 块注释。

判定规则（与 §6.2 一一对应）：
- 函数定义前紧邻（中间只允许空行、template<...>、[[...]] 属性行）有 /** ... */ 块，且块内有 @brief；
  或者同组件头文件里该函数的声明前有这样的块（公开 API 写在头文件声明处，.cpp 定义处不重复）。
- 每个具名参数都有 "@param <name>"（允许 "@param name:" / "@param[out] name"）。
- 返回类型不是 void、且不是构造/析构函数时，必须有 @retval 或 @return。
- 豁免：lambda；头文件声明带 override 的覆写函数（契约写在基类声明处）；= default / = delete。
函数清单来自 lizard：优先用 PATH 里的 lizard，否则 `uvx --offline lizard`（只用本机缓存，不联网）。

用法：
  python3 -I tools/doc_check/doc_check.py --base <git-rev>   # 门禁：只检查相对 base 新增/改动行所在的函数
  python3 -I tools/doc_check/doc_check.py --report           # 按组件统计覆盖率（不失败）
  python3 -I tools/doc_check/doc_check.py --list <路径前缀>   # 列出该前缀下所有不合格函数
退出码：0 PASS；1 有不合格函数；2 工具或输入错误。
"""

import argparse
import collections
import csv
import io
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCAN_DIRS = ("components", "main")
SRC_SUFFIX = (".c", ".cpp", ".h", ".hpp", ".inc")
HDR_SUFFIX = (".h", ".hpp", ".inc")
SKIP_PARTS = ("/test/", "/reference/", "/_template/")
# 经核验的第三方上游源码目录前缀（只豁免具体上游子目录，避免后续自有胶水代码静默绕过门禁，ROL1-07）。
VENDORED_PREFIXES = ("components/third_party/wououi/",)


def is_skipped_path(rel):
    return any(s in "/" + rel for s in SKIP_PARTS) or rel.startswith(VENDORED_PREFIXES)

DOC_END = re.compile(r"\*/\s*$")
PARAM_TAG = re.compile(r"@param(?:\[[a-z,]+\])?\s+([A-Za-z_]\w*)")
RET_TAG = re.compile(r"@(retval|return|returns)\b")


def is_code_lizard():
    """PATH 里的 lizard 是否为代码分析器：Arch 的 lizard 包是同名压缩工具，`--version` 不输出纯版本号。"""
    if not shutil.which("lizard"):
        return False
    try:
        ver = subprocess.run(["lizard", "--version"], capture_output=True, text=True, check=False, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return re.fullmatch(r"\d+(\.\d+)+", ver.stdout.strip()) is not None


def run_lizard(files):
    """调用 lizard 输出 CSV，返回 [(file, qualified_name, long_name, start, end)]。"""
    if not files:
        return []
    exe = ["lizard"] if is_code_lizard() else ["uvx", "--offline", "lizard"]
    cmd = exe +["-l", "cpp", "-l", "c", "--csv"] + [str(f) for f in files]
    try:
        out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=False).stdout
    except FileNotFoundError:
        print("doc_check: 找不到 lizard 或 uvx", file=sys.stderr)
        sys.exit(2)
    if not out.strip():
        print("doc_check: lizard 没有输出（本机缓存里没有 lizard？先由人工运行一次 `uvx lizard --version`）",
              file=sys.stderr)
        sys.exit(2)
    funcs = []
    for row in csv.reader(io.StringIO(out)):
        if len(row) < 11:
            continue
        funcs.append((row[6], row[7], row[8], int(row[9]), int(row[10])))
    return funcs


_cache = {}


def lines_of(path):
    if path not in _cache:
        _cache[path] = (ROOT / path).read_text(errors="replace").split("\n")
    return _cache[path]


def doc_block_before(lines, idx):
    """返回 idx 行之前紧邻的 /** */ 块文本；没有则返回 None。idx 为 0 基行号。"""
    j = idx - 1
    while j >= 0:
        s = lines[j].strip()
        if s == "" or s.startswith("template") or (s.startswith("[[") and s.endswith("]]")):
            j -= 1
            continue
        break
    if j < 0 or not DOC_END.search(lines[j]):
        return None
    end = j
    while j >= 0 and "/**" not in lines[j]:
        if "/*" in lines[j] and "/**" not in lines[j]:
            return None  # 普通 /* */ 块不算文档注释
        j -= 1
    if j < 0:
        return None
    return "\n".join(lines[j : end + 1])


def split_params(long_name):
    """从 lizard 的 long_name 中取参数名列表（无名参数跳过）。"""
    lp = long_name.find("(")
    if lp < 0:
        return []
    depth, cur, parts = 0, "", []
    for ch in long_name[lp + 1 :]:
        if ch in "(<[{":
            depth += 1
        elif ch in ")>]}":
            if depth == 0:
                break
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    names = []
    for p in parts:
        p = p.split("=")[0].strip()
        p = re.sub(r"\[[^\]]*\]\s*$", "", p).strip()
        p = re.sub(r"/\*.*?\*/", "", p).strip()
        if p in ("", "void", "...") or p.endswith(("&", "*", ">")):
            continue
        toks = re.findall(r"[A-Za-z_]\w*", p)
        if len(toks) >= 2 and toks[-1] not in ("const", "noexcept", "volatile"):
            names.append(toks[-1])
    return names


def returns_value(lines, start_idx, short):
    """定义头（从 start 行起到函数名为止）判断是否返回值。构造/析构/void 不需要 @retval。"""
    head = " ".join(lines[start_idx : start_idx + 3])
    pos = head.find(short + "(")
    if pos < 0:
        pos = head.find(short + " (")
    prefix = head[:pos] if pos >= 0 else head
    prefix = re.sub(r"\[\[[^\]]*\]\]", " ", prefix)
    prefix = re.sub(r"\b(static|inline|constexpr|consteval|virtual|explicit|extern|friend)\b", " ", prefix)
    prefix = re.sub(r"[A-Za-z_]\w*::\s*$", "", prefix.strip()).strip()
    if prefix == "" or prefix.endswith("~"):
        return False  # 构造/析构
    return not re.search(r"(^|\s)void\s*$", prefix)


class HeaderIndex:
    """组件内头文件声明索引：短名 → [(path, 0 基行号)]。"""

    def __init__(self):
        self.by_comp = {}

    def get(self, comp):
        if comp not in self.by_comp:
            idx = collections.defaultdict(list)
            base = ROOT / comp
            for p in base.rglob("*"):
                if p.suffix in HDR_SUFFIX and p.is_file():
                    rel = str(p.relative_to(ROOT))
                    for i, line in enumerate(lines_of(rel)):
                        for m in re.finditer(r"\b([A-Za-z_~]\w*)\s*\(", line):
                            idx[m.group(1)].append((rel, i))
            self.by_comp[comp] = idx
        return self.by_comp[comp]


def component_of(path):
    parts = path.split("/")
    if parts[0] == "components":
        return "/".join(parts[:2])
    return "main"


def check_function(f, hdr_index):
    """返回问题描述列表；空列表表示合格。"""
    path, qname, long_name, start, end = f
    short = qname.split("::")[-1]
    if short in ("(anonymous)", "") or "lambda" in qname:
        return []
    lines = lines_of(path)
    defline = " ".join(lines[start - 1 : end])
    if re.search(r"=\s*(default|delete)\s*;", defline):
        return []
    block = doc_block_before(lines, start - 1)
    if block is None or "@brief" not in block:
        # 同名函数可能出现在组件内多个头文件；按与定义文件同目录、再按 rglob 顺序匹配，
        # 避免文件系统遍历顺序不同导致误配到别的类的同名声明（如多个 init()）。
        def_dir = os.path.dirname(path)
        cands = sorted(hdr_index.get(component_of(path)).get(short, []),
                       key=lambda c: os.path.dirname(c[0]) != def_dir)
        for hpath, i in cands:
            decl = " ".join(lines_of(hpath)[i : i + 4])
            hb = doc_block_before(lines_of(hpath), i)
            if hb and "@brief" in hb:
                block = hb
                break
            if re.search(r"\boverride\b", decl.split(";")[0]):
                return []
    if re.search(r"\boverride\b", " ".join(lines[start - 1 : start + 2]).split("{")[0]):
        if block is None or "@brief" not in block:
            return []
    if block is None:
        return ["缺少 /** @brief */ 注释"]
    probs = []
    if "@brief" not in block:
        probs.append("注释缺少 @brief")
    documented = set(PARAM_TAG.findall(block))
    missing = [p for p in split_params(long_name) if p not in documented]
    if missing:
        probs.append("缺少 @param " + ", ".join(missing))
    if returns_value(lines, start - 1, short) and not RET_TAG.search(block):
        probs.append("缺少 @retval")
    return probs


def all_sources():
    out = []
    for d in SCAN_DIRS:
        for p in (ROOT / d).rglob("*"):
            rel = str(p.relative_to(ROOT))
            if p.is_file() and p.suffix in SRC_SUFFIX and not is_skipped_path(rel):
                out.append(rel)
    return sorted(out)


def changed_lines(base):
    """相对 base 的新增/改动行：{path: set(行号)}；未跟踪的新文件整份计入。"""
    res = collections.defaultdict(set)
    diff = subprocess.run(["git", "diff", "--relative", "-U0", base, "--", *SCAN_DIRS], cwd=ROOT, capture_output=True,
                          text=True, check=False)
    if diff.returncode != 0:
        print(f"doc_check: git diff 失败：{diff.stderr.strip()}", file=sys.stderr)
        sys.exit(2)
    cur = None
    for line in diff.stdout.split("\n"):
        if line.startswith("+++ "):
            cur = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("@@") and cur:
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            s, n = int(m.group(1)), int(m.group(2) or "1")
            res[cur].update(range(s, s + n))
    untracked = subprocess.run(["git", "ls-files", "--others", "--exclude-standard", "--", *SCAN_DIRS], cwd=ROOT,
                               capture_output=True, text=True, check=False).stdout.split()
    for u in untracked:
        res[u].update(range(1, len(lines_of(u)) + 1))
    return {p: v for p, v in res.items() if p.endswith(SRC_SUFFIX) and not is_skipped_path(p)}


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--base", help="只检查相对该 git 版本新增/改动的函数")
    g.add_argument("--report", action="store_true", help="按组件统计覆盖率")
    g.add_argument("--list", metavar="PREFIX", help="列出该路径前缀下所有不合格函数")
    args = ap.parse_args()
    hdr = HeaderIndex()

    if args.base:
        changed = changed_lines(args.base)
        funcs = [f for f in run_lizard(sorted(changed)) if changed[f[0]] & set(range(f[3], f[4] + 1))]
        bad = [(f, p) for f in funcs if (p := check_function(f, hdr))]
        for f, p in bad:
            print(f"{f[0]}:{f[3]}: {f[1]}: {'; '.join(p)}")
        print(f"doc_check: 改动函数 {len(funcs)} 个，不合格 {len(bad)} 个")
        return 1 if bad else 0

    files = all_sources()
    if args.list:
        files = [f for f in files if f.startswith(args.list)]
    funcs = run_lizard(files)
    if args.list:
        bad = 0
        for f in funcs:
            p = check_function(f, hdr)
            if p:
                bad += 1
                print(f"{f[0]}:{f[3]}: {f[1]}: {'; '.join(p)}")
        print(f"doc_check: {args.list} 函数 {len(funcs)} 个，不合格 {bad} 个")
        return 1 if bad else 0
    stat = collections.defaultdict(lambda: [0, 0])
    for f in funcs:
        c = component_of(f[0])
        stat[c][0] += 1
        stat[c][1] += not check_function(f, hdr)
    print(f"{'组件':24} {'函数':>5} {'合格':>5} {'覆盖率':>6}")
    tot = [0, 0]
    for c, (n, ok) in sorted(stat.items()):
        tot[0] += n
        tot[1] += ok
        print(f"{c:24} {n:5} {ok:5} {100 * ok // max(n, 1):5}%")
    print(f"{'合计':24} {tot[0]:5} {tot[1]:5} {100 * tot[1] // max(tot[0], 1):5}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
