#!/usr/bin/env python3
"""Write cmake/composite/native_gx_gen.inc: the fifth set's natives (the GX
SDK's FIFO writers, native_gx.c), each a translated function's prepaid
blocks replayed on local registers (cmake/composite/native_gx_run.h); and
their list, cmake/composite/native_gx_list.h (native_gx.h's entry points).

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
  `stops` (the sixth set), where the call is in the hook's own chunk, the run
  ending just before the call instruction - everything committed, the block
  prepaid - and the hook going on with the translation at that instruction
  (cycle_block_prepaid set, through the chunk's pc table), which makes the
  call as it always does; a hook at the return address (a resume) runs the
  rest natively. A branch to a block of the hook's chunk the native does not
  replay, and a block it is told not to replay (`stop_leaders`), stop the
  same way at the block's leader, before it is entered.

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

    def __init__(self, name, entry, ranges, indirect=None, stops=False, resumes=(), stop_leaders=()):
        self.name, self.entry, self.ranges = name, entry, ranges
        self.indirect = indirect or {}
        # stops: where the path reaches code not replayed here from the hook's
        # own chunk, the run ends there and the translation takes over (the
        # module docstring); instead of declining. resumes: further entries
        # (block leaders of the hook's chunk), each hooked, where the native
        # takes over again - the return addresses of the calls it stops at
        # are added to them. stop_leaders: blocks of the hook's chunk left to
        # the translation.
        self.stops, self.resumes = stops, tuple(resumes)
        self.stop_leaders = set(stop_leaders)


class Gen:
    def __init__(self, root, prefix="gxn_"):
        self.root = root
        self.chunks = {}
        self.prefix = prefix

    def chunk(self, address):
        start = 0x800016E0 + ((address - 0x800016E0) // 0x4000) * 0x4000
        if start not in self.chunks:
            self.chunks[start] = Chunk(self.root, start)
        return self.chunks[start]

    def in_ranges(self, native, address):
        return any(a <= address < b for a, b in native.ranges)

    # --- Text transforms --------------------------------------------------

    def transform(self, native, chunk, leader, text, from_copy):
        out = self.inline_gpr(text) if not native.stops else self.inline_gpr_stops(native, chunk, text)
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
        # The sixth set's hooks (native_entries.py's DRAW_HOOK).
        out = re.sub(r"    if \(bluewake_native_draw_enabled\) \{\n(?:        [^\n]*\n)*?    \}\n", "", out)
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
        paired = self.paired(out)
        if paired != out:
            native.fp_arith = True
        out = paired
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

    def paired(self, text):
        """The paired-single arithmetic and the fused multiply-adds (inline_fp.h's
        bw_fp_ps_*, bw_fp_fma) as native_gx_run.h's forms on the registers'
        locals."""
        pair = lambda r: f"gx_pair(F{r}, P{r})"
        def two(m):  # ps_mul, ps_add, ps_sub: d, a, c|b
            op, d, a, b = m.group(1), m.group(2), m.group(3), m.group(4)
            fn = {"mul_op": "nr_ps_mul", "add_op": "nr_ps_add", "sub_op": "nr_ps_sub"}[op]
            return f"gx_ps_set(&F{d}, &P{d}, {fn}(&s.fp, {pair(a)}, {pair(b)}));"
        text = re.sub(r"ppc_ps_(mul_op|add_op|sub_op)\(ctx, (\d+), (\d+), (\d+)\);", two, text)
        def scalar(m):  # ps_muls0, ps_muls1: c's first or second half
            half, d, a, c = m.group(1), m.group(2), m.group(3), m.group(4)
            return f"gx_ps_set(&F{d}, &P{d}, nr_ps_muls0(&s.fp, {pair(a)}, {'F' if half == '0' else 'P'}{c}));"
        text = re.sub(r"ppc_ps_muls([01])\(ctx, (\d+), (\d+), (\d+)\);", scalar, text)
        def madd(m):
            d, a, c, b, sub, neg = m.groups()
            return f"gx_ps_set(&F{d}, &P{d}, nr_ps_madd(&s.fp, {pair(a)}, {pair(c)}, {pair(b)}, {sub}, {neg}));"
        text = re.sub(r"ppc_ps_madd_op\(ctx, (\d+), (\d+), (\d+), (\d+), (true|false), (true|false)\);", madd, text)
        def madds(m):  # ps_madds0, ps_madds1: c's one half for both
            half, d, a, c, b = m.groups()
            h = f"{'F' if half == '0' else 'P'}{c}"
            return f"gx_ps_set(&F{d}, &P{d}, nr_ps_madd(&s.fp, {pair(a)}, gx_pair({h}, {h}), {pair(b)}, false, false));"
        text = re.sub(r"ppc_ps_madds([01])\(ctx, (\d+), (\d+), (\d+), (\d+)\);", madds, text)
        def sums(m):
            half, d, a, c, b = m.groups()
            third = f"P{c}" if half == "0" else f"F{c}"
            return f"gx_ps_set(&F{d}, &P{d}, gx_ps_sum{half}(&s.fp, F{a}, P{b}, {third}));"
        text = re.sub(r"ppc_ps_sum([01])\(ctx, (\d+), (\d+), (\d+), (\d+)\);", sums, text)
        text = text.replace("ppc_fma(ctx, ", "gx_fma(&s.fp, ")
        return text

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
        if not self.in_ranges(native, target) or target in native.stop_leaders:
            if native.stops and chunk is self.chunk(native.entry) and target in chunk.blocks:
                native.stops_at.add(target)
                return f"{{ gx_t = 0x{target:08X}u; goto GX_STOP; }} /* {target:08X}: the translation's */"
            return f"return 0; /* {target:08X}: not replayed here */"
        if target in native.stop_leaders:
            raise SystemExit(f"{native.name}: a stop at {target:08X} outside the hook's chunk")
        native.reach.add(target)
        return f"goto B_{target:08X};"

    def jump(self, native, target):
        """A jump that leaves the chunk (a branch or fall-through into the
        next chunk): the chassis loop dispatches the target."""
        if not self.in_ranges(native, target):
            return f"return 0; /* {target:08X}: not replayed here */"
        if native.stops and target not in self.chunk(target).blocks:
            return f"return 0; /* {target:08X}: not a block leader there (the translation charges it instruction by instruction) */"
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

    def inline_gpr_stops(self, native, chunk, text):
        """The inline _savegpr_N/_restgpr_N for a native that stops (both of
        inline_save_restore_gpr.py's forms): where the translation would run
        the routine itself through the chassis, a stop before the call; where
        the turn ends after it, a stop at the return address's leader, before
        it is entered (the translation returns to the chassis there, and a
        dispatch to that leader returns at its budget test alike)."""
        def save_restore(m):
            ret, form, cycles, body, suffix = (int(m.group(1), 16), m.group(2), m.group(3), m.group(4),
                                              m.group(5))
            short = re.match(r"ctx->downcount - (\d+) > -\(s64\)DOLRECOMP_C_LOOP_CYCLE_BUDGET$", form)
            if short:
                cond = f"s.downcount - {short.group(1)} > -s.budget"
            else:
                full = re.match(r"ctx->downcount > -\(s64\)DOLRECOMP_C_LOOP_CYCLE_BUDGET &&\n"
                                r" +\(ctx->cycle_deadline_budget <= 0 \|\|\n"
                                r" +\(ctx->cycle_deadline_budget >= (\d+) &&\n"
                                r" +dolrecomp_block_can_precharge\(ctx, (\d+)u\)\)\)$", form)
                if not full:
                    raise SystemExit(f"{native.name}: an inline register save of an unknown form: {form}")
                cond = (f"s.downcount > -s.budget && (s.deadline <= 0 || (s.deadline >= {full.group(1)} && "
                        f"s.deadline + s.downcount >= 0 && s.deadline + s.downcount >= {full.group(2)}))")
            before = self.stop_before(native, chunk, ret - 4)
            fail = ("\n".join("    " + l for l in before.rstrip("\n").split("\n")) if before
                    else "                return 0; /* the routine itself, through the chassis */")
            after = (f"{{ gx_t = 0x{ret:08X}u; goto GX_STOP; }} /* the turn ends: the translation's */"
                     if native.stops and chunk is self.chunk(native.entry) and ret in chunk.blocks
                     else "return 0;")
            if after != "return 0;":
                native.stops_at.add(ret)
            return (f"            if (!({cond})) {{\n{fail}\n            }}\n"
                    f"            lr = 0x{ret:08X}u;\n"
                    f"            s.downcount -= {cycles};\n"
                    + body.replace("                ", "            ") +
                    f"            s.suffix = {suffix}u;\n"
                    "            if (!gx_live(&s))\n"
                    f"                {after}\n"
                    f"            goto label_{ret:08X};\n")
        return re.sub(
            r"            ctx->lr = 0x([0-9A-F]{8})u;\n"
            r"            /\* bluewake: _savegpr/_restgpr inline \(scripts/windows/inline_save_restore_gpr\.py\) \*/\n"
            r"            if \(((?:[^{\n]|\n(?! +ctx->downcount -=))*)\) \{\n"
            r"                ctx->downcount -= (\d+);\n"
            r"((?:                (?!ctx->cycle_observation_suffix)[^\n]*\n)*?)"
            r"                ctx->cycle_observation_suffix = (\d+)u;\n"
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
        if not self.in_ranges(native, target) or target in native.stop_leaders:
            stop = self.stop_before(native, chunk, ret - 4)
            if stop:
                return stop
            return f"            return 0; /* a call to {target:08X}: not replayed here */\n"
        native.reach.add(target)
        native.reach.add(ret)
        stop = self.stop_before(native, chunk, ret - 4) if native.stops else None
        if stop:
            # Where the host would be asked or the budget is spent, the translation's call.
            fail = "\n".join("    " + l for l in stop.rstrip("\n").split("\n"))
            checks = ["!gx_live(&s)"]
            if boundary:
                native.boundaries |= {target, ret}
                checks += [f"!gx_silent(&s, 0x{target:08X}u)", f"!gx_silent(&s, 0x{ret:08X}u)"]
            return (f"            if ({' || '.join(checks)}) {{\n{fail}\n            }}\n"
                    f"            lr = 0x{ret:08X}u;\n            s.depth++;\n            goto B_{target:08X};\n")
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

    def stop_before(self, native, chunk, site):
        """A stop just before the instruction at `site` (a call in the hook's
        own chunk): the run ends with everything as the translation has it
        there, the block prepaid; the hook goes on with the translation at
        that instruction. A call that is its block's first instruction stops
        before the block is entered (its cycles given back)."""
        if not native.stops or chunk is not self.chunk(native.entry):
            return None
        native.stops_at.add(site)
        native.call_stops.add(site)
        undo = f"            s.downcount += {chunk.blocks[site]['cycles']};\n" if site in chunk.blocks else ""
        return (undo + f"            gx_t = 0x{site:08X}u;\n"
                "            goto GX_STOP; /* the translation makes this call */\n")

    def indirect(self, native, chunk, site, ret):
        """A transfer to `target` (the register's value): a call when `ret`
        is set (LR = ret), else a bctr. The targets the native knows are its
        `indirect` list for the site (or, for a bctr, every block leader in its
        ranges); anything else declines."""
        stop = ""
        if ret is not None:
            targets = native.indirect.get(site)
            # Any other target: a stop before the call, where the native may stop.
            stop = self.stop_before(native, chunk, site) or ""
            if targets is None:
                return stop or "            return 0; /* a call through a register: not replayed here */\n"
            native.returns.add(ret)
            native.reach.add(ret)
            if stop:
                return self.indirect_or_stop(native, chunk, targets, ret, stop)
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
        lines.append("            return 0;")
        return "\n".join(lines) + "\n"

    def indirect_or_stop(self, native, chunk, targets, ret, stop):
        """A call through a register where the native may stop: each known
        target replayed where its boundaries pass without the host and the
        budget allows the call; anything else - another target, the host to
        be asked, the budget spent - a stop before the call (LR and the depth
        untouched), and the translation makes it."""
        fail = "\n".join("    " + l for l in stop.rstrip("\n").split("\n"))
        lines = ["            switch (target) {"]
        for t in sorted(targets):
            same = chunk.owns(t) and t in chunk.returns
            lines.append(f"            case 0x{t:08X}u:")
            checks = ["!gx_live(&s)"]
            if not same:
                checks += [f"!gx_silent(&s, 0x{t:08X}u)", f"!gx_silent(&s, 0x{ret:08X}u)"]
                native.boundaries |= {t, ret}
            lines.append(f"                if ({' || '.join(checks)}) {{")
            lines.append(fail)
            lines.append("                }")
            lines.append(f"                lr = 0x{ret:08X}u;")
            lines.append("                s.depth++;")
            native.reach.add(t)
            lines.append(f"                goto B_{t:08X};")
        lines += ["            default:", "                break;", "            }", stop.rstrip("\n")]
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
        if not native.stops:
            return self.generate_once(native)
        # A native that stops: once to find its stops, then again with a
        # resume at each stopped call's return address (a block leader of the
        # hook's chunk, where the hook takes over again).
        explicit = tuple(native.resumes)
        self.generate_once(native)
        entry = self.chunk(native.entry)
        # (Only in its own function, the first range: a stop inside a callee it
        # replays returns to its caller translated, and it resumes there.)
        own = native.ranges[0]
        found = {s + 4 for s in native.call_stops if s + 4 in entry.blocks and own[0] <= s < own[1]}
        native.resumes = tuple(sorted((set(explicit) | found) - {native.entry}))
        for r in native.resumes:
            if r not in entry.blocks:
                raise SystemExit(f"{native.name}: a resume at {r:08X}, not a block leader of the hook's chunk")
        out = self.generate_once(native)
        native.resumes_explicit = explicit
        return out

    def generate_once(self, native):
        native.reach = {native.entry} | set(native.resumes)
        native.returns = set()
        native.stops_at = set()
        native.call_stops = set()
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
            if address in native.stop_leaders:
                if not native.stops or chunk is not self.chunk(native.entry):
                    raise SystemExit(f"{native.name}: a stop at {address:08X} outside the hook's chunk")
                native.stops_at.add(address)
                blocks.append((address, None, f"    gx_t = 0x{address:08X}u;\n    goto GX_STOP;", chunk))
                continue
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
        blocks.sort(key=lambda x: x[0])
        # Checkpoints (natives that stop): at each loop head in the hook's
        # chunk (a block a later block of the chunk branches back to), a run
        # whose logs are filling commits and goes on (native_gx_run.h).
        entry_chunk = self.chunk(native.entry)
        heads = set()
        if native.stops:
            for a, c, b, ch in blocks:
                if c is None or ch is not entry_chunk:
                    continue
                for m in re.finditer(r"goto B_([0-9A-F]{8});", b):
                    target = int(m.group(1), 16)
                    if target <= a and entry_chunk.owns(target):
                        heads.add(target)
        native.heads = heads

        def emit(a, c, b, ch):
            if c is None:
                return f"B_{a:08X}: /* left to the translation, before it is entered */\n{b}"
            check = ""
            if a in heads:
                check = (f"    if (s.stores > GX_CHECK_STORES || s.pipe_length > GX_CHECK_PIPE) {{\n"
                         f"        gx_t = 0x{a:08X}u;\n        goto GX_CHECKPOINT;\n    }}\nC_{a:08X}:\n")
            return (f"B_{a:08X}: /* {c} cycles{'' if ch.blocks[a]['copy'] else ', main path'} */\n{check}"
                    f"    if (!gx_block(&s, {c}u))\n        return 0;\n{b.rstrip()}")
        code = "\n".join(emit(a, c, b, ch) for a, c, b, ch in blocks)
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
                 f"static int {self.prefix}{native.entry:08X}(CPUState* cpu, u32 at) {{",
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
        if native.stops:
            lines.append("    int gx_r = 1; /* 1: done (pc the return address); 2: stopped (pc the translation's) */")
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
        if native.stops and heads:
            # A checkpoint: everything so far committed (as a stop at gx_t
            # would leave it), the logs emptied, on from the loop's head.
            lines.append("GX_CHECKPOINT:")
            lines.append("    if (s.bad || s.fp.bad)")
            lines.append("        return 0;")
            lines.append("    gx_commit(&s);")
            lines += [f"    cpu->gpr[{r}] = g{r};" for r in gw]
            lines += [f"    cpu->fpr[{r}] = F{r};" for r in fw]
            lines += [f"    cpu->ps1[{r}] = P{r};" for r in pw]
            lines += [f"    cpu->{r} = {r};" for r in special_w if r != "lr"]
            lines.append("    cpu->lr = lr;")
            lines.append("    cpu->pc = gx_t;")
            lines.append("    s.stores = s.pipe_length = 0u;")
            lines.append("    s.checkpoint = gx_t;")
            lines.append("    switch (gx_t) {")
            for h in sorted(heads):
                lines.append(f"    case 0x{h:08X}u: goto C_{h:08X};")
            lines.append("    default: return 0;")
            lines.append("    }")
        if native.stops:
            lines.append("GX_STOP:")
            lines.append("    gx_r = 2;")
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
        lines.append("    return gx_r;" if native.stops else "    return 1;")
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
CVT_FP2UNSIGNED = (0x80328E10, 0x80328E6C)  # __cvt_fp2unsigned (chunk 0201)
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
    # Texture coordinates, lighting channels, the current matrix.
    Native("GXSetTexCoordGen2", 0x80322604, [(0x80322604, 0x803228D4), (0x80327364, 0x803273E8)]),  # with __GXSetMatrixIndex
    Native("GXSetNumTexGens", 0x803228D4, [(0x803228D4, 0x80322914)]),
    Native("GXSetChanAmbColor", 0x80324390, [(0x80324390, 0x80324484)]),
    Native("GXSetChanMatColor", 0x80324484, [(0x80324484, 0x80324578)]),
    Native("GXSetNumChans", 0x80324578, [(0x80324578, 0x803245BC)]),
    Native("GXSetChanCtrl", 0x803245BC, [(0x803245BC, 0x80324688)]),
    Native("GXSetCurrentMtx", 0x80326FD8, [(0x80326FD8, 0x80327010), (0x80327364, 0x803273E8)]),  # with __GXSetMatrixIndex
    # With floating point: fog, texture LOD, indirect matrices.
    Native("GXSetFog", 0x803265A8, [(0x803265A8, 0x80326758), CVT_FP2UNSIGNED]),
    Native("GXSetFogRangeAdj", 0x80326758, [(0x80326758, 0x80326858)]),
    Native("GXInitTexObjLOD", 0x80324B68, [(0x80324B68, 0x80324CFC)]),
    Native("GXSetIndTexMtx", 0x80325810, [(0x80325810, 0x80325970)]),
    # J3D's display-list writers (GD: the current list's write pointer) and the
    # direct FIFO forms (GF).
    Native("J3DGDSetFog", 0x802D85F8, [(0x802D85F8, 0x802D895C), CVT_FP2UNSIGNED]),
    Native("J3DGDSetTevOrder", 0x802D80D0, [(0x802D80D0, 0x802D825C)]),
    Native("GFSetTevColor", 0x802AFDDC, [(0x802AFDDC, 0x802AFE38)]),
    Native("GFSetTevColorS10", 0x802AFE38, [(0x802AFE38, 0x802AFEA0)]),
    Native("GFSetFog", 0x802AFBD4, [(0x802AFBD4, 0x802AFD3C), CVT_FP2UNSIGNED]),
]

# Made, tested exact (60,000 cases each, no mismatch) and dropped: through the
# hooked chunks none was faster than its translation by 15 percent or more in
# every run (docs/status/NATIVE_GX_2026-10-04.md). Each is a setter of a few instructions that
# stores to RAM (__GXData, a GD list) more than to the pipe: there the
# translation's inline RAM stores are as fast as a native's, whose undo log
# and entry cost what the pipe stores it saves would have. Not generated.
DROPPED = [
    Native("GXSetVtxDesc", 0x80321608, [(0x80321608, 0x80321958)]),  # across chunks 0199 and 0200
    Native("GXClearVtxDesc", 0x80321AD0, [(0x80321AD0, 0x80321B08)]),
    Native("GXSetVtxAttrFmt", 0x80321B08, [(0x80321B08, 0x80321E60)]),
    Native("GXSetCullMode", 0x80323328, [(0x80323328, 0x80323374)]),
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
    Native("J3DGDSetTevKColor", 0x802D825C, [(0x802D825C, 0x802D83C4)]),
    Native("J3DGDSetTevColorS10", 0x802D83C4, [(0x802D83C4, 0x802D85F8)]),
    Native("J3DGDSetLightColor", 0x802D6624, [(0x802D6624, 0x802D6734)]),
    Native("J3DGDSetLightPos", 0x802D6734, [(0x802D6734, 0x802D6900)]),
    Native("J3DGDSetLightDir", 0x802D6900, [(0x802D6900, 0x802D6ACC)]),
    Native("J3DGDSetLightAttn", 0x802D632C, [(0x802D632C, 0x802D6624)]),
    Native("J3DGDSetTexLookupMode", 0x802D7400, [(0x802D7400, 0x802D759C)]),
    Native("J3DGDSetTexImgPtr", 0x802D7644, [(0x802D7644, 0x802D76D4)]),
    Native("J3DGDLoadTexMtxImm", 0x802E9984, [(0x802E9984, 0x802E9F04)]),
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
    pieces.append("")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(pieces))
    print(f"wrote {out_path}")
    # The natives by entry (native_gx.h: each native's own entry point, the
    # function its hook calls).
    rows = []
    for n in table:
        rows.append(f"    X(0x{n.entry:08X}u, gxn_{n.entry:08X}, \"{n.name}\", {n.entry:08X})")
        rows += [f"    X(0x{r:08X}u, gxn_{n.entry:08X}, \"{n.name}@{r:08X}\", {r:08X})" for r in n.resumes]
    listing = ["/* Generated by scripts/windows/native_gx_gen.py: the fifth set's natives by entry",
               " * (X(entry, native, name, the entry's digits)). Do not edit; regenerate. */",
               "#ifndef BLUEWAKE_NATIVE_GX_LIST_H", "#define BLUEWAKE_NATIVE_GX_LIST_H", "",
               "#define GX_NATIVES(X) \\", " \\\n".join(rows), "", "#endif", ""]
    list_path = out_path.with_name("native_gx_list.h")
    with open(list_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(listing))
    print(f"wrote {list_path}")
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
