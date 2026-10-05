#!/usr/bin/env python3
"""Write the seventh set's natives: leaf compute code (libm's fmod and the
random numbers and angles built on it, the collision blocks' bounds, the
planes and the Euler quaternions), each a translated function's prepaid
blocks replayed on local registers - scripts/windows/native_gx_gen.py's
generator, here for code that writes RAM only - into, for each group,

  cmake/composite/native_<group>_gen.inc   (the natives)
  cmake/composite/native_<group>_list.h    (X(entry, native, name, digits))

  native_leaf_gen.py COMPOSITE_SRC [OUT_DIR]

A developer's tool, run by hand on a builder's composite-src; its output is
committed, and scripts/windows/native_entries.py hooks each native only where
the translation hashes to the one its comparison test (tests/native_leaf_test.c)
was run against - the text this read.

What the fifth set's generator does (native_gx_gen.py's docstring) it does
here too: the translation's own C for every instruction on the path, from the
blocks' prepaid copies, on locals; each block leader a check that the block
is prepaid and the turn's budget not spent; calls inside the native's ranges
as gotos with LR in a local; FP through native_replay.h's inline paths. With,
for this set (cmake/composite/native_leaf_run.h):

- stores to plain RAM only (lf_st, lf_stp): a store anywhere else - the
  gather pipe included - declines, so a native here hands the host nothing;
- the translator's extracted loops (`label_X: loop_X(ctx); if (ctx->pc ==
  Y) goto label_Y; return;`, a `static void loop_X` beside the chunk's
  function): each replayed as a block of its own, its prepaid copy on the
  same locals - the loop function's precharge test and budget test at its
  head (one gx_block), its body, its back edge's budget test (gx_live), and
  on to label_Y where it leaves;
- the double divide (ppc_fdiv, which the translation hands GXRuntime's
  interpreter in every case) and the interpreter's fctiw, frsqrte and frsp,
  each the interpreter's own function on a scratch state that holds the run's
  FPSCR and the registers it reads;
- a decline's reason counted (lf_decline: the state at entry, the clock, an
  address, an FP operand off the inline path, a call or jump not replayed).

Anything else in the text (an instruction form the fifth set's generator does
not know) stops the script.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import native_gx_gen  # noqa: E402
from native_gx_gen import Chunk, Gen, Native  # noqa: E402

LOOP = re.compile(r"\nstatic void loop_([0-9A-F]{8})\(CPUState\* ctx_param\) \{\n(.*?)\n\}\n", re.S)


class LeafChunk(Chunk):
    """A chunk, with its extracted loops as blocks of their own."""

    def __init__(self, root, start):
        super().__init__(root, start)
        text = self.path.read_text(encoding="utf-8")
        self.loops = {}
        for m in LOOP.finditer(text):
            head, body = int(m.group(1), 16), m.group(2) + "\n"
            call = re.search(rf"\nlabel_{head:08X}:\n    loop_{head:08X}\(ctx\);\n"
                             rf"    if \(ctx->pc == 0x([0-9A-F]{{8}})u\) goto label_\1;\n    return;\n", self.main)
            if call is None:
                continue  # not entered from this function's blocks
            exit_ = int(call.group(1), 16)
            pre = re.match(
                r"    bool cycle_block_prepaid = false;\n"
                rf"label_{head:08X}:\n"
                r"    cycle_block_prepaid = dolrecomp_block_can_precharge\(ctx, (\d+)u\);\n"
                r"    if \(cycle_block_prepaid\) \{\n"
                r"        if \(ctx->downcount <= -\(s64\)DOLRECOMP_C_LOOP_CYCLE_BUDGET\) \{\n"
                rf"            ctx->pc = 0x{head:08X}u;\n"
                r"            return;\n"
                r"        \}\n"
                r"        ctx->downcount -= (\d+);\n"
                r"    \}\n"
                r"    if \(cycle_block_prepaid\) goto bwfast_0;\n", body)
            if pre is None or pre.group(1) != pre.group(2):
                raise SystemExit(f"loop_{head:08X}: an extracted loop of an unknown form")
            at = body.find("\nbwfast_0:\n")
            fast = body[at + len("\nbwfast_0:\n"):]
            end = re.search(rf"    ctx->pc = 0x{exit_:08X}u;\n    goto bwend_0;\n$", fast)
            if at < 0 or end is None or "bwfast_" in fast or re.search(r"(?m)^label_", fast):
                raise SystemExit(f"loop_{head:08X}: an extracted loop of an unknown form")
            # Leaving the loop: on at label_Y, where the function's own code
            # takes it on (the main path's `goto label_Y` after the call).
            fast = fast[:end.start()] + f"    goto label_{exit_:08X};\n"
            name = f"bwloop_{head:08X}"
            self.copies[name] = fast
            if head in self.blocks:
                raise SystemExit(f"loop_{head:08X}: a block leader too")
            self.blocks[head] = {"cycles": int(pre.group(1)), "body": None, "copy": name}
            self.loops[head] = exit_


class LeafGen(Gen):
    def __init__(self, root, prefix="lfn_"):
        super().__init__(root, prefix)

    def chunk(self, address):
        start = 0x800016E0 + ((address - 0x800016E0) // 0x4000) * 0x4000
        if start not in self.chunks:
            self.chunks[start] = LeafChunk(self.root, start)
        return self.chunks[start]

    def transform(self, native, chunk, leader, text, from_copy):
        # The double divide: GXRuntime's interpreter in every case, here on the
        # run's scratch state (native_leaf_run.h's lf_fdiv).
        text = re.sub(r"ppc_fdiv\(ctx, (\d+), (\d+), (\d+)\);",
                      r"lf_fdiv(&s.fp, &ctx->fpr[\1], ctx->fpr[\2], ctx->fpr[\3]);", text)
        return super().transform(native, chunk, leader, text, from_copy)

    def generate_once(self, native):
        out = super().generate_once(native)
        # This set's runtime (native_leaf_run.h): RAM-only stores, the run's own
        # log and scratch state, the decline's reason.
        out = out.replace("gx_start(&s, &s_gx_log, cpu)", "lf_start(&s, &s_lf_log, cpu)")
        out = re.sub(r"\bgx_st(p?)(8|16|32|64)\(", r"lf_st\1\2(", out)
        out = re.sub(r"\bgx_(psq_st|fctiw|frsqrte|frsp)\(", r"lf_\1(", out)
        out = out.replace("gx_commit(&s);", "lf_commit(&s);")
        # Each block's leader against the clock's floor (native_leaf_run.h's
        # lf_block: one test where gx_block makes three).
        start = "    if (!lf_start(&s, &s_lf_log, cpu))\n        return gx_decline(&s);\n"
        if out.count(start) != 1:
            raise SystemExit(f"{native.name}: no entry test")
        out = out.replace(start, start + "    const s64 lf_floor_ = lf_floor(&s);\n")
        out = re.sub(r"\bgx_block\(&s, (\d+)u\)", r"lf_block(&s, lf_floor_, \1u)", out)
        out = self.tag_declines(out)
        for bad in ("gx_put", "gx_commit", "gx_start", "s_gx_log", "gx_decline", "lf_decline(&s)", "gx_block"):
            if bad in out:
                raise SystemExit(f"{native.name}: `{bad}` left in the native")
        return out

    @staticmethod
    def tag_declines(out):
        """Each decline with its reason (native_leaf_run.h's LF_*), from the
        test it follows."""
        d = r"\n( +)return gx_decline\(&s\);"
        rules = [
            (r"(if \(!(?:lf_start\(&s, &s_lf_log, cpu\)|gx_fp_ready\(cpu\)|gx_fp_arith_ready\(cpu\)|"
             r"gx_psq_ready\([^\n]*\))\))" + d, "LF_STATE"),
            (r"(if \(!lf_block\(&s, lf_floor_, \d+u\)\))" + d, "LF_BLOCK"),
            (r"(if \(s\.bad \|\| s\.fp\.bad\))" + d, "LF_BLOCK"),
            (r"(if \(!gx_live\(&s\)\))" + d, "LF_CLOCK"),
            (r"(if \(!\(s\.downcount - \d+ > -s\.budget\)\))" + d, "LF_CLOCK"),  # the inline register save's room
            (r"(if \(!gx_silent\(&s, 0x[0-9A-F]{8}u\)(?: \|\| !gx_silent\(&s, 0x[0-9A-F]{8}u\))*\))" + d, "LF_HOST"),
        ]
        for pattern, why in rules:
            out = re.sub(pattern, rf"\1\n\2return lf_decline(&s, {why});", out)
        # Anything else: a call, a jump or a return the native does not replay.
        return out.replace("return gx_decline(&s);", "return lf_decline(&s, LF_PATH);")


# --- The natives ---------------------------------------------------------
# (function ranges from tww's config/GZLE01/symbols.txt)
IEEE754_FMOD = (0x8032EC1C, 0x8032EF58)  # __ieee754_fmod (chunk 0203)
FMOD = (0x80330E34, 0x80330E54)          # fmod: __ieee754_fmod's wrapper
CM_RAD2S = (0x80246044, 0x8024609C)      # cM_rad2s (chunk 0145): fmod
CM_RND = (0x802462C8, 0x802463B0)        # cM_rnd: fmod
CM_RNDF = (0x802463B0, 0x802463E8)       # cM_rndF: cM_rnd
CM_RNDFX = (0x802463E8, 0x80246430)      # cM_rndFX: cM_rnd
SIN = (0x80330C84, 0x80330D5C)           # sin (chunk 0203): __ieee754_rem_pio2, the kernels
COS = (0x8033071C, 0x803307F0)           # cos
TAN = (0x80330D5C, 0x80330DD4)           # tan
REM_PIO2 = (0x8032EF58, 0x8032F2F8)      # __ieee754_rem_pio2 (its __kernel_rem_pio2 path declines)
KERNEL_SIN = (0x80330240, 0x803302E0)
KERNEL_COS = (0x8032F2F8, 0x8032F3EC)
KERNEL_TAN = (0x803302E0, 0x803304F4)
CLIB_ADD_CALC = (0x802528E4, 0x802529A4)  # cLib_addCalc (chunk 0148)
CLIB_ADD_CALC2 = (0x802529A4, 0x802529E8)
MAKE_BLCK_MIN_MAX = (0x80247C4C, 0x80247CD4)  # cBgW::MakeBlckMinMax (chunk 0145)
MAKE_BLCK_BND = (0x80247CD4, 0x80247E48)      # cBgW::MakeBlckBnd: MakeBlckMinMax, MakeBlckTransMinMax
MAKE_BLCK_TRANS_MIN_MAX = (0x80247BF8, 0x80247C4C)
JMA_EULER_TO_QUAT = (0x80301150, 0x80301218)  # JMAEulerToQuat (chunk 0191)
PSVEC_ADD = (0x8030DCE0, 0x8030DD04)          # the SDK's vector leaves (chunk 0197)
PSVEC_SUBTRACT = (0x8030DD04, 0x8030DD28)
PSVEC_SCALE = (0x8030DD28, 0x8030DD44)
PSVEC_MAG = (0x8030DE68, 0x8030DEAC)
PSVEC_DOT = (0x8030DEAC, 0x8030DECC)
PSVEC_CROSS = (0x8030DECC, 0x8030DF08)
PSVEC_SQUARE_DISTANCE = (0x8030E0B4, 0x8030E0DC)
CM3D_CALC_PLA = (0x8024A6F0, 0x8024A7BC)      # cM3d_CalcPla (chunk 0146)
U_GET_ATAN_TABLE = (0x8024609C, 0x802460D0)   # (chunk 0145)
CM_ATAN2S = (0x802460D0, 0x80246270)
CM_ATAN2F = (0x80246270, 0x802462B8)
CSPOLAR_VAL = (0x80254214, 0x80254420)        # cSPolar::Val (chunk 0148)
PNTWIND_GET_INFO = (0x8008A230, 0x8008A4C8)   # dKyw_pntwind_get_info (chunk 0034)
DKYR_GET_VECTLE_CALC = (0x8008AB94, 0x8008ABB4)  # dKyr_get_vectle_calc: get_vectle_calc
GET_VECTLE_CALC = (0x8008AB3C, 0x8008AB94)
CSANGLE_VAL = (0x80253C4C, 0x80253C54)        # cSAngle::Val(s16) (chunk 0148)
CSPOLAR_FORMAL = (0x802540F0, 0x802541B0)     # cSPolar::Formal: cSAngle's
CSANGLE = [(0x80253BE0, 0x80253C10), (0x80253E14, 0x80253E44), (0x80253C40, 0x80253C4C), (0x80253D30, 0x80253D40),
           (0x80253DB8, 0x80253DE4)]               # cSAngle(s16), operator-, Val(cSAngle), Inv, unary -
VECTLE_CALC = (0x8008AA30, 0x8008AB3C)        # vectle_calc (chunk 0034)

GROUPS = {
    # libm's fmod, and the random numbers and the angle conversion built on it.
    "libm": [
        Native("__ieee754_fmod", 0x8032EC1C, [IEEE754_FMOD]),
        Native("fmod", 0x80330E34, [FMOD, IEEE754_FMOD]),
        Native("cM_rad2s", 0x80246044, [CM_RAD2S, FMOD, IEEE754_FMOD]),
        Native("cM_rnd", 0x802462C8, [CM_RND, FMOD, IEEE754_FMOD]),
        Native("cM_rndF", 0x802463B0, [CM_RNDF, CM_RND, FMOD, IEEE754_FMOD]),
        Native("cM_rndFX", 0x802463E8, [CM_RNDFX, CM_RND, FMOD, IEEE754_FMOD]),
        Native("sin", 0x80330C84, [SIN, REM_PIO2, KERNEL_SIN, KERNEL_COS]),
        Native("cos", 0x8033071C, [COS, REM_PIO2, KERNEL_SIN, KERNEL_COS]),
        Native("tan", 0x80330D5C, [TAN, REM_PIO2, KERNEL_TAN]),
    ],
    # The collision blocks' bounds (a moving background's, every frame).
    "bgblk": [
        Native("cBgW::MakeBlckMinMax", 0x80247C4C, [MAKE_BLCK_MIN_MAX]),
        Native("cBgW::MakeBlckBnd", 0x80247CD4, [MAKE_BLCK_BND, MAKE_BLCK_MIN_MAX, MAKE_BLCK_TRANS_MIN_MAX, PSVEC_ADD]),
    ],
    # Rotations.
    "rot": [
        Native("JMAEulerToQuat", 0x80301150, [JMA_EULER_TO_QUAT]),
    ],
    # The game's own small calculations: easing a value toward a target, the
    # arc tangent of a slope as an angle.
    "calc": [
        Native("cLib_addCalc", 0x802528E4, [CLIB_ADD_CALC]),
        Native("cLib_addCalc2", 0x802529A4, [CLIB_ADD_CALC2]),
        Native("cM_atan2s", 0x802460D0, [CM_ATAN2S, U_GET_ATAN_TABLE]),
        Native("cM_atan2f", 0x80246270, [CM_ATAN2F, CM_ATAN2S, U_GET_ATAN_TABLE]),
    ],
    # Geometry on the SDK's vector leaves.
    "geom": [
        Native("cM3d_CalcPla", 0x8024A6F0, [CM3D_CALC_PLA, PSVEC_SUBTRACT, PSVEC_CROSS, PSVEC_MAG, PSVEC_SCALE,
                                            PSVEC_DOT]),
        Native("cSPolar::Val", 0x80254214, [CSPOLAR_VAL, CM_ATAN2F, CM_ATAN2S, U_GET_ATAN_TABLE, CSANGLE_VAL,
                                              CSPOLAR_FORMAL] + CSANGLE),
        Native("dKyw_pntwind_get_info", 0x8008A230, [PNTWIND_GET_INFO, DKYR_GET_VECTLE_CALC, GET_VECTLE_CALC,
                                                       VECTLE_CALC, PSVEC_SQUARE_DISTANCE]),
    ],
}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[2] / "cmake/composite"
    only = set(sys.argv[3].split(",")) if len(sys.argv) > 3 else None
    gen = LeafGen(root)
    for group, natives in GROUPS.items():
        if only is not None and group not in only:
            continue
        pieces = [f"/* Generated by scripts/windows/native_leaf_gen.py from a builder's composite-src: the",
                  f" * seventh set's {group} natives, each the translation's own blocks on local registers",
                  " * (native_leaf_run.h). Do not edit; regenerate, retest, recertify. */", ""]
        rows = []
        for native in natives:
            pieces.append(gen.generate(native))
            rows.append(f"    X(0x{native.entry:08X}u, lfn_{native.entry:08X}, \"{native.name}\", {native.entry:08X})")
            print(f"{group} {native.entry:08X} {native.name}: {native.blocks} blocks, boundaries "
                  + (", ".join(f"{b:08X}" for b in sorted(native.boundaries)) or "none"))
        inc = out_dir / f"native_{group}_gen.inc"
        with open(inc, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(pieces) + "\n")
        print(f"wrote {inc}")
        macro = f"{group.upper()}_NATIVES"
        listing = [f"/* Generated by scripts/windows/native_leaf_gen.py: the seventh set's {group} natives by",
                   " * entry (X(entry, native, name, the entry's digits)). Do not edit; regenerate. */",
                   f"#ifndef BLUEWAKE_NATIVE_{group.upper()}_LIST_H", f"#define BLUEWAKE_NATIVE_{group.upper()}_LIST_H",
                   "", f"#define {macro}(X) \\", " \\\n".join(rows), "", "#endif", ""]
        lst = out_dir / f"native_{group}_list.h"
        with open(lst, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(listing))
        print(f"wrote {lst}")


if __name__ == "__main__":
    main()
