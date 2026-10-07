#!/usr/bin/env python3
"""Fail when C-like source spells a name <windows.h> defines as a macro.

<windows.h> makes `near` and `far` vanish, `small` a `char`, ERROR a 0 and
GetObject GetObjectA, so such a name breaks -- or silently changes -- only the
Windows build. tools/windows_macros.txt lists the names. A branch that only
Windows, or never Windows, compiles is exempt, as are Slang and Objective-C.
Every file on disk is lexed, tracked or not, outside the trees pruned below; a
file the lexer cannot make sense of fails too, as a syntax error or a lexer bug.

Usage:  python3 tools/check_winmacro.py [--quiet]
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXTS = {".h", ".hh", ".hpp", ".hxx", ".inl", ".c", ".cc", ".cpp", ".cxx",
        ".cu", ".cuh", ".slang", ".m", ".mm"}
NO_WINDOWS_H = {".slang", ".m", ".mm"}   # slangc, or macOS only
# Vendored, or output. Root `build*` trees and every dot-directory go too.
EXCLUDED = {"outputs", "src/external", "viewer/build"}

# Every branch opens on a literal, so the engine skips straight to [/"'#].
TOKEN = re.compile(r"""
    //[^\n]*(?:(?<=\\)\n[^\n]*)*
  | /\*.*?\*/
  | "(?:(?<=R")(?<!\wR")|(?<=[uUL]R")(?<!\w[uUL]R")|(?<=u8R")(?<!\wu8R"))
     ([^()\\\s]{0,16})\(.*?\)\1"
  | "(?:[^"\\\n]|\\.)*"
  | '(?:(?<=[uUL]')(?<!\w[uUL]')|(?<=u8')(?<!\wu8')|(?<!\w'))(?:[^'\\\n]|\\.)+'
  | '(?<=[0-9A-Fa-f]')(?=[0-9A-Fa-f])
  | \#[ \t]*(?:(?:include(?:_next)?|import)[ \t]*<[^>\n]*>|(?:error|warning)\b[^\n]*)
""", re.S | re.X)

# What may not survive outside a comment or a literal, but for a line splice.
STRAY = re.compile(r"""["'`$@\\\x80-\U0010ffff]""")
ALLOWED = {".slang": "$", ".m": "@", ".mm": "@"}   # spirv_asm, Objective-C
DIRECTIVE = re.compile(
    r"[ \t]*#[ \t]*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b(.*)")
TERM = re.compile(r"\s*(!?)\s*(?:defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)|(\w+))\s*$")
IDENT = re.compile(r"[A-Za-z_]\w*")
CALL = re.compile(r"\s*\(")
DEFINE = re.compile(r"[ \t]*#[ \t]*define[ \t]+(\w+)")

# Defined by a Windows compiler only, or never by one. Only _WIN32 splits the
# platforms exactly: a compiler without _MSC_VER may still be MinGW.
WINDOWS = {"_WIN32", "_WIN64", "WIN32", "_MSC_VER", "__MINGW32__"}
ELSEWHERE = {"__APPLE__", "__MACH__", "__linux__", "__ANDROID__",
             "__EMSCRIPTEN__", "__FreeBSD__"}


def load_names():
    """Map each name in windows_macros.txt to whether it is function-like."""
    names = {}
    with open(os.path.join(HERE, "windows_macros.txt"), encoding="utf-8") as f:
        for line in f:
            for word in line.split("#", 1)[0].split():
                names[word.rstrip("()")] = word.endswith("()")
    return names


NAMES = load_names()


def lex(source):
    """Blank comments and literals; newlines are kept, so offsets map to lines."""
    parts, pos = [], 0
    for m in TOKEN.finditer(source):
        text = m.group()
        parts.append(source[pos:m.start()])
        if text != "'":                # a digit separator joins its digits
            parts.append(" " + "\n" * text.count("\n"))
        pos = m.end()
    parts.append(source[pos:])
    return "".join(parts)


def first_branches(code):
    """Drop every #elif and #else branch, which may each open what one #endif closes."""
    out, depth, skip = [], 0, None
    for line in code.split("\n"):
        m = DIRECTIVE.match(line)
        d = m.group(1) if m else ""
        if d.startswith("if"):
            depth += 1
        elif d.startswith("el") and skip is None:
            skip = depth
        elif d == "endif":
            skip = None if skip == depth else skip
            depth -= 1
        if skip is None:
            out.append(line)
    return "\n".join(out)


def unbalanced(text):
    return [o + c for o, c in ("()", "[]", "{}") if text.count(o) != text.count(c)]


def sides(kind, cond):
    """Return which platform compiles this branch, and every later one in its
    chain: "W" for Windows only, "N" for never Windows, None for unknown."""
    if kind.endswith("def"):
        neg, name, alone = kind.endswith("ndef"), (cond.split() or [""])[0], True
    else:
        terms = cond.split("&&")
        m = TERM.match(terms[0])
        if not m or "||" in cond:
            return None, None
        neg, alone = m.group(1) == "!", len(terms) == 1
        name = m.group(2) or m.group(3) or m.group(4)
    side = "W" if name in WINDOWS else "N" if name in ELSEWHERE else None
    if side is None:
        return None, None
    if not neg:
        return side, "N" if name == "_WIN32" and alone else None
    return "N" if name == "_WIN32" else None, side if alone else None


def platform_only(code):
    """Per line, whether it sits in a branch only one side of _WIN32 compiles."""
    flags, stack = [], []          # per #if: [this branch's side, later ones']
    for line in code.split("\n"):
        m = DIRECTIVE.match(line)
        kind = m.group(1) if m else ""
        if kind.startswith("if"):
            stack.append(list(sides(kind, m.group(2))))
        elif kind == "endif" and stack:
            stack.pop()
        elif kind.startswith("el") and stack:
            here, rest = sides(kind[2:], m.group(2)) if kind != "else" else (None, None)
            stack[-1] = [stack[-1][1] or here, stack[-1][1] or rest]
        flags.append(any(side for side, _ in stack))
    return flags


def macro_uses(code):
    """Return [(line, name)] for each listed name portable code spells."""
    exempt, found = None, []
    for m in IDENT.finditer(code):
        name = m.group()
        if name not in NAMES:
            continue
        exempt = exempt or platform_only(code)
        line = code.count("\n", 0, m.start())
        if exempt[line]:
            continue
        if NAMES[name] and not CALL.match(code, m.end()):
            start = code.rfind("\n", 0, m.start()) + 1
            d = DEFINE.match(code, start)
            if not d or d.start(1) != m.start():
                continue
        found.append((line + 1, name))
    return found


def check(path, source):
    """Return ([(line, name)], problem) for one file."""
    code = lex(source)
    ext = os.path.splitext(path)[1]
    ok = ALLOWED.get(ext, "")
    stray = [(m.start(), m.group()) for m in STRAY.finditer(code) if m.group() != ok
             and not (m.group() == "\\" and code.startswith("\n", m.end()))]
    stray += [(code.find(t), t) for t in ("/*", "*/") if t in code]
    if stray:
        at, text = min(stray)
        line = code.count("\n", 0, at) + 1
        return [], f"{path}:{line}: cannot lex {text!r}"
    pairs = unbalanced(code) and unbalanced(first_branches(code))
    if pairs:
        return [], f"{path}: unbalanced {' '.join(pairs)}"
    if ext in NO_WINDOWS_H or NAMES.keys().isdisjoint(IDENT.findall(code)):
        return [], None
    return macro_uses(code), None


def sources(root):
    for top, dirs, files in os.walk(root):
        rel = os.path.relpath(top, root).replace(os.sep, "/")
        rel = "" if rel == "." else rel + "/"
        dirs[:] = sorted(d for d in dirs if rel + d not in EXCLUDED
                         and not d.startswith(".")
                         and not (rel == "" and d.startswith("build")))
        for f in sorted(files):     # isfile: an editor lock is a dangling link
            if (os.path.splitext(f)[1] in EXTS
                    and os.path.isfile(os.path.join(top, f))):
                yield rel + f


def main():
    args = sys.argv[1:]
    quiet = "--quiet" in args          # say nothing when the tree is clean
    unknown = [a for a in args if a != "--quiet"]
    if unknown:
        print(f"check_winmacro: unknown argument {unknown[0]}")
        return 2

    root = os.path.dirname(HERE)
    hits, broken, count = 0, [], 0
    for path in sources(root):
        count += 1
        with open(os.path.join(root, path), encoding="utf-8-sig",
                  errors="replace") as f:
            source = f.read()
        found, problem = check(path, source)
        if problem:
            broken.append(problem)
        lines = source.split("\n") if found else []
        for line, name in found:
            print(f"{path}:{line}: `{name}`")
            print(f"      {lines[line - 1].strip()[:100]}")
        hits += len(found)

    for problem in broken:
        print(problem)
    if broken:
        print(f"check_winmacro: {len(broken)} file(s) did not lex. Fix the "
              "syntax, or the lexer in tools/check_winmacro.py.")
    if hits:
        print(f"check_winmacro: rename {hits} identifier(s); <windows.h> "
              "defines each as a macro (tools/windows_macros.txt).")
    if hits or broken:
        return 1
    if not quiet:
        print(f"check_winmacro: {count} files clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
