#!/usr/bin/env python3
"""A return into another chunk leaves the return dispatch at once.

  return_ranges.py COMPOSITE_SRC

Each chunk's return dispatch is a switch over its return addresses (a few
hundred cases), and every blr goes through it. The compiler lowers a switch
this sparse to a binary search, so a return to an address in another chunk -
which ends at `default: return;` - takes a dozen compares first. Here the
dispatch first tests the address against the lowest and highest case: outside
them no case can match, and it returns as the default does. Inside, the
switch is as it was.

The change is repeatable (a prepared chunk is left as it is) and keeps LF
line ends. It touches only the return dispatch, after the last label the
certified natives' hashes cover.
"""
import re
import sys
from pathlib import Path

MARK = "/* bluewake: return dispatch range tests (scripts/windows/return_ranges.py) */\n"
INCLUDE = '#include "../generated.h"\n'
DISPATCH = re.compile(r"^return_dispatch_[0-9A-F]{8}:$")
BUDGET = "    if (ctx->downcount <= -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET) return;"
SWITCH = "    switch (ctx->pc) {"
CASE = re.compile(r"^    case 0x([0-9A-F]{8})u: goto label_\1;$")
DEFAULT = "    default: return;"


def transform(text):
    if MARK in text or INCLUDE not in text:
        return text, 0
    lines = text.split("\n")
    out, i, done = [], 0, 0
    while i < len(lines):
        out.append(lines[i])
        if DISPATCH.match(lines[i]) and i + 2 < len(lines) and lines[i + 1] == BUDGET and lines[i + 2] == SWITCH:
            j = i + 3
            cases = []
            while j < len(lines) and CASE.match(lines[j]):
                cases.append(int(CASE.match(lines[j]).group(1), 16))
                j += 1
            if cases and j < len(lines) and lines[j] == DEFAULT:
                low, high = min(cases), max(cases)
                out.append(BUDGET)
                out.append(f"    if ((u32)(ctx->pc - 0x{low:08X}u) > 0x{high - low:08X}u) return;")
                i += 1
                done += 1
        i += 1
    if not done:
        return text, 0
    return "\n".join(out).replace(INCLUDE, INCLUDE + MARK, 1), done


def main():
    root = Path(sys.argv[1])
    chunks = sorted(root.glob("chunks_*/*.c"))
    if not chunks:
        sys.exit(f"no chunks under {root}")
    dispatches = files = 0
    for path in chunks:
        with open(path, encoding="utf-8", newline="") as file:
            original = file.read()
        converted, count = transform(original)
        if count:
            temporary = path.with_suffix(".c.tmp")
            with open(temporary, "w", encoding="utf-8", newline="") as file:
                file.write(converted)
            temporary.replace(path)
            dispatches += count
            files += 1
    print(f"return dispatch range tests: {dispatches} in {files} chunks")


if __name__ == "__main__":
    main()
