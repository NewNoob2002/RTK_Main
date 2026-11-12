# R2 源码契约、函数规模及全局对象核对脚本。
# 仅 host 离线运行；读取基线和构建数据库，在 builds/r2* 生成检查文件，不访问硬件或网络。
"""R2 离线证据：格式串/命令契约、函数规模、文件作用域对象；不修改源码。"""
import collections
import importlib.util
import json
import os
import re
from pathlib import Path

from clang.cindex import CursorKind as Kind
from clang.cindex import Diagnostic, Index, TypeKind

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "builds/r2-base-src"
SPEC = importlib.util.spec_from_file_location("tidy_check", ROOT / "tools/tidy_check/tidy_check.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("无法加载现有 tidy_check 工具")
TIDY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TIDY)
TOKEN = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|[^\s]', re.S)
CONTROL = {Kind.IF_STMT, Kind.FOR_STMT, Kind.CXX_FOR_RANGE_STMT, Kind.WHILE_STMT, Kind.DO_STMT, Kind.SWITCH_STMT}
FUNCTION = {Kind.FUNCTION_DECL, Kind.CXX_METHOD, Kind.CONSTRUCTOR, Kind.DESTRUCTOR, Kind.CONVERSION_FUNCTION}


def contracts(path):
    tokens = [t for t in TOKEN.findall(path.read_text()) if not t.startswith(("//", "/*"))]
    result = collections.Counter()
    for i, lexeme in enumerate(tokens[:-2]):
        if path.name.endswith("_cmds.cpp") and token_is_command_entry(tokens, i):
            result[("command", (tokens[i + 1],))] += 1
            result[("help", (tokens[i + 3],))] += 1
        if lexeme == "." and tokens[i + 1] in {"command", "help"} and tokens[i + 2] == "=":
            end = i + 3
            while end < len(tokens) and tokens[end] not in {",", "}"}:
                end += 1
            result[(tokens[i + 1], tuple(tokens[i + 3:end]))] += 1
        if (lexeme == "printf" or re.fullmatch(r"ESP_LOG[A-Z]+", lexeme)) and tokens[i + 1] == "(":
            arguments, part, depth = [], [], 0
            for item in tokens[i + 2:]:
                if item == ")" and depth == 0:
                    arguments.append(part)
                    break
                if item == "," and depth == 0:
                    arguments.append(part)
                    part = []
                    continue
                if item in {"(", "[", "{"}:
                    depth += 1
                if item in {")", "]", "}"}:
                    depth -= 1
                part.append(item)
            result[(lexeme, tuple(arguments[0 if lexeme == "printf" else 1]))] += 1
    return result


def token_is_command_entry(tokens, index):
    return (index + 4 < len(tokens) and tokens[index] == "{" and tokens[index + 1].startswith('"')
            and tokens[index + 2] == "," and tokens[index + 3].startswith('"') and tokens[index + 4] == ",")


SOURCES = {}


def nesting(cursor, level=0):
    source = cursor.extent.start.file
    if cursor.kind in CONTROL and source:
        path = str(source)
        if path not in SOURCES:
            SOURCES[path] = Path(path).read_bytes()
        offset = cursor.extent.start.offset
        # 统计源码中的控制语句，不将 ESP_LOG 等 SDK 宏展开出的 do/if 算作业务嵌套。
        level += bool(re.match(rb"(?:if|for|while|do|switch)\b", SOURCES[path][offset:offset + 12]))
    return max([level] + [nesting(child, level) for child in cursor.get_children()])


def inspect(root, build, db):
    TIDY.make_esp_db(build, db)
    functions, objects = {}, {}
    for entry in json.loads((db / "compile_commands.json").read_text()):
        path = Path(entry["file"])
        if not path.is_relative_to(root / "main"):
            continue
        args, clean, i = entry["arguments"][1:], [], 0
        while i < len(args):
            if args[i] in {"-o", "-MF", "-MT", "-MQ"}:
                i += 2
            elif args[i] == "-c" or args[i] == str(path):
                i += 1
            else:
                clean.append(args[i])
                i += 1
        clean += ["-Wno-unknown-warning-option", "-Wno-extern-c-compat"]
        os.chdir(entry["directory"])
        tu = Index.create().parse(str(path), args=clean)
        errors = [str(d) for d in tu.diagnostics if d.severity >= Diagnostic.Error]
        if errors:
            raise RuntimeError("\n".join(errors))

        def visit(cursor):
            source = Path(str(cursor.location.file)) if cursor.location.file else None
            if source and not source.is_relative_to(root / "main"):
                return
            if source and cursor.kind in FUNCTION and cursor.is_definition():
                start, end = cursor.extent.start.line, cursor.extent.end.line
                bodies = [c for c in cursor.get_children() if c.kind == Kind.COMPOUND_STMT]
                label = f"{cursor.semantic_parent.spelling}::{cursor.spelling}"
                functions[(str(source.relative_to(root)), start, label)] = (end - start + 1, max([0] + [nesting(b) for b in bodies]))
                return
            if (source and cursor.kind == Kind.VAR_DECL and cursor.is_definition()
                    and cursor.semantic_parent.kind in {Kind.NAMESPACE, Kind.TRANSLATION_UNIT}):
                declaration = " ".join(t.spelling for t in cursor.get_tokens())
                element = cursor.type
                while element.kind in {TypeKind.CONSTANTARRAY, TypeKind.INCOMPLETEARRAY}:
                    element = element.element_type
                constant = element.is_const_qualified() or declaration.startswith(("constexpr ", "inline constexpr "))
                objects[(str(source.relative_to(root)), cursor.location.line, cursor.spelling)] = (constant, cursor.type.spelling)
            for child in cursor.get_children():
                visit(child)

        visit(tu.cursor)
    os.chdir(ROOT)
    return functions, objects


os.chdir(ROOT)
counts = collections.Counter()
for source in sorted((ROOT / "main").rglob("*.cpp")):
    relative = source.relative_to(ROOT)
    old, new = contracts(BASE / relative), contracts(source)
    if old != new:
        raise RuntimeError(f"FAIL {relative}: removed={old-new}; added={new-old}")
    for (kind, _), count in new.items():
        counts[kind] += count
print("CONTRACT PASS", dict(sorted(counts.items())))
for title, root, build, db in [
    ("BEFORE", BASE, ROOT / "builds/r2-base", ROOT / "builds/r2-base-clang-db"),
    ("AFTER", ROOT, ROOT / "builds/r2", ROOT / "builds/r2-clang-db"),
]:
    funcs, globals_ = inspect(root, build, db)
    print(f"\n{title} GLOBALS")
    for (file, line, name), (constant, type_name) in sorted(globals_.items()):
        print(f"{file}:{line} {name}: {'constant' if constant else 'mutable'} {type_name}")
    mutable = sum(not constant for constant, _ in globals_.values())
    print(f"{title} FILE_OBJECTS={len(globals_)} MUTABLE={mutable}")
    module_funcs = [(key, value) for key, value in funcs.items() if key[0].startswith("main/modules/")]
    print(f"\n{title} MODULE FUNCTIONS (signature through closing brace; control nesting)")
    for (file, line, name), (lines, depth) in sorted(module_funcs):
        print(f"{file}:{line} {name}: lines={lines} nesting={depth}")
    excessive = [(key, value) for key, value in module_funcs if value[0] > 60 or value[1] > 3]
    print(f"{title} FUNCTIONS={len(module_funcs)} OVER_LIMIT={len(excessive)}")
    if title == "AFTER" and excessive:
        raise RuntimeError(f"FAIL limits: {excessive}")
print("R2 SOURCE CHECK PASS")
