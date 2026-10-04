#!/usr/bin/env python3
"""Certified GZLE01 game math entry hooks (native_game_math.c).

  native_game_math.py COMPOSITE_SRC

Run after direct_calls.py/native_skin.py and before fast_blocks.py and the
simulation/native-math manifests. Every base/mod translation of an entry and
its nested SDK/helper dependencies must match the comparison test's hashes.
The cXyz subtraction spans two chunks: BOTH fragments must be certified.
Changes to the clipper's extracted zeroing loop also invalidate its native.

The hash excludes only whitespace and fast_blocks.py's added labels/jumps;
it includes the original block charges, suffixes, instruction bodies and
rewritten calls. Already prepared chunks can be checked and hooked too.
Insert at the entry label, then use the chunk's own blr return-dispatch path:
this covers internal goto calls, external direct calls, and dispatcher entries.
Failure leaves the original block machinery in control. No main checkout is
modified unless it is explicitly supplied as COMPOSITE_SRC by its builder.
"""
import hashlib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from direct_calls import watched_addresses
# The later native steps' hooks (native_entries.py, and prepare_native_j3d.py's,
# which it strips too) are not part of the translation certified here. The
# builder finishes a tree twice (the base with the mods' variants, then in
# place); on the second pass this step met the fourth set's hooks inside the
# animation fragments it certifies, took them for a different translation and
# removed its own hooks, and native_entries.py, whose hashes include those,
# then removed its own too (2026-10-04).
from native_entries import strip_hooks as strip_entry_hooks

MARK = "/* bluewake: certified game math (scripts/windows/native_game_math.py) */\n"
INCLUDE = '#include "../generated.h"\n'
HOOK = re.compile(r"    if \(bluewake_native_game_math_enabled &&\n"
                  r"        bluewake_native_game_math\(ctx, 0x([0-9A-F]{8})u\)\)\n"
                  r"        goto return_dispatch_([0-9A-F]{8});\n")

# name -> (chunk start, first label, end label or the chunk's return table,
#          SHA256 of the canonical translated fragment).
FRAGMENTS = {
    'xyz_add': (0x802416E0, 0x80245674, 0x802456C4,
        '318d175d0d96371bb905bae085afd3e947891f0c9a76d6b29ef1cf9f349cb9c9'),
    'xyz_sub_entry': (0x802416E0, 0x802456C4, None,
        '13a6c7b4b463c9e72c5aa72618a3812c98bd27f24268b2ca43e3a056f904c76b'),
    'xyz_sub_tail': (0x802456E0, 0x802456E0, 0x80245714,
        '32c692124efbf804b3d4ce883233b1dd1dc83cdc7dff4519ea2a2405459be1a1'),
    'xyz_scale': (0x802456E0, 0x80245714, 0x80245760,
        'eaeb337907c0a8f8937dac7017df7bba71579024d96b9f46bddbc2e18d8d9ad6'),
    'aab_cyl': (0x802496E0, 0x8024A8E0, 0x8024A988,
        '564cd18f6294430a1dd2d9104299e81590559eb62466b29fecd017a81a3461c0'),
    'xrot_s': (0x800096E0, 0x8000CD28, 0x8000CD88,
        'b4e6e6cc76beed4c6fc92087b557983008376363aaa4d3db46554b90c25e5de9'),
    'yrot_s': (0x800096E0, 0x8000CDC8, 0x8000CE28,
        'aac461900373c8bc91d7c0d8fc0974d23ff1e0eec9933de91e545095c4ad7fcb'),
    'zrot_s': (0x800096E0, 0x8000CE68, 0x8000CEC8,
        '9cd5e46e5f6330c248f90053e5b1be235ce6a0295de4f7bb93250db6e2b767ce'),
    'box_line': (0x802496E0, 0x8024AE3C, 0x8024BA18,
        '455476a76f2cce77ffd345573832ca3b0608adca1bbef131bc9ec5efc0126962'),
    'sphere_clip': (0x802556E0, 0x80256888, 0x802569D0,
        '443ed3290b5e01bab458c1d91cd1281ed2c2f0424ebabf443e3363a07afa084c'),
    'box_clip': (0x802556E0, 0x802569D0, 0x80256CB8,
        '1454a2da821bad307a99bf4ad07ff2b6ad59c134ea68aac85ff385c0fc8440a5'),
    'clip_zero_loop': (0x802556E0, 'loop_80256A08', None,
        'ca995ffae8fbd8cc4379b142a7338430a92cf8ec0ffeec1d3934761d010c1cbb'),
    'key_s': (0x802ED6E0, 0x802F072C, 0x802F0954,
        '84801732c70ac69c35446f028e8e0921afa9ff7e0ab178a6565e50cced9ea3d9'),
    'hermite_s': (0x802ED6E0, 0x802F06D8, 0x802F072C,
        'a69c6cac2656fbb847d1b6ad99a5205b95fb3e00fb94008bbffdfeae92150a3a'),
    'sdk_add': (0x8030D6E0, 0x8030DCE0, 0x8030DD04,
        '58694b1d26c00948de83054d0bf3be20e59c95bc45172aa813b04f5f45e3799f'),
    'sdk_sub': (0x8030D6E0, 0x8030DD04, 0x8030DD28,
        '9c5e38c526a3a92ef067c8283b8b8c0dfa3a728fd860eacbfb181b9a65871916'),
    'sdk_scale': (0x8030D6E0, 0x8030DD28, 0x8030DD44,
        'c1c5adae1761a7944e710bf410dac0b608e5f9953cfdb65db1acd99353b981f9'),
    'sdk_multvec': (0x8030D6E0, 0x8030DA44, 0x8030DA98,
        'cf4cfa14c036cdcc192f4b3ee9ed8ceda627c576a3a26de1fd922be96b51287e'),
    'transform_simple': (0x802ED6E0, 0x802F0954, 0x802F0E20,
        'db0ac9b5c2ec524e5ac396d06faa6c5d1d95023d8623e14ad32a06c9131b138e'),
}
# entry -> required fragments (including all native-emulated calls).
ENTRIES = {
    0x80245674: ('xyz_add', 'sdk_add'),
    0x802456C4: ('xyz_sub_entry', 'xyz_sub_tail', 'sdk_sub'),
    0x80245714: ('xyz_scale', 'sdk_scale'),
    0x8024A8E0: ('aab_cyl',),
    0x8000CD28: ('xrot_s',),
    0x8000CDC8: ('yrot_s',),
    0x8000CE68: ('zrot_s',),
    0x8024AE3C: ('box_line',),
    0x80256888: ('sphere_clip', 'sdk_multvec'),
    0x802569D0: ('box_clip', 'clip_zero_loop', 'sdk_multvec'),
    0x802F072C: ('key_s', 'hermite_s'),
    0x802F0954: ('transform_simple',),
}


def canonical(text):
    text = strip_entry_hooks(HOOK.sub("", text))
    text = re.sub(r"^    if \(cycle_block_prepaid\) goto bwfast_\w+;\n", "", text, flags=re.M)
    text = re.sub(r"^(?:bwslow|bwend)_\w+: ;\n", "", text, flags=re.M)
    return " ".join(text.split())


def fragment(text, chunk, start, end):
    if isinstance(start, str):
        # A loop extracted by the translator into a static function. The fast
        # copy lives at the end of that function, after the original body.
        match = re.search(rf"^static void {start}\(CPUState\* ctx_param\) \{{", text, re.M)
        if not match:
            return None
        finish = re.search(r"^(?:static )?void \w+\(CPUState\* ctx_param\)", text[match.end():], re.M)
        body = text[match.start():match.end() + finish.start()] if finish else text[match.start():]
        fast = body.find("\nbwfast_")
        if fast >= 0:
            body = body[:fast] + "\n}\n"
        return canonical(body)
    begin = text.find(f"\nlabel_{start:08X}:\n")
    finish = text.find(f"\nlabel_{end:08X}:\n", begin) if end else text.find(f"\nreturn_dispatch_{chunk:08X}:\n", begin)
    if begin < 0 or finish <= begin:
        return None
    return canonical(text[begin:finish])


def certify(chunks, watched):
    certified = set()
    for name, (chunk, start, end, expected) in FRAGMENTS.items():
        paths = [p for p in chunks if p.name.endswith(f"_{chunk:08X}.c")]
        valid = bool(paths)
        for path in paths:
            body = fragment(path.read_text(encoding="utf-8"), chunk, start, end)
            if body is None or hashlib.sha256(body.encode()).hexdigest() != expected:
                print(f"{path.name}: unverified {name}; dependent natives disabled")
                valid = False
            # Include the internal labels/returns and callees, in both mirrors.
            if body:
                # The certified inline save/restore's slow fallback PCs are
                # unreachable under the native's whole-function budget guard.
                # Their stores/loads run inline in the reference too, without
                # dispatching a host-watched save-routine entry.
                observed = re.sub(r"ctx->pc = 0x80328F[0-9A-F]{2}u;", "", body)
                names = re.findall(r"label_([8C][0-9A-Fa-f]{7})|// ([8C][0-9A-Fa-f]{7}):|"
                                   r"ctx->(?:pc|lr) = 0x([8C][0-9A-Fa-f]{7})", observed)
                pcs = [int(next(value for value in match if value), 16) for match in names]
                if any(pc in watched or (pc & ~0x40000000) in watched for pc in pcs):
                    print(f"{name}: host watches an internal address; dependent natives disabled")
                    valid = False
        if valid:
            certified.add(name)
    return {entry for entry, names in ENTRIES.items() if set(names) <= certified}


def transform(text, chunk, entries):
    # A rerun after a dependency/mod changes must remove its old hooks too.
    text = HOOK.sub(lambda m: m[0] if int(m[1], 16) in entries else "", text)
    done = 0
    for entry in sorted(entries):
        label = f"\nlabel_{entry:08X}:\n"
        if label not in text:
            continue
        hook = ("    if (bluewake_native_game_math_enabled &&\n"
                f"        bluewake_native_game_math(ctx, 0x{entry:08X}u))\n"
                f"        goto return_dispatch_{chunk:08X};\n")
        if hook in text:
            continue
        if f"\nreturn_dispatch_{chunk:08X}:\n" not in text:
            raise ValueError(f"no return table for {entry:08X}")
        text = text.replace(label, label + hook, 1)
        done += 1
    if done and MARK not in text:
        if INCLUDE not in text:
            raise ValueError("no generated.h include")
        text = text.replace(INCLUDE, INCLUDE + MARK + '#include "native_game_math.h"\n', 1)
    return text, done


def main():
    root = Path(sys.argv[1])
    chunks = sorted(root.glob("chunks_*/*.c"))
    entries = certify(chunks, watched_addresses())
    hooks = files = 0
    for path in chunks:
        match = re.search(r"_([0-9A-F]{8})\.c$", path.name)
        if not match:
            continue
        original = path.read_text(encoding="utf-8")
        converted, count = transform(original, int(match[1], 16), entries)
        if converted != original:
            temporary = path.with_suffix(".c.tmp")
            temporary.write_text(converted, encoding="utf-8", newline="\n")
            temporary.replace(path)
            hooks += count
            files += 1
    print(f"native game math: {len(entries)}/{len(ENTRIES)} certified entries, {hooks} new hooks in {files} chunks")


if __name__ == "__main__":
    main()
