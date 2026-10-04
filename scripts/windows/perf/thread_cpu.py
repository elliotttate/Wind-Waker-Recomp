"""Every thread of a running process: CPU time over an interval, as percent of one core, with its name.

  python thread_cpu.py PID SECONDS [TOP]
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.GetThreadTimes.argtypes = [wt.HANDLE] + [ctypes.c_void_p] * 4
k32.CloseHandle.argtypes = [wt.HANDLE]
GetThreadDescription = getattr(k32, "GetThreadDescription", None)
if GetThreadDescription:
    GetThreadDescription.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG),
                ("dwFlags", wt.DWORD)]


def thread_ids(pid):
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    ids = []
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            ids.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return ids


def times(tid):
    h = k32.OpenThread(0x0040 | 0x0800, False, tid)  # query information (+limited)
    if not h:
        return None, ""
    c, e, kt, ut = (ctypes.c_ulonglong() for _ in range(4))
    k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(kt), ctypes.byref(ut))
    name = ""
    if GetThreadDescription:
        p = ctypes.c_wchar_p()
        if GetThreadDescription(h, ctypes.byref(p)) >= 0 and p.value:
            name = p.value
    k32.CloseHandle(h)
    return kt.value + ut.value, name


pid, seconds = int(sys.argv[1]), float(sys.argv[2])
top = int(sys.argv[3]) if len(sys.argv) > 3 else 15
before = {t: times(t) for t in thread_ids(pid)}
time.sleep(seconds)
rows = []
total = 0.0
for t in thread_ids(pid):
    now, name = times(t)
    if now is None or t not in before or before[t][0] is None:
        continue
    pct = (now - before[t][0]) / (seconds * 1e7) * 100
    total += pct
    rows.append((pct, t, name))
rows.sort(reverse=True)
print(f"process total {total:.0f}% of a core over {seconds:.0f} s, {len(rows)} threads")
for pct, t, name in rows[:top]:
    print(f"  {pct:5.1f}%  {t:6d}  {name}")
