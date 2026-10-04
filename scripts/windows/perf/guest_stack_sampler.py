"""Inclusive guest profile: sample the game thread's guest pc and walk the
guest's own stack (the PowerPC back chain) in guest RAM, so each function is
credited with the time of everything it called.

  python guest_stack_sampler.py PID STATE_ADDR SECONDS OUT [TID]

STATE_ADDR: the "[host] guest cpu state %p" of the app's stderr.
Writes OUT.txt (inclusive and exclusive by function) and OUT-stacks.txt (the
most common stacks, innermost first).
"""
import collections
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time
from pathlib import Path

OLD = Path(__file__).resolve().parent  # guest_sampler.py and thread_sampler.py
sys.path.insert(0, str(OLD))
import thread_sampler as ts  # noqa: E402
import guest_sampler as gs  # noqa: E402

GPR, PC, LR, RAM = 0, 640, 644, 3456
STACK_BYTES = 0x4000


def main():
    pid, state, seconds, out = int(sys.argv[1]), int(sys.argv[2], 16), float(sys.argv[3]), sys.argv[4]
    tid = int(sys.argv[5]) if len(sys.argv) > 5 else None
    k32 = ts.k32
    handles = {t: k32.OpenThread(ts.THREAD_ALL, False, t) for t in ts.threads_of(pid)}
    handles = {t: h for t, h in handles.items() if h}
    if tid is None:
        before = {t: ts.cpu_time(h) for t, h in handles.items()}
        time.sleep(1.0)
        tid = max(handles, key=lambda t: ts.cpu_time(handles[t]) - before[t])
    h = handles[tid]
    process = k32.OpenProcess(0x0010, False, pid)
    k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    head = (ctypes.c_uint8 * 3464)()
    k32.ReadProcessMemory(process, state, head, 3464, None)
    ram = struct.unpack_from("<Q", bytes(head), RAM)[0]
    print(f"guest RAM at {ram:#x}")
    stack = (ctypes.c_uint8 * STACK_BYTES)()
    regs = (ctypes.c_uint8 * 648)()
    got = ctypes.c_size_t()
    samples = []
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        if k32.SuspendThread(h) == 0xFFFFFFFF:
            break
        ok = k32.ReadProcessMemory(process, state, regs, 648, None)
        chain = None
        if ok:
            b = bytes(regs)
            sp = struct.unpack_from("<I", b, GPR + 4)[0]
            pc = struct.unpack_from("<I", b, PC)[0]
            lr = struct.unpack_from("<I", b, LR)[0]
            base = sp & 0x01FFFFFF
            if k32.ReadProcessMemory(process, ram + base, stack, STACK_BYTES, ctypes.byref(got)):
                s = bytes(stack)
                chain = [pc, lr]
                cur = sp
                for _ in range(48):
                    off = (cur & 0x01FFFFFF) - base
                    if off < 0 or off + 4 > len(s):
                        break
                    back = struct.unpack_from(">I", s, off)[0]
                    boff = (back & 0x01FFFFFF) - base
                    if back <= cur or boff + 8 > len(s):
                        break
                    chain.append(struct.unpack_from(">I", s, boff + 4)[0])
                    cur = back
        k32.ResumeThread(h)
        if chain:
            samples.append(chain)
        time.sleep(0.0005)
    table = gs.symbols()
    starts = [x for x, _, _ in table]
    incl = collections.Counter()
    excl = collections.Counter()
    stacks = collections.Counter()
    for chain in samples:
        names = [gs.name_of(table, starts, a) for a in chain]
        excl[names[0]] += 1
        seen = []
        for n in names:
            if n not in seen:
                seen.append(n)
        for n in seen:
            incl[n] += 1
        stacks[" < ".join(seen[:8])] += 1
    total = len(samples)
    with open(out + ".txt", "w") as o:
        o.write(f"# thread {tid}; {total} samples\n# inclusive (with callees)  exclusive  function\n")
        for n, c in incl.most_common(250):
            o.write(f"{c / total:7.2%} {excl[n] / total:7.2%}  {n}\n")
    with open(out + "-stacks.txt", "w") as o:
        for st, c in stacks.most_common(300):
            o.write(f"{c / total:6.2%}  {st}\n")
    print(f"thread {tid}: {total} samples -> {out}.txt")


if __name__ == "__main__":
    main()
