#!/usr/bin/env python3
"""Certified native entries, the second set: hooks at the entries of
translated GZLE01 functions with a native form in cmake/composite
(native_fifo.c, native_bg.c, native_vec.c's PSMTXMultVecSR,
native_mtxcalc.c).
The third set (native_search.c: strcmp and dStage_searchName, the actor
search by name) is hooked the same way, by the entries added below the
second set's.

  native_entries.py COMPOSITE_SRC
  native_entries.py --hashes COMPOSITE_SRC    (print each entry's body hash)

Each hook goes right after the function's entry label, so every way into
the function meets it - a call inside the chunk (a goto), a direct call from
another chunk, a dispatch - and tries the native first:

    if (bluewake_native_fifo_enabled && bluewake_native_fifo(ctx, 0x802D8BD8u))
        goto return_dispatch_802D56E0;

The native either ran the whole function to its blr, leaving pc at the
return address, and the chunk's return dispatch carries on from there as the
blr's would; or it changed nothing and the translation runs as before.

Only where the translation is the one the native's comparison test
(tests/native_*_test.c) compared it against: every fragment of the function
(the blocks from its entry label, in each chunk it spans) and every prepaid
copy those blocks jump to (scripts/windows/fast_blocks.py and lean_memory.py
made them, and they are what runs), whitespace aside and this script's hooks
removed, hashes to the certified value - in the base chunk and in every mod's
variant of it. An entry whose fragments differ anywhere, or one of whose
addresses the host watches (direct_calls.watched_addresses), is not hooked,
and the step says so; a rerun after a change removes a hook it no longer
certifies. The change is repeatable and keeps LF line ends.

Run it after lean_memory.py and prepare_native_j3d.py, and before
scripts/mods/prepare_simulation_60hz.py and prepare_native_math.py, whose
manifests hash the chunks as they end up.
"""
import hashlib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from direct_calls import watched_addresses  # noqa: E402

MARK = "/* bluewake: certified native entries (scripts/windows/native_entries.py) */\n"
INCLUDE = '#include "../generated.h"\n'

# group -> (header, enable flag, native call with {entry})
GROUPS = {
    "fifo": ("native_fifo.h", "bluewake_native_fifo_enabled", "bluewake_native_fifo(ctx, 0x{entry:08X}u)"),
    "bg": ("native_bg.h", "bluewake_native_bg_enabled", "bluewake_native_bg(ctx, 0x{entry:08X}u)"),
    "vec_sr": ("native_vec.h", "bluewake_native_vec_sr_enabled", "bluewake_native_vec_sr(ctx)"),
    "mtxcalc": ("native_mtxcalc.h", "bluewake_native_mtxcalc_enabled", "bluewake_native_mtxcalc(ctx, 0x{entry:08X}u)"),
}
# --- The third set: the actor search by name (cmake/composite/native_search.c) ---
GROUPS["search"] = ("native_search.h", "bluewake_native_search_enabled",
                    "bluewake_native_search(ctx, 0x{entry:08X}u)")
# --- end of the third set's groups ---

# entry -> (name, group, fragments [(chunk start, first address, end address)],
#           SHA-256 of the canonical fragments and prepaid copies)
ENTRIES = {
    0x802D8BD8: ("J3DFifoLoadPosMtxImm", "fifo", [(0x802D56E0, 0x802D8BD8, 0x802D8C58)],
                 "c1f2664aa12f9dd8c195fcf4a862e77793b889902ca390018f1b1b5090c4dbdf"),
    0x802D8C58: ("J3DFifoLoadNrmMtxImm", "fifo", [(0x802D56E0, 0x802D8C58, 0x802D8CC4)],
                 "6cee7486d365cb81b8d1b9e5b63dd0720e271af0d4bb323be726d4333e3ec9ad"),
    0x802D8CC4: ("J3DFifoLoadNrmMtxImm3x3", "fifo", [(0x802D56E0, 0x802D8CC4, 0x802D8D30)],
                 "2affe02fcb1211515e55f97b2eab1c7f685fe661c30259af77361385fec49ee9"),
    0x8024734C: ("cBgS_Chk::ChkSameActorPid", "bg", [(0x802456E0, 0x8024734C, 0x8024738C)],
                 "a3cf44b28b18dfc3b21022150567e7e5223ea3ff3ffdc777e374f7fdd806d1ee"),
    0x800A9684: ("dBgW::ChkGrpThrough", "bg", [(0x800A56E0, 0x800A9684, 0x800A96E0),
                                               (0x800A96E0, 0x800A96E0, 0x800A974C)],
                 "d491b8f1f60e6ad9ae20524d879bb8b1d74d9608e919e6c07601e8dcfa69227f"),
    0x8030DB24: ("PSMTXMultVecSR", "vec_sr", [(0x8030D6E0, 0x8030DB24, 0x8030DB78)],
                 "600139a8e4e0caabf4a3006b3eb737577c0934568e1741186eeec1b9565b7db9"),
    # The joint matrix calculations, and the callees they stand in for too:
    # J3DGetTranslateRotateMtx (802DA64C or 802DA724), PSMTXConcat (8030D0FC),
    # PSMTXCopy (8030D0C8).
    0x802F5090: ("J3DMtxCalcBasic::calcTransform", "mtxcalc",
                 [(0x802F16E0, 0x802F5090, 0x802F525C), (0x802D96E0, 0x802DA64C, 0x802DA724),
                  (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803096E0, 0x8030D0C8, 0x8030D0FC)],
                 "3e442f9bac67c268093ed4abdbcb9c4659fad59339a700282a7eb9e023b11d67"),
    0x802F52BC: ("J3DMtxCalcSoftimage::calcTransform", "mtxcalc",
                 [(0x802F16E0, 0x802F52BC, 0x802F5508), (0x802D96E0, 0x802DA724, 0x802DA7E4),
                  (0x803096E0, 0x8030D0FC, 0x8030D1C8), (0x803096E0, 0x8030D0C8, 0x8030D0FC)],
                 "7ad20637ab7fb125cfee5c1aa81bd982abfc8bbe5f26747bae359fa320fd56d2"),
    0x802F5508: ("J3DMtxCalcMaya::calcTransform", "mtxcalc",
                 [(0x802F16E0, 0x802F5508, 0x802F56E0), (0x802F56E0, 0x802F56E0, 0x802F5724),
                  (0x802D96E0, 0x802DA64C, 0x802DA724), (0x803096E0, 0x8030D0FC, 0x8030D1C8),
                  (0x803096E0, 0x8030D0C8, 0x8030D0FC)],
                 "8df3c54d0620dc3152b7b640038b7637d9514d3e51cd6133f4dfb4b836f9c324"),
}
# --- The third set (tests/native_search_test.c): strcmp, and dStage_searchName
# with the strcmps it calls in strcmp's chunk, which its native stands in for
# too. ---
ENTRIES[0x8032DB44] = ("strcmp", "search", [(0x8032D6E0, 0x8032DB44, 0x8032DC6C)],
                       "0a578813114c0553f81a447d7d0c6ef7d36b22afc5e5f6465ae01517bb5a2375")
ENTRIES[0x80041544] = ("dStage_searchName", "search",
                       [(0x8003D6E0, 0x80041544, 0x800415B4), (0x8032D6E0, 0x8032DB44, 0x8032DC6C)],
                       "bdf91cee67046b81e7b60bb4669129749a912db84cd9fd96028b74a4cac01dd7")
# cTgIt_JudgeFilter with fopAcM_findObjectCB as its judge (one call), with the
# callees its native stands in for: fopAcM_findObjectCB, dStage_searchName and
# strcmp; and cNdIt_Judge's loop, which the walk native the host may call
# (bluewake_native_search_judge) runs too - the hook arms it.
ENTRIES[0x80245640] = ("cTgIt_JudgeFilter", "search",
                       [(0x802416E0, 0x80245640, 0x80245674), (0x802416E0, 0x80244F78, 0x80244FB4),
                        (0x800256E0, 0x8002833C, 0x80028410), (0x8003D6E0, 0x80041544, 0x800415B4),
                        (0x8032D6E0, 0x8032DB44, 0x8032DC6C)],
                       "795c6cb80232a10a8e2ceca570ee310e52f37a4ca4931a70920261c6f75385c2")
# --- end of the third set's entries ---
# --- The third set, resumable (tests/native_search_test.c): dStage_searchName's
# loop at its call block, a strcmp's return and its step, where a search the
# native stopped for the host's window, or the translation began, goes on
# natively. The same translation as the entry's (the whole function and
# strcmp), so the same hash. ---
for _resume in (0x8004156C, 0x80041578, 0x80041588):
    ENTRIES[_resume] = ("dStage_searchName at %08X" % _resume, "search",
                        [(0x8003D6E0, 0x80041544, 0x800415B4), (0x8032D6E0, 0x8032DB44, 0x8032DC6C)],
                        "bdf91cee67046b81e7b60bb4669129749a912db84cd9fd96028b74a4cac01dd7")
# --- end of the resumable entries ---
# --- The fourth set (tests/native_kankyo_test.c): the environment's colour
# blends (cmake/composite/native_kankyo.c). kankyo_color_ratio_set stands in
# for the three s16_data_ratio_set calls it makes inside its chunk, so its
# hash covers that function too. ---
GROUPS["kankyo"] = ("native_kankyo.h", "bluewake_native_kankyo_enabled",
                    "bluewake_native_kankyo(ctx, 0x{entry:08X}u)")
ENTRIES[0x8018F894] = ("s16_data_ratio_set (d_kankyo)", "kankyo", [(0x8018D6E0, 0x8018F894, 0x8018F8E4)],
                       "a97d798df755a9dfa8d8c76e9387bd63fcb1a3a01ac77dea64257a70f3c02ed2")
ENTRIES[0x8018F8E4] = ("kankyo_color_ratio_set", "kankyo",
                       [(0x8018D6E0, 0x8018F8E4, 0x8018F9E8), (0x8018D6E0, 0x8018F894, 0x8018F8E4)],
                       "97603052f10971cf557a41245d089cb826682dbbbd57ffdc4fc68d9c38f2106b")
ENTRIES[0x8019803C] = ("s16_data_ratio_set (d_kyeff)", "kankyo", [(0x801956E0, 0x8019803C, 0x8019808C)],
                       "5d00d5f30faa943ea2066d221c0027d91cf9cdf449c1dd493964417803fc6c82")
# --- end of the fourth set's colour blends ---
# --- The fourth set (tests/native_anim_test.c): J3D's key-frame animation
# (cmake/composite/native_anim.c). Each entry's hash covers the callees its
# native stands in for: JMAHermiteInterpolation (in its own chunk) under
# J3DGetKeyFrameInterpolation<f32>, J3DHermiteInterpolationS under
# J3DGetKeyFrameInterpolationS, and all four under calcTransform. ---
GROUPS["anim"] = ("native_anim.h", "bluewake_native_anim_enabled", "bluewake_native_anim(ctx, 0x{entry:08X}u)")
_ANIM_HERMITE = (0x802FD6E0, 0x803012D8, 0x80301350)
_ANIM_KEY_F = (0x802F16E0, 0x802F2DAC, 0x802F2EF8)
_ANIM_HERMITE_S = (0x802ED6E0, 0x802F06D8, 0x802F072C)
_ANIM_KEY_S = (0x802ED6E0, 0x802F072C, 0x802F0954)
ENTRIES[0x803012D8] = ("JMAHermiteInterpolation", "anim", [_ANIM_HERMITE], "3db6672c3359fdffca724dccbc68927255deb4eb8e325ab50b41a5024002283e")
ENTRIES[0x802F2DAC] = ("J3DGetKeyFrameInterpolation<f32>", "anim", [_ANIM_KEY_F, _ANIM_HERMITE], "392cbc991dd96ab0eb97fb421508640e977421cdeb4f38dbc174605658c9d53a")
ENTRIES[0x802F072C] = ("J3DGetKeyFrameInterpolationS", "anim", [_ANIM_KEY_S, _ANIM_HERMITE_S], "1eea0238808243d866100a41ad995a9f50c1fd4df493d3f22335ec9bfa2f5813")
ENTRIES[0x802F0954] = ("J3DAnmTransformKey::calcTransform", "anim",
                       [(0x802ED6E0, 0x802F0954, 0x802F0E20), _ANIM_KEY_S, _ANIM_HERMITE_S, _ANIM_KEY_F,
                        _ANIM_HERMITE], "0e3e908a655916320aaef9b8bf7bcd304406d7f9ad3983ea056905c670cbd549")
# --- end of the fourth set's animation ---
# prepare_native_j3d.py's hooks, in the J3DGetTranslateRotateMtx fragments
# (it runs first): not part of the translation certified here.
J3D_HOOK = re.compile(r"    /\* bluewake: recovered J3D matrix [0-9A-F]{8} \*/\n"
                      r"    if \(bluewake_native_j3d_enabled && bluewake_native_j3d_transform\(ctx, 0x[0-9A-F]{8}u\)\)\n"
                      r"        goto return_dispatch_[0-9A-F]{8};\n")

LABEL = re.compile(r"\n(?:label_([0-9A-F]{8})|return_dispatch_[0-9A-F]{8}):")
FAST_JUMP = re.compile(r"\bgoto (bwfast_\d+);")


def hook_text(entry, chunk):
    _, group, _, _ = ENTRIES[entry]
    _, flag, call = GROUPS[group]
    return (f"    if ({flag} && {call.format(entry=entry)})\n"
            f"        goto return_dispatch_{chunk:08X};\n")


def strip_hooks(text):
    for entry, (_, _, fragments, _) in ENTRIES.items():
        text = text.replace(hook_text(entry, fragments[0][0]), "")
    return J3D_HOOK.sub("", text)


def main_function(text, chunk):
    """The chunk's own function (the loops the translator extracted come
    first, with prepaid copies of their own under the same names)."""
    begin = text.find(f"\nvoid func_{chunk:08X}(CPUState* ctx_param) {{\n")
    if begin < 0:
        return None
    end = text.find("\n}\n", begin)
    return text[begin:end + 3] if end >= 0 else None


def fragment(function, start, end):
    """The blocks from label_START up to the first label at END or beyond (or
    the chunk's return dispatch), and the prepaid copies they jump to."""
    begin = function.find(f"\nlabel_{start:08X}:\n")
    if begin < 0:
        return None
    finish = None
    for m in LABEL.finditer(function, begin + 1):
        if m.group(1) is None or int(m.group(1), 16) >= end:
            finish = m.start()
            break
    if finish is None:
        return None
    body = function[begin:finish]
    copies = []
    dispatch = function.find("\nreturn_dispatch_")
    for name in FAST_JUMP.findall(body):
        at = function.find(f"\n{name}:\n", dispatch)
        if at < 0:
            return None
        stop = function.find("\nbwfast_", at + 1)
        if stop < 0:
            stop = function.rfind("\n}\n")
        copies.append(function[at:stop])
    return body, copies


def canonical(text):
    return " ".join(strip_hooks(text).split())


def addresses(text):
    found = set()
    for m in re.finditer(r"label_([8C][0-9A-F]{7})|// ([8C][0-9A-F]{7}):|ctx->(?:pc|lr) = 0x([8C][0-9A-F]{7})u",
                         text):
        found.add(int(next(v for v in m.groups() if v), 16))
    return found


# --- The third set: addresses on the watch list its hooks do not run past ---
# The host names cTgIt_JudgeFilter's entry (0x80245640), the bctrl in
# cNdIt_Judge that calls it (0x80244F84) and that call's return (0x80244F88)
# for its own actor-search native (runtime/host/src/main.c,
# BW_SEARCH_JUDGE_FILTER, BW_SEARCH_NDIT_RETURN), so all three are on the
# watch list. None is a boundary the third set's hooks run past: the
# JudgeFilter hook sits at the entry, which is reached only by a dispatch
# after the host's edge service (cNdIt_Judge's bctrl goes round the loop: its
# return is watched) or by a goto inside the chunk, as the translation reaches
# it; its call ends at its blr, whose return to cNdIt_Judge goes through the
# chunk's own return dispatch as before; 0x80244F84 is the middle of a block.
# The cNdIt_Judge fragment is hashed for the walk native, which only the host
# calls, at that boundary. And fopAcM_findObjectCB calls OSPanic (0x80006C4C,
# which the host reports) only on a NULL search parameter, where the native
# declines: the translation it stands in for never reaches it. So in those
# fragments - and only there - those addresses are left out of the check.
SEARCH_HOST_OWN = {
    "\nlabel_80245640:\n": {0x80245640, 0x80244F84, 0x80244F88},  # cTgIt_JudgeFilter
    "\nlabel_80244F78:\n": {0x80245640, 0x80244F84, 0x80244F88},  # cNdIt_Judge's loop
    "\nlabel_8002833C:\n": {0x80006C4C},                          # fopAcM_findObjectCB
}
_addresses_of = addresses


def addresses(text):  # noqa: F811 (the check above, less what SEARCH_HOST_OWN leaves out)
    found = _addresses_of(text)
    for start, own in SEARCH_HOST_OWN.items():
        if text.startswith(start):
            found -= own
    return found
# --- end of the third set's exemption ---


# --- The fourth set: mods' variants of a chunk ---
# A mod's variant of a chunk (scripts/mods/build_mod_variants.py) names its
# function func_XXXXXXXX__mod_NAME. The second and third sets' chunks have no
# variants; the fourth set's kankyo chunk (8018D6E0) has two (the widescreen
# mods), whose fragments must hash the same as the base's to be hooked.
_main_function_of = main_function


def main_function(text, chunk):  # noqa: F811 (the base's function, or a mod variant's)
    found = _main_function_of(text, chunk)
    if found is not None:
        return found
    m = re.search(rf"\nvoid func_{chunk:08X}__mod_\w+\(CPUState\* ctx_param\) {{\n", text)
    if m is None:
        return None
    end = text.find("\n}\n", m.start())
    return text[m.start():end + 3] if end >= 0 else None
# --- end of the fourth set's mod variants ---


def chunk_files(root, chunk):
    return sorted(p for p in root.glob("chunks_*/*.c") if p.name.endswith(f"_{chunk:08X}.c"))


def entry_hash(texts, entry):
    """The entry's hash over one translation (texts: chunk start -> text), and
    the guest addresses its fragments name."""
    _, _, fragments, _ = ENTRIES[entry]
    pieces, named = [], set()
    for chunk, start, end in fragments:
        function = main_function(texts[chunk], chunk)
        piece = fragment(function, start, end) if function else None
        if piece is None:
            return None, named
        body, copies = piece
        pieces.append(canonical(body))
        pieces += [canonical(copy) for copy in copies]
        # The inline register save and restore's fallback (the routine itself,
        # 80328Fxx, through the chassis loop) runs only when the turn's budget
        # is too short for the inline form, which the natives decline: the
        # translation they replace never reaches it.
        named |= addresses(re.sub(r"ctx->pc = 0x80328F[0-9A-F]{2}u;", "", strip_hooks(body)))
    return hashlib.sha256("\n".join(pieces).encode()).hexdigest(), named


def translations(root, entry):
    """Every translation of the entry: the base chunks, and each mod variant of
    any of them with the base's other chunks."""
    _, _, fragments, _ = ENTRIES[entry]
    chunks = sorted({chunk for chunk, _, _ in fragments})
    files = {chunk: chunk_files(root, chunk) for chunk in chunks}
    if any(not paths for paths in files.values()):
        return None
    base = {chunk: next((p for p in paths if p.parent.name == "chunks_dol"), paths[0])
            for chunk, paths in files.items()}
    sets = [dict(base)]
    for chunk, paths in files.items():
        for path in paths:
            if path != base[chunk]:
                variant = dict(base)
                variant[chunk] = path
                sets.append(variant)
    return sets


def certify(root, watched, report=False):
    certified = {}
    for entry, (name, _, _, expected) in ENTRIES.items():
        sets = translations(root, entry)
        if sets is None:
            print(f"{name}: no translation found; not hooked")
            continue
        ok = True
        for paths in sets:
            texts = {chunk: path.read_text(encoding="utf-8") for chunk, path in paths.items()}
            digest, named = entry_hash(texts, entry)
            where = ", ".join(sorted(p.relative_to(root).as_posix() for p in paths.values()))
            if report:
                print(f"{entry:08X} {name}: {digest} ({where})")
            if digest != expected:
                if not report:
                    print(f"{name}: not the certified translation ({where}); not hooked")
                ok = False
            hit = sorted(a for a in named if a in watched or (a & ~0x40000000) in watched)
            if hit:
                print(f"{name}: the host watches {', '.join(f'{a:08X}' for a in hit)}; not hooked")
                ok = False
        if ok:
            certified[entry] = name
    return certified


def transform(text, chunk, certified):
    """Hooks for the certified entries whose first fragment is this chunk;
    stale hooks (entries no longer certified) removed."""
    for entry, (_, _, fragments, _) in ENTRIES.items():
        if fragments[0][0] == chunk and entry not in certified:
            text = text.replace(hook_text(entry, chunk), "")
    done = 0
    for entry in sorted(certified):
        _, group, fragments, _ = ENTRIES[entry]
        if fragments[0][0] != chunk:
            continue
        label = f"\nlabel_{entry:08X}:\n"
        hook = hook_text(entry, chunk)
        if label + hook in text:
            continue
        if text.count(label) != 1 or f"\nreturn_dispatch_{chunk:08X}:\n" not in text:
            raise ValueError(f"no entry label or return dispatch for {entry:08X}")
        text = add_header(text.replace(label, label + hook, 1), f'#include "{GROUPS[group][0]}"\n')
        done += 1
    return text, done


def add_header(text, header):
    """The group's header among the includes under this script's mark."""
    if MARK not in text:
        if INCLUDE not in text:
            raise ValueError("no generated.h include")
        text = text.replace(INCLUDE, INCLUDE + MARK, 1)
    at = text.index(MARK) + len(MARK)
    line = at
    while text.startswith('#include "', line):
        end = text.index("\n", line) + 1
        if text[line:end] == header:
            return text
        line = end
    return text[:at] + header + text[at:]


def main():
    args = sys.argv[1:]
    report = args[:1] == ["--hashes"]
    if report:
        args = args[1:]
    if len(args) != 1:
        sys.exit(__doc__)
    root = Path(args[0])
    watched = watched_addresses()
    certified = certify(root, watched, report)
    if report:
        return
    chunks = sorted({fragments[0][0] for _, _, fragments, _ in ENTRIES.values()})
    hooks = files = 0
    for chunk in chunks:
        for path in chunk_files(root, chunk):
            with open(path, encoding="utf-8", newline="") as file:
                original = file.read()
            converted, count = transform(original, chunk, certified)
            if converted != original:
                temporary = path.with_suffix(".c.tmp")
                with open(temporary, "w", encoding="utf-8", newline="") as file:
                    file.write(converted)
                temporary.replace(path)
                files += 1
            hooks += count
    print(f"native entries: {len(certified)}/{len(ENTRIES)} certified, {hooks} new hooks in {files} chunks")


if __name__ == "__main__":
    main()
