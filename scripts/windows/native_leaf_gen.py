#!/usr/bin/env python3
"""Write the seventh set's natives: leaf compute code (libm's fmod, sin, cos
and tan, the random numbers and angle built on fmod, the collision blocks'
bounds, the Euler quaternions, the arc tangents, the planes, polar
coordinates and point winds), each a translated function's prepaid blocks -
callees included - replayed on local registers: scripts/windows/native_gx_gen.py's
generator, here for code that writes RAM only. For each group:

  cmake/composite/native_<group>_gen.inc   (the natives)
  cmake/composite/native_<group>_list.h    (X(entry, native, name, digits))

  native_leaf_gen.py COMPOSITE_SRC [OUT_DIR [GROUP,...]]

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
  function, reached by a branch or by a block's text running on into it):
  each replayed as a block of its own, its prepaid copy on the same locals -
  the loop function's precharge test and budget test at its head (one block
  test), its body, its back edge's budget test (gx_live), and on to label_Y
  where it leaves;
- each block's test one comparison against the clock's floor (lf_block);
- single loads widened by the hardware where the single is normal, as the
  chunks' inline_fp.h does (lf_f32_from_bits);
- the double divide (ppc_fdiv, which the translation hands GXRuntime's
  interpreter in every case) and the interpreter's fctiw, frsqrte and frsp:
  fctiwz and frsp of anything but a NaN written out on the run's FPSCR
  (nr_fctiwz, lf_frsp), the rest the interpreter's own function on a scratch
  state that holds the run's FPSCR and the registers it reads;
- a decline's reason counted (lf_decline: the state at entry, the clock, an
  address, an FP operand off the inline path, the host at a boundary, a call
  or jump not replayed).

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
        # ps_neg and the other sign operations on a pair's halves: generated.h's
        # dolrecomp_ps_from_bits/to_bits are the single conversions.
        text = text.replace("dolrecomp_ps_from_bits(", "gx_f32_from_bits(").replace(
            "dolrecomp_ps_to_bits(", "gx_f32_to_bits(")
        # A block that runs on into an extracted loop (the loop function called
        # where its head falls in the block's text): a jump to the loop's own
        # block, whose test is the loop function's first.
        def into_loop(m):
            head, exit_ = int(m.group(1), 16), int(m.group(2), 16)
            if chunk.loops.get(head) != exit_:
                raise SystemExit(f"{native.name}: loop_{head:08X} of an unknown form")
            return f"    goto label_{head:08X};\n"
        text = re.sub(r"    loop_([0-9A-F]{8})\(ctx\);\n    if \(ctx->pc == 0x([0-9A-F]{8})u\) goto label_\2;\n"
                      r"    return;\n", into_loop, text)
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
        out = re.sub(r"\bgx_(psq_st|psq_l|f32_from_bits|fctiw|frsqrte|frsp)\(", r"lf_\1(", out)
        out = out.replace("gx_commit(&s);", "lf_commit(&s);")
        # Each block's leader against the clock's floor (native_leaf_run.h's
        # lf_block: one test where gx_block makes three).
        start = "    if (!lf_start(&s, &s_lf_log, cpu))\n        return gx_decline(&s);\n"
        if out.count(start) != 1:
            raise SystemExit(f"{native.name}: no entry test")
        out = out.replace(start, start + "    const s64 lf_floor_ = lf_floor(&s);\n")
        # A native whose work can outlast the clock by far (a loop over a
        # block's triangles): its group's room test first, which declines at
        # once where the whole run cannot fit (lf_room_<entry>, written by
        # hand beside the natives: nothing has changed yet, so it is always
        # exact; it only spares the replay a run it would undo).
        if getattr(native, "room", False):
            room = (f"    if (!lf_room_{native.entry:08X}(cpu, lf_floor_))\n"
                    "        return gx_decline(&s); /* the clock: no room for the run */\n")
            out = out.replace("    const s64 lf_floor_ = lf_floor(&s);\n",
                              "    const s64 lf_floor_ = lf_floor(&s);\n" + room, 1)
        out = re.sub(r"\bgx_block\(&s, (\d+)u\)", r"lf_block(&s, lf_floor_, \1u)", out)
        out = self.tag_declines(out)
        # The boundaries' test with the host's half asked once a run (lf_silent).
        out = re.sub(r"\bgx_silent\(", "lf_silent(", out)
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
            (r"(if \(!lf_room_[0-9A-F]{8}\(cpu, lf_floor_\)\))" + d, "LF_CLOCK"),
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


def roomy(native):
    """A native with a room test (LeafGen.generate_once)."""
    native.room = True
    return native


# --- The natives ---------------------------------------------------------
# Made, tested exact and dropped as no faster than their translation (or not
# by 25 percent in every run): cM3dGAab::SetMinMax (1.26x through the
# dispatcher), SetMin and SetMax (1.0x: a dozen instructions each, the native's
# entry and exit cost what the blocks save), JASystem::Player::pitchToCent
# (0.94x); cXyz::operator- (with PSVECSubtract) and mDoLib_project (with
# PSMTXMultVec), 1.6-1.8x against a translation whose leaf runs translated but
# 1.22-1.24x through the hooked chunks, where the leaf is native_vec.c's or
# native_math.c's as in play. Left out unmade: cLib_addCalc and cLib_addCalc2 (60 Hz simulation
# sites, rewritten by frame-rate mode), dKyw_pntwind_get_vecpow (its
# cXyz::operator* is a game-math entry whose hook sits before its leader).
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
MAKE_BLCK_MIN_MAX = (0x80247C4C, 0x80247CD4)  # cBgW::MakeBlckMinMax (chunk 0145)
MAKE_BLCK_BND = (0x80247CD4, 0x80247E48)      # cBgW::MakeBlckBnd: MakeBlckMinMax, MakeBlckTransMinMax
MAKE_BLCK_TRANS_MIN_MAX = (0x80247BF8, 0x80247C4C)
JMA_EULER_TO_QUAT = (0x80301150, 0x80301218)  # JMAEulerToQuat (chunk 0191)
PSVEC_ADD = (0x8030DCE0, 0x8030DD04)          # the SDK's vector leaves (chunk 0195)
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
OSC_GET_OFFSET = (0x8028DF2C, 0x8028E070)     # JASystem::TOscillator::getOffset (chunk 0163): calc
OSC_CALC = (0x8028E238, 0x8028E5EC)
DRIVER_UPDATE_INTERVAL = (0x8028AAE4, 0x8028AAEC)  # JASystem::Driver::getUpdateInterval (chunk 0162)
CVT_FP2UNSIGNED = (0x80328E10, 0x80328E6C)    # __cvt_fp2unsigned (chunk 0201)
CH_UPDATE_EFFECTOR = (0x8028C3A8, 0x8028C62C)  # JASystem::TChannel::updateEffectorParam (chunk 0162)
CH_CALC_EFFECT = (0x8028CABC, 0x8028CB88)
CH_CALC_PAN = (0x8028CB88, 0x8028CC90)
CH_UPDATE_AUTO_MIXER = (0x8028CD90, 0x8028CEA8)
CH_UPDATE_MIXER = (0x8028CEA8, 0x8028D128)
CALC_SINF_T = (0x8027A9C8, 0x8027A9F4)        # JASystem::Calc::sinfT, sinfDolby2 (chunk 0158)
CALC_SINF_DOLBY2 = (0x8027A9F4, 0x8027AA20)
DSP_SET_AUTO_MIXER = (0x8028A740, 0x8028A764)  # DSPInterface::DSPBuffer::setAutoMixer
DRIVER_LEVELS = [(0x8028AAC4, 0x8028AACC), (0x8028AACC, 0x8028AAD4), (0x8028AADC, 0x8028AAE4)]
                                               # Driver::getChannelLevel, getAutoLevel, getOutputMode


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
        roomy(Native("cBgW::MakeBlckBnd", 0x80247CD4, [MAKE_BLCK_BND, MAKE_BLCK_MIN_MAX, MAKE_BLCK_TRANS_MIN_MAX,
                                                     PSVEC_ADD])),
    ],
    # Rotations.
    "rot": [
        Native("JMAEulerToQuat", 0x80301150, [JMA_EULER_TO_QUAT]),
    ],
    # The game's arc tangent of a slope as an angle (cLib_addCalc and
    # cLib_addCalc2, its easing, are 60 Hz simulation sites -
    # prepare_simulation_60hz.py rewrites them by frame-rate mode - and stay
    # translated).
    "calc": [
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
    # JASystem's leaves: an envelope oscillator's value, a pitch's cents.
    "jas": [
        Native("JASystem::TOscillator::getOffset", 0x8028DF2C, [OSC_GET_OFFSET, OSC_CALC, DRIVER_UPDATE_INTERVAL,
                                                                 CVT_FP2UNSIGNED]),
        Native("JASystem::TOscillator::calc", 0x8028E238, [OSC_CALC, DRIVER_UPDATE_INTERVAL, CVT_FP2UNSIGNED]),
        # A channel's pan, effect and surround sends and its mixer's volumes.
        Native("JASystem::TChannel::updateEffectorParam", 0x8028C3A8,
               [CH_UPDATE_EFFECTOR, CH_CALC_EFFECT, CH_CALC_PAN, CH_UPDATE_AUTO_MIXER, CH_UPDATE_MIXER, CALC_SINF_T,
                CALC_SINF_DOLBY2, DSP_SET_AUTO_MIXER] + DRIVER_LEVELS),
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
