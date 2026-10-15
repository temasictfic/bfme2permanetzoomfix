/*
 * zoomfix.c - dinput8.dll proxy for The Battle for Middle-earth II v1.00
 *             (game.dat FileVersion 1.0.2194.40862, PE timestamp 0x43E44B4A)
 *
 * Replaces the "!RemoveZoomLimits.map" trick and makes the camera configurable:
 *
 *   - TheTacticalView zoom-limited flag stays off (W3DView+0x44 = FALSE), as after
 *     the map's CAMERA_LETTERBOX_BEGIN + MAP_EXIT, so the camera moves freely.
 *   - W3DView::setHeightAboveGround always clamps the height, using MinHeight and
 *     MaxHeight from zoomfix.ini instead of the map's limits (stock: 120..300).
 *     The clamp is skipped while letterboxed (cutscenes), as in the stock game.
 *   - The camera opens at DefaultHeight when a map starts or the camera is reset
 *     (stock: the map's max height, 300).
 *   - Far clip depth multiplier = ClipMultiplier (GlobalData+0x950, far plane
 *     1800 * multiplier; the map trick used 100).
 *
 * Settings: %APPDATA%\My Battle for Middle-earth(tm) II Files\zoomfix.ini, created
 * with defaults on first run and read once at game start.
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
#include <float.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

#define GAME_IMAGE_BASE   0x00400000u
#define GAME_TIMESTAMP    0x43E44B4Au
#define GAME_SIZEOFIMAGE  0x00AD0496u

#define THE_TACTICAL_VIEW   0x00DF98E4u  /* W3DView*                                   */
#define THE_DISPLAY         0x00DF9880u  /* W3DDisplay*; +0xD8 = letterboxed (bool)    */
#define VIEW_CAMERA_LIMITS  0x24C8u      /* W3DView: camera limits object, min +4, max +8 */
#define DISPLAY_LETTERBOXED 0xD8u

/* Camera limits vtable: [0] getMinHeight, [1] getMaxHeight (both plain "fld [ecx+N]") */
#define LIMITS_VTABLE       0x00BCC590u

/* Return addresses of the getMinHeight/getMaxHeight calls that get configured values */
#define RA_CLAMP_MIN_1  0x0048D2AFu      /* W3DView::setHeightAboveGround */
#define RA_CLAMP_MIN_2  0x0048D2C0u
#define RA_CLAMP_MAX_1  0x0048D2CAu
#define RA_CLAMP_MAX_2  0x0048D2DAu
#define RA_SCROLL_MIN   0x0048C933u      /* W3DView::update, height adjust while scrolling */
#define RA_SCROLL_MAX   0x0048C957u
#define RA_RESET_1      0x0048D464u      /* W3DView::setZoomToDefault */
#define RA_RESET_2      0x0048D477u
#define RA_MAPSTART_1   0x0048D938u      /* W3DView camera init for a new map */
#define RA_MAPSTART_2   0x0048D94Eu

#define INI_NAME "zoomfix.ini"
#define USER_DATA_FOLDER "My Battle for Middle-earth(tm) II Files"

typedef struct Settings {
    float minHeight;        /* 0 = map's own value */
    float maxHeight;        /* 0 = map's own value */
    float defaultHeight;    /* 0 = map's own value (its max height) */
    float clipMultiplier;
} Settings;

static Settings g_cfg = { 120.0f, 1500.0f, 500.0f, 100.0f };

static const char g_default_ini[] =
    "; BFME2 zoom fix settings. Read when the game starts: restart the game after editing.\n"
    ";\n"
    "; Heights are the camera height above the ground. The stock game uses MinHeight 120\n"
    "; and MaxHeight 300, and every game opens at 300. Set a height to 0 to use the map's\n"
    "; own value.\n"
    "\n"
    "; How close you can zoom in (keeps the camera above the map)\n"
    "MinHeight = 120\n"
    "\n"
    "; How far you can zoom out\n"
    "MaxHeight = 1500\n"
    "\n"
    "; Camera height when a game starts and when the camera is reset\n"
    "DefaultHeight = 500\n"
    "\n"
    "; Draw distance multiplier (1 = stock, 100 = same as the !RemoveZoomLimits map)\n"
    "ClipMultiplier = 100\n";

typedef struct Patch {
    DWORD       addr;
    SIZE_T      len;
    const char *what;
    BYTE        orig[18];
    BYTE        repl[18];
} Patch;

enum { PATCH_CLIP = 4, PATCH_VTABLE = 5 };   /* entries whose replacement is built at runtime */

static Patch g_patches[] = {
    /* View::View: m_zoomLimited = TRUE  ->  FALSE */
    { 0x0065E3EF, 4, "camera starts with zoom-limited flag off",
      { 0xC6, 0x40, 0x44, 0x01 },
      { 0xC6, 0x40, 0x44, 0x00 } },
    /* W3DDisplay::enableLetterBox(FALSE): setZoomLimited(TRUE)  ->  (FALSE) */
    { 0x0044459E, 2, "letterbox end keeps flag off",
      { 0x6A, 0x01 },
      { 0x6A, 0x00 } },
    /* W3DDisplay::toggleLetterBox: setZoomLimited(!letterboxed)  ->  (FALSE) */
    { 0x00444547, 3, "letterbox toggle keeps flag off",
      { 0x0F, 0x94, 0xC2 },                  /* sete dl            */
      { 0x32, 0xD2, 0x90 } },                /* xor dl,dl ; nop    */
    /* W3DView::setHeightAboveGround: "if (m_zoomLimited) clamp" -> always clamp */
    { 0x0048D2A1, 2, "height clamp always on",
      { 0x74, 0x3A },                        /* je skip_clamp      */
      { 0x90, 0x90 } },
    /* GlobalData::GlobalData: m_clipDepthMultiplier = 1.0f  ->  ClipMultiplier
       movss [esi+950h],xmm1 ; mov dword [esi+988h],3
       -> mov dword [esi+950h],<float> ; push 3 ; pop dword [esi+988h] */
    { 0x00636446, 18, "clip depth multiplier",
      { 0xF3, 0x0F, 0x11, 0x8E, 0x50, 0x09, 0x00, 0x00,
        0xC7, 0x86, 0x88, 0x09, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00 },
      { 0 } },
    /* camera limits vtable: getMinHeight, getMaxHeight -> hooks */
    { LIMITS_VTABLE, 8, "camera height hooks",
      { 0x1B, 0xA1, 0x8E, 0x00,              /* 0x008EA11B */
        0x5C, 0x41, 0x8F, 0x00 },            /* 0x008F415C */
      { 0 } },
};
#define NUM_PATCHES (sizeof(g_patches) / sizeof(g_patches[0]))

static FILE *g_log;

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    if (!g_log)
        return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
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

/* ---- settings -------------------------------------------------------------- */

static BOOL ini_path(char *out, DWORD size)
{
    char appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    DWORD attr;

    if (n == 0 || n >= MAX_PATH)
        return FALSE;
    if (_snprintf(out, size, "%s\\%s", appdata, USER_DATA_FOLDER) < 0)
        return FALSE;
    attr = GetFileAttributesA(out);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return FALSE;
    return _snprintf(out, size, "%s\\%s\\%s", appdata, USER_DATA_FOLDER, INI_NAME) >= 0;
}

static float parse_height(const char *value, float fallback)
{
    char *end;
    double v = strtod(value, &end);
    if (end == value || !(v >= 0.0 && v <= 1000000.0))
        return fallback;
    return (float)v;
}

static void load_settings(void)
{
    char path[MAX_PATH], line[256];
    FILE *f;

    if (!ini_path(path, sizeof(path))) {
        log_msg("settings: user data folder not found, using defaults");
        return;
    }
    f = fopen(path, "r");
    if (!f) {
        f = fopen(path, "w");
        if (f) {
            fputs(g_default_ini, f);
            fclose(f);
            log_msg("settings: created %s with defaults", path);
        }
        return;
    }
    log_msg("settings: %s", path);
    while (fgets(line, sizeof(line), f)) {
        char *key = line, *eq, *value, *p;
        if ((p = strchr(line, ';')) != NULL)
            *p = '\0';
        if ((eq = strchr(line, '=')) == NULL)
            continue;
        *eq = '\0';
        value = eq + 1;
        while (*key == ' ' || *key == '\t')
            key++;
        for (p = eq - 1; p >= key && (*p == ' ' || *p == '\t'); p--)
            *p = '\0';
        if (_stricmp(key, "MinHeight") == 0)
            g_cfg.minHeight = parse_height(value, g_cfg.minHeight);
        else if (_stricmp(key, "MaxHeight") == 0)
            g_cfg.maxHeight = parse_height(value, g_cfg.maxHeight);
        else if (_stricmp(key, "DefaultHeight") == 0)
            g_cfg.defaultHeight = parse_height(value, g_cfg.defaultHeight);
        else if (_stricmp(key, "ClipMultiplier") == 0) {
            float m = parse_height(value, g_cfg.clipMultiplier);
            if (m > 0.0f)
                g_cfg.clipMultiplier = m;
        }
    }
    fclose(f);
}

/* ---- camera height hooks ------------------------------------------------------ */

#define RAW_MIN(self) (*(const float *)((self) + 4))
#define RAW_MAX(self) (*(const float *)((self) + 8))

static BOOL is_tactical_view_limits(const BYTE *self)
{
    const BYTE *view = *(const BYTE * const *)(ULONG_PTR)THE_TACTICAL_VIEW;
    return view != NULL && self == view + VIEW_CAMERA_LIMITS;
}

static BOOL letterboxed(void)
{
    const BYTE *display = *(const BYTE * const *)(ULONG_PTR)THE_DISPLAY;
    return display != NULL && display[DISPLAY_LETTERBOXED] != 0;
}

static float limit_min(const BYTE *self)
{
    return g_cfg.minHeight > 0.0f ? g_cfg.minHeight : RAW_MIN(self);
}

static float limit_max(const BYTE *self)
{
    float lo = limit_min(self);
    float hi = g_cfg.maxHeight > 0.0f ? g_cfg.maxHeight : RAW_MAX(self);
    return hi < lo ? lo : hi;
}

static float start_height(const BYTE *self)
{
    float h = g_cfg.defaultHeight > 0.0f ? g_cfg.defaultHeight : RAW_MAX(self);
    float lo = limit_min(self), hi = limit_max(self);
    return h < lo ? lo : (h > hi ? hi : h);
}

/* Both replace "fld dword ptr [ecx+N] ; ret" (thiscall, no stack args): __fastcall
   receives this in ECX, ignores EDX, and returns the float in ST(0). Calls that are
   not ours get the map's raw value, exactly like the original getter. */
static float __fastcall hook_get_min_height(const BYTE *self, void *unused_edx)
{
    DWORD ra = (DWORD)(ULONG_PTR)_ReturnAddress();
    (void)unused_edx;
    if (is_tactical_view_limits(self)) {
        if (ra == RA_CLAMP_MIN_1 || ra == RA_CLAMP_MIN_2)
            return letterboxed() ? -FLT_MAX : limit_min(self);
        if (ra == RA_SCROLL_MIN)
            return limit_min(self);
    }
    return RAW_MIN(self);
}

static float __fastcall hook_get_max_height(const BYTE *self, void *unused_edx)
{
    DWORD ra = (DWORD)(ULONG_PTR)_ReturnAddress();
    (void)unused_edx;
    if (is_tactical_view_limits(self)) {
        if (ra == RA_CLAMP_MAX_1 || ra == RA_CLAMP_MAX_2)
            return letterboxed() ? FLT_MAX : limit_max(self);
        if (ra == RA_SCROLL_MAX)
            return limit_max(self);
        if (ra == RA_RESET_1 || ra == RA_RESET_2 || ra == RA_MAPSTART_1 || ra == RA_MAPSTART_2)
            return start_height(self);
    }
    return RAW_MAX(self);
}

/* ---- patching ------------------------------------------------------------------ */

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

static void build_runtime_patches(void)
{
    static const BYTE clip_head[6] = { 0xC7, 0x86, 0x50, 0x09, 0x00, 0x00 };
    static const BYTE clip_tail[8] = { 0x6A, 0x03, 0x8F, 0x86, 0x88, 0x09, 0x00, 0x00 };
    Patch *clip = &g_patches[PATCH_CLIP];
    Patch *vtbl = &g_patches[PATCH_VTABLE];
    DWORD hooks[2];

    memcpy(clip->repl, clip_head, sizeof(clip_head));
    memcpy(clip->repl + 6, &g_cfg.clipMultiplier, 4);
    memcpy(clip->repl + 10, clip_tail, sizeof(clip_tail));

    hooks[0] = (DWORD)(ULONG_PTR)hook_get_min_height;
    hooks[1] = (DWORD)(ULONG_PTR)hook_get_max_height;
    memcpy(vtbl->repl, hooks, sizeof(hooks));
}

static BOOL write_memory(DWORD addr, const BYTE *bytes, SIZE_T len)
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
    log_msg("bfme2 zoomfix: host %s", exe);

    why = check_host();
    if (why) {
        log_msg("skipped: %s", why);
        return;
    }

    load_settings();
    log_msg("MinHeight=%g MaxHeight=%g DefaultHeight=%g ClipMultiplier=%g  (0 = map value)",
         g_cfg.minHeight, g_cfg.maxHeight, g_cfg.defaultHeight, g_cfg.clipMultiplier);
    build_runtime_patches();

    /* All-or-nothing: every site must still hold its original bytes. */
    for (i = 0; i < NUM_PATCHES; i++) {
        const Patch *p = &g_patches[i];
        if (memcmp((const void *)(ULONG_PTR)p->addr, p->orig, p->len) != 0) {
            log_msg("skipped: unexpected bytes at site \"%s\"", p->what);
            return;
        }
    }

    for (i = 0; i < NUM_PATCHES; i++) {
        const Patch *p = &g_patches[i];
        if (write_memory(p->addr, p->repl, p->len)) {
            applied++;
            log_msg("patched: %s", p->what);
        } else {
            log_msg("FAILED: %s (VirtualProtect)", p->what);
        }
    }
    log_msg("patched %u/%u", (unsigned)applied, (unsigned)NUM_PATCHES);
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
