"""Read-only check of a running BFME2 v1.00 game.dat.

Shows whether the zoomfix patches are in memory and the camera state they produce.
Usage: python probe.py [--wait SECONDS]
"""
import ctypes
import struct
import sys
import time
from ctypes import wintypes

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TH32CS_SNAPPROCESS = 0x2

THE_TACTICAL_VIEW = 0x00DF98E4      # W3DView*;    +0x44 = zoom limited (bool)
THE_GLOBAL_DATA = 0x00DF9618        # GlobalData*; +0x950 = clip depth multiplier (float)
THE_DISPLAY = 0x00DF9880            # W3DDisplay*; +0xD8 = letterboxed (bool)

PATCHES = [
    (0x0065E3EF, "c6404400"),
    (0x0044459E, "6a00"),
    (0x00444547, "32d290"),
    (0x00636446, "c786500900000000c8426a038f8688090000"),
]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
k32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.OpenProcess.restype = wintypes.HANDLE
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.LPVOID,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.CloseHandle.argtypes = [wintypes.HANDLE]


def find_pid(name="game.dat"):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    pid = None
    ok = k32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == name:
            pid = entry.th32ProcessID
            break
        ok = k32.Process32NextW(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return pid


def read(handle, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(handle, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)) \
            or got.value != size:
        raise OSError(f"ReadProcessMemory({addr:#x}) failed, error {ctypes.get_last_error()}")
    return buf.raw


def ptr(handle, addr):
    return struct.unpack("<I", read(handle, addr, 4))[0]


def main():
    wait = 0.0
    if len(sys.argv) == 3 and sys.argv[1] == "--wait":
        wait = float(sys.argv[2])
    deadline = time.time() + wait

    pid = find_pid()
    while pid is None and time.time() < deadline:
        time.sleep(1)
        pid = find_pid()
    if pid is None:
        print("game.dat is not running")
        return 1

    handle = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        print(f"cannot open game.dat (pid {pid}), error {ctypes.get_last_error()}")
        return 1
    try:
        while ptr(handle, THE_TACTICAL_VIEW) == 0 or ptr(handle, THE_GLOBAL_DATA) == 0:
            if time.time() >= deadline:
                break
            time.sleep(1)

        present = sum(read(handle, a, len(h) // 2).hex() == h for a, h in PATCHES)
        print(f"game.dat pid {pid}")
        print(f"code patches in memory: {present}/{len(PATCHES)}")

        good = present == len(PATCHES)
        view = ptr(handle, THE_TACTICAL_VIEW)
        if view:
            limited = read(handle, view + 0x44, 1)[0]
            print(f"TheTacticalView {view:#010x}: zoomLimited = {limited}  (0 = unlimited)")
            good &= limited == 0
        else:
            print("TheTacticalView not created yet")
            good = False

        gdata = ptr(handle, THE_GLOBAL_DATA)
        if gdata:
            mult = struct.unpack("<f", read(handle, gdata + 0x950, 4))[0]
            print(f"TheGlobalData   {gdata:#010x}: clipDepthMultiplier = {mult:g}  (far clip {1800 * mult:g})")
            good &= mult == 100.0
        else:
            print("TheGlobalData not created yet")
            good = False

        display = ptr(handle, THE_DISPLAY)
        if display:
            print(f"TheDisplay      {display:#010x}: letterboxed = {read(handle, display + 0xD8, 1)[0]}")

        print("RESULT:", "zoom limits lifted" if good else "NOT as expected")
        return 0 if good else 1
    finally:
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())
