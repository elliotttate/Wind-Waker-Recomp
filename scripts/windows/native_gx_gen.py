#!/usr/bin/env python3
"""Write cmake/composite/native_gx_gen.inc: the fifth set's natives (the GX
SDK's FIFO writers, native_gx.c), each a translated function's prepaid
blocks replayed on local registers (cmake/composite/native_gx_run.h).

  native_gx_gen.py COMPOSITE_SRC [OUT]

A developer's tool, run by hand on a builder's composite-src; its output is
committed, and scripts/windows/native_entries.py hooks each native only where
the translation hashes to the one its comparison test (tests/native_gx_test.c)
was run against - the text this read.

Each native is the translation's own C for every instruction on its path,
taken from the blocks' prepaid copies (scripts/windows/fast_blocks.py,
lean_memory.py) - or a block's main path where it has none - with:

- the guest registers in locals (ctx->gpr[3] becomes g3, ctx->fpr[1] F1,
  ctx->ps1[1] P1, ctx->cr cr, ...), written back at the end;
- each block's leader as native_gx_run.h's gx_block (prepaid, not spent), so
  the per-instruction charges and the prepaid copies' deadline refunds - which
  cannot happen on such a path - are left out, as are the pc stores;
- each load through gx_ldN (plain RAM), each store through gx_stN or gx_stpN
  (RAM in place, its old bytes kept; or the gather pipe's bytes collected),
  the suffixes the copies store kept; the inline register saves and restores
  (inline_save_restore_gpr.py) where the budget allows them;
- the FP arithmetic through native_replay.h's inline paths on the run's FPSCR
  (fadds, fsubs, fmuls, fdivs, fcmpo, ...) and the interpreter's own fctiw,
  frsqrte and frsp;
- the branches between its blocks as gotos; a call as a goto with the return
  address in the local LR, a return as a dispatch on it; a boundary between
  chunks (a direct call, a call or bctr that leaves the chunk, a branch into
  another chunk, a return into another chunk) only where gx_silent says the
  host would not be asked;
- a call to anything not replayed here (or a jump the native does not know)
  declining (gx_decline puts back what the run stored); or, for a native that
  `stops`, where the call leaves the chunk, the run ending there with pc and
  LR as the translation leaves them for the chassis (and a `resumes` hook at
  the return address taking the rest).

Anything else in the text (an instruction form not listed here) stops the
script, so a translator change cannot slip through unnoticed.
"""
import re
import sys
from pathlib import Path

LEADER = re.compile(
    r"\nlabel_([0-9A-F]{8}):\n(?:    (?!ctx->pc = |cycle_block_prepaid = )[^\n]*\n)*?"
    r"(?:    ctx->pc = 0x\1u;\n)?    cycle_block_prepaid = dolrecomp_block_can_precharge\(ctx, (\d+)u\);\n")


class Chunk:
    def __init__(self, root, start):
        paths = sorted((root / "chunks_dol").glob(f"chunk_*_{start:08X}.c"))
        if len(paths) != 1:
            raise SystemExit(f"no chunk at {start:08X}")
        self.start = start
        self.path = paths[0]
        text = self.path.read_text(encoding="utf-8")
        begin = text.find(f"\nvoid func_{start:08X}(CPUState* ctx_param) {{\n")
        end = text.find("\n}\n", begin)
        self.fn = text[begin:end + 3]
        dispatch = self.fn.find(f"\nreturn_dispatch_{start:08X}:\n")
        self.main = self.fn[:dispatch]
        rest = self.fn[dispatch:]
        # The return dispatch's in-chunk targets.
        switch_end = rest.find("    default: return;")
        self.returns = {int(a, 16) for a in re.findall(r"case 0x([0-9A-F]{8})u: goto label_", rest[:switch_end])}
        # The prepaid copies.
        self.copies = {}
        for m in re.finditer(r"\n(bwfast_\d+):\n", rest):
            stop = rest.find("\nbwfast_", m.end() - 1)
            if stop < 0:
                stop = rest.rfind("\n}\n")
            self.copies[m.group(1)] = rest[m.end():stop + 1]
        # The block leaders, in order, with their cycles, main-path bodies and copies.
        self.leaders = []
        self.blocks = {}
        found = list(LEADER.finditer(self.main))
        for i, m in enumerate(found):
            address = int(m.group(1), 16)
            body_end = found[i + 1].start() if i + 1 < len(found) else len(self.main)
            body = self.main[m.end():body_end]
            fast = re.search(r"    if \(cycle_block_prepaid\) goto (bwfast_\d+);\n", body)
            self.leaders.append(address)
            self.blocks[address] = {"cycles": int(m.group(2)), "body": body,
                                    "copy": fast.group(1) if fast else None}
        self.next_leader = {a: b for a, b in zip(self.leaders, self.leaders[1:])}
        # bwend_K -> the leader that follows block K's main path.
        self.bwend = {}
        for address in self.leaders:
            m = re.search(r"\nbwend_(\d+): ;", self.blocks[address]["body"])
            if m:
                self.bwend[m.group(1)] = self.next_leader.get(address)

    def owns(self, address):
        return self.start <= address < self.start + 0x4000


class Native:
    """One native: its name, entry, the function ranges it replays
    [(start, end)], and the expected targets of its indirect jumps and calls
    {site address: [targets]}."""

    def __init__(self, name, entry, ranges, indirect=None, stops=False, resumes=()):
        self.name, self.entry, self.ranges = name, entry, ranges
        self.indirect = indirect or {}
        # stops: at a call into another chunk to code not replayed here, the
        # run ends where the translation leaves its chunk for the chassis
        # (pc the callee, LR the return address), instead of declining.
        # resumes: return addresses where a hook runs the rest natively.
        self.stops, self.resumes = stops, tuple(resumes)


class Gen:
    def __init__(self, root):
        self.root = root
        self.chunks = {}

    def chunk(self, address):
        start = 0x800016E0 + ((address - 0x800016E0) // 0x4000) * 0x4000
        if start not in self.chunks:
            self.chunks[start] = Chunk(self.root, start)
        return self.chunks[start]

    def in_ranges(self, native, address):
        return any(a <= address < b for a, b in native.ranges)

    # --- Text transforms --------------------------------------------------

    def transform(self, native, chunk, leader, text, from_copy):
        out = self.inline_gpr(text)
        # Nothing after an unconditional transfer (blr, b, bctr) runs; the
        # copies chain the next block's text after it all the same.
        m = re.search(r"\n    // [0-9A-F]{8}: (?:blr|b +0x[0-9A-F]{8}|bctr)\n    \{\n(?:        [^\n]*\n)*    \}\n", out)
        if m:
            out = out[:m.end()]
            from_copy = True  # nothing falls through
        # Hooks of other natives (other sets') are not part of the translation.
        out = re.sub(r"    if \(bluewake_native_\w+_enabled &&[^\n]*\n(?:        [^\n]*\n)*?"
                     r"        goto return_dispatch_[0-9A-F]{8};\n", "", out)
        out = re.sub(r"(?m)^    /\* bluewake: recovered J3D matrix [^\n]*\*/\n", "", out)
        # A leader inside the text (a block fast_blocks.py did not make a copy
        # for, chained into the copy before it, or the next block on the main
        # path): its own start, as gx_block.
        out = re.sub(r"(?:    ctx->pc = 0x[0-9A-F]{8}u;\n)?    cycle_block_prepaid = dolrecomp_block_can_precharge\(ctx, (\d+)u\);\n"
                     r"    if \(ctx->downcount <= -\(s64\)DOLRECOMP_C_LOOP_CYCLE_BUDGET\) \{\n"
                     r"        ctx->pc = 0x([0-9A-F]{8})u;\n        return;\n    \}\n"
                     r"    ctx->downcount -= cycle_block_prepaid \? \1u : \d+u;\n",
                     r"    /* \2: a block of \1 cycles */\n    if (!gx_block(&s, \1u))\n        return 0;\n", out)
        # The prepaid copies' deadline refunds and the main path's.
        out = re.sub(r"    if \(ctx->cycle_deadline_budget > 0 &&\n        \(s64\)[^\n]*\n(?:        [^\n]*\n)*?    \}\n",
                     "", out)
        out = re.sub(r"    if \(cycle_block_prepaid &&\n        ctx->cycle_deadline_budget > 0 &&\n[^\n]*\n"
                     r"(?:        [^\n]*\n)*?    \}\n", "", out)
        out = re.sub(r"    if \(!cycle_block_prepaid && !dolrecomp_charge_precise\(ctx, \d+u, 0x[0-9A-F]{8}u\)\) return;\n",
                     "", out)
        out = re.sub(r"(?m)^(?:bwslow_\d+_\d+|bwend_\d+): ;\n", "", out)
        out = re.sub(r"(?m)^label_[0-9A-F]{8}:\n", "", out)
        out = re.sub(r"    ctx->downcount -= cycle_block_prepaid \? \d+u : \d+u;\n", "", out)
        out = re.sub(r"    if \(cycle_block_prepaid\) goto bwfast_\d+;\n", "", out)
        # The suffix stores.
        out = re.sub(r"ctx->cycle_observation_suffix = (?:cycle_block_prepaid \? )?(\d+)u(?: : 0u)?;",
                     r"s.suffix = \1u;", out)
        out = re.sub(r"    if \(!ppc_fp_available_inline\(ctx, 0x[0-9A-F]{8}u\)\) return;\n", "", out)
        if "ppc_fp_available_inline" in text:
            native.fp = True
        out = re.sub(r"\n        if \(ctx->exception\) return;", "", out)

        # Calls and transfers (before the generic pc removal).
        out = self.calls(native, chunk, out)

        # stmw and lmw: unrolled.
        def unroll(m):
            first, body = int(m.group(1)), m.group(2)
            lines = []
            for r in range(first, 32):
                lines.append("        " + body.replace("ctx->gpr[r]", f"ctx->gpr[{r}]") + "\n        ea += 4;")
            return "\n".join(lines)
        out = re.sub(r"        for \(u32 r = (\d+); r < 32; r\+\+, ea \+= 4\) ([^\n]*;)", unroll, out)

        # Instruction addresses, for the pc a store hands the pipe.
        def stores(m):
            return m.group(0)
        out = re.sub(r"    ctx->pc = 0x[0-9A-F]{8}u;\n", "", out)
        # Loads and stores.
        out = re.sub(r"bw_read(8|16|32|64)_at\(ctx, 0x[0-9A-F]{8}u, \d+u, ", r"gx_ld\1(&s, ", out)
        out = re.sub(r"bw_write(8|16|32|64)_at\(ctx, (0x[0-9A-F]{8}u), (\d+u), ", r"gx_st\1(&s, \2, \3, ", out)
        out = re.sub(r"\bmem_read(8|16|32|64)\(ctx, ", r"gx_ld\1(&s, ", out)
        out = self.plain_stores(out)
        # Paired singles (type 0 by the GQR named; gx_psq_ready at entry).
        def psq_l(m):
            reg, w, gqr, indexed = m.group(1), m.group(2), int(m.group(3)), m.group(4)
            native.psq_load |= 1 << gqr
            native.psq_indexed |= indexed == "true"
            return f"gx_psq_l(&s, &F{reg}, &P{reg}, ea, {w});"
        out = re.sub(r"ppc_psq_load_inline\(ctx, (\d+)u, ea, (true|false), (\d+)u, (true|false), 0x[0-9A-F]{8}u\);",
                     psq_l, out)

        def psq_st(m):
            reg, w, gqr, indexed, pc = m.group(1), m.group(2), int(m.group(3)), m.group(4), m.group(5)
            native.psq_store |= 1 << gqr
            native.psq_indexed |= indexed == "true"
            return f"gx_psq_st(&s, 0x{pc}u, F{reg}, P{reg}, ea, {w});"
        out = re.sub(r"ppc_psq_store_inline\(ctx, (\d+)u, ea, (true|false), (\d+)u, (true|false), 0x([0-9A-F]{8})u\);",
                     psq_st, out)
        for a, b in (("dolrecomp_f32_from_bits", "gx_f32_from_bits"), ("dolrecomp_f32_to_bits", "gx_f32_to_bits"),
                     ("dolrecomp_f64_from_bits", "gx_f64_from_bits"), ("dolrecomp_f64_to_bits", "gx_f64_to_bits"),
                     ("dolrecomp_rotl32", "gx_rotl32")):
            out = out.replace(a, b)
        # Registers.
        out = re.sub(r"ctx->gpr\[(\d+)\]", r"g\1", out)
        out = re.sub(r"ctx->fpr\[(\d+)\]", r"F\1", out)
        out = re.sub(r"ctx->ps1\[(\d+)\]", r"P\1", out)
        for field in ("cr", "xer", "lr", "ctr"):
            out = re.sub(rf"ctx->{field}\b", field, out)
        out = re.sub(r"ctx->fpscr\b", "s.fp.fpscr", out)
        # FP arithmetic (native_gx_run.h): the single and double operations
        # on the run's FPSCR, the compares into their CR field, and the
        # interpreter's own fctiw, frsqrte and frsp.
        before = out
        for op in ("fadds", "fsubs", "fmuls", "fdivs"):
            out = re.sub(rf"ppc_{op}\(ctx, (\d+), (\d+), (\d+)\);",
                         rf"F\1 = P\1 = nr_{op}(&s.fp, F\2, F\3);", out)
        for op in ("fadd", "fsub", "fmul"):
            out = re.sub(rf"ppc_{op}\(ctx, (\d+), (\d+), (\d+)\);", rf"F\1 = gx_{op}(&s.fp, F\2, F\3);", out)
        out = re.sub(r"ppc_fcmp\(ctx, (\d+), (F\d+), (F\d+), (?:true|false)\);",
                     r"gx_fcmp(&s.fp, &cr, \1u, \2, \3);", out)
        out = re.sub(r"ppc_fctiw\(ctx, ", "gx_fctiw(&s.fp, ", out)
        out = re.sub(r"ppc_frsqrte\(ctx, ", "gx_frsqrte(&s.fp, ", out)
        out = re.sub(r"ppc_frsp\(ctx, (\d+), (\d+)\);", r"gx_frsp(&s.fp, &F\1, &P\1, F\2);", out)
        if out != before:
            native.fp_arith = True
        # Branches.
        def branch(m):
            target = int(m.group(1), 16)
            return self.goto(native, chunk, target)
        out = re.sub(r"goto label_([0-9A-F]{8});", branch, out)

        def fall(m):
            if m.group(1) not in chunk.bwend:
                raise SystemExit(f"{native.name}: no block after bwend_{m.group(1)}")
            nxt = chunk.bwend[m.group(1)]
            # Past the chunk's last block: the next chunk, across a boundary.
            return self.goto(native, chunk, nxt) if nxt is not None else self.jump(native, chunk.start + 0x4000)
        out = re.sub(r"goto bwend_(\d+);", fall, out)
        if not from_copy:
            nxt = chunk.next_leader.get(leader)
            out = out.rstrip("\n") + "\n    " + (self.goto(native, chunk, nxt) if nxt is not None
                                                else self.jump(native, chunk.start + 0x4000)) + "\n"
        # Anything left over is a form this script does not know.
        for bad in ("ctx", "return;", "bwslow", "cycle_block_prepaid", "goto label_", "bw_direct", "bw_chunk_fns",
                    "bw_native_call", "ppc_"):
            if bad in out:
                line = next(l for l in out.split("\n") if bad in l)
                raise SystemExit(f"{native.name}: block {leader:08X}: unhandled `{bad}`: {line.strip()}")
        return out

    def plain_stores(self, text):
        """mem_writeN(ctx, ea, value) -> gx_stpN(&s, pc, ea, value), pc the
        instruction's (its comment above)."""
        out, last = [], None
        for line in text.split("\n"):
            m = re.match(r"\s+// ([0-9A-F]{8}): ", line)
            if m:
                last = m.group(1)
            if "mem_write" in line and "bw_mem_write" not in line:
                if last is None:
                    raise SystemExit(f"a store with no instruction: {line}")
                line = re.sub(r"\bmem_write(8|16|32|64)\(ctx, ", rf"gx_stp\1(&s, 0x{last}u, ", line)
            out.append(line)
        return "\n".join(out)

    def goto(self, native, chunk, target):
        if target is None:
            raise SystemExit(f"{native.name}: a fall-through past the end of a chunk")
        if not self.in_ranges(native, target):
            return f"return 0; /* {target:08X}: not replayed here */"
        native.reach.add(target)
        return f"goto B_{target:08X};"

    def jump(self, native, target):
        """A jump that leaves the chunk (a branch or fall-through into the
        next chunk): the chassis loop dispatches the target."""
        if not self.in_ranges(native, target):
            return f"return 0; /* {target:08X}: not replayed here */"
        native.reach.add(target)
        native.boundaries.add(target)
        return f"if (!gx_silent(&s, 0x{target:08X}u))\n        return 0;\n    goto B_{target:08X};"

    def inline_gpr(self, text):
        # The inline _savegpr_N/_restgpr_N (inline_save_restore_gpr.py): its
        # cycles and accesses when the budget allows them, then the turn's
        # test, and on to the return address; the routine itself (through the
        # chassis) otherwise, which declines here.
        def save_restore(m):
            ret, room, cycles, body = m.group(1), m.group(2), m.group(3), m.group(4)
            return (f"            lr = 0x{ret}u;\n"
                    f"            if (!(s.downcount - {room} > -s.budget))\n"
                    f"                return 0; /* the routine itself, through the chassis */\n"
                    f"            s.downcount -= {cycles};\n"
                    + body.replace("                ", "            ") +
                    "            s.suffix = 0u;\n"
                    "            if (!gx_live(&s))\n"
                    "                return 0;\n"
                    f"            goto label_{ret};\n")
        return re.sub(
            r"            ctx->lr = 0x([0-9A-F]{8})u;\n"
            r"            /\* bluewake: _savegpr/_restgpr inline \(scripts/windows/inline_save_restore_gpr\.py\) \*/\n"
            r"            if \(ctx->downcount - (\d+) > -\(s64\)DOLRECOMP_C_LOOP_CYCLE_BUDGET\) \{\n"
            r"                ctx->downcount -= (\d+);\n"
            r"((?:                (?!ctx->cycle_observation_suffix)[^\n]*\n)*?)"
            r"                ctx->cycle_observation_suffix = 0u;\n"
            r"                ctx->pc = 0x\1u;\n"
            r"                if \(ctx->downcount <= -\(s64\)DOLRECOMP_C_LOOP_CYCLE_BUDGET \|\| ctx->exception != 0u \|\|\n"
            r"                    \(ctx->cycle_budget > 0 && ctx->downcount <= -ctx->cycle_budget\)\)\n"
            r"                    return;\n"
            r"                goto label_\1;\n"
            r"            \}\n"
            r"            ctx->pc = 0x[0-9A-F]{8}u;\n"
            r"            return;\n", save_restore, text)

    def calls(self, native, chunk, text):
        # A direct call into another chunk (scripts/windows/direct_calls.py).
        def direct(m):
            ret, target = int(m.group(1), 16), int(m.group(2), 16)
            return self.call(native, chunk, target, ret, boundary=True)
        text = re.sub(
            r"            ctx->lr = 0x([0-9A-F]{8})u;\n            ctx->pc = 0x([0-9A-F]{8})u;\n"
            r"            if \(bw_direct_call_ready\(ctx\)\) \{\n(?:                [^\n]*\n)*?            \}\n"
            r"            return;\n", direct, text)
        # A call inside the chunk, with or without the budget test.
        def inner(m):
            ret, target = int(m.group(1), 16), int(m.group(3), 16)
            return self.call(native, chunk, target, ret, boundary=False)
        text = re.sub(
            r"            ctx->lr = 0x([0-9A-F]{8})u;\n"
            r"(            if \(ctx->downcount <= -\(s64\)DOLRECOMP_C_LOOP_CYCLE_BUDGET\) \{\n"
            r"                ctx->pc = 0x[0-9A-F]{8}u;\n                return;\n            \}\n)?"
            r"            goto label_([0-9A-F]{8});\n", inner, text)
        # A call that leaves the chunk for the chassis loop.
        def leave(m):
            ret, target = int(m.group(1), 16), int(m.group(2), 16)
            return self.call(native, chunk, target, ret, boundary=True)
        text = re.sub(r"            ctx->lr = 0x([0-9A-F]{8})u;\n            ctx->pc = 0x([0-9A-F]{8})u;\n"
                      r"            return;\n", leave, text)
        # A call through LR or CTR (blrl, bctrl): the target from the register.
        def indirect_call(m):
            ret = int(m.group(1), 16)
            site = ret - 4
            return self.indirect(native, chunk, site, ret)
        text = re.sub(r"            ctx->lr = 0x([0-9A-F]{8})u;\n            ctx->pc = target;\n"
                      r"            (?:goto return_dispatch_[0-9A-F]{8};|return;)\n", indirect_call, text)
        # The same through bw_call_translated (direct_calls.py's indirect calls).
        text = re.sub(r"            ctx->lr = 0x([0-9A-F]{8})u;\n            ctx->pc = target;\n"
                      r"            if \(bw_direct_call_ready\(ctx\) && bw_call_translated\(ctx, target\) &&\n"
                      r"                ctx->pc == 0x\1u && bw_direct_call_ready\(ctx\)\)\n"
                      r"                goto label_\1;\n"
                      r"            return;\n", indirect_call, text)
        # A return (blr, bclr).
        def ret(m):
            # A return from another chunk than the hook's: its own return
            # dispatch decides between a goto and the chassis; the hook's
            # decides alike only for a target in neither chunk.
            entry = self.chunk(native.entry).start
            if chunk.start == entry:
                return "            gx_t = target;\n            goto GX_RETURN;\n"
            return ("            gx_t = target;\n"
                    f"            if (s.depth == 0u && (gx_t - 0x{entry:08X}u < 0x4000u || gx_t - 0x{chunk.start:08X}u < 0x4000u))\n"
                    "                return 0;\n"
                    "            goto GX_RETURN;\n")
        text = re.sub(r"            ctx->pc = target;\n            goto return_dispatch_[0-9A-F]{8};\n", ret, text)
        # A jump through CTR (bctr): the chassis loop dispatches the target.
        def jump(m):
            comment_site = None
            return self.indirect(native, chunk, None, None)
        text = re.sub(r"            ctx->pc = target;\n            return;\n", jump, text)
        # The budget tests before a call's goto.
        text = re.sub(r"( +)if \(ctx->downcount <= -\(s64\)DOLRECOMP_C_LOOP_CYCLE_BUDGET\) \{\n"
                      r" +ctx->pc = 0x[0-9A-F]{8}u;\n +return;\n +\}\n",
                      r"\1if (!gx_live(&s))\n\1    return 0;\n", text)
        # A branch (or the last block's fall-through) into another chunk.
        def leave(m):
            return m.group(1) + self.jump(native, int(m.group(2), 16)).replace("\n    ", "\n" + m.group(1)) + "\n"
        text = re.sub(r"( +)ctx->pc = 0x([0-9A-F]{8})u;\n +return;\n", leave, text)
        return text

    def call(self, native, chunk, target, ret, boundary):
        native.returns.add(ret)
        native.reach.add(ret) if self.in_ranges(native, ret) else None
        if not self.in_ranges(native, target):
            entry = self.chunk(native.entry)
            if native.stops and ((boundary and not entry.owns(target)) or
                                 (not boundary and chunk is entry and target in entry.returns)):
                native.stops_at.add(target)
                return (f"            lr = 0x{ret:08X}u;\n"
                        f"            gx_t = 0x{target:08X}u;\n"
                        f"            goto GX_DONE; /* a stop: {target:08X} runs as the translation's call runs it */\n")
            return f"            return 0; /* a call to {target:08X}: not replayed here */\n"
        native.reach.add(target)
        native.reach.add(ret)
        lines = [f"            lr = 0x{ret:08X}u;", "            s.depth++;"]
        if boundary:
            native.boundaries |= {target, ret}
            lines.append(f"            if (!gx_silent(&s, 0x{target:08X}u) || !gx_silent(&s, 0x{ret:08X}u))")
            lines.append("                return 0;")
        else:
            lines.append("            if (!gx_live(&s))")
            lines.append("                return 0;")
        lines.append(f"            goto B_{target:08X};")
        return "\n".join(lines) + "\n"

    def indirect(self, native, chunk, site, ret):
        """A transfer to `target` (the register's value): a call when `ret`
        is set (LR = ret), else a bctr. The targets the native knows are its
        `indirect` list for the site (or, for a bctr, every block leader in its
        ranges); anything else declines."""
        entry = self.chunk(native.entry)
        stop = ""
        if ret is not None:
            targets = native.indirect.get(site)
            if native.stops:
                # Any other target, in neither the hook's chunk nor this one: a stop.
                stop = (f"            if (target - 0x{entry.start:08X}u < 0x4000u || target - 0x{chunk.start:08X}u < 0x4000u)\n"
                        "                return 0;\n"
                        f"            lr = 0x{ret:08X}u;\n"
                        "            gx_t = target;\n"
                        "            goto GX_DONE; /* a stop: the target runs as the translation's call runs it */\n")
                native.stops_at.add(None)
            if targets is None:
                return stop or "            return 0; /* a call through a register: not replayed here */\n"
            native.returns.add(ret)
            native.reach.add(ret)
        else:
            targets = None
        lines = []
        if ret is not None:
            lines += [f"            lr = 0x{ret:08X}u;", "            s.depth++;"]
        lines += ["            if (!gx_live(&s))", "                return 0;", "            switch (target) {"]
        if targets is None:
            native.bctr = True
            cases = [a for a in self.all_leaders(native)]
        else:
            cases = targets
        for t in sorted(cases):
            same = chunk.owns(t) and t in chunk.returns and ret is not None
            lines.append(f"            case 0x{t:08X}u:")
            if not same:
                checks = [f"!gx_silent(&s, 0x{t:08X}u)"]
                if ret is not None:
                    checks.append(f"!gx_silent(&s, 0x{ret:08X}u)")
                    native.boundaries.add(ret)
                native.boundaries.add(t)
                lines.append(f"                if ({' || '.join(checks)})")
                lines.append("                    return 0;")
            native.reach.add(t)
            lines.append(f"                goto B_{t:08X};")
        lines += ["            default:", "                break;", "            }"]
        if stop:
            lines.append("            s.depth--;")
            lines.append(stop.rstrip("\n"))
        else:
            lines.append("            return 0;")
        return "\n".join(lines) + "\n"

    def all_leaders(self, native):
        found = []
        for a, b in native.ranges:
            at = a
            while at < b:
                ch = self.chunk(at)
                found += [x for x in ch.leaders if a <= x < b and ch.owns(x)]
                at = ch.start + 0x4000
        return found

    # --- One native -------------------------------------------------------

    def generate(self, native):
        native.reach = {native.entry} | set(native.resumes)
        native.returns = set()
        native.stops_at = set()
        native.boundaries = set()
        native.fp = False
        native.fp_arith = False
        native.bctr = False
        native.psq_load = native.psq_store = 0
        native.psq_indexed = False
        done, blocks = set(), []
        while native.reach - done:
            address = min(native.reach - done)
            done.add(address)
            chunk = self.chunk(address)
            block = chunk.blocks.get(address)
            if block is None:
                raise SystemExit(f"{native.name}: {address:08X} is not a block leader")
            if block["copy"]:
                text = chunk.copies[block["copy"]]
                body = self.transform(native, chunk, address, text, True)
            else:
                body = self.transform(native, chunk, address, block["body"], False)
            blocks.append((address, block["cycles"], body, chunk))
            # No suffix in a block exceeds its cycles: with the block prepaid
            # and the downcount at most zero, the deadline is then never
            # nearer than a suffix, so no refund can happen on the path.
            segments = re.split(r"/\* [0-9A-F]{8}: a block of (\d+) cycles \*/", body)
            limits = [block["cycles"]] + [int(x) for x in segments[1::2]]
            for limit, segment in zip(limits, segments[0::2]):
                for k in re.findall(r"s\.suffix = (\d+)u;|gx_st\d+\(&s, 0x[0-9A-F]{8}u, (\d+)u,", segment):
                    value = int(k[0] or k[1])
                    if value > limit:
                        raise SystemExit(f"{native.name}: block {address:08X}: a suffix of {value} in {limit} cycles")
        blocks.sort()
        code = "\n".join(f"B_{a:08X}: /* {c} cycles{'' if ch.blocks[a]['copy'] else ', main path'} */\n"
                         f"    if (!gx_block(&s, {c}u))\n        return 0;\n{b.rstrip()}" for a, c, b, ch in blocks)
        gprs = sorted({int(x) for x in re.findall(r"\bg(\d+)\b", code)})
        fprs = sorted({int(x) for x in re.findall(r"\bF(\d+)\b", code)})
        pss = sorted({int(x) for x in re.findall(r"\bP(\d+)\b", code)})
        written = lambda prefix: sorted({int(x) for x in re.findall(rf"\b{prefix}(\d+) = (?!=)", code)}
                                        | {int(x) for x in re.findall(rf"&{prefix}(\d+)\b", code)})
        gw, fw, pw = written("g"), written("F"), written("P")
        specials = [r for r in ("cr", "xer", "lr", "ctr") if re.search(rf"\b{r}\b", code)]
        special_w = [r for r in specials if re.search(rf"\b{r} = (?!=)|&{r}\b", code)]
        returns = sorted(native.returns)
        lines = [f"/* {native.name} ({native.entry:08X}): "
                 + ", ".join(f"{a:08X}..{b:08X}" for a, b in native.ranges) + " */",
                 f"static int gxn_{native.entry:08X}(CPUState* cpu, u32 at) {{",
                 "    GxRun s;",
                 "    if (!gx_start(&s, &s_gx_log, cpu))",
                 "        return 0;"]
        if native.fp or native.psq_load or native.psq_store:
            lines += ["    if (!gx_fp_ready(cpu))", "        return 0;"]
        if native.fp_arith:
            lines += ["    if (!gx_fp_arith_ready(cpu))", "        return 0;"]
        if native.psq_load or native.psq_store:
            lines += [f"    if (!gx_psq_ready(cpu, 0x{native.psq_load:02X}u, 0x{native.psq_store:02X}u))",
                      "        return 0;"]
        for r in gprs:
            lines.append(f"    u32 g{r} = cpu->gpr[{r}];")
        for r in fprs:
            lines.append(f"    f64 F{r} = cpu->fpr[{r}];")
        for r in pss:
            lines.append(f"    f64 P{r} = cpu->ps1[{r}];")
        for r in ("cr", "xer", "ctr"):
            if r in specials:
                lines.append(f"    u32 {r} = cpu->{r};")
        lines.append("    u32 lr = cpu->lr;")
        lines.append("    u32 gx_t = 0;")
        if native.resumes:
            lines.append("    switch (at) {")
            for r in (native.entry,) + native.resumes:
                lines.append(f"    case 0x{r:08X}u: goto B_{r:08X};")
            lines.append("    default: return 0;")
            lines.append("    }")
        else:
            lines.append("    (void)at;")
            lines.append(f"    goto B_{native.entry:08X};")
        lines.append(code)
        lines.append("GX_RETURN:")
        lines.append("    if (s.depth == 0u)")
        lines.append("        goto GX_DONE; /* the function's own blr: the hook's return dispatch takes it on */")
        if returns:
            lines.append("    s.depth--;")
            lines.append("    if (!gx_live(&s))")
            lines.append("        return 0;")
            lines.append("    switch (gx_t) {")
            for r in returns:
                if self.in_ranges(native, r):
                    lines.append(f"    case 0x{r:08X}u: goto B_{r:08X};")
            lines.append("    default: return 0;")
            lines.append("    }")
        else:
            lines.append("    return 0;")
        lines.append("GX_DONE:")
        lines.append("    if (s.bad || s.fp.bad)")
        lines.append("        return 0;")
        lines.append("    gx_commit(&s);")
        for r in gw:
            lines.append(f"    cpu->gpr[{r}] = g{r};")
        for r in fw:
            lines.append(f"    cpu->fpr[{r}] = F{r};")
        for r in pw:
            lines.append(f"    cpu->ps1[{r}] = P{r};")
        for r in special_w:
            if r != "lr":
                lines.append(f"    cpu->{r} = {r};")
        lines.append("    cpu->lr = lr;")
        lines.append("    cpu->pc = gx_t;")
        lines.append("    return 1;")
        lines.append("}")
        out = "\n".join(lines) + "\n"
        # Unused locals and labels.
        out = re.sub(r"\n    u32 gx_t = 0;\n", "\n    u32 gx_t = 0;\n    (void)gx_t;\n", out)
        # Every decline puts back what the run stored (native_gx_run.h).
        out = out.replace("return 0;", "return gx_decline(&s);")
        native.blocks = len(blocks)
        return out


# --- The natives -----------------------------------------------------------
# (function ranges from tww's config/GZLE01/symbols.txt)
TEX_PRELOADED = [(0x80324D50, 0x80324EE8), (0x8031FAC4, 0x8031FAE8)]  # with __GXDefaultTlutRegionCallback
NATIVES = [
    Native("GXLoadPosMtxImm", 0x80326F38, [(0x80326F38, 0x80326F88)]),
    Native("GXLoadNrmMtxImm", 0x80326F88, [(0x80326F88, 0x80326FD8)]),
    Native("GXSetTevColor", 0x80325FA8, [(0x80325FA8, 0x8032601C)]),
    Native("GXSetTevColorS10", 0x8032601C, [(0x8032601C, 0x80326090)]),
    Native("GXSetTevKColor", 0x80326090, [(0x80326090, 0x80326104)]),
    Native("GXSetArray", 0x80322568, [(0x80322568, 0x803225F4)]),
    Native("GXSetTevOrder", 0x803263A0, [(0x803263A0, 0x80326578)]),
    Native("GXCallDisplayList", 0x80326B80, [(0x80326B80, 0x80326BF0)]),
    # GXBegin alone: where the dirty state needs its calls, it declines, and the
    # translation makes them, each callee native (below) at its own hook.
    Native("GXBegin", 0x803230C4, [(0x803230C4, 0x803231B4)]),
    Native("GXLoadTexObjPreLoaded", 0x80324D50, TEX_PRELOADED, {0x80324E70: [0x8031FAC4]}),
    Native("GXLoadTexObj", 0x80324EE8,
           [(0x80324EE8, 0x80324F3C), (0x8031FA48, 0x8031FAC4), (0x80324D28, 0x80324D30)] + TEX_PRELOADED,
           {0x80324F10: [0x8031FA48], 0x80324E70: [0x8031FAC4]}),
    Native("__GXSetSUTexRegs", 0x803253B8, [(0x803253B8, 0x80325534), (0x80325300, 0x803253B8)]),
    Native("__GXSetVAT", 0x803221D8, [(0x803221D8, 0x80322274)]),
    Native("__GXSetMatrixIndex", 0x80327364, [(0x80327364, 0x803273E8)]),
    Native("__GXUpdateBPMask", 0x80325CD4, [(0x80325CD4, 0x80325DA0)]),
    Native("__GXSetGenMode", 0x803233B0, [(0x803233B0, 0x803233D4)]),
    Native("__GXSetVCD", 0x80321958, [(0x80321958, 0x803219AC), (0x803214B0, 0x80321608)]),  # with __GXXfVtxSpecs
    Native("__GXXfVtxSpecs", 0x803214B0, [(0x803214B0, 0x80321608)]),
    Native("__GXCalculateVLim", 0x803219AC, [(0x803219AC, 0x80321AD0)]),
    # The vertex format and the TEV, channel and pixel state.
    Native("GXSetVtxDesc", 0x80321608, [(0x80321608, 0x80321958)]),  # across chunks 0199 and 0200
    Native("GXClearVtxDesc", 0x80321AD0, [(0x80321AD0, 0x80321B08)]),
    Native("GXSetVtxAttrFmt", 0x80321B08, [(0x80321B08, 0x80321E60)]),
    Native("GXSetTexCoordGen2", 0x80322604, [(0x80322604, 0x803228D4), (0x80327364, 0x803273E8)]),  # with __GXSetMatrixIndex
    Native("GXSetNumTexGens", 0x803228D4, [(0x803228D4, 0x80322914)]),
    Native("GXSetCullMode", 0x80323328, [(0x80323328, 0x80323374)]),
    Native("GXSetChanAmbColor", 0x80324390, [(0x80324390, 0x80324484)]),
    Native("GXSetChanMatColor", 0x80324484, [(0x80324484, 0x80324578)]),
    Native("GXSetNumChans", 0x80324578, [(0x80324578, 0x803245BC)]),
    Native("GXSetChanCtrl", 0x803245BC, [(0x803245BC, 0x80324688)]),
    Native("GXGetTexObjFmt", 0x80324D28, [(0x80324D28, 0x80324D30)]),
    Native("GXSetTevIndirect", 0x80325774, [(0x80325774, 0x80325810)]),
    Native("GXSetNumIndStages", 0x80325C00, [(0x80325C00, 0x80325C28)]),
    Native("GXSetTevDirect", 0x80325C28, [(0x80325C28, 0x80325C70), (0x80325774, 0x80325810)]),
    Native("GXSetTevColorIn", 0x80325E50, [(0x80325E50, 0x80325E94)]),
    Native("GXSetTevAlphaIn", 0x80325E94, [(0x80325E94, 0x80325ED8)]),
    Native("GXSetTevColorOp", 0x80325ED8, [(0x80325ED8, 0x80325F40)]),
    Native("GXSetTevAlphaOp", 0x80325F40, [(0x80325F40, 0x80325FA8)]),
    Native("GXSetTevKColorSel", 0x80326104, [(0x80326104, 0x80326170)]),
    Native("GXSetTevKAlphaSel", 0x80326170, [(0x80326170, 0x803261DC)]),
    Native("GXSetTevSwapMode", 0x803261DC, [(0x803261DC, 0x80326230)]),
    Native("GXSetAlphaCompare", 0x803262C8, [(0x803262C8, 0x8032631C)]),
    Native("GXSetNumTevStages", 0x80326578, [(0x80326578, 0x803265A8)]),
    Native("GXSetBlendMode", 0x80326858, [(0x80326858, 0x803268AC)]),
    Native("GXSetColorUpdate", 0x803268AC, [(0x803268AC, 0x803268D8)]),
    Native("GXSetAlphaUpdate", 0x803268D8, [(0x803268D8, 0x80326904)]),
    Native("GXSetZMode", 0x80326904, [(0x80326904, 0x80326938)]),
    Native("GXSetZCompLoc", 0x80326938, [(0x80326938, 0x80326970)]),
    Native("GXSetDstAlpha", 0x80326A8C, [(0x80326A8C, 0x80326AC8)]),
    Native("GXSetCurrentMtx", 0x80326FD8, [(0x80326FD8, 0x80327010), (0x80327364, 0x803273E8)]),  # with __GXSetMatrixIndex
]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else \
        Path(__file__).resolve().parents[2] / "cmake/composite/native_gx_gen.inc"
    gen = Gen(root)
    pieces = ["/* Generated by scripts/windows/native_gx_gen.py from a builder's composite-src: the",
              " * fifth set's natives, each the translation's own blocks on local registers",
              " * (native_gx_run.h). Do not edit; regenerate, retest, recertify. */", ""]
    table = []
    for native in NATIVES:
        pieces.append(gen.generate(native))
        table.append(native)
        print(f"{native.entry:08X} {native.name}: {native.blocks} blocks, boundaries "
              + (", ".join(f"{b:08X}" for b in sorted(native.boundaries)) or "none"))
    pieces.append("/* The natives by entry. */")
    pieces.append("#define GX_NATIVES(X) \\")
    rows = []
    for n in table:
        rows.append(f"    X(0x{n.entry:08X}u, gxn_{n.entry:08X}, \"{n.name}\", {n.entry:08X})")
        rows += [f"    X(0x{r:08X}u, gxn_{n.entry:08X}, \"{n.name}@{r:08X}\", {r:08X})" for r in n.resumes]
    pieces.append(" \\\n".join(rows))
    pieces.append("")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(pieces))
    print(f"wrote {out_path}")
    # For the test: the block leaders a jump through CTR may reach in each
    # native that makes one (its jump tables are filled with them).
    leaders = ["/* Generated by scripts/windows/native_gx_gen.py: the block leaders a native's",
               " * jumps through CTR may reach, for tests/native_gx_test.c's jump tables. */", ""]
    for native in table:
        if native.bctr:
            found = sorted(gen.all_leaders(native))
            leaders.append(f"static const u32 k_leaders_{native.entry:08X}[] = {{")
            for i in range(0, len(found), 6):
                leaders.append("    " + " ".join(f"0x{a:08X}u," for a in found[i:i + 6]))
            leaders.append("};")
    test_path = out_path.parents[2] / "tests/native_gx_leaders.h" if len(sys.argv) <= 2 else \
        out_path.with_name("native_gx_leaders.h")
    with open(test_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(leaders) + "\n")
    print(f"wrote {test_path}")


if __name__ == "__main__":
    main()
