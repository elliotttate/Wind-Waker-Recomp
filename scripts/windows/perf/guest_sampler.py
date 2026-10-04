"""Sample which guest (GameCube) functions a running BlueWake spends its time in.

  python guest_sampler.py PID STATE_ADDR PC_OFFSET SECONDS OUT [TID]

STATE_ADDR and PC_OFFSET are the "[host] guest cpu state %p ..., pc at +N" line
of the app's stderr. Stops the game thread (TID, or the busiest thread) about
every millisecond, reads its instruction pointer and the guest pc, and names
the guest pc with the decompilation's symbols (a tww checkout). Writes OUT.txt:
functions by samples, with how many of each were in the translated module
itself and how many in host helpers called from it.
"""
import bisect
import collections
import ctypes
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import thread_sampler as ts  # noqa: E402  (its main() runs only when __main__)

# The zeldaret decompilation's symbols: BLUEWAKE_TWW, by default a tww checkout beside this repository.
TWW = Path(os.environ.get("BLUEWAKE_TWW", str(Path(__file__).resolve().parents[4] / "tww"))) / "config/GZLE01"
SRC = Path(__file__).resolve().parents[3] / "build/windows/composite-src"
SYMBOL = re.compile(r"^(\S+) = \.text:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")


def symbols():
    table = []  # (start, end, name)
    for line in open(TWW / "symbols.txt"):
        m = SYMBOL.match(line)
        if m:
            start = int(m.group(2), 16)
            table.append((start, start + int(m.group(3), 16), m.group(1)))
    ids = {}
    for d in SRC.glob("chunks_*_*"):
        m = re.match(r"chunks_(.+)_(\d+)$", d.name)
        if m:
            ids[int(m.group(2))] = m.group(1)
    text_base = {}
    for m in re.finditer(r"\{(\d+)u, 1u, 0x([0-9A-F]+)u, 0x([0-9A-F]+)u\}", (SRC / "rel_modules.inc").read_text()):
        text_base[int(m.group(1))] = int(m.group(2), 16)
    for rel_id, base in text_base.items():
        name = ids.get(rel_id)
        path = TWW / "rels" / (name or "") / "symbols.txt"
        if not name or not path.exists() or base == 0:
            continue
        for line in open(path):
            m = SYMBOL.match(line)
            if m:
                start = base + int(m.group(2), 16)
                table.append((start, start + int(m.group(3), 16), f"{name}:{m.group(1)}"))
    table.sort()
    return table


def name_of(table, starts, pc):
    i = bisect.bisect_right(starts, pc) - 1
    if i >= 0 and table[i][0] <= pc < table[i][1]:
        return table[i][2]
    return f"?{pc:08X}"


def main():
    pid, state, pc_offset = int(sys.argv[1]), int(sys.argv[2], 16), int(sys.argv[3])
    seconds, out = float(sys.argv[4]), sys.argv[5]
    tid = int(sys.argv[6]) if len(sys.argv) > 6 else None
    k32 = ts.k32
    handles = {t: k32.OpenThread(ts.THREAD_ALL, False, t) for t in ts.threads_of(pid)}
    handles = {t: h for t, h in handles.items() if h}
    if tid is None:
        before = {t: ts.cpu_time(h) for t, h in handles.items()}
        time.sleep(1.0)
        tid = max(handles, key=lambda t: ts.cpu_time(handles[t]) - before[t])
    h = handles[tid]
    modules = ts.modules_of(pid)
    process = k32.OpenProcess(0x0010, False, pid)
    k32.ReadProcessMemory.argtypes = [ctypes.wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_size_t, ctypes.c_void_p]
    ctx = ts.CONTEXT()
    pc = ctypes.c_uint32()
    samples = collections.Counter()  # (guest pc, host module)
    end = time.perf_counter() + seconds
    t0 = ts.cpu_time(h)
    w0 = time.perf_counter()
    while time.perf_counter() < end:
        if k32.SuspendThread(h) == 0xFFFFFFFF:
            break
        ctx.ContextFlags = ts.CONTEXT_FULL
        ok = k32.GetThreadContext(h, ctypes.byref(ctx))
        got = k32.ReadProcessMemory(process, state + pc_offset, ctypes.byref(pc), 4, None)
        k32.ResumeThread(h)
        if ok and got:
            module = next((name for lo, hi, name in modules if lo <= ctx.Rip < hi), "?")
            samples[(pc.value, module)] += 1
        time.sleep(0.0005)
    busy = (ts.cpu_time(h) - t0) / 1e7 / (time.perf_counter() - w0)
    table = symbols()
    starts = [s for s, _, _ in table]
    by_function = collections.Counter()
    split = collections.defaultdict(collections.Counter)
    by_module = collections.Counter()
    for (guest, module), n in samples.items():
        f = name_of(table, starts, guest)
        by_function[f] += n
        split[f][module] += n
        by_module[module] += n
    total = sum(by_function.values())
    with open(out + ".txt", "w") as o:
        o.write(f"# thread {tid}: {busy:.0%} of a core busy; {total} samples\n")
        o.write("# host modules: " + ", ".join(f"{m} {n / total:.1%}" for m, n in by_module.most_common(6)) + "\n")
        cumulative = 0
        for f, n in by_function.most_common():
            cumulative += n
            parts = ", ".join(f"{m.split('.')[0]} {c}" for m, c in split[f].most_common(3))
            o.write(f"{n / total:6.2%} {cumulative / total:6.1%}  {f}   [{parts}]\n")
    print(f"thread {tid} ({busy:.0%} busy), {total} samples -> {out}.txt")


if __name__ == "__main__":
    main()
