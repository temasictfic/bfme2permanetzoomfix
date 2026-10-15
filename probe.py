"""Read-only check of a running BFME2 v1.00 game.dat (run from an admin prompt,
because the game runs elevated).

Shows whether the zoomfix patches are in memory and the camera state they produce.
Usage: python probe.py [--wait SECONDS] [--watch SECONDS]
"""
import ctypes
import struct
import sys
import time
from ctypes import wintypes

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TH32CS_SNAPPROCESS = 0x2

THE_TACTICAL_VIEW = 0x00DF98E4      # W3DView*;    +0x44 zoom-limited flag, +0x40 height
THE_GLOBAL_DATA = 0x00DF9618        # GlobalData*; +0x950 clip depth multiplier (float)
THE_DISPLAY = 0x00DF9880            # W3DDisplay*; +0xD8 letterboxed (bool)
VIEW_CAMERA_LIMITS = 0x24C8         # map limits: +4 min height, +8 max height
LIMITS_VTABLE = 0x00BCC590
ORIGINAL_GETTERS = (0x008EA11B, 0x008F415C)

# Fixed code patches (address, expected bytes); None in a pattern = any byte.
PATCHES = [
    (0x0065E3EF, bytes.fromhex("c6404400")),
    (0x0044459E, bytes.fromhex("6a00")),
    (0x00444547, bytes.fromhex("32d290")),
    (0x0048D2A1, bytes.fromhex("9090")),
]
CLIP_PATCH = 0x00636446  # c7 86 50 09 00 00 <float> 6a 03 8f 86 88 09 00 00


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


def f32(handle, addr):
    return struct.unpack("<f", read(handle, addr, 4))[0]


def snapshot(handle):
    """Return a dict describing the patches and the current camera state."""
    clip = read(handle, CLIP_PATCH, 18)
    getters = struct.unpack("<II", read(handle, LIMITS_VTABLE, 8))
    state = {
        "patches": sum(read(handle, a, len(b)) == b for a, b in PATCHES)
        + (clip[:6].hex() == "c78650090000" and clip[10:].hex() == "6a038f8688090000")
        + all(g != o for g, o in zip(getters, ORIGINAL_GETTERS)),
        "patch_total": len(PATCHES) + 2,
        "view": ptr(handle, THE_TACTICAL_VIEW),
        "gdata": ptr(handle, THE_GLOBAL_DATA),
        "display": ptr(handle, THE_DISPLAY),
    }
    view, gdata, display = state["view"], state["gdata"], state["display"]
    if view:
        state["zoom_limited"] = read(handle, view + 0x44, 1)[0]
        state["height"] = round(f32(handle, view + 0x40), 1)
        state["map_min"] = f32(handle, view + VIEW_CAMERA_LIMITS + 4)
        state["map_max"] = f32(handle, view + VIEW_CAMERA_LIMITS + 8)
    if gdata:
        state["clip_mult"] = f32(handle, gdata + 0x950)
    if display:
        state["letterboxed"] = read(handle, display + 0xD8, 1)[0]
    return state


def describe(s):
    parts = [f"patches {s['patches']}/{s['patch_total']}"]
    if s["view"]:
        parts.append(f"zoomLimited={s['zoom_limited']} height={s['height']:g} "
                     f"(map limits {s['map_min']:g}..{s['map_max']:g})")
    if s["gdata"]:
        parts.append(f"clipMult={s['clip_mult']:g}")
    if s["display"]:
        parts.append(f"letterboxed={s['letterboxed']}")
    return "  ".join(parts)


def main():
    args = sys.argv[1:]
    wait = float(args[args.index("--wait") + 1]) if "--wait" in args else 0.0
    watch = float(args[args.index("--watch") + 1]) if "--watch" in args else 0.0
    deadline = time.time() + max(wait, watch)

    pid = find_pid()
    while pid is None and time.time() < deadline:
        time.sleep(0.5)
        pid = find_pid()
    if pid is None:
        print("game.dat is not running")
        return 1

    handle = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        print(f"cannot open game.dat (pid {pid}), error {ctypes.get_last_error()}"
              " - run this from an admin prompt")
        return 1
    try:
        print(f"game.dat pid {pid}")
        if watch:
            start, last = time.time(), None
            while time.time() < deadline and find_pid() == pid:
                line = describe(snapshot(handle))
                if line != last:
                    print(f"[{time.time() - start:6.1f}s] {line}", flush=True)
                    last = line
                time.sleep(0.5)
            return 0

        s = snapshot(handle)
        while (not s["view"] or not s["gdata"]) and time.time() < deadline:
            time.sleep(1)
            s = snapshot(handle)
        print(describe(s))
        good = s["patches"] == s["patch_total"] and s["view"] and s["zoom_limited"] == 0
        print("RESULT:", "zoomfix active" if good else "NOT as expected")
        return 0 if good else 1
    finally:
        k32.CloseHandle(handle)


if __name__ == "__main__":
    sys.exit(main())
