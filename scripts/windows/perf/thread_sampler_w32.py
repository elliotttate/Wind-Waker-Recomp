"""Sample the busiest threads of a running BlueWake from outside.

  python thread_sampler.py PID SECONDS OUT_PREFIX [THREADS]

Picks the THREADS (default 4) threads with the most CPU time over one second,
then suspends each about every millisecond and records its instruction
pointer, for SECONDS. Writes OUT_PREFIX-<tid>.txt: "count module offset",
most frequent first. Symbolize with the linker map (symbolize_map.py).
"""
import ctypes
import ctypes.wintypes as wt
import collections
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
for _f in (k32.OpenProcess, k32.OpenThread, k32.CreateToolhelp32Snapshot):
    _f.restype = wt.HANDLE
k32.SuspendThread.argtypes = k32.ResumeThread.argtypes = [wt.HANDLE]
k32.SuspendThread.restype = k32.ResumeThread.restype = wt.DWORD
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.GetThreadTimes.argtypes = [wt.HANDLE] + [ctypes.c_void_p] * 4
k32.CloseHandle.argtypes = [wt.HANDLE]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p, wt.DWORD]
psapi.GetModuleBaseNameW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]
psapi.GetModuleInformation.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wt.DWORD]

TH32CS_SNAPTHREAD = 0x4
THREAD_ALL = 0x0002 | 0x0008 | 0x0040  # suspend/resume, get context, query information
CONTEXT_FULL = 0x10000B


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


class M128A(ctypes.Structure):
    _fields_ = [("Low", ctypes.c_uint64), ("High", ctypes.c_int64)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [("P1Home", ctypes.c_uint64), ("P2Home", ctypes.c_uint64), ("P3Home", ctypes.c_uint64),
                ("P4Home", ctypes.c_uint64), ("P5Home", ctypes.c_uint64), ("P6Home", ctypes.c_uint64),
                ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
                ("SegCs", wt.WORD), ("SegDs", wt.WORD), ("SegEs", wt.WORD), ("SegFs", wt.WORD),
                ("SegGs", wt.WORD), ("SegSs", wt.WORD), ("EFlags", wt.DWORD),
                ("Dr0", ctypes.c_uint64), ("Dr1", ctypes.c_uint64), ("Dr2", ctypes.c_uint64),
                ("Dr3", ctypes.c_uint64), ("Dr6", ctypes.c_uint64), ("Dr7", ctypes.c_uint64),
                ("Rax", ctypes.c_uint64), ("Rcx", ctypes.c_uint64), ("Rdx", ctypes.c_uint64),
                ("Rbx", ctypes.c_uint64), ("Rsp", ctypes.c_uint64), ("Rbp", ctypes.c_uint64),
                ("Rsi", ctypes.c_uint64), ("Rdi", ctypes.c_uint64), ("R8", ctypes.c_uint64),
                ("R9", ctypes.c_uint64), ("R10", ctypes.c_uint64), ("R11", ctypes.c_uint64),
                ("R12", ctypes.c_uint64), ("R13", ctypes.c_uint64), ("R14", ctypes.c_uint64),
                ("R15", ctypes.c_uint64), ("Rip", ctypes.c_uint64),
                ("FltSave", ctypes.c_byte * 512), ("VectorRegister", M128A * 26),
                ("VectorControl", ctypes.c_uint64), ("DebugControl", ctypes.c_uint64),
                ("LastBranchToRip", ctypes.c_uint64), ("LastBranchFromRip", ctypes.c_uint64),
                ("LastExceptionToRip", ctypes.c_uint64), ("LastExceptionFromRip", ctypes.c_uint64)]


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    tids = []
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            tids.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return tids


def cpu_time(handle):
    c, e, kt, ut = wt.FILETIME(), wt.FILETIME(), wt.FILETIME(), wt.FILETIME()
    k32.GetThreadTimes(handle, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    return ((kt.dwHighDateTime << 32) | kt.dwLowDateTime) + ((ut.dwHighDateTime << 32) | ut.dwLowDateTime)


def modules_of(pid):
    process = k32.OpenProcess(0x0410, False, pid)
    handles = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(process, handles, ctypes.sizeof(handles), ctypes.byref(needed), 3)
    found = []
    for i in range(needed.value // ctypes.sizeof(ctypes.c_void_p)):
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(process, handles[i], name, 260)

        class MODULEINFO(ctypes.Structure):
            _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]
        info = MODULEINFO()
        psapi.GetModuleInformation(process, handles[i], ctypes.byref(info), ctypes.sizeof(info))
        found.append((info.base or 0, (info.base or 0) + info.size, name.value))
    k32.CloseHandle(process)
    return found


def main():
    pid, seconds, prefix = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 4
    handles = {}
    for tid in threads_of(pid):
        h = k32.OpenThread(THREAD_ALL, False, tid)
        if h:
            handles[tid] = h
    before = {tid: cpu_time(h) for tid, h in handles.items()}
    time.sleep(1.0)
    busy = sorted(((cpu_time(h) - before[tid]) / 1e5, tid) for tid, h in handles.items())[::-1][:count]
    print("busiest threads (percent of a core over 1 s):", [(tid, round(p)) for p, tid in busy])
    modules = modules_of(pid)
    # Leaf runtime routines (memcpy, memcmp) push nothing: the qword at RSP is
    # their caller's return address. Read it while the thread is stopped.
    leaf = [(lo, hi) for lo, hi, name in modules if name.lower() in ("win32u.dll", "vcruntime140.dll")]
    process = k32.OpenProcess(0x0010, False, pid)  # PROCESS_VM_READ
    k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    samples = {tid: collections.Counter() for _, tid in busy}
    callers = {tid: collections.Counter() for _, tid in busy}
    ctx = CONTEXT()
    ret = ctypes.c_uint64()
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        for _, tid in busy:
            h = handles[tid]
            if k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            ctx.ContextFlags = CONTEXT_FULL
            ok = k32.GetThreadContext(h, ctypes.byref(ctx))
            caller = 0
            if ok and any(lo <= ctx.Rip < hi for lo, hi in leaf):
                if k32.ReadProcessMemory(process, ctx.Rsp, ctypes.byref(ret), 8, None):
                    caller = ret.value
            k32.ResumeThread(h)
            if ok:
                samples[tid][ctx.Rip] += 1
                if caller:
                    callers[tid][caller] += 1
        time.sleep(0.001)
    for pct, tid in busy:
        by_place = collections.Counter()
        for rip, n in samples[tid].items():
            where = next(((name, rip - lo) for lo, hi, name in modules if lo <= rip < hi), ("?", rip))
            by_place[where] += n
        by_caller = collections.Counter()
        for rip, n in callers[tid].items():
            where = next(((name, rip - lo) for lo, hi, name in modules if lo <= rip < hi), ("?", rip))
            by_caller[where] += n
        with open(f"{prefix}-{tid}-callers.txt", "w") as out:
            out.write(f"# thread {tid}: callers of runtime leaf routines\n")
            for (name, offset), n in by_caller.most_common():
                out.write(f"{n} {name} 0x{offset:x}\n")
        total = sum(by_place.values())
        with open(f"{prefix}-{tid}.txt", "w") as out:
            out.write(f"# thread {tid}: {round(pct)} percent of a core; {total} samples\n")
            for (name, offset), n in by_place.most_common():
                out.write(f"{n} {name} 0x{offset:x}\n")
        by_module = collections.Counter()
        for (name, _), n in by_place.items():
            by_module[name] += n
        print(f"thread {tid} ({round(pct)}%): " + ", ".join(f"{m} {100 * n // max(total, 1)}%"
                                                           for m, n in by_module.most_common(5)))


if __name__ == "__main__":
    main()
