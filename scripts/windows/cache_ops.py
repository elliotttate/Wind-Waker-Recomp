#!/usr/bin/env python3
"""The data and instruction cache operations without the call into the host.

  cache_ops.py COMPOSITE_SRC

The translator leaves dcbf, dcbst, dcbi and icbi to the instruction fallback:
`ppc_fallback_instruction(ctx, RAW, CIA);`, which calls the host's
instruction_fallback (runtime/host/src/main.c) through the CPU state. For
these four the host only steps past them - `ctx->pc = cia + 4u;` - and that
is all the fallback does: the host keeps no cache model, and nothing
observes the instruction. DCFlushRange, DCStoreRange, DCInvalidateRange and
ICInvalidateRange run one of them per 32-byte line, so flushing a vertex
array before the GX reads it made a call through two pointers per line.
Here each such call is the pc store the host would have made.

The change is repeatable (a prepared site is left as it is) and keeps LF
line ends. No certified native's fragment holds one of these sites
(native_game_math.py, native_entries.py, native_skin.py,
prepare_native_j3d.py), and the steps after this one hash the chunks as they
end up.
"""
import re
import sys
from pathlib import Path

# Primary opcode 31, extended opcode: the instructions the host's fallback
# only steps past (instruction_fallback: xo 54, 86, 470, 982).
CACHE_OPS = {54: "dcbst", 86: "dcbf", 470: "dcbi", 982: "icbi"}
SITE = re.compile(r"ppc_fallback_instruction\(ctx, 0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u\);")


def replace(match):
    raw, cia = int(match.group(1), 16), int(match.group(2), 16)
    xo = (raw >> 1) & 0x3FF
    if raw >> 26 != 31 or xo not in CACHE_OPS:
        return match.group(0)
    return f"ctx->pc = 0x{cia + 4:08X}u; /* {CACHE_OPS[xo]}: the host's fallback only steps past it */"


def main():
    root = Path(sys.argv[1])
    chunks = sorted(root.glob("chunks_*/*.c"))
    if not chunks:
        sys.exit(f"no chunks under {root}")
    sites = files = 0
    for path in chunks:
        with open(path, encoding="utf-8", newline="") as file:
            original = file.read()
        if "ppc_fallback_instruction" not in original:
            continue
        converted = SITE.sub(replace, original)
        if converted == original:
            continue
        changed = len(SITE.findall(original)) - len(SITE.findall(converted))
        temporary = path.with_suffix(".c.tmp")
        with open(temporary, "w", encoding="utf-8", newline="") as file:
            file.write(converted)
        temporary.replace(path)
        sites += changed
        files += 1
    print(f"cache operations stepped past inline: {sites} in {files} chunks")


if __name__ == "__main__":
    main()
