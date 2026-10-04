"""Aggregate thread_sampler output by function.

usage: symprof.py PROFILE.txt [TOP]
BlueWake.exe via llvm-symbolizer + PDB (innermost and outermost inlined frame),
system DLLs by nearest export, the game module by offset only. Leaf runtime
routines' callers come from PROFILE-callers.txt."""
import bisect, collections, os, re, subprocess, sys

VS = r"E:\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin"
APP = os.environ.get("SYMPROF_APP", str(Path(__file__).resolve().parents[3] / "build/windows/app/BlueWake.exe"))
DLLDIR = str(Path(__file__).resolve().parents[3] / "build/windows/BlueWake")
SYSDIR = r"C:\Windows\System32"


def symbolize(offsets):
    addrs = "\n".join(hex(0x140000000 + o) for o in offsets) + "\n"
    out = subprocess.run([os.path.join(VS, "llvm-symbolizer.exe"), f"--obj={APP}", "--functions=linkage", "--demangle"],
                         input=addrs, capture_output=True, text=True).stdout
    blocks = [b for b in out.split("\n\n") if b.strip()]
    result = {}
    for off, block in zip(offsets, blocks):
        lines = block.strip().splitlines()
        funcs = lines[0::2]
        locs = lines[1::2]
        inner = funcs[0] if funcs else "?"
        outer = funcs[-1] if funcs else "?"
        loc = locs[0].split("\\")[-1] if locs else ""
        result[off] = (inner, outer, loc)
        CHAINS[off] = [f.split("(")[0][-90:] for f in funcs]
    return result


_exports = {}
CHAINS = {}


def export_name(dll, off):
    if dll not in _exports:
        path = os.path.join(DLLDIR, dll)
        if not os.path.exists(path):
            path = os.path.join(SYSDIR, dll)
        syms = []
        if os.path.exists(path):
            out = subprocess.run([os.path.join(VS, "llvm-readobj.exe"), "--coff-exports", path],
                                 capture_output=True, text=True).stdout
            name = None
            for line in out.splitlines():
                line = line.strip()
                if line.startswith("Name:"):
                    name = line.split(":", 1)[1].strip()
                elif line.startswith("RVA:") and name:
                    syms.append((int(line.split(":", 1)[1].strip(), 16), name))
        syms.sort()
        _exports[dll] = syms
    syms = _exports[dll]
    i = bisect.bisect_right(syms, (off, "\uffff")) - 1
    return f"{syms[i][1]}+0x{off - syms[i][0]:x}" if i >= 0 else hex(off)


def read(path):
    rows = []
    header = ""
    for line in open(path):
        if line.startswith("#"):
            header = line.strip()
            continue
        n, mod, off = line.split()
        rows.append((int(n), mod, int(off, 16)))
    return header, rows


def main():
    path = sys.argv[1]
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    header, rows = read(path)
    total = sum(n for n, _, _ in rows)
    app_offs = sorted({o for _, m, o in rows if m.lower() == "bluewake.exe"})
    syms = symbolize(app_offs) if app_offs else {}
    by_inner = collections.Counter()
    by_outer = collections.Counter()
    for n, mod, off in rows:
        if mod.lower() == "bluewake.exe":
            inner, outer, loc = syms.get(off, ("?", "?", ""))
            by_inner[f"{inner}  [{loc}]"] += n
            by_outer[outer] += n
        elif mod.lower() in ("ntdll.dll", "kernelbase.dll", "vcruntime140.dll", "ucrtbase.dll", "kernel32.dll",
                             "msvcp140.dll", "webgpu_dawn.dll"):
            name = f"{mod}!{export_name(mod, off)}"
            by_inner[name] += n
            by_outer[f"{mod}!{export_name(mod, off).split('+')[0]}"] += n
        else:
            by_inner[f"{mod}"] += n
            by_outer[f"{mod}"] += n
    print(header)
    print(f"-- by outermost function ({total} samples)")
    for name, n in by_outer.most_common(top):
        print(f"{100 * n / total:5.1f}%  {name[:150]}")
    print("-- by innermost location")
    for name, n in by_inner.most_common(top):
        print(f"{100 * n / total:5.1f}%  {name[:170]}")
    incl = collections.Counter()
    for n, mod, off in rows:
        if mod.lower() == "bluewake.exe":
            for f in set(CHAINS.get(off, [])):
                incl[f] += n
    print("-- inclusive over inline chains (BlueWake.exe)")
    for name, n in incl.most_common(top):
        print(f"{100 * n / total:5.1f}%  {name}")
    callers = path[:-4] + "-callers.txt"
    if os.path.exists(callers):
        _, crow = read(callers)
        ctotal = sum(n for n, _, _ in crow)
        if ctotal:
            offs = sorted({o for _, m, o in crow if m.lower() == "bluewake.exe"})
            csyms = symbolize(offs) if offs else {}
            by = collections.Counter()
            for n, mod, off in crow:
                if mod.lower() == "bluewake.exe":
                    inner, outer, loc = csyms.get(off, ("?", "?", ""))
                    by[f"{inner} [{loc}] (in {outer})"] += n
                else:
                    by[mod] += n
            print(f"-- callers of memcpy/memset etc ({ctotal} samples)")
            for name, n in by.most_common(12):
                print(f"{100 * n / ctotal:5.1f}%  {name[:200]}")


if __name__ == "__main__":
    main()
