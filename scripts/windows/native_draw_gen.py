#!/usr/bin/env python3
"""Write cmake/composite/native_draw_gen.inc and native_draw_list.h: the sixth
set's natives (the particle draw code and the sea's waves, native_draw.c),
generated as scripts/windows/native_gx_gen.py generates the fifth set's - each
a translated function's prepaid blocks replayed on local registers
(cmake/composite/native_gx_run.h) - with stops: where a native's path reaches
code it does not replay (a call it does not know, a boundary the host would be
asked about, a block it leaves to the translation) from its hook's own chunk,
the run ends just before that instruction with everything committed and the
hook goes on with the translation there; a hook at each such call's return
address (a resume) takes over again.

  native_draw_gen.py COMPOSITE_SRC [OUT]

A developer's tool, run by hand on a builder's composite-src; its output is
committed, and scripts/windows/native_entries.py hooks each entry and resume
only where the translation hashes to the one tests/native_draw_test.c was run
against.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_gx_gen import Gen, Native  # noqa: E402

# --- Function lists (tww's config/GZLE01/symbols.txt) ----------------------

def starts(*items):
    return [int(x, 16) for x in " ".join(items).split()]

# JPABaseShapeArc's getters (its destructor, 8025759C, left out).
JPA_BASE_GETTERS = starts(
    "80257550 80257560 80257570 8025757C 8025758C 802575F8 80257608 80257618 80257628 80257634",
    "80257640 8025764C 80257654 8025765C 80257668 80257674 80257684 80257694 802576B0 802576CC",
    "802576E8 80257704 80257720 8025773C 8025774C 8025775C 80257778 80257788 80257798 802577B4",
    "802577D0 802577EC 802577F8 80257804 80257814 80257824 80257834 8025784C 8025785C 8025786C",
    "80257878 80257888 80257898 802578AC 802578B8 802578CC 802578D8 802578E8 802578F4 80257904",
    "80257910 8025791C 80257928 80257934 80257940 8025794C 80257958 80257964 80257970 8025797C")
JPA_BASE_RANGE = (0x80257550, 0x80257988)
# JPAExtraShapeArc's getters (its destructor, 80257AF0, left out).
JPA_EXTRA_GETTERS = starts(
    "80257B4C 80257B5C 80257B6C 80257B7C 80257B8C 80257B9C 80257BAC 80257BB8 80257BC4 80257BD0",
    "80257BDC 80257BEC 80257BFC 80257C08 80257C14 80257C1C 80257C24 80257C2C 80257C34 80257C44",
    "80257C54 80257C60 80257C70 80257C80 80257C8C 80257C98 80257CA4 80257CB0 80257CC0 80257CCC",
    "80257CD8 80257CE4 80257CF0 80257CF8 80257D00 80257D10 80257D1C 80257D28 80257D34 80257D40")
JPA_EXTRA_RANGE = (0x80257B4C, 0x80257D4C)
GETTERS = JPA_BASE_GETTERS + JPA_EXTRA_GETTERS
GETTER_RANGES = [JPA_BASE_RANGE, JPA_EXTRA_RANGE]

# JPADrawContext::pcb->mDirTypeFunc's targets (JPADrawVisitor.cpp), with
# dirTypePrevPtcl's JPABaseEmitter::calcEmitterGlobalPosition.
DIR_TYPES = starts("8026134C 80261368 80261384 802613C4 802613E8")
DIR_TYPE_RANGES = [(0x8026134C, 0x802614A8), (0x8025DCDC, 0x8025DD5C)]
STRIPE_NEXT = starts("80263508 80263510")  # stripeGetNext, stripeGetPrev
STRIPE_NEXT_RANGE = (0x80263508, 0x80263518)

PSMTX_MULT_VEC = (0x8030DA44, 0x8030DA98)
# The matrix leaves the particle draws call: PSMTXMultVec, PSMTXMultVecArray,
# PSMTXMultVecSR, PSMTXConcat.
PSMTX = [PSMTX_MULT_VEC, (0x8030DA98, 0x8030DB24), (0x8030DB24, 0x8030DB78), (0x8030D0FC, 0x8030D1C8)]
# GXBegin's own blocks: where its dirty state needs a call (a draw's first
# after state changes), the native declines - the call is in GXBegin's chunk -
# and the translation makes it, the fifth set's natives at those callees'
# hooks.
GX_BEGIN = [(0x803230C4, 0x803231B4)]
GX_SET_TEV_COLOR = (0x80325FA8, 0x8032601C)
GX_SET_TEV_KCOLOR = (0x80326090, 0x80326104)
# GXLoadTexObj with GXLoadTexObjPreLoaded and the SDK's region callbacks (as
# native_gx_gen.py's GXLoadTexObj).
GX_LOAD_TEX_OBJ = [(0x80324EE8, 0x80324F3C), (0x8031FA48, 0x8031FAC4), (0x80324D28, 0x80324D30),
                   (0x80324D50, 0x80324EE8), (0x8031FAC4, 0x8031FAE8)]
GX_LOAD_TEX_OBJ_INDIRECT = {0x80324F10: [0x8031FA48], 0x80324E70: [0x8031FAC4]}
# sin, __kernel_sin, __ieee754_rem_pio2, __kernel_cos (not __kernel_rem_pio2:
# arguments past 2^20 pi/2 decline).
MSL_SIN = [(0x80330C84, 0x80330D5C), (0x80330240, 0x803302E0), (0x8032EF58, 0x8032F2F8), (0x8032F2F8, 0x8032F3EC)]
REGISTER_GLOBAL_OBJECT = (0x80328990, 0x803289A8)

# JPADrawContext::pcb->mRotTypeFunc's and mBasePlaneTypeFunc's targets.
ROT_TYPES = starts("802614A8 802614E8 80261528 80261568 802615C4")
BASE_PLANES = starts("8026161C 80261654")
CLIPBOARD_RANGES = [(0x802614A8, 0x8026168C)] + DIR_TYPE_RANGES

# The particle calc visitors (JPADrawCalc*::calc(const JPADrawContext*, JPABaseParticle*)).
CALC_PTCL = starts(
    "80264C88 80264DB8 80264EE8 802650B8 80265294 802652A4 80265374 80265444 80265588 802656CC",
    "80265734 8026579C 802657E8 80265880 80265918 802659C4 80265A90 80265B14 80265C40 80265D54",
    "80265EC4 80266048 80266100 802661B4 80266284 8026636C 8026640C 80266420")
CALC_PTCL_RANGE = (0x80264C88, 0x80266450)


# The vtables' slots (offset 8 + 4 x slot): JPABaseShape's and JPAExtraShape's
# virtual functions in their headers' order, JPABaseShapeArc's and
# JPAExtraShapeArc's functions there (an overloaded name: both); the one
# class of each that the game makes. A call through a vtable slot may reach
# either class's function there.
BASE_SLOTS = [(), (0x8025758C,), (0x802575F8,), (0x80257608,), (0x80257618,), (0x80257628,), (0x80257634,), (0x80257640,), (0x8025764C,), (0x80257654,), (0x8025765C,), (0x80257668,), (0x80257674,), (0x80257684,), (0x80257694,), (0x802576B0,), (0x8025773C,), (0x802576CC,), (0x802576E8,), (0x80257704,), (0x80257720,), (0x8025774C,), (0x8025775C,), (0x80257778,), (0x80257788,), (0x80257798,), (0x802577B4,), (0x802577D0,), (0x802577EC,), (0x802577F8,), (0x80257804,), (0x80257814,), (0x80257824,), (0x8025757C,), (0x80257834,), (0x8025784C,), (0x80257570,), (0x8025785C, 0x8025786C), (0x8025785C, 0x8025786C), (0x80257878,), (0x80257888,), (0x80257560,), (0x80257550,), (0x80257898, 0x802578AC), (0x802578B8, 0x802578CC), (0x80257898, 0x802578AC), (0x802578B8, 0x802578CC), (0x802578D8,), (0x802578E8,), (0x802578F4,), (0x80257904,), (0x80257910,), (0x8025791C,), (0x80257928,), (0x80257934,), (0x80257940,), (0x8025794C,), (0x80257958,), (0x80257964,), (0x80257970,), (0x8025797C,)]
EXTRA_SLOTS = [(), (0x80257B4C,), (0x80257B5C,), (0x80257B6C,), (0x80257B7C,), (0x80257B8C,), (0x80257B9C,), (0x80257BAC,), (0x80257BB8,), (0x80257BC4,), (0x80257BD0,), (0x80257BDC,), (0x80257BEC,), (0x80257BFC,), (0x80257C08,), (0x80257C14,), (0x80257C1C,), (0x80257C24,), (0x80257C2C,), (0x80257C34,), (0x80257C44,), (0x80257C54,), (0x80257C60,), (0x80257C70,), (0x80257C80,), (0x80257C8C,), (0x80257C98,), (0x80257CA4,), (0x80257CB0,), (0x80257CC0,), (0x80257CCC,), (0x80257CD8,), (0x80257CE4,), (0x80257CF0,), (0x80257CF8,), (0x80257D00,), (0x80257D10,), (0x80257D1C,), (0x80257D28,), (0x80257D34,), (0x80257D40,)]


def slot_targets(offset):
    slot = (offset - 8) // 4
    found = set()
    for table in (BASE_SLOTS, EXTRA_SLOTS):
        if offset >= 8 and offset % 4 == 0 and slot < len(table) and table[slot]:
            found |= set(table[slot])
    return sorted(found)


def call_sites(gen, entry, ranges, base_targets, vtable_targets):
    """The native's calls through CTR in its own function(s): a target loaded
    from a vtable (lwz r12, N(r12); mtctr r12; bctrl) may be any of
    `vtable_targets`; one loaded from the draw clipboard's function pointers
    (lwz r12, N(rX), N in `base_targets`) one of those."""
    found = {}
    for a, b in ranges:
        chunk = gen.chunk(a)
        if not chunk.owns(a):
            continue
        # The instructions, by address (each once: the main path's comments).
        text = {}
        for m in re.finditer(r"// ([0-9A-F]{8}): ([^\n]*)", chunk.main):
            text.setdefault(int(m.group(1), 16), m.group(2).strip())
        for site in sorted(x for x in text if a <= x < b and text[x] == "bctrl"):
            if not re.fullmatch(r"mtctr +r12", text.get(site - 4, "")):
                continue
            m = re.fullmatch(r"lwz +r12, (\d+)\((r\d+)\)", text.get(site - 8, ""))
            if not m:
                continue
            offset, base = int(m.group(1)), m.group(2)
            if base == "r12" and vtable_targets:
                found[site] = slot_targets(offset)
            elif base != "r12" and offset in base_targets:
                found[site] = base_targets[offset]
    return found


CLIPBOARD_POINTERS = {0xA0: DIR_TYPES, 0xA4: ROT_TYPES, 0xA8: BASE_PLANES}


def native(name, entry, ranges, indirect=None, virtual=None, **kw):
    n = Native(name, entry, ranges, dict(indirect or {}), stops=True, **kw)
    n.virtual = virtual
    return n


def particle(name, entry, size):
    """A draw visitor of one particle (exec(const JPADrawContext*, JPABaseParticle*))."""
    return native(name, entry, [(entry, entry + size)] + PSMTX + GX_BEGIN + CLIPBOARD_RANGES + GETTER_RANGES +
                  [GX_SET_TEV_COLOR], virtual=GETTERS)


NATIVES = [
    # The particle draws (JPADrawVisitor.cpp): one particle each, or a strip
    # through all of an emitter's.
    particle("JPADrawExecRotBillBoard::exec", 0x80260D24, 0x208),
    particle("JPADrawExecBillBoard::exec", 0x80260BAC, 0x178),
    particle("JPADrawExecYBillBoard::exec", 0x80260F2C, 0x1E0),
    particle("JPADrawExecRotYBillBoard::exec", 0x8026110C, 0x240),
    particle("JPADrawExecRotDirectional::exec", 0x80261AD0, 0x490),
    particle("JPADrawExecDirectionalCross::exec", 0x80261F60, 0x574),
    particle("JPADrawExecRotDirectionalCross::exec", 0x802624D4, 0x5C4),
    particle("JPADrawExecDirBillBoard::exec", 0x80262A98, 0x328),
    particle("JPADrawExecRotation::exec", 0x80262DC0, 0x1FC),
    particle("JPADrawExecRotationCross::exec", 0x80262FBC, 0x330),
    particle("JPADrawExecPoint::exec", 0x802632EC, 0x94),
    particle("JPADrawExecLine::exec", 0x80263380, 0x188),
    native("JPADrawExecStripe::exec", 0x80263518,
           [(0x80263518, 0x80263A68), STRIPE_NEXT_RANGE] + CLIPBOARD_RANGES + GX_BEGIN + GETTER_RANGES,
           {0x802639E4: STRIPE_NEXT}, virtual=GETTERS),
    native("JPADrawExecStripeCross::exec", 0x80263A68,
           [(0x80263A68, 0x802643B0), STRIPE_NEXT_RANGE] + CLIPBOARD_RANGES + GX_BEGIN + GETTER_RANGES,
           {0x80263F48: STRIPE_NEXT, 0x80264328: STRIPE_NEXT}, virtual=GETTERS),
    # The colour registers, per particle and per emitter (the three with two
    # colours; the others are in NOT_FASTER below).
    particle("JPADrawExecRegisterPrmCEnv::exec", 0x802605FC, 0x12C),
    particle("JPADrawExecRegisterPrmAEnv::exec", 0x80260728, 0x130),
    particle("JPADrawExecRegisterColorEmitterPE::exec", 0x802643B0, 0x104),
    # The particle calc visitors, and JPADraw::calcParticle with all of them.
    native("JPADrawCalcScaleX::calc", 0x80264C88, [(0x80264C88, 0x80264DB8)] + GETTER_RANGES, virtual=GETTERS),
    native("JPADrawCalcScaleY::calc", 0x80264DB8, [(0x80264DB8, 0x80264EE8)] + GETTER_RANGES, virtual=GETTERS),
    native("JPADrawCalcScaleXBySpeed::calc", 0x80264EE8, [(0x80264EE8, 0x802650B8)] + GETTER_RANGES, virtual=GETTERS),
    native("JPADrawCalcAlpha::calc", 0x80265B14, [(0x80265B14, 0x80265C40)] + GETTER_RANGES, virtual=GETTERS),
    native("JPADraw::calcParticle", 0x80268940, [(0x80268940, 0x802689C4), CALC_PTCL_RANGE] + GETTER_RANGES,
           {0x80268994: CALC_PTCL}, virtual=GETTERS),
    # The sea (d_kankyo_rain.cpp drawWave): its loop over the waves, from the
    # loop's head on to the function's blr - sin, GXLoadTexObj, GXSetTevKColor,
    # four PSMTXMultVec and the quad's statics' one-time registration with it,
    # each wave stopping before GXBegin (left to the translation, the fifth
    # set's natives at its hooks: its dirty state needs calls every wave) and
    # resuming after it with the quad's four vertices.
    native("drawWave", 0x8009A094, [(0x8009A094, 0x8009A5D4)] + MSL_SIN + GX_LOAD_TEX_OBJ +
           [GX_SET_TEV_KCOLOR, PSMTX_MULT_VEC, REGISTER_GLOBAL_OBJECT], GX_LOAD_TEX_OBJ_INDIRECT),
]

# Exact (60,000 cases each, 0 mismatches) but not faster than their
# translation through the hooks with the fifth set's natives on (0.92x to
# 1.05x over five benchmark runs): one GXSetTevColor each, which the fifth
# set's native already runs, and little else. Listed, not built.
NOT_FASTER = [
    particle("JPADrawExecRegisterPrmColorAnm::exec", 0x802603E4, 0xC8),
    particle("JPADrawExecRegisterPrmAlphaAnm::exec", 0x802604AC, 0xCC),
    particle("JPADrawExecRegisterEnvColorAnm::exec", 0x80260578, 0x84),
    particle("JPADrawExecRegisterColorEmitterP::exec", 0x802644B4, 0xA0),
    particle("JPADrawExecRegisterColorEmitterE::exec", 0x80264554, 0x88),
]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = Path(sys.argv[1])
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else \
        Path(__file__).resolve().parents[2] / "cmake/composite/native_draw_gen.inc"
    gen = Gen(root, "drn_")
    pieces = ["/* Generated by scripts/windows/native_draw_gen.py from a builder's composite-src: the",
              " * sixth set's natives, each the translation's own blocks on local registers",
              " * (native_gx_run.h), with stops. Do not edit; regenerate, retest, recertify. */", ""]
    rows = []
    for n in NATIVES:
        for site, targets in call_sites(gen, n.entry, n.ranges, CLIPBOARD_POINTERS, n.virtual).items():
            n.indirect.setdefault(site, targets)
        pieces.append(gen.generate(n))
        rows.append(f"    X(0x{n.entry:08X}u, drn_{n.entry:08X}, \"{n.name}\", {n.entry:08X})")
        rows += [f"    X(0x{r:08X}u, drn_{n.entry:08X}, \"{n.name}@{r:08X}\", {r:08X})" for r in n.resumes]
        print(f"{n.entry:08X} {n.name}: {n.blocks} blocks, {len(n.resumes)} resumes, stops at "
              + (" ".join(f"{s:08X}" for s in sorted(n.stops_at)) or "none") + "; boundaries "
              + (" ".join(f"{b:08X}" for b in sorted(n.boundaries)) or "none"))
    pieces.append("")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(pieces))
    print(f"wrote {out_path}")
    listing = ["/* Generated by scripts/windows/native_draw_gen.py: the sixth set's natives by entry, each",
               " * resume its own row (X(entry, native, name, the entry's digits)). Do not edit; regenerate. */",
               "#ifndef BLUEWAKE_NATIVE_DRAW_LIST_H", "#define BLUEWAKE_NATIVE_DRAW_LIST_H", "",
               "#define DRAW_NATIVES(X) \\", " \\\n".join(rows), "", "#endif", ""]
    list_path = out_path.with_name("native_draw_list.h")
    with open(list_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(listing))
    print(f"wrote {list_path}")


if __name__ == "__main__":
    main()
