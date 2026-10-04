#!/usr/bin/env python3
"""Call across translated chunks directly (cmake/composite/direct_calls.h).

  direct_calls.py COMPOSITE_SRC

A bl whose target is in another chunk is translated as "set lr and pc, leave
the chunk": the chassis loop then dispatches the target, and the callee's blr
leaves its chunk the same way to come back. Such calls and their returns are
about half of all block boundaries in play (the boundary census, over Outset).
At each one this adds a direct path: when bw_direct_call_ready says the loop
and the host's edge service would have nothing to do, the callee's chunk is
called through the chunk table, and if control comes back to the return
address with nothing to do there either, the caller carries on at its block
there. Otherwise the chunk leaves as before, with ctx->pc where the guest is.

Calls into the main executable's code are rewritten, including the REL
modules' calls to it through its 0xC0 mirror (which the dispatcher otherwise
resolves on its slow path), and calls between a REL module's chunks. None
whose target or return address the host names (runtime/host/src,
windows/src, either mirror form) is: the host hooks those addresses in its
edge service, which a direct call does not consult. Calls to the register save and restore routines are already inline
(scripts/windows/inline_save_restore_gpr.py) and keep that form. A call to a
leaf with a native form (the SDK's matrix and vector leaves: DISPATCHER_NATIVE,
VEC_LEAVES) tries the native first and runs the translated body where it
declines. The same test lets an instruction handed to the interpreter carry on
in its chunk instead of leaving it (transform_fallback).

The change is repeatable (a prepared chunk is left as it is) and keeps LF line
ends. Run it after inline_save_restore_gpr.py and before
scripts/mods/prepare_simulation_60hz.py, whose manifest hashes the chunks as
they finally are.
"""
import bisect
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MARK = "/* bluewake: direct calls between chunks (cmake/composite/direct_calls.h) */\n"
INCLUDE = '#include "../generated.h"\n'
CALL = re.compile(
    r"    // ([0-9A-F]{8}): bl      0x([0-9A-F]{8})\n"
    r"    \{\n"
    r"            ctx->lr = 0x([0-9A-F]{8})u;\n"
    r"            ctx->pc = 0x\2u;\n"
    r"            return;\n"
    r"    \}\n")
FUNCTION = re.compile(r"^(?:static )?void \w+\(CPUState\* ctx_param\) \{$", re.M)
TABLE = re.compile(r"static DolRecompFunction s_dolrecomp_chunk_fns\[\] = \{(.*?)\};", re.S)
DOL_CODE = (0x80003100, 0x80400000)
REL_CODE = (0xC0400000, 0xC2000000)  # the REL modules' translated code (rel_modules.inc)
MIRROR = 0x40000000
# Entries the dispatcher answers with native code when BLUEWAKE_NATIVE_MATH is
# on (cmake/composite/native_math.c: PSMTXCopy, PSMTXConcat, PSMTXMultVec,
# PSMTXMultVecArray). A direct call to one tries the native first, as the
# dispatcher does (bw_native_call), and runs the translated body where the
# native declines.
DISPATCHER_NATIVE = {0x8030D0C8, 0x8030D0FC, 0x8030DA44, 0x8030DA98}
# The SDK vector leaves cmake/composite/native_vec.c runs natively (PSVECAdd,
# PSVECSubtract, PSVECScale, PSVECSquareMag, PSVECDotProduct,
# PSVECCrossProduct, PSVECSquareDistance, PSVECNormalize, PSVECMag): start ->
# (end, the hash of the translated body tests/native_vec_test.c compared them
# against). A direct call tries the native first only where every translation
# of the leaf has that body.
VEC_LEAVES = {
    0x8030DCE0: (0x8030DD04, "58694b1d26c00948de83054d0bf3be20e59c95bc45172aa813b04f5f45e3799f"),
    0x8030DD04: (0x8030DD28, "9c5e38c526a3a92ef067c8283b8b8c0dfa3a728fd860eacbfb181b9a65871916"),
    0x8030DD28: (0x8030DD44, "c1c5adae1761a7944e710bf410dac0b608e5f9953cfdb65db1acd99353b981f9"),
    0x8030DE50: (0x8030DE68, "0a5077731582f5f871952bee6ba70d1ac05eeb54feb0c8a498eb307774cd7ea8"),
    0x8030DEAC: (0x8030DECC, "62a35df011a51e2aef7a0db1bfa512390017e42567a52275ea91e01c3398607b"),
    0x8030DECC: (0x8030DF08, "a1eedeef0d07eb445d5d0b52313325a54945cb299e4abc81b058c2107676fe04"),
    0x8030E0B4: (0x8030E0DC, "6f48e68ae5da2ea1221917e6486557c9016dc527a74d3e503dce9306b313f9fb"),
    0x8030DE0C: (0x8030DE50, "3c64781d1609e952cae0ef86ac223dbc9847175b09b00487805b8fb1c3bab731"),
    0x8030DE68: (0x8030DEAC, "ed8be5c3e04ec0bb6b8b8af13095e391acb4291521073be27f3c74ec63909313"),
}


def watched_addresses():
    """Guest addresses the host names: its edge service may act at any of them.
    Both mirror forms of each: the service tests a boundary's address with the
    0x40000000 bit cleared (host_canonical_linked_pc), so a REL chunk's
    0xC1E01B88 is its 0x81E01B88. Text between `bluewake-unwatched-begin` and
    `bluewake-unwatched-end` is left out: addresses only a diagnostic reads,
    whose switch also turns direct calls off."""
    found = set()
    for folder in ("runtime/host/src", "windows/src"):
        for path in (ROOT / folder).rglob("*"):
            if path.suffix in (".c", ".h", ".cpp", ".mm", ".m"):
                text = re.sub(r"bluewake-unwatched-begin.*?bluewake-unwatched-end", "",
                              path.read_text(errors="replace"), flags=re.S)
                for m in re.finditer(r"0x([8C][0-9A-Fa-f]{7})u?\b", text):
                    address = int(m.group(1), 16)
                    found.update((address, address | 0x40000000, address & ~0x40000000))
    return found


def chunk_table(root):
    """The dispatch table's order: chunk start address -> index."""
    text = (root / "generated_composite.h").read_text()
    body = TABLE.search(text)
    if body is None:
        raise ValueError("generated_composite.h: no s_dolrecomp_chunk_fns table")
    starts = [int(m.group(1), 16) for m in re.finditer(r"func_([0-9A-F]{8})\b", body.group(1))]
    return starts, {start: index for index, start in enumerate(starts)}


def resolve(target):
    """The address the dispatcher runs for a call to `target`, or None when a
    direct call cannot stand in for it: main-executable code; that code through
    its 0xC0 mirror, which REL modules call it by and which the dispatcher
    strips before it looks the code up (dolrecomp_call_slow); or REL code."""
    if DOL_CODE[0] <= target < DOL_CODE[1]:
        return target
    if DOL_CODE[0] | MIRROR <= target < DOL_CODE[1] | MIRROR:
        return target & ~MIRROR
    if REL_CODE[0] <= target < REL_CODE[1]:
        return target
    return None


def certified_vec_leaves(chunks):
    """The vector leaves whose every translation (the base chunk and any mod's
    variant of it) is the body the native was compared against."""
    found = {start: 0 for start in VEC_LEAVES}
    certified = set(VEC_LEAVES)
    # The chunk that starts at 0x8030D6E0 holds them all; none found, none certified.
    for path in (p for p in chunks if "8030D6E0" in p.name):
        text = path.read_text(encoding="utf-8")
        for start, (end, expected) in VEC_LEAVES.items():
            begin, finish = text.find(f"\nlabel_{start:08X}:"), text.find(f"\nlabel_{end:08X}:")
            if begin < 0:
                continue
            found[start] += 1
            body = " ".join(text[begin:finish].split()) if finish > begin else ""
            if hashlib.sha256(body.encode()).hexdigest() != expected:
                certified.discard(start)
    return {start for start in certified if found[start]}


def transform(text, own_start, starts, index_of, watched, natives=DISPATCHER_NATIVE):
    if MARK in text:
        return text, 0
    if INCLUDE not in text:
        raise ValueError("no generated.h include")
    all_starts = sorted(starts)
    bounds = [m.start() for m in FUNCTION.finditer(text)] + [len(text)]
    out, done, last = [], 0, 0
    for begin, end in zip(bounds, bounds[1:]):
        body = text[begin:end]
        pieces, cursor = [], 0
        for m in CALL.finditer(body):
            site, target, ret = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)
            run = resolve(target)
            if run is None:
                continue
            i = bisect.bisect_right(all_starts, run) - 1
            if i < 0 or all_starts[i] == own_start:
                continue
            chunk = all_starts[i]
            if (target in watched or run in watched or ret in watched
                    or site in watched or ret != site + 4
                    or f"\nlabel_{ret:08X}:\n" not in body):
                continue
            # The dispatcher enters the callee with ctx->pc at the address it
            # runs; a mirrored call leaves the original pc if it goes round.
            enter = f"                ctx->pc = 0x{run:08X}u;\n" if run != target else ""
            translated = (
                "                bw_direct_depth++;\n"
                f"                bw_chunk_fns[{index_of[chunk]}](ctx);\n"
                "                bw_direct_depth--;\n")
            if run in natives:
                translated = (
                    f"                if (!bw_native_call(ctx, 0x{run:08X}u)) {{\n"
                    + translated.replace("                ", "                    ") +
                    "                }\n")
            pieces.append(body[cursor:m.start()])
            pieces.append(
                f"    // {m.group(1)}: bl      0x{m.group(2)}\n"
                "    {\n"
                f"            ctx->lr = 0x{m.group(3)}u;\n"
                f"            ctx->pc = 0x{m.group(2)}u;\n"
                "            if (bw_direct_call_ready(ctx)) {\n"
                + enter + translated +
                f"                if (ctx->pc == 0x{m.group(3)}u && bw_direct_call_ready(ctx))\n"
                f"                    goto label_{m.group(3)};\n"
                "            }\n"
                "            return;\n"
                "    }\n")
            cursor = m.end()
            done += 1
        pieces.append(body[cursor:])
        out.append(text[last:begin])
        out.append("".join(pieces))
        last = end
    out.append(text[last:])
    converted = "".join(out)
    if done:
        converted = converted.replace(INCLUDE, INCLUDE + MARK + '#include "direct_calls.h"\n', 1)
    return converted, done


INDIRECT_MARK = "/* bluewake: indirect calls run directly (cmake/composite/direct_calls.h) */\n"
INDIRECT = re.compile(
    r"    // ([0-9A-F]{8}): bctrl\n"
    r"    \{\n"
    r"        u32 target = ctx->ctr & ~3u;\n"
    r"        bool ctr_ok = true;\n"
    r"        bool cr_ok = true;\n"
    r"        if \(ctr_ok && cr_ok\) \{\n"
    r"            ctx->lr = 0x([0-9A-F]{8})u;\n"
    r"            ctx->pc = target;\n"
    r"            return;\n"
    r"        \}\n"
    r"    \}\n")


def transform_indirect(text, watched):
    """A bctrl's target is known only when it runs: bw_call_translated checks it
    against the host's watch list and runs it as the dispatcher would."""
    if INDIRECT_MARK in text:
        return text, 0
    if INCLUDE not in text:
        raise ValueError("no generated.h include")
    bounds = [m.start() for m in FUNCTION.finditer(text)] + [len(text)]
    out, done, last = [], 0, 0
    for begin, end in zip(bounds, bounds[1:]):
        body = text[begin:end]
        pieces, cursor = [], 0
        for m in INDIRECT.finditer(body):
            site, ret = int(m.group(1), 16), int(m.group(2), 16)
            if (ret in watched or site in watched or ret != site + 4
                    or f"\nlabel_{ret:08X}:\n" not in body):
                continue
            pieces.append(body[cursor:m.start()])
            pieces.append(
                f"    // {m.group(1)}: bctrl\n"
                "    {\n"
                "        u32 target = ctx->ctr & ~3u;\n"
                "        bool ctr_ok = true;\n"
                "        bool cr_ok = true;\n"
                "        if (ctr_ok && cr_ok) {\n"
                f"            ctx->lr = 0x{m.group(2)}u;\n"
                "            ctx->pc = target;\n"
                "            if (bw_direct_call_ready(ctx) && bw_call_translated(ctx, target) &&\n"
                f"                ctx->pc == 0x{m.group(2)}u && bw_direct_call_ready(ctx))\n"
                f"                goto label_{m.group(2)};\n"
                "            return;\n"
                "        }\n"
                "    }\n")
            cursor = m.end()
            done += 1
        pieces.append(body[cursor:])
        out.append(text[last:begin])
        out.append("".join(pieces))
        last = end
    out.append(text[last:])
    converted = "".join(out)
    if done:
        header = "" if MARK in converted else '#include "direct_calls.h"\n'
        converted = converted.replace(INCLUDE, INCLUDE + INDIRECT_MARK + header, 1)
    return converted, done


FALLBACK_MARK = "/* bluewake: interpreted instructions continue in the chunk (cmake/composite/direct_calls.h) */\n"
FALLBACK = re.compile(
    r"    // ([0-9A-F]{8}): ([^\n]*)\n"
    r"    ppc_fallback_instruction\(ctx, (0x[0-9A-F]{8}u), 0x\1u\);\n"
    r"    return;\n")


def transform_fallback(text, watched):
    """An instruction the translation hands to the interpreter (the cache
    operations, the OS's special registers) ends its chunk: the chassis loop
    then dispatches the next address, which is a block the chunk already has.
    Where the interpreter has moved on to that address and the loop and the
    edge service would have nothing to do, the chunk carries on there instead.
    Every such block sets cycle_block_prepaid before it reads it, as it does
    when a dispatch enters it."""
    if FALLBACK_MARK in text:
        return text, 0
    if INCLUDE not in text:
        raise ValueError("no generated.h include")
    bounds = [m.start() for m in FUNCTION.finditer(text)] + [len(text)]
    out, done, last = [], 0, 0
    for begin, end in zip(bounds, bounds[1:]):
        body = text[begin:end]
        pieces, cursor = [], 0
        for m in FALLBACK.finditer(body):
            site = int(m.group(1), 16)
            following = site + 4
            if (site in watched or following in watched
                    or resolve(following) is None
                    or f"\nlabel_{following:08X}:\n" not in body):
                continue
            pieces.append(body[cursor:m.start()])
            pieces.append(
                f"    // {m.group(1)}: {m.group(2)}\n"
                f"    ppc_fallback_instruction(ctx, {m.group(3)}, 0x{m.group(1)}u);\n"
                f"    if (ctx->pc == 0x{following:08X}u && bw_direct_call_ready(ctx))\n"
                f"        goto label_{following:08X};\n"
                "    return;\n")
            cursor = m.end()
            done += 1
        pieces.append(body[cursor:])
        out.append(text[last:begin])
        out.append("".join(pieces))
        last = end
    out.append(text[last:])
    converted = "".join(out)
    if done:
        header = "" if (MARK in converted or INDIRECT_MARK in converted) else '#include "direct_calls.h"\n'
        converted = converted.replace(INCLUDE, INCLUDE + FALLBACK_MARK + header, 1)
    return converted, done


def write_watch_list(root, watched):
    """bw_edge_watch.inc: the canonical addresses (0x40000000 clear) the host's
    edge service acts at, for the module's filter (cmake/composite/direct_calls.c)."""
    canonical = sorted({a & ~MIRROR for a in watched})
    lines = ["/* Generated by scripts/windows/direct_calls.py: the guest addresses the",
             " * host names (runtime/host/src, windows/src), canonical. */",
             "static const u32 bw_edge_watch_list[] = {"]
    lines += [f"    0x{a:08X}u," for a in canonical]
    lines.append("};")
    target = root / "bw_edge_watch.inc"
    text = "\n".join(lines) + "\n"
    if not target.exists() or target.read_text() != text:
        with open(target, "w", encoding="utf-8", newline="") as file:
            file.write(text)
    return len(canonical)


def main():
    root = Path(sys.argv[1])
    chunks = sorted(root.glob("chunks_*/*.c"))
    if not chunks:
        sys.exit(f"no chunks under {root}")
    starts, index_of = chunk_table(root)
    watched = watched_addresses()
    vec = certified_vec_leaves(chunks)
    natives = DISPATCHER_NATIVE | vec
    sites = files = indirect = fallback = 0
    for path in chunks:
        m = re.search(r"_([0-9A-F]{8})\.c$", path.name)
        own_start = int(m.group(1), 16) if m else None
        with open(path, encoding="utf-8", newline="") as file:
            original = file.read()
        converted, count = transform(original, own_start, starts, index_of, watched, natives)
        converted, count_indirect = transform_indirect(converted, watched)
        converted, count_fallback = transform_fallback(converted, watched)
        if count or count_indirect or count_fallback:
            temporary = path.with_suffix(".c.tmp")
            with open(temporary, "w", encoding="utf-8", newline="") as file:
                file.write(converted)
            temporary.replace(path)
            sites += count
            indirect += count_indirect
            fallback += count_fallback
            files += 1
    listed = write_watch_list(root, watched)
    print(f"direct calls between chunks: {sites} calls, {indirect} indirect calls and "
          f"{fallback} interpreted instructions in {files} chunks; {listed} watched addresses; "
          f"{len(vec)} of {len(VEC_LEAVES)} vector leaves native")


if __name__ == "__main__":
    main()
