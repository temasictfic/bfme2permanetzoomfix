/*
 * zoomfix.c - dinput8.dll proxy for The Battle for Middle-earth II v1.00
 *             (game.dat FileVersion 1.0.2194.40862, PE timestamp 0x43E44B4A)
 *
 * Re-creates, at startup, the state that "!RemoveZoomLimits.map" leaves behind,
 * so the map no longer has to be loaded first:
 *
 *   - TheTacticalView zoom limits off (W3DView+0x44 = FALSE). The map does this
 *     with CAMERA_LETTERBOX_BEGIN -> W3DDisplay::enableLetterBox(TRUE) ->
 *     TheTacticalView->setZoomLimited(FALSE), then exits before letterbox ends.
 *   - Far clip depth multiplier 100.0 (GlobalData+0x950, far plane 1800 -> 180000).
 *     The map does this with SET_CAMERA_CLIP_DEPTH_MULTIPLIER 100.
 *
 * Only memory of this process is patched; game.dat on disk is never modified.
 * Every patch site is checked against the original bytes first; if anything
 * differs (other game version, other exe), nothing is patched.
 *
 * Install: copy dinput8.dll next to game.dat.  Uninstall: delete it.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define GAME_IMAGE_BASE   0x00400000u
#define GAME_TIMESTAMP    0x43E44B4Au
#define GAME_SIZEOFIMAGE  0x00AD0496u

typedef struct Patch {
    DWORD       addr;
    SIZE_T      len;
    const char *what;
    BYTE        orig[18];
    BYTE        repl[18];
} Patch;

static const Patch g_patches[] = {
    /* View::View: m_zoomLimited = TRUE  ->  FALSE */
    { 0x0065E3EF, 4, "View::View zoom limits off",
      { 0xC6, 0x40, 0x44, 0x01 },
      { 0xC6, 0x40, 0x44, 0x00 } },
    /* W3DDisplay::enableLetterBox(FALSE): setZoomLimited(TRUE)  ->  (FALSE) */
    { 0x0044459E, 2, "letterbox end keeps zoom limits off",
      { 0x6A, 0x01 },
      { 0x6A, 0x00 } },
    /* W3DDisplay::toggleLetterBox: setZoomLimited(!letterboxed)  ->  (FALSE) */
    { 0x00444547, 3, "letterbox toggle keeps zoom limits off",
      { 0x0F, 0x94, 0xC2 },                  /* sete dl            */
      { 0x32, 0xD2, 0x90 } },                /* xor dl,dl ; nop    */
    /* GlobalData::GlobalData: m_clipDepthMultiplier = 1.0f  ->  100.0f
       movss [esi+950h],xmm1 ; mov dword [esi+988h],3
       -> mov dword [esi+950h],42C80000h ; push 3 ; pop dword [esi+988h] */
    { 0x00636446, 18, "clip depth multiplier 100",
      { 0xF3, 0x0F, 0x11, 0x8E, 0x50, 0x09, 0x00, 0x00,
        0xC7, 0x86, 0x88, 0x09, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00 },
      { 0xC7, 0x86, 0x50, 0x09, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x42,
        0x6A, 0x03,
        0x8F, 0x86, 0x88, 0x09, 0x00, 0x00 } },
};
#define NUM_PATCHES (sizeof(g_patches) / sizeof(g_patches[0]))

static FILE *g_log;

static void log_line(const char *fmt, const char *arg)
{
    if (!g_log)
        return;
    fprintf(g_log, fmt, arg);
    fputc('\n', g_log);
}

static void open_log(void)
{
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH - 32)
        return;
    strcat(path, "bfme2_zoomfix.log");
    g_log = fopen(path, "w");
}

/* The main module must be the exact game.dat build the addresses belong to. */
static const char *check_host(void)
{
    const BYTE *base = (const BYTE *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *nt;

    if ((DWORD)(ULONG_PTR)base != GAME_IMAGE_BASE)
        return "main module is not at 0x400000";
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return "main module has no MZ header";
    nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return "main module has no PE header";
    if (nt->FileHeader.TimeDateStamp != GAME_TIMESTAMP ||
        nt->OptionalHeader.SizeOfImage != GAME_SIZEOFIMAGE)
        return "main module is not BFME2 v1.00 game.dat";
    return NULL;
}

static BOOL write_code(DWORD addr, const BYTE *bytes, SIZE_T len)
{
    DWORD old;
    if (!VirtualProtect((LPVOID)(ULONG_PTR)addr, len, PAGE_EXECUTE_READWRITE, &old))
        return FALSE;
    memcpy((void *)(ULONG_PTR)addr, bytes, len);
    VirtualProtect((LPVOID)(ULONG_PTR)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)addr, len);
    return TRUE;
}

static void apply_patches(void)
{
    char exe[MAX_PATH] = "?";
    const char *why;
    SIZE_T i, applied = 0;

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    log_line("bfme2 zoomfix: host %s", exe);

    why = check_host();
    if (why) {
        log_line("skipped: %s", why);
        return;
    }

    /* All-or-nothing: every site must still hold its original bytes. */
    for (i = 0; i < NUM_PATCHES; i++) {
        const Patch *p = &g_patches[i];
        const void *at = (const void *)(ULONG_PTR)p->addr;
        if (memcmp(at, p->orig, p->len) != 0) {
            log_line("skipped: unexpected bytes at site \"%s\"", p->what);
            return;
        }
    }

    for (i = 0; i < NUM_PATCHES; i++) {
        const Patch *p = &g_patches[i];
        if (write_code(p->addr, p->repl, p->len)) {
            applied++;
            log_line("patched: %s", p->what);
        } else {
            log_line("FAILED: %s (VirtualProtect)", p->what);
        }
    }

    if (g_log)
        fprintf(g_log, "patched %u/%u\n", (unsigned)applied, (unsigned)NUM_PATCHES);
}

/* ---- forwarding to the real system dinput8.dll ---------------------------- */

static HMODULE g_real;

static FARPROC real_proc(const char *name)
{
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16)
            return NULL;
        strcat(path, "\\dinput8.dll");      /* SysWOW64 for this 32-bit process */
        g_real = LoadLibraryA(path);
        if (!g_real)
            return NULL;
    }
    return GetProcAddress(g_real, name);
}

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, const void *, void **, void *);
typedef HRESULT (WINAPI *DllCanUnloadNow_t)(void);
typedef HRESULT (WINAPI *DllGetClassObject_t)(const void *, const void *, void **);
typedef HRESULT (WINAPI *DllRegisterServer_t)(void);

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD version, const void *riid,
                                  void **out, void *outer)
{
    DirectInput8Create_t fn = (DirectInput8Create_t)real_proc("DirectInput8Create");
    return fn ? fn(hinst, version, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    DllCanUnloadNow_t fn = (DllCanUnloadNow_t)real_proc("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(const void *clsid, const void *riid, void **out)
{
    DllGetClassObject_t fn = (DllGetClassObject_t)real_proc("DllGetClassObject");
    return fn ? fn(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI DllRegisterServer(void)
{
    DllRegisterServer_t fn = (DllRegisterServer_t)real_proc("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    DllRegisterServer_t fn = (DllRegisterServer_t)real_proc("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        open_log();
        apply_patches();
        if (g_log) {
            fclose(g_log);
            g_log = NULL;
        }
    }
    return TRUE;
}
