#!/usr/bin/env python3
"""Prepaid block copies that never resume after an out-of-line call.

  straight_copies.py COMPOSITE_SRC

In fast_blocks.py's copies (as lean_memory.py leaves them) every guest load
and store has an inline path for ordinary RAM and an out-of-line one for
everything else, and both continue in the copy. Since the out-of-line call
may change anything, after every access the compiler reloads the guest
registers it had in host registers, the three tests of the inline path
(aliases over MEM1, a reservation, a write journal) and the deadline budget.
In the module's code that is about half of a store.

Here an instruction whose only calls are its accesses (and the floating
point availability check) continues, after an out-of-line access, in the
original block at the next instruction, where the deadline refund already
continues: the original block, which charges nothing more while
cycle_block_prepaid is set, does exactly what the copy would. The copy is
then a straight line from its start to its end on the inline paths. A
gather-pipe store that fits in the batch is one of them (gather_pipe.h,
bw_writeN_out).

The deadline test after such an instruction is also left out of the copy's
straight line where it cannot fire: the budget changes only in a host call,
the block's start established that the budget is beyond the whole block,
and a test that passed after a call established it for every instruction
after it (each later suffix is smaller). It is made on the way out, before
the refund or the original block.

Copies the certified native entries' hashes cover (scripts/windows/
native_entries.py, which hashes the copies its fragments jump to) are left
as they are. The change is repeatable (a prepared chunk is left as it is)
and keeps LF line ends. Run it right after lean_memory.py.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import native_entries  # noqa: E402

MARK = "/* bluewake: straight prepaid copies (scripts/windows/straight_copies.py) */\n"
FAST_MARK = "/* bluewake: prepaid block copies (scripts/windows/fast_blocks.py) */\n"
FUNCTION = re.compile(r"^(?:static )?void (\w+)\(CPUState\* ctx_param\) \{$")
COPY = re.compile(r"^bwfast_(\d+):$")
END = re.compile(r"^    goto bwend_(\d+);$")
PC = re.compile(r"^    ctx->pc = 0x([0-9A-F]{8})u;$")
SUFFIX = re.compile(r"^    ctx->cycle_observation_suffix = (\d+)u;$")
COMMENT = re.compile(r"^    // ([0-9A-F]{8}): ")
AT = re.compile(r"\bbw_(read|write)(8|16|32|64)_at\(ctx, ")
MEM = re.compile(r"\bmem_(read|write)(8|16|32|64)\(ctx, ")
FP_CHECK = re.compile(r"^    if \(!ppc_fp_available_inline\(ctx, 0x([0-9A-F]{8})u\)\) return;$")
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
PURE = {"if", "dolrecomp_rotl32", "dolrecomp_f32_from_bits", "dolrecomp_f64_from_bits",
        "dolrecomp_f32_to_bits", "dolrecomp_f64_to_bits", "dolrecomp_ps_from_bits",
        "dolrecomp_ps_to_bits", "sizeof", "while", "for", "switch", "return"}
ACCESS_NAMES = re.compile(r"(?:mem_(?:read|write)(?:8|16|32|64)|bw_(?:read|write)(?:8|16|32|64)_at|"
                          r"ppc_fp_available_inline)")
TEST_HEAD = "    if (ctx->cycle_deadline_budget > 0 &&"
VARIABLE_TEST = [
    TEST_HEAD,
    "        (s64)ctx->cycle_observation_suffix > ctx->cycle_deadline_budget) {",
    "        ctx->downcount += (s64)ctx->cycle_observation_suffix;",
    "        cycle_block_prepaid = false;",
]
CONSTANT_TEST = re.compile(r"^        \(s64\)(\d+)u > ctx->cycle_deadline_budget\) \{$")
GOTO = re.compile(r"^        goto (bwslow_\d+_\d+|bwend_\d+);$")


def calls(text):
    return [name for name in CALL.findall(text)
            if name not in PURE and not name.startswith("__builtin_") and not ACCESS_NAMES.fullmatch(name)]


def test_at(lines, i):
    """The deadline test at lines[i]: (length, target, constant suffix or None), or None."""
    if lines[i:i + 4] == VARIABLE_TEST and i + 5 < len(lines) and GOTO.match(lines[i + 4]) \
            and lines[i + 5] == "    }":
        return 6, GOTO.match(lines[i + 4]).group(1), None
    if i + 7 < len(lines) and lines[i] == TEST_HEAD and CONSTANT_TEST.match(lines[i + 1]):
        k = CONSTANT_TEST.match(lines[i + 1]).group(1)
        if (PC.match("    " + lines[i + 2].strip()) and lines[i + 3] == f"        ctx->cycle_observation_suffix = {k}u;"
                and lines[i + 4] == f"        ctx->downcount += (s64){k}u;"
                and lines[i + 5] == "        cycle_block_prepaid = false;" and GOTO.match(lines[i + 6])
                and lines[i + 7] == "    }"):
            return 8, GOTO.match(lines[i + 6]).group(1), int(k)
    return None


def units(lines):
    """A copy's lines as ('insn', pre, comment, body, test) and ('line', text)."""
    items, i = [], 0
    while i < len(lines):
        j = i
        while j < len(lines) and (PC.match(lines[j]) or SUFFIX.match(lines[j])):
            j += 1
        if j < len(lines) and COMMENT.match(lines[j]):
            k = j + 1
            while k < len(lines) and not (PC.match(lines[k]) or SUFFIX.match(lines[k]) or COMMENT.match(lines[k])
                                          or test_at(lines, k)):
                k += 1
            test = None
            found = test_at(lines, k) if k < len(lines) else None
            if found:
                test = lines[k:k + found[0]]
            items.append(("insn", lines[i:j], lines[j], lines[j + 1:k], test))
            i = k + (found[0] if found else 0)
            continue
        items.append(("line", lines[i]))
        i += 1
    return items


def straighten(pre, comment, body, test):
    """The instruction's straight form ([lines], dropped test?) or None."""
    if test is None:
        return None
    length, target, constant = test_at(test, 0)
    text = "\n".join(body)
    if calls(text) or "goto" in text:
        return None
    checks = [line for line in body if FP_CHECK.match(line)]
    if text.count("return") != len(checks):
        return None
    if not AT.search(text) and not MEM.search(text) and not checks:
        return None
    pc = next((PC.match(line).group(1) for line in pre if PC.match(line)), None)
    suffix = next((int(SUFFIX.match(line).group(1)) for line in pre if SUFFIX.match(line)), None)
    if MEM.search(text) and (pc is None or suffix is None):
        return None
    if constant is None and suffix is None:
        return None
    out = list(pre) + [comment, "    {", "        bool bw_out = false;"]
    for line in body:
        m = FP_CHECK.match(line)
        if m:
            out.extend([
                "        if (__builtin_expect(!(ctx->msr & PPC_MSR_FP), 0)) {",
                f"            if (!ppc_fp_available(ctx, 0x{m.group(1)}u)) return;",
                "            bw_out = true;",
                "        }",
            ])
            continue
        line = AT.sub(r"bw_\1\2_out(&bw_out, ctx, ", line)
        line = MEM.sub(lambda a: f"bw_{a.group(1)}{a.group(2)}_out(&bw_out, ctx, 0x{pc}u, {suffix}u, ", line)
        out.append(line)
    out.append("        if (__builtin_expect(bw_out, 0)) {")
    # The deadline test as the copy made it, without its goto, then the
    # original block at the next instruction (whether refunded or not).
    out.extend("        " + line for line in test[:length - 2])
    out.append("            }")
    out.append(f"            goto {target};")
    out.append("        }")
    out.append("    }")
    return out


def straight_copy(lines):
    """The copy's lines with its accesses straight, or None if none qualified."""
    out, changed, clean = [], 0, True
    for item in units(lines):
        if item[0] == "line":
            if calls(item[1]) or AT.search(item[1]) or MEM.search(item[1]):
                clean = False
            out.append(item[1])
            continue
        _, pre, comment, body, test = item
        straight = straighten(pre, comment, body, test)
        if straight is not None:
            out.extend(straight)
            if not clean:
                out.extend(test)
            clean = True
            changed += 1
            continue
        out.extend(pre)
        out.append(comment)
        out.extend(body)
        text = "\n".join(body)
        if test is not None:
            out.extend(test)
            clean = True
        elif calls(text) or AT.search(text) or MEM.search(text) or "ppc_fp_available_inline" in text:
            clean = False
    return (out if changed else None), changed


def excluded(text, chunk):
    """The main function's copies that native_entries.py's certified fragments jump to."""
    names = set()
    for _, _, fragments, _ in native_entries.ENTRIES.values():
        for start_chunk, start, end in fragments:
            if start_chunk != chunk:
                continue
            function = native_entries.main_function(text, chunk)
            piece = native_entries.fragment(function, start, end) if function else None
            if piece is not None:
                names.update(native_entries.FAST_JUMP.findall(piece[0]))
    return {int(name.split("_")[1]) for name in names}


def transform(text, chunk):
    if MARK in text or FAST_MARK not in text:
        return text, 0
    skip = excluded(text, chunk) if chunk is not None else set()
    lines = text.split("\n")
    out, i, total, function = [], 0, 0, None
    while i < len(lines):
        f = FUNCTION.match(lines[i])
        if f:
            function = f.group(1)
        m = COPY.match(lines[i])
        if not m:
            out.append(lines[i])
            i += 1
            continue
        n = int(m.group(1))
        j = i + 1
        while j < len(lines) and not (END.match(lines[j]) and int(END.match(lines[j]).group(1)) == n):
            j += 1
        if j >= len(lines):
            raise ValueError(f"copy bwfast_{n} has no end")
        body = lines[i + 1:j]
        main = chunk is not None and re.fullmatch(rf"func_{chunk:08X}(?:__mod_\w+)?", function or "") is not None
        straight, count = (None, 0) if main and n in skip else straight_copy(body)
        out.append(lines[i])
        out.extend(straight if straight is not None else body)
        out.append(lines[j])
        total += count
        i = j + 1
    converted = "\n".join(out)
    if total:
        converted = converted.replace(FAST_MARK, FAST_MARK + MARK, 1)
    return converted, total


def main():
    root = Path(sys.argv[1])
    chunks = sorted(root.glob("chunks_*/*.c"))
    if not chunks:
        sys.exit(f"no chunks under {root}")
    instructions = files = 0
    for path in chunks:
        match = re.search(r"_([0-9A-F]{8})\.c$", path.name)
        with open(path, encoding="utf-8", newline="") as file:
            original = file.read()
        converted, count = transform(original, int(match.group(1), 16) if match else None)
        if count:
            temporary = path.with_suffix(".c.tmp")
            with open(temporary, "w", encoding="utf-8", newline="") as file:
                file.write(converted)
            temporary.replace(path)
            instructions += count
            files += 1
    print(f"straight prepaid copies: {instructions} instructions in {files} chunks")


if __name__ == "__main__":
    main()
