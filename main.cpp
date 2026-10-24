// =============================================================================
//  Proper Cover and Drive-By  v72 -  GTA IV: The Complete Edition
//  (ProperCoverandDrive-By.asi; replaces GTAIVCoverAim.asi and GTAIVDriveByPreAim.asi)
//
//  Features: pop-out aiming from cover, manual crouch in tall cover, forced cover reload,
//  faster cover entry and standing cover animations, weapon switching while aiming
//  (on foot, in cover, in vehicles), sniper scope switching, shoulder swap (LB),
//  GTA V-style drive-bys (LB aim / RB fire, look behind, thrown weapons),
//  NPCs reacting to being aimed at (cover and vehicles).
//
//  Native-feeling cover shooting:
//    * Aim in cover (LT / right mouse): pop out of cover and stay out,
//      aiming. Fire only when you pull the trigger (RT / left mouse).
//      Release aim: slide back into cover, using the game's own animations.
//    * Peek = keep pushing the stick while you can't move any further
//      (against a corner edge, or into low cover).
//    * Fire without aiming = vanilla blind fire.
//
//  How vanilla works (and what we change):
//    The cover task (CTaskComplexNewUseCover) pops you out only for
//    "aim + fire" (cover state 8 = slide out, 9 = out). While out, a
//    CTaskComplexGun runs with a small "plan" stored in the task:
//        +0x6C first action      (1 aim, 2 reload, 3 fire)
//        +0x6D after aiming      (1 aim, 3 fire, 4 end)
//        +0x6E after a burst     (1 aim, 2 reload, 3 fire, 4 end)
//    Vanilla uses "fire, then end", hence: pop out, one shot, back in.
//
//    1. While aim is held, the single "aim + fire?" check that decides to
//       slide out (cover state 8, call at 0xCFFFD8) answers "yes", so you pop
//       out. Nothing else sees a fake fire (same spot the original
//       ImprovedCoverSystem used).
//    2. While aim is held, the player's gun task plan is rewritten every
//       frame: aim -> keep aiming, fire only while the trigger is held.
//       When aim is released, the plan becomes "end", and the game slides
//       you back into cover by itself.
//    3. The gun task picks "fire" as its very first step on its own (call at
//       0xD4D127). While only aiming, it creates the "aim" step instead.
//    4. If the gun task is about to end while aim is still held (e.g. after
//       a shot), it gets another "aim" step, so you stay out.
//    5. Released aim: the cover gun task is flagged "keep aiming", so its aim
//       step never stops by itself. We stop it with the game's own
//       "stop aiming" call (0xDCE880, what vanilla does on foot), the plan
//       says "end", and the game slides you back into cover.
//    All of 2-5 only apply to pop-outs this mod started (cover states 8/9),
//    never to vanilla aim+fire pop-outs or gun-peeks (state 18), and they
//    step aside when the clip is empty so vanilla returns you to cover to
//    reload (cover state 7). Holding aim then pops you straight back out.
//    The gun task reads the real trigger, so firing is fully manual.
//
//  Everything is found by pattern; addresses are for reference only.
// =============================================================================

#include <stdint.h>

extern "C" {
__declspec(dllimport) void*    __stdcall GetModuleHandleA(const char*);
__declspec(dllimport) int      __stdcall VirtualProtect(void*, uint32_t, uint32_t, uint32_t*);
__declspec(dllimport) int      __stdcall DisableThreadLibraryCalls(void*);
__declspec(dllimport) uint32_t __stdcall GetEnvironmentVariableA(const char*, char*, uint32_t);
__declspec(dllimport) void*    __stdcall AddVectoredExceptionHandler(uint32_t, void*);
__declspec(dllimport) int      __stdcall GetModuleHandleExA(uint32_t, const char*, void**);
__declspec(dllimport) uint32_t __stdcall GetModuleFileNameA(void*, char*, uint32_t);
__declspec(dllimport) void*    __stdcall CreateFileA(const char*, uint32_t, uint32_t, void*, uint32_t, uint32_t, void*);
__declspec(dllimport) int      __stdcall WriteFile(void*, const void*, uint32_t, uint32_t*, void*);
__declspec(dllimport) int      __stdcall CloseHandle(void*);
__declspec(dllimport) void*    __stdcall LoadLibraryA(const char*);
__declspec(dllimport) void*    __stdcall GetProcAddress(void*, const char*);
__declspec(dllimport) uint32_t __stdcall GetPrivateProfileIntA(const char*, const char*, int, const char*);
__declspec(dllimport) uint32_t __stdcall GetFileAttributesA(const char*);
__declspec(dllimport) uint32_t __stdcall GetCurrentProcessId();
__declspec(dllimport) int16_t  __stdcall GetAsyncKeyState(int);
__declspec(dllimport) void*    __stdcall GetForegroundWindow();
__declspec(dllimport) uint32_t __stdcall GetWindowThreadProcessId(void*, uint32_t*);
int _fltused = 0;
}
#define PAGE_EXECUTE_READWRITE 0x40

// ---- Tuning ---------------------------------------------------------------------
static const int   kAimThreshold   = 0x40;   // aim trigger (0..255) counted as "aiming"
static const float kStickPush      = 0.6f;   // stick push needed to peek
static const float kStuckSpeed     = 0.35f;  // m/s; slower = "can't move further"
static const float kPeekDelay      = 0.20f;  // seconds of pushing before the peek starts

// ---- Memory / pattern helpers -----------------------------------------------------
static void Unprotect(void* at, uint32_t size, uint32_t* old) { VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, old); }
static void Reprotect(void* at, uint32_t size, uint32_t old)  { uint32_t t; VirtualProtect(at, size, old, &t); }
static uintptr_t CallDest(uintptr_t at) { return (at && *(uint8_t*)at == 0xE8) ? at + 5 + *(int32_t*)(at + 1) : 0; }
static void PatchCall(uintptr_t at, void* to) {
    uint32_t prot; Unprotect((void*)at, 5, &prot);
    *(int32_t*)(at + 1) = (int32_t)((uintptr_t)to - (at + 5));
    Reprotect((void*)at, 5, prot);
}
static void PatchPtr(uintptr_t* slot, uintptr_t value) {
    uint32_t prot; Unprotect(slot, 4, &prot); *slot = value; Reprotect(slot, 4, prot);
}
static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static bool IsCommonByte(uint8_t b) {
    switch (b) {
    case 0x00: case 0xFF: case 0x8B: case 0x89: case 0xE8: case 0x83: case 0xCC: case 0x24:
    case 0x44: case 0x04: case 0xC7: case 0x0F: case 0x8D: case 0x50: case 0x56: case 0x57:
    case 0x74: case 0x75: case 0x85: case 0xC0: case 0x01: case 0x08: case 0x10:
        return true;
    }
    return false;
}
static uint8_t* SearchBlock(uint8_t* start, uint32_t size, const uint8_t* bytes, const bool* wild, int n) {
    if (n <= 0 || (uint32_t)n > size) return nullptr;
    int anchor = -1;
    for (int k = 0; k < n; ++k) if (!wild[k] && !IsCommonByte(bytes[k])) { anchor = k; break; }
    if (anchor < 0) for (int k = 0; k < n; ++k) if (!wild[k]) { anchor = k; break; }
    if (anchor < 0) return start;
    const uint8_t a = bytes[anchor];
    const uint32_t last = size - (uint32_t)n;
    for (uint32_t i = 0; i <= last; ++i) {
        if (start[i + anchor] != a) continue;
        int k = 0;
        while (k < n && (wild[k] || start[i + k] == bytes[k])) ++k;
        if (k == n) return start + i;
    }
    return nullptr;
}
static uintptr_t FindPattern(const char* pat) {
    uint8_t bytes[128]; bool wild[128]; int n = 0;
    for (const char* p = pat; *p && n < 128;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { wild[n] = true; bytes[n++] = 0; ++p; if (*p == '?') ++p; continue; }
        bytes[n] = (uint8_t)(HexVal(p[0]) * 16 + HexVal(p[1])); wild[n++] = false; p += 2;
    }
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    uint8_t* nt   = base + *(int32_t*)(base + 0x3C);
    uint16_t nsec = *(uint16_t*)(nt + 6);
    uint8_t* sec  = nt + 24 + *(uint16_t*)(nt + 20);
    for (int s = 0; s < nsec; ++s, sec += 40) {
        if (!(*(uint32_t*)(sec + 36) & 0x20000000)) continue;
        if (uint8_t* m = SearchBlock(base + *(uint32_t*)(sec + 12), *(uint32_t*)(sec + 8), bytes, wild, n))
            return (uintptr_t)m;
    }
    return 0;
}
// Redirect every direct call (E8 rel32) to `target` in the game's code. Returns the count.
static int PatchAllCallsTo(uintptr_t target, void* to) {
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    uint8_t* nt   = base + *(int32_t*)(base + 0x3C);
    uint16_t nsec = *(uint16_t*)(nt + 6);
    uint8_t* sec  = nt + 24 + *(uint16_t*)(nt + 20);
    int count = 0;
    for (int s = 0; s < nsec; ++s, sec += 40) {
        if (!(*(uint32_t*)(sec + 36) & 0x20000000)) continue;
        uint8_t* p = base + *(uint32_t*)(sec + 12);
        uint32_t size = *(uint32_t*)(sec + 8);
        for (uint32_t i = 0; i + 5 <= size; ++i) {
            if (p[i] != 0xE8) continue;
            uintptr_t at = (uintptr_t)(p + i);
            if (at + 5 + *(int32_t*)(at + 1) != target) continue;
            uint32_t prot; VirtualProtect((void*)at, 5, 0x40, &prot);
            *(int32_t*)(at + 1) = (int32_t)((uintptr_t)to - (at + 5));
            uint32_t t; VirtualProtect((void*)at, 5, prot, &t);
            ++count; i += 4;
        }
    }
    return count;
}
template <typename T> static T& At(void* base, uint32_t off) { return *(T*)((uint8_t*)base + off); }
static float Sqrt(float v) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(v)); return r; }

// ---- Small test log on the Desktop (ProperCoverandDrive-By.log) -----------------------------
static char g_logPath[600];
static int  g_logLines = 0;
static void LogNum(char*& p, int v) {
    char t[12]; int n = 0; bool neg = v < 0; unsigned u = neg ? -v : v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) *p++ = '-';
    while (n) *p++ = t[--n];
}
static void Log(const char* a, int x = 0x7FFFFFFF, const char* b = nullptr, int y = 0x7FFFFFFF) {
    if (!g_logPath[0] || g_logLines > 2500) return;
    ++g_logLines;
    char line[200]; char* p = line;
    for (const char* q = a; *q && p < line + 120; ++q) *p++ = *q;
    if (x != 0x7FFFFFFF) LogNum(p, x);
    if (b) for (const char* q = b; *q && p < line + 160; ++q) *p++ = *q;
    if (y != 0x7FFFFFFF) LogNum(p, y);
    *p++ = '\r'; *p++ = '\n';
    static void* h = nullptr;                   // opened once (append), kept open
    if (!h) { h = CreateFileA(g_logPath, 4, 1, nullptr, 4, 0x80, nullptr); if (h == (void*)-1) { h = nullptr; g_logPath[0] = 0; return; } }
    uint32_t w; WriteFile(h, line, (uint32_t)(p - line), &w, nullptr);
}
// Crash diagnostics: log the first few access violations (address, registers, call stack).
static int g_crashLogs = 0;
static void LogHex(const char* a, uint32_t v) {
    char t[16]; char* p = t; const char* hx = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) *p++ = hx[(v >> i) & 15];
    *p = 0; int keep = g_logLines; g_logLines = 0; Log(a, 0x7FFFFFFF, t); g_logLines = keep;
}
static long __stdcall CrashLogger(void** info) {
    uint32_t* rec = (uint32_t*)info[0]; uint8_t* ctx = (uint8_t*)info[1];
    if (rec[0] != 0xC0000005u || g_crashLogs >= 3) return 0;     // EXCEPTION_CONTINUE_SEARCH
    ++g_crashLogs;
    LogHex("ACCESS VIOLATION at ", *(uint32_t*)(ctx + 0xB8));
    {   // which module the crash is in (game, this mod, or something else)
        void* mod = nullptr; char name[300];
        if (GetModuleHandleExA(6, (const char*)*(uintptr_t*)(ctx + 0xB8), &mod) && mod &&
            GetModuleFileNameA(mod, name, 299)) {
            name[299] = 0; int keep = g_logLines; g_logLines = 0; Log("  in module ", 0x7FFFFFFF, name); g_logLines = keep;
            LogHex("  module base ", (uint32_t)(uintptr_t)mod);
        } else { int keep = g_logLines; g_logLines = 0; Log("  in no loaded module"); g_logLines = keep; }
    }
    LogHex("  address ", rec[6]);
    LogHex("  eax ", *(uint32_t*)(ctx + 0xB0)); LogHex("  ecx ", *(uint32_t*)(ctx + 0xAC));
    LogHex("  edx ", *(uint32_t*)(ctx + 0xA8)); LogHex("  esi ", *(uint32_t*)(ctx + 0xA0));
    LogHex("  edi ", *(uint32_t*)(ctx + 0x9C));
    uint32_t* sp = *(uint32_t**)(ctx + 0xC4); int found = 0;
    for (int i = 0; i < 64 && found < 14; ++i) {
        uint32_t v = sp[i];
        static uintptr_t lo = 0, hi = 0;
        if (!lo) { uint8_t* b = (uint8_t*)GetModuleHandleA(nullptr); lo = (uintptr_t)b; hi = lo + *(uint32_t*)(b + *(int32_t*)(b + 0x3C) + 0x50); }
        if (v > lo && v < hi) { LogHex("  stack ", v); ++found; }
    }
    return 0;
}
static float g_logTime = 0.0f;   // mirrors g_time for timestamps
static void LogT(const char* a) { Log(a, (int)(g_logTime * 1000.0f), " ms"); }

// ---- Settings (ProperCoverandDrive-By.ini next to the ASI; written with defaults if missing) ----
struct Config {
    bool coverAim = true, coverPeek = true, coverCrouch = true, coverReload = true;
    bool fastCoverEntry = true, crouchAccuracy = true, coverExitAimCamera = true;
    int  coverAnimSpeed = 120;                   // percent, standing tall cover only; 100 = off
    bool npcReactions = true;
    bool scrollSwitchAiming = false;
    bool weaponSwitchAiming = true, weaponSwitchVehicle = true, sniperSwitchCamera = true, meleeSwitchDropsAim = true;
    bool shoulderSwap = true;
    int  shoulderPad = 4, shoulderKey = 0x45;    // LB, E
    bool driveBy = true, driveByLookBehind = true, driveByThrown = true;
    int  dbAimPad = 4, dbFirePad = 6, dbAimMouse = 2, dbFireMouse = 1;   // LB, RB, right, left mouse
    bool log = false;
};
static Config g_cfg;
static char g_iniPath[600];
static const char kDefaultIni[] =
"; ProperCoverandDrive-By settings. \r\n"
"; Delete this file to get the defaults back.\r\n"
"\r\n"
"[Cover]\r\n"
"; Aim in cover pops you out and keeps you out; fire only on the trigger.\r\n"
"PopOutAiming=1\r\n"
"\r\n"
"[Weapons]\r\n"
"; Switch weapons while aiming (on foot and in cover).\r\n"
"SwitchWhileAiming=1\r\n"
"\r\n"
"; Enables mousewheel weapon switching while aiming. \r\n"
"; rebind zoom in the game's controls first, then set to 1 \r\n"
"ScrollSwitchWhileAiming=0\r\n"
"\r\n"
"; Switch weapons while drive-by aiming.\r\n"
"SwitchInVehicle=1\r\n"
"\r\n"
"[ShoulderSwap]\r\n"
"; Swap shoulders while aiming on foot\r\n"
"Enable=1\r\n"
"\r\n"
"; Controller button: 4 LB, 5 LT, 6 RB, 7 RT, 14 X/Square, 15 Y/Triangle,\r\n"
"; 16 A/Cross, 17 B/Circle. -1 = none.\r\n"
"PadButton=4\r\n"
"\r\n"
"; Keyboard key, Windows virtual-key code (69 = E, 81 = Q, 70 = F, 9 = Tab).\r\n"
"; https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes  0 = none.\r\n"
"Key=69\r\n"
"\r\n"
"[DriveBy]\r\n"
"; GTA 5 style drive-bys\r\n"
"Enable=1\r\n"
"\r\n"
"; Controller buttons (same numbers as above).\r\n"
"AimPadButton=4\r\n"
"FirePadButton=6\r\n"
"\r\n"
"\r\n"
"[Debug]\r\n"
"; Write a log to the Desktop.\r\n"
"Log=0\r\n";
static int IniInt(const char* sec, const char* key, int def) { return (int)GetPrivateProfileIntA(sec, key, def, g_iniPath); }
static bool IniBool(const char* sec, const char* key, bool def) { return IniInt(sec, key, def ? 1 : 0) != 0; }
static void LoadConfig(void* hModule) {
    uint32_t n = GetModuleFileNameA(hModule, g_iniPath, 590);
    if (!n || n >= 590) return;
    while (n && g_iniPath[n - 1] != '.' && g_iniPath[n - 1] != '\\') --n;
    if (!n || g_iniPath[n - 1] != '.') return;
    g_iniPath[n] = 'i'; g_iniPath[n + 1] = 'n'; g_iniPath[n + 2] = 'i'; g_iniPath[n + 3] = 0;
    if (GetFileAttributesA(g_iniPath) == 0xFFFFFFFFu) {   // missing: write the defaults
        void* h = CreateFileA(g_iniPath, 0x40000000, 1, nullptr, 2, 0x80, nullptr);
        if (h != (void*)-1) { uint32_t w; WriteFile(h, kDefaultIni, sizeof(kDefaultIni) - 1, &w, nullptr); CloseHandle(h); }
    }
    Config d;
    g_cfg.coverAim            = IniBool("Cover", "PopOutAiming", d.coverAim);
    g_cfg.weaponSwitchAiming  = IniBool("Weapons", "SwitchWhileAiming", d.weaponSwitchAiming);
    g_cfg.weaponSwitchVehicle = IniBool("Weapons", "SwitchInVehicle", d.weaponSwitchVehicle);
    g_cfg.scrollSwitchAiming  = IniBool("Weapons", "ScrollSwitchWhileAiming", d.scrollSwitchAiming);
    g_cfg.shoulderSwap        = IniBool("ShoulderSwap", "Enable", d.shoulderSwap);
    g_cfg.shoulderPad         = IniInt ("ShoulderSwap", "PadButton", d.shoulderPad);
    g_cfg.shoulderKey         = IniInt ("ShoulderSwap", "Key", d.shoulderKey);
    g_cfg.driveBy             = IniBool("DriveBy", "Enable", d.driveBy);
    g_cfg.dbAimPad            = IniInt ("DriveBy", "AimPadButton", d.dbAimPad);
    g_cfg.dbFirePad           = IniInt ("DriveBy", "FirePadButton", d.dbFirePad);
    g_cfg.log                 = IniBool("Debug", "Log", d.log);
}
// Keyboard key held. (Aim has to be held in the game too, so no focus check is needed.)
static bool KeyHeld(int vk) {
    if (vk <= 0 || vk > 254) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
static void InitLog() {
    if (!g_cfg.log) { g_logPath[0] = 0; return; }
    uint32_t len = GetEnvironmentVariableA("USERPROFILE", g_logPath, 500);
    if (!len || len >= 500) { g_logPath[0] = 0; return; }
    const char* tail = "\\Desktop\\ProperCoverandDrive-By.log";
    for (const char* q = tail; *q; ++q) g_logPath[len++] = *q;
    g_logPath[len] = 0;
    void* h = CreateFileA(g_logPath, 0x40000000, 1, nullptr, 2, 0x80, nullptr);
    if (h == (void*)-1) { g_logPath[0] = 0; return; }
    CloseHandle(h);
    Log("Proper Cover and Drive-By v72 loaded");
    Log(g_iniPath[0] ? "settings: ProperCoverandDrive-By.ini" : "settings: defaults (ini path not found)");
    AddVectoredExceptionHandler(1, (void*)&CrashLogger);
}

// ---- Game data --------------------------------------------------------------------
//  CControl +0x2698: inputs, 16 bytes each; byte +4 = key, +6 = value ^ key.
static const int INPUT_ATTACK     = 4;
static const int INPUT_ATTACK2    = 5;
static const int INPUT_AIM        = 6;
static const int INPUT_MOVE_LEFT  = 12, INPUT_MOVE_RIGHT = 13, INPUT_MOVE_UP = 14, INPUT_MOVE_DOWN = 15;
static const int INPUT_DUCK       = 20;
static const int INPUT_FREE_AIM   = 87;   // right mouse on foot

// CTaskComplexGun plan bytes
static const uint32_t GUN_FIRST = 0x6C, GUN_AFTER_AIM = 0x6D, GUN_AFTER_FIRE = 0x6E;
static const uint8_t  ACT_AIM = 1, ACT_FIRE = 3, ACT_END = 4;

static void* (__cdecl* Game_FindPlayerPed)(int) = nullptr;
static void* (__cdecl* Game_GetPlayerControls)() = nullptr;
static bool  (__cdecl* Game_IsFirePressed)() = nullptr;        // the game's own "player fire" check
static uint32_t (__cdecl* Orig_AfterControls)() = nullptr;
static bool (__cdecl* Orig_State8Fire)() = nullptr;
static void* (__thiscall* Game_GunCreateAim)(void* task, void* ped, bool a) = nullptr;               // 0xD4C760
static void* (__thiscall* Game_GetCurrentWeapon)(void* weaponData) = nullptr;                       // 0xB64E30
static void* (__cdecl*    Game_GetWeaponInfo)(int weaponType) = nullptr;                             // 0xAF65D0
static void* (__thiscall* Game_GunCreateReload)(void* task, void* ped, bool a) = nullptr;            // 0xD4D610
static void  (__thiscall* Game_AimStop)(void* aimTask, bool a) = nullptr;                             // 0xDCE880
static void* (__thiscall* Orig_GunCreateFire)(void* task, void* ped, bool a, bool b) = nullptr;      // 0xD4C9E0
typedef void* (__thiscall* TaskFn)(void* task, void* ped);
static TaskFn Orig_CoverNext, Orig_CoverFirst, Orig_CoverControl;   // CTaskComplexPlayerInCover 18/19/20
static TaskFn Orig_UseNext,   Orig_UseFirst,   Orig_UseControl;     // CTaskComplexNewUseCover   18/19/20
static void (__thiscall* Game_CoverRefreshIdle)(void* coverTask, void* ped) = nullptr;   // 0xD1C860
static void* (__thiscall* Game_StartAnim)(void* blender, int group, int anim, float blend, int fallback) = nullptr; // 0xB54D70
static void* (__thiscall* Game_FindAnim)(void* blender, int group, int anim) = nullptr;                            // 0xB55C20
static void  (__thiscall* Game_AnimSetBlend)(void* assoc, float delta) = nullptr;                                   // 0xC62BC0
static bool (__thiscall* Game_CoverCorner)(void* coverTask, void* ped) = nullptr;       // 0xD12120
static TaskFn Orig_GunNext,   Orig_GunFirst,   Orig_GunControl;     // CTaskComplexGun           18/19/20
static uint8_t* g_controls = nullptr;
static float*   g_timeStep = nullptr;

static uint8_t* Input(int i)              { return g_controls + 0x2698 + i * 16; }
static int  InputValue(int i)             { uint8_t* v = Input(i); return (uint8_t)(v[6] ^ v[4]); }
static void SetInputValue(int i, int val) { uint8_t* v = Input(i); v[6] = (uint8_t)(val ^ v[4]); }
// Same as the game (0x8F00C0): the "negative" input carries the axis on a controller; the
// "positive" one is only read when the first isn't pushed. Centre 128 with a small dead zone:
// on keyboard the first input rests at 127, which used to read as "pushing left" forever.
static float Axis(int neg, int pos) {
    float a = (InputValue(neg) - 128.0f) * (1.0f / 128.0f);
    if (a < -0.1f) return a;
    float b = (InputValue(pos) - 128.0f) * (1.0f / 128.0f);
    return (b > 0.1f || b < -0.1f) ? b : 0.0f;
}
static int TaskType(void* task) {
    if (!task) return -1;
    return ((int(__thiscall*)(void*))(*(uintptr_t**)task)[3])(task);
}

// ---- State --------------------------------------------------------------------------
static uint32_t g_frame = 0;
static float g_time = 0.0f;          // seconds, advanced every frame
static uint32_t g_coverFrame = 0xFFFFFFF0u;   // last frame the player's cover task ran
static bool  g_realAim  = false;             // player really holds aim (not our peek)
static float g_pushTime = 0.0f;
static float g_lastX = 0.0f, g_lastY = 0.0f;
static bool  g_havePos = false;
static bool  InCover() { return (g_frame - g_coverFrame) <= 2; }
static bool  IsPlayer(void* ped) { return ped && ped == Game_FindPlayerPed(0); }

// ---- Manual crouch state ------------------------------------------------------------
//  Cover point flags (dword at cp+0): bits 3-4 = height. 3 = standing (tall) cover.
//  For the player's own cover point the game itself only ever uses 3 or 0 (it clears any
//  other value to 0), so crouching writes 0: the same value real low cover gets.
static const uint32_t kHeightMask = 0x18, kTall = 0x18;
static bool      g_crouched   = false;      // player chose to crouch in this cover
static bool      g_duckWanted = false;      // DUCK pressed, toggle on the cover task's next update
static bool      g_duckHeld   = false;      // swallow DUCK until it's released
static bool      g_atCorner   = false;      // cached from the cover task
static uint32_t* g_fakedCp    = nullptr;    // cover point we lowered
static void*     g_fakedPed   = nullptr;    // ...belonging to this ped
static float     g_tallX = 0.0f, g_tallY = 0.0f;   // where the game last said "tall"
static bool      g_stanceSet = false;       // player changed stance in this cover
static bool      g_enteredCrouched = false; // took this cover while crouching
static bool      g_stanceCrouched = false;  // ...and it's now crouched
static uint32_t* g_lastLoggedCp = nullptr;
static bool NearLastTall(void* ped) {
    uint8_t* m = ped ? At<uint8_t*>(ped, 0x20) : nullptr;
    if (!m) return false;
    float dx = At<float>(m, 0x30) - g_tallX, dy = At<float>(m, 0x34) - g_tallY;
    return dx * dx + dy * dy < 1.0f;
}
static uint32_t g_fakedOrig = 0;            // original height bits of the point we lowered
static void RestoreCoverPoint() {
    if (!g_fakedCp) return;
    void* ped = Game_FindPlayerPed(0);
    // A world cover point (not the player's own copy at ped+0xE70): the game won't recompute
    // it, so put back exactly what it had, wherever the player is now.
    if (!g_fakedPed || g_fakedCp != (uint32_t*)((uint8_t*)g_fakedPed + 0xE70)) {
        *g_fakedCp = (*g_fakedCp & ~kHeightMask) | (g_fakedOrig & kHeightMask);
        g_fakedCp = nullptr; g_fakedPed = nullptr;
        return;
    }
    // Put "tall" back only where the game itself last said tall; elsewhere 0 is the truth.
    if (ped && ped == g_fakedPed && (*g_fakedCp & kHeightMask) == 0 && NearLastTall(ped))
        *g_fakedCp |= kTall;
    g_fakedCp = nullptr; g_fakedPed = nullptr;
}
static bool CrouchedMiddle() { return g_crouched && g_fakedCp && !g_atCorner; }
// Lower a cover point to "low" for the crouch, remembering its original height.
static void LowerCp(uint32_t* cp, void* ped) {
    if (g_fakedCp && g_fakedCp != cp) RestoreCoverPoint();
    if (g_fakedCp != cp) g_fakedOrig = *cp;
    *cp &= ~kHeightMask; g_fakedCp = cp; g_fakedPed = ped;
}
// Crouch pressed while popped out: switch stance in place. The gun task's aim animation
// (gun task +0x4C..+0x52, picked from the cover height) is recomputed and the aim step
// restarted, so the next aim plays the other stance's pose.
static bool  g_swapWanted = false;
static const bool kStandUpViaCover = false;   // diagnostic build: in-place again
static bool  g_standReaim = false;     // standing up from a crouched pop-out: going back in first
static float g_standStart = 0.0f;
static float g_swapStart  = 0.0f;
static bool  g_rawAimHeld = false;
static void* g_useTask    = nullptr;   // player's NewUseCover task (valid while in cover)
// AimGun blend-in rate (+0x5C, vanilla 8.0 = ~0.13 s). Rising from a crouch at that speed
// looks like a snap, so the aim step after a crouch->stand swap blends in slower.
// Old pose blends out at the same rate the new one blends in (+0x60 = blend-out, vanilla
// -4.0), so the two always add up to a full pose instead of dipping in between.
static const float kSwapBlend = 5.0f;
static float g_swapBlend = 0.0f;
static void* g_swapOldAim = nullptr;   // aim task being replaced by a stance swap
// The AimGun task deletes its animations instantly (blend -1000) when it's destroyed, which
// was the snap when standing up. Before the old aim is destroyed, take its animations
// off it (unhook the callback, clear its pointers) and let them fade out normally.
static void (__thiscall* Game_AnimRemoveCallback)(void* assoc, void* task) = nullptr; // 0xC62AB0
static void DetachAimAnims(void* aim) {
    static const uint32_t offs[3] = { 0x64, 0x68, 0xAC };
    for (uint32_t o : offs) {
        void* a = At<void*>(aim, o);
        if (!a) continue;
        Game_AnimRemoveCallback(a, aim);
        Game_AnimSetBlend(a, -kSwapBlend);
        At<void*>(aim, o) = nullptr;
    }
}
static float g_lastSwapTime = -100.0f;
// Popped out of low cover (real, or our crouch) counts as "ducked" for accuracy and the
// reticle. Blind fire keeps its vanilla spread.
static int g_coverState = -1;   // player's current cover state
static bool CoverCrouched(void* ped) {
    if (!ped || !InCover() || (g_coverState != 8 && g_coverState != 9) || !IsPlayer(ped)) return false;
    uint32_t* cp = At<uint32_t*>(ped, 0xD68);
    return cp && (*cp & kHeightMask) != kTall;
}

static bool (__thiscall* Orig_IsDucking)(void* ped) = nullptr;   // 0x9E8110
static float g_holdAimCamT = -100.0f;     // keep the aim camera shortly after this time
static float g_holdAimCamLen = 1.0f;
static float g_sniperSwapT = -100.0f;     // last weapon switch to/from the sniper while aiming
static float g_toSniperT   = -100.0f;     // switched TO the sniper while aiming
static float g_fromSniperT = -100.0f;     // switched FROM the sniper while aiming
static uint8_t* g_scopeFxFlag = nullptr;   // the scope screen overlay flag (0x11F703A)
static float g_cutInterpT  = -100.0f;     // follow -> aim blends right after this are instant
static bool  g_aimSuppress = false;       // switched to fists/melee while aiming: drop aim until released
static const int INPUT_RELOAD = 29;       // CControl +0x2868
// Forced cover reload: popped out and the clip runs dry (or reload is pressed): go back
// into cover, reload there (cover state 7), then pop out again if aim is still held.
static bool  g_coverReload = false;
static float g_coverReloadT = 0.0f;
static bool  g_coverReloadSeen7 = false;
static int   g_lastCoverState = -999;
static int   g_lastGunSub = -999;
static bool  g_popOutByAim = false;   // the current pop-out was started by aiming, not by firing
static void* Weapon(void* ped);
static bool  IsMeleeSlot(void* ped) {        // fists or a melee weapon in hand
    uint32_t slot = At<uint32_t>(ped, 0x2B0);
    if (slot > 12) return false;
    int type = At<int>(ped, 0x2B0 + (slot + 3) * 12);   // weapon type in the current slot
    return slot <= 1 || (type >= 0 && type <= 3);       // unarmed, bat, pool cue, knife
}
static void StartCoverReload(const char* why) {
    if (g_coverReload || !g_cfg.coverReload) return;
    g_coverReload = true; g_coverReloadT = g_time; g_coverReloadSeen7 = false;
    g_realAim = false;
    Log(why);
}
static const char* CamName(void* cam);
// Entering cover while aiming keeps the aim camera (it has a cover mode) and blends it.
// Leaving, the "aiming" flags drop for a frame or two (cover aim over, on-foot aim not yet
// set), and in that gap the camera manager swaps the aim camera for the follow camera.
// Keep both flags up across the gap so the aim camera stays, as it does on entry.
static void KeepAimFlags(void* ped) {
    if (!ped) return;
    At<uint32_t>(ped, 0x26C) |= 0x40000;                       // gun raised
    if (uint8_t* pinfo = At<uint8_t*>(ped, 0x228)) At<uint32_t>(pinfo, 0x440) |= 8;   // aiming on foot
}
// ---- Diagnostic: leaving cover while aiming ------------------------------------------
static float g_exitT = -100.0f;
static int   g_exitAimPulse = 0;
static bool  g_exitFromCover = false;   // the open window is a cover exit (not the on-foot baseline)   // frames left of the aim "re-press" when leaving cover
static bool  InExitWindow() { return g_time - g_exitT < 1.5f; }
static void  LogX(const char* a, int v = 0x7FFFFFFF) {   // "[+123 ms] text value"
    char buf[160]; char* p = buf; const char* pre = "[+";
    while (*pre) *p++ = *pre++;
    int ms = (int)((g_time - g_exitT) * 1000.0f); char t[12]; int n = 0;
    if (ms < 0) ms = 0;
    do { t[n++] = (char)('0' + ms % 10); ms /= 10; } while (ms);
    while (n) *p++ = t[--n];
    const char* mid = " ms] "; while (*mid) *p++ = *mid++;
    for (const char* q = a; *q && p < buf + 150; ++q) *p++ = *q;
    *p = 0;
    Log(buf, v);
}
// ---- Diagnostic: dump the ped's playing animations ---------------------------------
//  Blender (ped+0x78) +0x1A28 = first node; node+0x48 == 1: anim node, clip at node+4
//  (group +0xC, anim +0x10, flags +4, blend +0x58); next node at +0x8C.
static float g_dumpStart = -100.0f;
static int   g_dumpIndex = 0;
static const char* g_dumpWhat = "";
static const bool kDiagDumps = false;   // animation dumps (diagnostic builds only)
static void DumpAnims(void* ped, const char* tag) {
    if (!kDiagDumps) return;
    void* bl = At<void*>(ped, 0x78);
    if (!bl) return;
    Log(tag, (int)((g_time - g_dumpStart) * 1000.0f), " ms:");
    int n = 0;
    for (uint8_t* node = At<uint8_t*>(bl, 0x1A28); node && n < 24; node = At<uint8_t*>(node, 0x8C), ++n) {
        if (At<uint16_t>(node, 0x48) != 1) continue;
        uint8_t* a = node + 4;
        if (!At<void*>(a, 0x40)) continue;
        int blend = (int)(At<float>(a, 0x58) * 100.0f);
        Log("   anim ", At<int>(a, 0xC) * 1000 + At<int>(a, 0x10), " blend% ", blend);
        Log("      flags ", (int)At<uint32_t>(a, 4), " phase% ", (int)(At<float>(a, 0x4C) * 100.0f));
    }
}
// ---- Stand-up while popped out: make it look like on foot -------------------------
//  The diagnostic showed what on-foot stand-up has that cover doesn't:
//   * the stand-up transition clip (group 81, clip 60) plays as the body animation;
//   * the standing body clip blends in at a normal rate; in cover it crept in at a few %
//     per frame while the crouched one was gone after ~0.15 s, leaving the body "empty".
static const float kStandBlend = 2.5f;   // ~0.4 s crossfade (vanilla cover used ~8, i.e. 0.13 s)
static float  g_standFixStart = -100.0f;
static void*  g_standTrans = nullptr;
static bool   g_standTransOut = false;
static void StartStandFix(void* ped) {
    void* bl = At<void*>(ped, 0x78);
    if (!bl || !Game_StartAnim) return;
    g_standFixStart = g_time; g_standTransOut = false;
    g_standTrans = Game_StartAnim(bl, 81, 60, 4.0f, -1);
    Log(g_standTrans ? "stand fix: stand-up transition started" : "stand fix: transition clip not available");
}
static void TickStandFix(void* ped) {
    float t = g_time - g_standFixStart;
    if (t < 0.0f || t > 1.2f || !ped) return;
    void* bl = At<void*>(ped, 0x78);
    if (!bl) return;
    if (t < 0.9f) {
        // Slow, even crossfade for the body clips that are changing: the ones
        // coming in and the ones going out (flag 0x4000) all move at kStandBlend, so the
        // stand-up takes about as long as on foot / in passive aim.
        int n = 0;
        for (uint8_t* node = At<uint8_t*>(bl, 0x1A28); node && n < 24; node = At<uint8_t*>(node, 0x8C), ++n) {
            if (At<uint16_t>(node, 0x48) != 1) continue;
            uint8_t* a = node + 4;
            if (!At<void*>(a, 0x40) || (void*)a == g_standTrans || At<int>(a, 0xC) < 0) continue;
            uint32_t f = At<uint32_t>(a, 4);
            // Body clips only (flag 0x40000000). The aim clips are driven by the aim task
            // (the old one is already playing its lower-the-gun part), so slowing them
            // made the arms flap; they keep the game's own timing.
            if (!(f & 0x40000000)) continue;
            float w = At<float>(a, 0x58);
            if (f & 0x4000) { if (w > 0.01f) Game_AnimSetBlend(a, -kStandBlend); }
            else if (w < 0.99f) Game_AnimSetBlend(a, kStandBlend);
        }
    }
    // Let the transition go once it has played (or after ~0.9 s).
    if (g_standTrans && !g_standTransOut) {
        void* tr = Game_FindAnim(bl, 81, 60);
        if (tr != g_standTrans) { g_standTrans = nullptr; return; }
        if (At<float>(tr, 0x4C) >= 0.85f || t > 0.9f) { Game_AnimSetBlend(tr, -4.0f); g_standTransOut = true; Log("stand fix: transition fading out"); }
    }
}
static void StartDump(const char* what) { if (!kDiagDumps) return; g_dumpStart = g_time; g_dumpIndex = 0; g_dumpWhat = what; }
static void TickDump(void* ped) {
    static const float at[8] = { 0.0f, 0.08f, 0.16f, 0.25f, 0.4f, 0.6f, 0.9f, 1.3f };
    if (g_dumpIndex >= 8 || !ped) return;
    if (g_time - g_dumpStart >= at[g_dumpIndex]) { DumpAnims(ped, g_dumpWhat); ++g_dumpIndex; }
}

// ---- Shoulder swap while aiming (LB) ----------------------------------------------------
//  Rebuilt from LibertyShoulder: the aim camera reads its sideways shoulder offset from a
//  15-row settings table (rows of 40 bytes, offset at +0x10; table found from the aim
//  camera's "lea eax,[eax*8+table]" at 0xA26F88). Negating it puts the camera over the
//  left shoulder. Changes from the original:
//    * the shoulder slides across (kSwapTime) instead of jumping,
//    * one button (default LB / E, set in the ini), back to the right shoulder when you stop aiming,
//    * guns only (the sniper too), on foot only: not in cover or a vehicle.
static const float kSwapTime = 0.18f;          // seconds for a full shoulder change
static float* g_camTable = nullptr;            // 15 rows x 10 floats
static float  g_camOffset[15];                 // original sideways offsets
static float  g_shoulder = 1.0f;               // current: +1 right ... -1 left
static float  g_shoulderTarget = 1.0f;
static float  g_lastToggle = -100.0f;
static bool   g_lbHeld = false;
static bool ShoulderButtonHeld();
static void InitShoulderSwap(uintptr_t site) {
    float* t = *(float**)(site + 29);
    static const float expect[15] = { 0.475f, 0.475f, 0.475f, 0.2f, 0.2f, 0.2f, 0.2f, 0.2f, 0.2f, 0.375f, 0.375f, 0.375f, 0.475f, 0.475f, 0.475f };
    for (int r = 0; r < 15; ++r) {
        float d = t[r * 10 + 4] - expect[r];
        if (d > 0.01f || d < -0.01f) { Log("shoulder swap: camera table not as expected, disabled"); return; }
        g_camOffset[r] = t[r * 10 + 4];
    }
    uint32_t prot; Unprotect(t, 15 * 40, &prot);   // left writable
    g_camTable = t;
    Log("shoulder swap enabled, pad button ", g_cfg.shoulderPad, ", key ", g_cfg.shoulderKey);
}
static void ApplyShoulder() {
    for (int r = 0; r < 15; ++r) g_camTable[r * 10 + 4] = g_camOffset[r] * g_shoulder;
}
static void UpdateShoulderSwap(void* ped) {
    if (!g_camTable) return;
    bool lb = ShoulderButtonHeld();
    bool press = lb && !g_lbHeld;
    g_lbHeld = lb;

    bool aiming = InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F;
    bool inVeh = ped && (At<uint32_t>(ped, 0x26C) & 4);
    bool gun = ped && !IsMeleeSlot(ped);
    bool scoped = ped && At<uint32_t>(ped, 0x2B0) == 6;
    // The mod itself drops aim for a moment in a few places (cover reload, leaving cover
    // while aiming, standing up): only count it as "stopped aiming" after 0.25 s.
    static float lastAim = -100.0f;
    if (aiming || g_coverReload) lastAim = g_time;
    bool stillAiming = g_time - lastAim < 0.25f;
    bool allowed = g_cfg.shoulderSwap && ped && stillAiming && gun && !inVeh && !InCover() && !g_aimSuppress;
    (void)scoped;

    if (!allowed) g_shoulderTarget = 1.0f;     // back to the right shoulder
    else if (press && aiming && g_time - g_lastToggle > 0.2f) {
        g_lastToggle = g_time;
        g_shoulderTarget = g_shoulderTarget > 0.0f ? -1.0f : 1.0f;
        Log(g_shoulderTarget < 0.0f ? "shoulder: left" : "shoulder: right");
    }
    if (inVeh) g_shoulder = g_shoulderTarget;   // no slide where it can't be seen
    float step = (g_timeStep ? *g_timeStep : 0.016f) * (2.0f / kSwapTime);
    float prev = g_shoulder;
    if (g_shoulder < g_shoulderTarget) { g_shoulder += step; if (g_shoulder > g_shoulderTarget) g_shoulder = g_shoulderTarget; }
    else if (g_shoulder > g_shoulderTarget) { g_shoulder -= step; if (g_shoulder < g_shoulderTarget) g_shoulder = g_shoulderTarget; }
    if (g_shoulder != prev || (g_shoulder == g_shoulderTarget && g_camTable[4] != g_camOffset[0] * g_shoulder)) {
        // Ease in/out: the offset follows a smoothstep of the linear slide.
        float u = (1.0f - g_shoulder) * 0.5f;            // 0 = right, 1 = left
        float e = u * u * (3.0f - 2.0f * u);
        float saved = g_shoulder;
        g_shoulder = 1.0f - 2.0f * e;
        ApplyShoulder();
        g_shoulder = saved;
    }
}

// ---- Per-frame input logic ------------------------------------------------------------
static void DriveByControls();
static void UpdateStandSmooth(void* ped);
static uint32_t __cdecl Hook_AfterControls() {
    uint32_t r = Orig_AfterControls();
    ++g_frame;
    if (g_timeStep) g_time += *g_timeStep;
    g_logTime = g_time;
    g_realAim = false;
    DriveByControls();
    if (Game_GetPlayerControls() != g_controls) {
        g_pushTime = 0.0f; g_havePos = false;
        if (g_camTable && g_shoulder != 1.0f) { g_shoulder = g_shoulderTarget = 1.0f; ApplyShoulder(); }
        return r;
    }

    {   // diagnostic: on foot, aiming, crouch pressed -> dump
        void* p = Game_FindPlayerPed(0);
        static bool lastDuck = false;
        bool duck = InputValue(INPUT_DUCK) > 0x7F;
        bool aiming = InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F;
        if (p && !InCover() && aiming && duck && !lastDuck)
            StartDump(Orig_IsDucking(p) ? "ON FOOT stand-up while aiming, " : "ON FOOT crouch while aiming, ");
        lastDuck = duck;
        TickDump(p);
        {   // diagnostic: which inputs does the d-pad/keys produce while aiming
            static int lastMask = 0;
            int mask = (InputValue(8) > 0x7F ? 1 : 0) | (InputValue(9) > 0x7F ? 2 : 0) | (InputValue(10) > 0x7F ? 4 : 0) | (InputValue(11) > 0x7F ? 8 : 0);
            if (aiming && mask && mask != lastMask) Log("while aiming: next_weapon+prev*2+target_left*4+target_right*8 = ", mask);
            lastMask = mask;
        }
        if (p && g_exitFromCover && g_time - g_exitT < 0.08f &&
            (InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F)) KeepAimFlags(p);
        if (p && InExitWindow()) {
            static int lastBits = -1;
            int bits = ((InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F) ? 1 : 0) | (InCover() ? 2 : 0) | ((At<uint32_t>(p, 0x26C) & 0x40000) ? 4 : 0) | (Orig_IsDucking(p) ? 8 : 0);
            uint8_t* pinfo = At<uint8_t*>(p, 0x228);
            if (pinfo && (At<uint32_t>(pinfo, 0x440) & 8)) bits |= 16;   // on-foot "aiming" flag
            if (bits != lastBits) { lastBits = bits; LogX("state: aimHeld+inCover*2+gunRaised*4+ducking*8+onFootAiming*16 = ", bits); }
        }
        TickStandFix(p);
        UpdateStandSmooth(p);
    }
    // Crouch toggle (DUCK) while in cover; the cover task applies it.
    if (InCover()) {
        bool duck = InputValue(INPUT_DUCK) > 0x7F;
        if (g_cfg.coverCrouch) {
            if (duck && !g_duckHeld) { g_duckWanted = true; g_duckHeld = true; }
            if (!duck) g_duckHeld = false;
            if (g_duckHeld) SetInputValue(INPUT_DUCK, 0);
        }
        g_rawAimHeld = InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F;
        if (g_swapWanted && (g_time - g_swapStart > 2.0f || !g_rawAimHeld)) { g_swapWanted = false; Log("stance swap dropped"); }
        if (g_standReaim) {
            if (g_time - g_standStart > 2.0f || !g_rawAimHeld) { g_standReaim = false; g_duckWanted = false; Log("stand while aiming: dropped"); }
            else { SetInputValue(INPUT_AIM, 0); SetInputValue(INPUT_FREE_AIM, 0); }
        }
        // Crouched behind a tall wall, away from a corner: nothing to shoot over.
        if (CrouchedMiddle()) {
            SetInputValue(INPUT_AIM, 0); SetInputValue(INPUT_FREE_AIM, 0);
            SetInputValue(INPUT_ATTACK, 0); SetInputValue(INPUT_ATTACK2, 0);
        }
    } else {
        g_duckWanted = false; g_duckHeld = false; g_swapWanted = false; g_standReaim = false; g_useTask = nullptr;
        g_coverState = -1; g_popOutByAim = false; g_lastCoverState = -999; g_lastGunSub = -999;
        if (g_crouched) { g_crouched = false; Log("left cover: crouch off"); }
        g_stanceSet = false;
        RestoreCoverPoint();
    }

    // Leaving cover while aiming: on foot only starts aiming on a fresh press of aim, and
    // aim has been held since cover, so the game walked you around with the gun up until
    // something re-triggered it. Let go of aim for two frames right as cover ends, so the
    // on-foot code sees a new press while you're still holding it.
    if (g_exitAimPulse > 0) {
        SetInputValue(INPUT_AIM, 0); SetInputValue(INPUT_FREE_AIM, 0);
        if (--g_exitAimPulse == 0 && InExitWindow()) LogX("exit: aim re-pressed for on-foot");
    }

    void* ped = Game_FindPlayerPed(0);

    // Switched to fists / a melee weapon while aiming: no aim camera or reticle for those.
    // Drop aim until it's released (or a gun is selected again).
    if (g_aimSuppress) {
        bool held = InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F;
        if (!held || !ped || !IsMeleeSlot(ped)) { g_aimSuppress = false; Log(held ? "aim restored (gun selected)" : "aim released after melee switch"); }
        else { SetInputValue(INPUT_AIM, 0); SetInputValue(INPUT_FREE_AIM, 0); }
    }

    // Switched away from the sniper: the scope overlay flag only gets cleared on the
    // game's on-foot camera path, which the direct scope -> aim cut skips. Clear it.
    if (g_scopeFxFlag && ped && g_time - g_fromSniperT < 3.0f && At<uint32_t>(ped, 0x2B0) != 6 && *g_scopeFxFlag) {
        *g_scopeFxFlag = 0;
        static float lastLog = -100.0f;
        if (g_time - lastLog > 1.0f) { lastLog = g_time; Log("scope overlay cleared"); }
    }

    // Forced cover reload.
    if (ped && InCover() && !g_coverReload && g_popOutByAim && (g_coverState == 8 || g_coverState == 9)) {
        uint8_t* rl = Input(INPUT_RELOAD);
        bool reloadPress = (uint8_t)(rl[6] ^ rl[4]) > 0x7F && (uint8_t)(rl[7] ^ rl[4]) <= 0x7F;
        if (void* w = Weapon(ped)) {
            int clip = At<int>(w, 0x5C), spare = At<int>(w, 0x60) - clip;
            int clipSize = 0;
            if (Game_GetWeaponInfo) if (void* info = Game_GetWeaponInfo(At<int>(w, 0x18))) clipSize = At<int16_t>(info, 0x86);
            if (spare > 0 && clip <= 0) StartCoverReload("cover reload: clip empty, back into cover");
            else if (spare > 0 && reloadPress && clip < clipSize) {
                StartCoverReload("cover reload: reload pressed, back into cover");
                rl[6] = rl[4];   // don't let the gun task reload out here
            }
        }
    }
    if (g_coverReload) {
        if (g_coverState == 7) g_coverReloadT = g_time;   // long reloads: wait as long as it plays
        if (!InCover() || !ped || g_time - g_coverReloadT > 3.0f) { g_coverReload = false; Log("cover reload: cancelled"); }
        else {
            // Hold aim and fire off until the reload in cover is done.
            SetInputValue(INPUT_AIM, 0); SetInputValue(INPUT_FREE_AIM, 0);
            SetInputValue(INPUT_ATTACK, 0); SetInputValue(INPUT_ATTACK2, 0);
            if (g_coverState == 7) {
                if (!g_coverReloadSeen7) Log("cover reload: reloading in cover");
                g_coverReloadSeen7 = true;
            } else if (g_coverReloadSeen7) {
                g_coverReload = false; Log("cover reload: done");
            } else if (g_coverState == 3) {
                // Back in cover. Clip empty reloads by itself; a partial clip needs the
                // reload button, which was used up while popped out: press it again.
                uint8_t* rl = Input(INPUT_RELOAD);
                rl[6] = (uint8_t)(0xFF ^ rl[4]); rl[7] = rl[4];
            }
        }
    }
    g_realAim = !g_coverReload && (InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F);
    UpdateShoulderSwap(ped);
    uint8_t* m = ped ? At<uint8_t*>(ped, 0x20) : nullptr;
    if (!m) { g_pushTime = 0.0f; g_havePos = false; return r; }

    float x = At<float>(m, 0x30), y = At<float>(m, 0x34);
    float dt = *g_timeStep, speed = 0.0f;
    if (g_havePos && dt > 0.0001f) { float dx = x - g_lastX, dy = y - g_lastY; speed = Sqrt(dx * dx + dy * dy) / dt; }
    g_lastX = x; g_lastY = y; g_havePos = true;

    // Peek: in cover, not aiming, stick pushed but the player isn't moving.
    if (g_cfg.coverPeek && InCover() && !g_realAim && !g_coverReload && !g_aimSuppress && !CrouchedMiddle()) {
        float sx = Axis(INPUT_MOVE_LEFT, INPUT_MOVE_RIGHT), sy = Axis(INPUT_MOVE_UP, INPUT_MOVE_DOWN);
        bool pushing = sx * sx + sy * sy > kStickPush * kStickPush;
        if (pushing && speed < kStuckSpeed) g_pushTime += dt; else g_pushTime = 0.0f;
        if (g_pushTime >= kPeekDelay) SetInputValue(INPUT_AIM, 0xFF);   // vanilla peek
    } else {
        g_pushTime = 0.0f;
    }
    return r;
}

// ---- 1. Pop out: the slide-out check sees "fire" while really aiming ------------------
static bool __cdecl Hook_State8Fire() {
    bool real = Orig_State8Fire();
    if (!g_cfg.coverAim) return real;
    if (real) { g_popOutByAim = false; return true; }          // genuine aim + fire: vanilla
    if (g_realAim && InCover()) { g_popOutByAim = true; return true; }
    return false;
}
static void MarkCover(void* ped) { if (IsPlayer(ped)) g_coverFrame = g_frame; }
static void LogCoverState(void* task, void* ped) {
    if (!IsPlayer(ped)) return;
    int st = At<int>(task, 0x38);
    if (st == 22 && g_lastCoverState != 22 && g_realAim && g_cfg.coverExitAimCamera) { g_exitT = g_time; LogX("EXIT WHILE AIMING from state ", g_lastCoverState); g_exitFromCover = true; KeepAimFlags(ped); g_holdAimCamT = g_time; g_holdAimCamLen = 1.0f; }
    g_coverState = st;
    if (st == 3) g_popOutByAim = false;            // back in cover
    if (st != g_lastCoverState) { Log("cover state ", g_lastCoverState, " -> ", st); g_lastCoverState = st; }
}
static bool (__thiscall* Orig_UpdateCover)(void* task, void* ped, void* out) = nullptr;   // 0xD0FC00
static void UpdateAimReactions(void* ped);
// ---- Faster standing cover animations -------------------------------------------------
//  Standing in tall cover, the move and pop-out/back-in animations play kCoverAnimSpeed
//  faster (clip speed = assoc +0x54). Crouched / low cover, entry, exit, reload and
//  shooting keep the game's speed.
static float kCoverAnimSpeed = 1.2f;          // set from the ini (Cover / AnimationSpeed)
// The game sets the move clips' speed (+0x54) again every frame, so changing it does
// nothing. Instead scale what the clip advances by each frame: CAnimPlayer's "phase step"
// virtual (vtable slot 2, 0xC624F0). Each cover update lists the player's clips that should
// run faster; the hook scales only those.
static void* g_fastClips[32];
static int   g_fastCount = 0;
static uint32_t g_fastFrame = 0;
static uintptr_t g_animPlayerVt = 0;
static float (__thiscall* Orig_ClipStep)(void* clip) = nullptr;
// ---- Smoother crouch -> stand in cover -------------------------------------------------
//  Standing up (or a wall that steps up while moving) swaps the body clips' weights in a
//  frame or two, which is the jerk. For a short window, each of the player's body clips
//  may only change its weight (+0x58) by kStandSmoothRate per second; the game's target is
//  followed, just not instantly.
static const float kStandSmoothTime = 0.8f;    // seconds the smoothing stays on
static const float kStandSmoothRate = 3.0f;    // max weight change per second
static const float kStandSmoothEase = 7.0f;    // ease-out speed (~0.4 s to settle)
static int   g_smoothTraces = 0, g_smoothFootTraces = 0;
static bool  g_smoothOnFoot = false;
struct SmoothClip { void* clip; float w; };
static SmoothClip g_smooth[32];
static int   g_smoothCount = 0;
static float g_smoothStart = -100.0f;
static void* g_bodyClips[32];                  // player's body clips this frame
static int   g_bodyCount = 0;
static uint32_t g_bodyFrame = 0;
static void StartStandSmooth(void* ped, const char* why) {
    void* bl = At<void*>(ped, 0x78);
    if (!bl) return;
    g_smoothCount = 0;
    int n = 0;
    for (uint8_t* node = At<uint8_t*>(bl, 0x1A28); node && n < 32; node = At<uint8_t*>(node, 0x8C), ++n) {
        if (At<uint16_t>(node, 0x48) != 1) continue;
        uint8_t* a = node + 4;
        if (!(At<uint32_t>(a, 4) & 0x41000000) || g_smoothCount >= 32) continue;
        g_smooth[g_smoothCount].clip = a; g_smooth[g_smoothCount].w = At<float>(a, 0x58); ++g_smoothCount;
    }
    bool onFoot = why[0] == 'o';
    if (g_time - g_smoothStart > kStandSmoothTime) { if (onFoot) ++g_smoothFootTraces; else ++g_smoothTraces; }
    g_smoothOnFoot = onFoot;
    g_smoothStart = g_time;
    Log(why, g_smoothCount, " clips");
}
// Every frame: the player's body clips (used by the cover stand-up smoothing).
static void UpdateStandSmooth(void* ped) {
    g_bodyCount = 0;
    if (!ped) return;
    if (void* b = At<void*>(ped, 0x78)) {
        int k = 0;
        for (uint8_t* node = At<uint8_t*>(b, 0x1A28); node && k < 32; node = At<uint8_t*>(node, 0x8C), ++k)
            if (At<uint16_t>(node, 0x48) == 1 && (At<uint32_t>(node + 4, 4) & 0x41000000) && g_bodyCount < 32)
                g_bodyClips[g_bodyCount++] = node + 4;
    }
    g_bodyFrame = g_frame;
    // (On-foot stand-up is left vanilla; smoothing only runs for cover stand-ups.)
}
static void SmoothClipWeight(void* clip) {
    if (g_time - g_smoothStart > kStandSmoothTime || g_frame - g_bodyFrame > 1) return;
    bool mine = false;
    for (int i = 0; i < g_bodyCount; ++i) if (g_bodyClips[i] == clip) { mine = true; break; }
    if (!mine) return;
    SmoothClip* sc = nullptr;
    for (int i = 0; i < g_smoothCount; ++i) if (g_smooth[i].clip == clip) { sc = &g_smooth[i]; break; }
    if (!sc) {                                 // a clip that came in after standing up: fade it in
        if (g_smoothCount >= 32) return;
        sc = &g_smooth[g_smoothCount++]; sc->clip = clip; sc->w = 0.0f;
    }
    float target = At<float>(clip, 0x58);
    // Ease-out toward the game's weight (fast start, soft landing), capped so nothing
    // jumps more than kStandSmoothRate per second.
    float dt = g_timeStep ? *g_timeStep : 0.016f;
    float k = dt * kStandSmoothEase; if (k > 1.0f) k = 1.0f;
    float step = (target - sc->w) * k;
    float maxStep = kStandSmoothRate * dt;
    if (step > maxStep) step = maxStep; else if (step < -maxStep) step = -maxStep;
    float w = sc->w + step;
    if ((target - w) * (target - w) < 0.0001f) w = target;
    sc->w = w;
    At<float>(clip, 0x58) = w;
}
static float __fastcall Hook_ClipStep(void* clip, void*) {
    float r = Orig_ClipStep(clip);
    SmoothClipWeight(clip);
    if (g_fastCount && g_frame - g_fastFrame <= 1)
        for (int i = 0; i < g_fastCount; ++i) if (g_fastClips[i] == clip) {
            return r * kCoverAnimSpeed;
        }
    return r;
}
static void CoverAnimSpeed(void* task, void* ped) {
    g_fastCount = 0;

    kCoverAnimSpeed = g_cfg.coverAnimSpeed * 0.01f;
    if (g_cfg.coverAnimSpeed == 100) return;
    void* bl = At<void*>(ped, 0x78);
    if (!bl || !Orig_ClipStep) return;
    int st = At<int>(task, 0x38);
    uint32_t* cp = At<uint32_t*>(ped, 0xD68);
    bool standing = cp && (*cp & kHeightMask) == kTall && !g_crouched;
    bool want = standing && (st == 5 || st == 8 || st == 10 || st == 16 || st == 17);
    if (!want) return;
    int n = 0;
    for (uint8_t* node = At<uint8_t*>(bl, 0x1A28); node && n < 32; node = At<uint8_t*>(node, 0x8C), ++n) {
        if (At<uint16_t>(node, 0x48) != 1) continue;
        uint8_t* a = node + 4;
        if (*(uintptr_t*)a != g_animPlayerVt) continue;
        if (!At<void*>(a, 0x40) || At<int>(a, 0xC) < 0) continue;
        // Body clips: 0x40000000 (stand/idle body) and 0x01000000 (the cover move clips
        // you actually see while moving: groups 67/28/71/40, anim 52, synced together).
        // Popping out / back in (5 peek, 8 slide out, 10 slide back): the slide is played by
        // the gun/aim clips too, so speed up everything on the player then. Shooting (9)
        // is never in this list, so it plays at normal speed.
        bool popping = st == 5 || st == 8 || st == 10;
        if (!popping && !(At<uint32_t>(a, 4) & 0x41000000)) continue;
        if (g_fastCount < 32) g_fastClips[g_fastCount++] = a;
    }
    g_fastFrame = g_frame;
    (void)0;
}
static void CrouchUpdate(void* task, void* ped) {
    if (!IsPlayer(ped)) return;
    CoverAnimSpeed(task, ped);
    uint32_t* cp = At<uint32_t*>(ped, 0xD68);
    int st = At<int>(task, 0x38);
    if (cp && cp != g_lastLoggedCp) { Log("cover point height ", (int)((*cp >> 3) & 3), cp == (uint32_t*)((uint8_t*)ped + 0xE70) ? " (own)" : " (world)"); g_lastLoggedCp = cp; }
    if (g_fakedCp && cp != g_fakedCp) RestoreCoverPoint();       // moved to another cover point
    bool tallNow = cp && (*cp & kHeightMask) == kTall;            // the game's own value
    {   // vanilla: a low wall that steps up to tall while moving along it
        static bool wasLow = false;
        bool lowNow = cp && (*cp & kHeightMask) != kTall;
        if (wasLow && tallNow && !g_crouched && (st == 16 || st == 17) && g_time - g_smoothStart > 0.2f) StartStandSmooth(ped, "wall stepped up, smoothing: ");
        wasLow = lowNow;
    }
    if (tallNow) { uint8_t* m = At<uint8_t*>(ped, 0x20); if (m) { g_tallX = At<float>(m, 0x30); g_tallY = At<float>(m, 0x34); } }

    // Idle, passive aim, or moving along the wall (16/17): changing the cover height while
    // moving plays the game's own tall <-> low transition, like a wall that steps down.
    if (g_duckWanted && (st == 3 || st == 18 || st == 16 || st == 17)) {
        g_duckWanted = false;
        bool changed = false;
        if (!g_crouched) {
            if (tallNow) { g_crouched = true; changed = true; Log("crouch on"); }
            else Log("crouch ignored: not tall cover");
        } else {
            g_crouched = false; changed = true; Log("crouch off");
            StartStandSmooth(ped, "stand-up smoothing: ");
            // Standing up in place: we only ever lowered tall cover, so put tall back
            // (RestoreCoverPoint's distance check failed right after entering crouched).
            uint32_t* f = g_fakedCp;
            if (f && g_fakedPed == ped && (*f & kHeightMask) == 0) *f |= kTall;
            if (cp && (*cp & kHeightMask) != kTall) {
                // The point we're on now still says low (e.g. took cover crouched in the
                // middle and the game measured from crouch height): measure again standing.
                alignas(16) uint8_t tmp[256] = {};
                Orig_UpdateCover(task, ped, tmp);
                Log("crouch off: re-measured cover, height ", (int)((*cp >> 3) & 3));
                if ((*cp & kHeightMask) != kTall && (f || g_enteredCrouched)) { *cp |= kTall; Log("crouch off: marked tall"); }
            }
            g_fakedCp = nullptr; g_fakedPed = nullptr;
        }
        if (changed && g_crouched && cp) { LowerCp(cp, ped); }
        if (changed) {
            if (st == 3 || st == 18) Game_CoverRefreshIdle(task, ped);   // moving: the move code blends it
            else Log(g_crouched ? "crouch while moving" : "stand while moving");
            g_stanceSet = true; g_stanceCrouched = g_crouched;
        }
        if (g_standReaim) { g_standReaim = false; Log("stand while aiming: standing, popping out"); }
        tallNow = false;
    } else if (g_duckWanted && (st == 8 || st == 9) && !g_standReaim) {
        if (g_rawAimHeld && g_crouched && kStandUpViaCover) {
            // Crouched -> standing while popped out: duck back in, stand, pop out again.
            // (Standing up in place can't be made smooth: see notes in the reply.)
            g_standReaim = true; g_standStart = g_time; Log("stand while aiming: back in, stand, out");
        } else {
            g_duckWanted = false;
            if (g_rawAimHeld && !g_swapWanted && (tallNow || g_crouched)) { g_swapWanted = true; g_swapStart = g_time; Log("stance swap requested"); }
        }
    } else if (g_duckWanted && !g_standReaim && st != 3 && st != 16 && st != 17) {
        g_duckWanted = false;                                    // only toggle from idle
    }
    // Stay crouched when the game re-marks the point tall (sliding along the wall).
    if (g_crouched && tallNow) { LowerCp(cp, ped); }
    // Came in crouched but this is real low cover: nothing to fake, plain vanilla.
    if (g_crouched && !g_fakedCp && cp && !tallNow && st == 3) { g_crouched = false; Log("real low cover: crouch flag cleared"); }
    g_atCorner = cp && Game_CoverCorner(task, ped);
    g_useTask = task;
    // NPCs react to the gun: popped out and aiming, or "passive aim" (the game raises the
    // gun by itself when you look away from the wall). 0x40000 in +0x26C = gun raised.
    bool raised = (At<uint32_t>(ped, 0x26C) & 0x40000) != 0;
    static bool lastRaised = false;
    if (raised != lastRaised) { Log(raised ? "gun raised in cover, state " : "gun lowered in cover, state ", st); lastRaised = raised; }
    if ((g_realAim && (st == 8 || st == 9)) || raised) UpdateAimReactions(ped);
}
// ---- Crouched accuracy -------------------------------------------------------------
//  0x9E8110 = "ped is ducking". The weapon spread (x0.5 when ducking) and the reticle use it;
//  the reticle code also refuses the ducked size while a cover task is running.
static bool (__thiscall* Orig_FindTask)(void* tm, int type, int b) = nullptr;
static bool g_reticleCoverCrouch = false;
static bool __fastcall Hook_SpreadDucking(void* ped, void*) {
    return Orig_IsDucking(ped) || (g_cfg.crouchAccuracy && CoverCrouched(ped));
}
static bool __fastcall Hook_ReticleDucking(void* ped, void*) {
    g_reticleCoverCrouch = g_cfg.crouchAccuracy && CoverCrouched(ped);
    return g_reticleCoverCrouch || Orig_IsDucking(ped);
}
static bool __fastcall Hook_ReticleHasCoverTask(void* tm, void*, int type, int b) {
    if (g_reticleCoverCrouch) return false;                     // allow the ducked reticle
    return Orig_FindTask(tm, type, b);
}

// The game's "is this cover tall" check (0xD14C70), replaced so every animation choice
// (idle, moving along the wall, turning corners) sees low cover while crouched, even on
// a frame where the game has just re-marked the point as tall.
static bool __stdcall Hook_TooHigh(void* ped) {
    if (!ped) return false;
    uint32_t* cp = At<uint32_t*>(ped, 0xD68);
    if (!cp || (*cp & kHeightMask) != kTall) return false;
    if (g_crouched && InCover() && IsPlayer(ped)) {
        uint8_t* m = At<uint8_t*>(ped, 0x20);
        if (m) { g_tallX = At<float>(m, 0x30); g_tallY = At<float>(m, 0x34); }
        LowerCp(cp, ped);
        return false;
    }
    return true;
}
// State 5 (peek) only allows the pop-out after 0.3 s in the state, so re-aiming right after
// returning to cover showed a short peek first. A real aim skips that wait.
static void TightenEntry(void* task, void* ped);
static void SkipPeekWait(void* task, void* ped) {
    if (g_cfg.coverAim && g_realAim && InCover() && At<int>(task, 0x38) == 5 && IsPlayer(ped) && At<float>(task, 0x48) < 0.31f)
        At<float>(task, 0x48) = 0.31f;
    TightenEntry(task, ped);
}
// ---- Faster cover entry --------------------------------------------------------------
//  Taking cover (state 2) only hands over to idle cover (state 3) once the player has
//  reached the cover spot AND the cover timer (+0x40) passes 0.4 s; the corner/settle
//  state 15 waits 0.2 s. Those are the "dead" frames. When the player wants to do
//  something (move, aim, fire), let that wait end after kEntryMinTime.
static const float kEntryMinTime = 0.12f;
static float g_entryStart = -100.0f, g_lastT40 = 0.0f;
static int   g_entryLogN = 0;
// The take-cover states also wait until the player is within 2.5 cm of the cover spot
// (0xD18B60). The slide there slows down as it gets close, so the last few centimetres
// are most of the dead time. When the player wants to act, call it "arrived" within kArriveRadius.
static const float kArriveRadius = 0.12f;
static bool (__thiscall* Orig_CoverArrived)(void* task, void* ped, float* target, void* c) = nullptr;
static bool EntryWanted() {
    float sx = Axis(INPUT_MOVE_LEFT, INPUT_MOVE_RIGHT), sy = Axis(INPUT_MOVE_UP, INPUT_MOVE_DOWN);
    return sx * sx + sy * sy > 0.25f || g_realAim || Game_IsFirePressed();
}
static bool __fastcall Hook_CoverArrived(void* task, void*, void* ped, float* target, void* c) {
    bool r = Orig_CoverArrived(task, ped, target, c);   // also steers the player toward the spot
    if (r || !g_cfg.fastCoverEntry || !IsPlayer(ped) || !target) return r;
    int st = At<int>(task, 0x38);
    if (st != 2 && st != 14 && st != 15) return r;
    uint8_t* m = At<uint8_t*>(ped, 0x20);
    if (!m || !EntryWanted()) return r;
    float dx = At<float>(m, 0x30) - target[0], dy = At<float>(m, 0x34) - target[1];
    if (dx * dx + dy * dy < kArriveRadius * kArriveRadius) {
        static float lastLog = -100.0f;
        if (g_time - lastLog > 0.5f) { lastLog = g_time; Log("entry: close enough to the cover spot (cm) ", (int)(Sqrt(dx * dx + dy * dy) * 100)); }
        return true;
    }
    return r;
}
static void TightenEntry(void* task, void* ped) {
    if (!IsPlayer(ped) || !g_cfg.fastCoverEntry) return;
    int st = At<int>(task, 0x38);
    float t40 = At<float>(task, 0x40), t48 = At<float>(task, 0x48);
    if (st == 2 && g_lastCoverState != 2) { g_entryStart = g_time; g_entryLogN = 0; }
    bool entering = (st == 2 || st == 15) && g_time - g_entryStart < 3.0f;
    // diagnostic: how the entry goes
    if (entering && g_time - g_entryStart < 1.5f && g_entryLogN < 20) {
        static float lastLog = -100.0f;
        if (g_time - lastLog > 0.05f) {
            lastLog = g_time; ++g_entryLogN;
            float sx = Axis(INPUT_MOVE_LEFT, INPUT_MOVE_RIGHT), sy = Axis(INPUT_MOVE_UP, INPUT_MOVE_DOWN);
            Log("entry: state ", st, " at ms ", (int)(g_time * 1000));
            Log("   timer40 ms ", (int)(t40 * 1000), " inState ms ", (int)(t48 * 1000));
            Log("   stick x100 ", (int)((sx * sx + sy * sy) * 100), g_realAim ? " aim" : "");
        }
    }
    float dt = g_timeStep ? *g_timeStep : 0.0f;
    bool timerLike = dt > 0.0f && t40 - g_lastT40 > dt * 0.5f && t40 - g_lastT40 < dt * 1.5f;
    g_lastT40 = t40;
    if (!entering || !timerLike) return;
    float sx = Axis(INPUT_MOVE_LEFT, INPUT_MOVE_RIGHT), sy = Axis(INPUT_MOVE_UP, INPUT_MOVE_DOWN);
    bool wants = sx * sx + sy * sy > 0.25f || g_realAim || Game_IsFirePressed();
    float need = st == 2 ? 0.41f : 0.21f;
    if (wants && t48 >= kEntryMinTime && t40 < need) {
        At<float>(task, 0x40) = need; g_lastT40 = need;
        Log(st == 2 ? "entry: cut the take-cover wait" : "entry: cut the settle wait");
    }
}
// Keep the player's cover point low while crouched, right after the game recomputes it.
static void EnforceCrouch(void* ped) {
    if (!g_crouched || !IsPlayer(ped)) return;
    uint32_t* cp = At<uint32_t*>(ped, 0xD68);
    if (!cp || (*cp & kHeightMask) != kTall) return;
    uint8_t* m = At<uint8_t*>(ped, 0x20);
    if (m) { g_tallX = At<float>(m, 0x30); g_tallY = At<float>(m, 0x34); }
    LowerCp(cp, ped);
}
// 0xCFECC0: sets up the cover point on entering states 2/3/15 (re-marks it tall).
static void (__thiscall* Orig_SetupCover)(void* task, void* ped, void* a, void* b) = nullptr;
static void __fastcall Hook_SetupCover(void* task, void*, void* ped, void* a, void* b) {
    Orig_SetupCover(task, ped, a, b);
    EnforceCrouch(ped);
}
// 0xD0FFF0 / 0xD0FC00: compute the player's cover point when taking / moving in cover.
static void (__cdecl* Orig_ComputeCover)(void* ped, void* a, void* b, uint32_t c, uint32_t d) = nullptr;
static void __cdecl Hook_ComputeCover(void* ped, void* a, void* b, uint32_t c, uint32_t d) {
    Orig_ComputeCover(ped, a, b, c, d); EnforceCrouch(ped);
}
static bool __fastcall Hook_UpdateCover(void* task, void*, void* ped, void* out) {
    bool r = Orig_UpdateCover(task, ped, out); EnforceCrouch(ped); return r;
}
// 0xCFF460: picks over-the-top vs. around-the-corner when popping out (bit 0x20 of +0x35)
// from the aim direction. Crouched behind a tall wall there is no "over the top".
static void (__thiscall* Orig_PickOverTop)(void* task, void* ped) = nullptr;
static void __fastcall Hook_PickOverTop(void* task, void*, void* ped) {
    Orig_PickOverTop(task, ped);
    if (g_crouched && g_fakedCp && IsPlayer(ped) && (At<uint8_t>(task, 0x35) & 0x30)) {
        At<uint8_t>(task, 0x35) &= ~0x30;
        Log("crouched tall cover: forced corner pop-out");
    }
}
// Leaving cover (PlayerInCover slot 18): the game keeps you crouched only if you were
// ducking when you took cover (+0x19) and still are. Use the stance you're in now instead.
static bool g_exiting = false;
static bool __fastcall Hook_ExitIsDucking(void* ped, void*) {
    if (g_exiting && g_stanceSet && IsPlayer(ped)) return g_stanceCrouched;
    return Orig_IsDucking(ped);
}
static void* __fastcall Hook_CoverNext(void* t, void*, void* ped) {
    MarkCover(ped);
    bool mine = g_stanceSet && IsPlayer(ped);
    if (mine) { At<uint8_t>(t, 0x19) = g_stanceCrouched ? 1 : 0; Log(g_stanceCrouched ? "exit stance: crouched" : "exit stance: standing"); }
    g_exiting = true;
    void* r = Orig_CoverNext(t, ped);
    g_exiting = false;
    return r;
}
static void* __fastcall Hook_CoverFirst(void* t, void*, void* ped) {
    MarkCover(ped);
    // Crouched on foot when taking cover: stay crouched (low-cover entry and idle).
    if (IsPlayer(ped)) g_enteredCrouched = false;
    if (g_cfg.coverCrouch && IsPlayer(ped) && Orig_IsDucking(ped) && !g_crouched) { g_crouched = true; g_enteredCrouched = true; Log("entered cover crouched"); }
    EnforceCrouch(ped);
    return Orig_CoverFirst(t, ped);
}
static void* __fastcall Hook_CoverControl(void* t, void*, void* ped) { MarkCover(ped); return Orig_CoverControl(t, ped); }
static void* __fastcall Hook_UseNext(void* t, void*, void* ped)      { SkipPeekWait(t, ped); EnforceCrouch(ped); void* r = Orig_UseNext(t, ped); CrouchUpdate(t, ped); SkipPeekWait(t, ped);    LogCoverState(t, ped); return r; }
static void* __fastcall Hook_UseFirst(void* t, void*, void* ped)     { SkipPeekWait(t, ped); EnforceCrouch(ped); void* r = Orig_UseFirst(t, ped); CrouchUpdate(t, ped); SkipPeekWait(t, ped);   LogCoverState(t, ped); return r; }
static void* __fastcall Hook_UseControl(void* t, void*, void* ped)   { SkipPeekWait(t, ped); EnforceCrouch(ped); void* r = Orig_UseControl(t, ped); CrouchUpdate(t, ped); SkipPeekWait(t, ped); LogCoverState(t, ped); return r; }

// ---- 2. Rewrite the out-of-cover gun task's plan --------------------------------------
//  More plan bytes: +0x6F after a reload (1 aim, 3 fire, 4 end),
//                   +0x70 after a burst that ran the clip dry (1 aim, 2 reload, 4 end)
static const uint32_t GUN_AFTER_RELOAD = 0x6F, GUN_AFTER_EMPTY = 0x70;
static const uint8_t  ACT_RELOAD = 2;
static const int TASK_AIM_GUN = 0x411, TASK_FIRE_GUN = 0x412;   // 0x413 = reload

static float g_lastShotTime = -100.0f;

static void* Weapon(void* ped) { return Game_GetCurrentWeapon((uint8_t*)ped + 0x2B0); }
static bool ClipEmpty(void* ped)  { void* w = Weapon(ped); return w && At<int>(w, 0x5C) <= 0; }
static bool CanReload(void* ped)  { void* w = Weapon(ped); return w && At<int>(w, 0x60) - At<int>(w, 0x5C) > 0; }
static float TimeBetweenShots(void* ped) {   // CWeaponInfo +0x8C, milliseconds (weapon data)
    void* w = Weapon(ped);
    if (!w || !Game_GetWeaponInfo) return 0.0f;
    void* info = Game_GetWeaponInfo(At<int>(w, 0x18));
    if (!info) return 0.0f;
    int ms = At<int>(info, 0x8C);
    if (ms < 0) ms = 0;
    if (ms > 2000) ms = 2000;          // safety clamp
    return ms * 0.001f;
}

static void SetGunPlan(void* task) {
    if (g_realAim) {
        bool fire = Game_IsFirePressed();          // real trigger (we're outside the fake-fire scope)
        At<uint8_t>(task, GUN_FIRST)        = fire ? ACT_FIRE : ACT_AIM;
        At<uint8_t>(task, GUN_AFTER_AIM)    = fire ? ACT_FIRE : ACT_AIM;
        At<uint8_t>(task, GUN_AFTER_FIRE)   = ACT_AIM;
        At<uint8_t>(task, GUN_AFTER_RELOAD) = ACT_AIM;
        At<uint8_t>(task, GUN_AFTER_EMPTY)  = g_cfg.coverReload ? ACT_END : ACT_RELOAD;   // clip ran dry: back into cover (or reload out here)
    } else {
        At<uint8_t>(task, GUN_AFTER_AIM)    = ACT_END;   // let go of aim: finish -> back into cover
        At<uint8_t>(task, GUN_AFTER_FIRE)   = ACT_END;
        At<uint8_t>(task, GUN_AFTER_RELOAD) = ACT_END;
        At<uint8_t>(task, GUN_AFTER_EMPTY)  = ACT_END;
    }
}
// Our own pop-out, currently sliding out / out of cover.
static bool PlayerCoverGun(void* ped) {
    return g_cfg.coverAim && g_useTask && IsPlayer(ped) && InCover() && g_popOutByAim && (g_coverState == 8 || g_coverState == 9);
}
static void NoteSubtask(void* r) {
    int t = TaskType(r);
    if (t == TASK_FIRE_GUN && t != g_lastGunSub) g_lastShotTime = g_time;
    if (t != g_lastGunSub) { Log("cover gun subtask ", g_lastGunSub, " -> ", t); g_lastGunSub = t; }
}

static void* __fastcall Hook_GunNext(void* t, void*, void* ped) {
    if (g_swapOldAim) {
        if (At<void*>(t, 0x8) == g_swapOldAim) { DetachAimAnims(g_swapOldAim); LogT("swap: old aim anims fading out"); }
        g_swapOldAim = nullptr;
    }
    if (!PlayerCoverGun(ped)) return Orig_GunNext(t, ped);
    // Clip empty and not aiming any more: vanilla (back into cover, reload there).
    if (ClipEmpty(ped) && !g_realAim) { void* r = Orig_GunNext(t, ped); NoteSubtask(r); return r; }
    SetGunPlan(t);
    void* r = Orig_GunNext(t, ped);
    if (!r && g_realAim) {
        if (ClipEmpty(ped)) {
            // Forced cover reload: end the pop-out; the cover task slides back in and
            // reloads in cover (state 7).
            if (CanReload(ped)) {
                if (g_cfg.coverReload) StartCoverReload("cover reload: clip empty (gun task), back into cover");
                else { r = Game_GunCreateReload(t, ped, false); Log("reloading while popped out"); }
            }
        } else {
            // Right after a stance change: start the new aim already raised (gun-task flag
            // 0x8, what the game uses after a reload) instead of replaying the raise.
            bool afterSwap = g_time - g_lastSwapTime < 0.5f;
            uint32_t saved = At<uint32_t>(t, 0x74);
            if (afterSwap) At<uint32_t>(t, 0x74) = saved | 0x8;
            r = Game_GunCreateAim(t, ped, true);
            At<uint32_t>(t, 0x74) = saved;
            Log(afterSwap ? "kept aiming (already raised, after stance change)" : "kept aiming instead of ending");
        }
    }
    if (r && TaskType(r) == TASK_AIM_GUN && (g_swapBlend > 0.0f || g_time - g_lastSwapTime < 1.0f)) {
        if (g_swapBlend > 0.0f) { At<float>(r, 0x5C) = g_swapBlend; g_swapBlend = 0.0f; LogT("swap: new aim, slow blend"); }
        else LogT("swap: another aim created");
    }
    NoteSubtask(r); return r;
}
// The gun task's own "start by firing" step: aim instead while only aiming.
static void* __fastcall Hook_GunCreateFire(void* t, void*, void* ped, bool a, bool b) {
    // Only aiming (or it was a quick tap of aim that's already released):
    // never start by firing unless the trigger is really pulled.
    if (PlayerCoverGun(ped) && !ClipEmpty(ped) && !Game_IsFirePressed())
        return Game_GunCreateAim(t, ped, true);
    return Orig_GunCreateFire(t, ped, a, b);
}
static void* __fastcall Hook_GunFirst(void* t, void*, void* ped) {
    if (IsPlayer(ped) && InExitWindow() && !InCover()) LogX("gun task started on foot");
    if (!PlayerCoverGun(ped) || ClipEmpty(ped)) return Orig_GunFirst(t, ped);
    SetGunPlan(t); void* r = Orig_GunFirst(t, ped); NoteSubtask(r); return r;
}

// Hides the trigger from one gun-task decision (used to respect the weapon's fire rate).
struct ScopedNoFire {
    bool active = false; uint8_t s1 = 0, s2 = 0;
    void Engage() {
        uint8_t* a = Input(INPUT_ATTACK); uint8_t* b = Input(INPUT_ATTACK2);
        s1 = a[6]; s2 = b[6];
        SetInputValue(INPUT_ATTACK, 0); SetInputValue(INPUT_ATTACK2, 0);
        active = true;
    }
    ~ScopedNoFire() { if (active) { Input(INPUT_ATTACK)[6] = s1; Input(INPUT_ATTACK2)[6] = s2; } }
};

static bool (__cdecl* Game_CoverAnims)(void* ped, int* group, int* anim, int state, bool high, bool tooHigh,
                                       bool side, bool corner, bool overTop) = nullptr;   // 0xD11EB0
// Flip the stance and give the gun task the matching aim animation. True if it changed.
static bool SwapStance(void* gunTask, void* ped) {
    void* ct = g_useTask;
    uint32_t* cp = At<uint32_t*>(ped, 0xD68);
    if (!ct || !cp) return false;
    if (!g_crouched) {
        if ((*cp & kHeightMask) != kTall) return false;
        uint8_t* m = At<uint8_t*>(ped, 0x20);
        if (m) { g_tallX = At<float>(m, 0x30); g_tallY = At<float>(m, 0x34); }
        g_crouched = true; LowerCp(cp, ped);
    } else {
        g_crouched = false; RestoreCoverPoint();
        if ((*cp & kHeightMask) != kTall) *cp |= kTall;          // we're at the tall spot we lowered
    }
    uint8_t f = At<uint8_t>(ct, 0x35);
    int group = -1, anim = -1;
    bool tall = (*cp & kHeightMask) == kTall;
    if (Game_CoverAnims(ped, &group, &anim, 9, (*cp & kHeightMask) == 0x10, tall,
                        (f >> 1) & 1, Game_CoverCorner(ct, ped), (f & 0x30) != 0)) {
        At<int16_t>(gunTask, 0x4C) = (int16_t)group; At<int16_t>(gunTask, 0x4E) = (int16_t)anim;
        At<int16_t>(gunTask, 0x50) = (int16_t)group; At<int16_t>(gunTask, 0x52) = (int16_t)anim;
    }
    g_stanceSet = true; g_stanceCrouched = g_crouched;
    Log(g_crouched ? "stance swap: crouched, anim group " : "stance swap: standing, anim group ", group);
    g_lastSwapTime = g_time;
    return true;
}

// Swap the running aim's animation for the other stance's without restarting the aim:
// start the new clip on the ped, give it the aim's settings, phase and callback, point the
// aim at it, and fade the old clip out at the same rate the new one fades in.
static void  (__thiscall* Game_AnimSetPhase)(void* assoc, float phase) = nullptr;                                  // 0xC62CA0
static void  (__thiscall* Game_AnimLoadEvents)(void* assoc, void* data) = nullptr;                                  // 0xC626B0
__attribute__((unused)) static bool SwapAimAnimInPlace(void* aim, void* gunTask, void* ped) {
    void* old = At<void*>(aim, 0x64);
    void* blender = At<void*>(ped, 0x78);
    int group = At<int16_t>(gunTask, 0x4C), anim = At<int16_t>(gunTask, 0x4E);
    if (!old || !blender || group < 0 || anim < 0) {
        Log("in-place: missing clip/blender, old=", old ? 1 : 0, " blender=", blender ? 1 : 0); return false; }
    if (At<int>(aim, 0x4C) == group && At<int>(aim, 0x50) == anim) {
        Log("in-place: aim already uses group ", group, " anim ", anim); return false; }
    void* nw = Game_StartAnim(blender, group, anim, kSwapBlend, -1);
    if (!nw || nw == old) { Log("in-place: start clip failed, got ", nw ? 1 : 0, " same=", nw == old ? 1 : 0); return false; }
    // Same setup the aim gives its clip: flags, event phases, current phase.
    uint32_t f = At<uint32_t>(nw, 4) & ~0xC0u;
    f |= 0x10 | 0x8000 | 0x4000 | (At<uint32_t>(old, 4) & 0x200001u);
    At<uint32_t>(nw, 4) = f;
    Game_AnimLoadEvents(nw, At<void*>(nw, 0x40));
    At<float>(aim, 0x6C) = At<float>(nw, 0x64); At<float>(aim, 0x70) = At<float>(nw, 0x68);
    At<float>(aim, 0x74) = At<float>(nw, 0x6C); At<float>(aim, 0x78) = At<float>(nw, 0x70);
    Game_AnimSetPhase(nw, At<float>(old, 0x4C));
    // Move the "clip finished" callback to the new clip.
    At<uint32_t>(nw, 0x1C) = At<uint32_t>(old, 0x1C);
    At<uint32_t>(nw, 0x20) = At<uint32_t>(old, 0x20);
    At<uint32_t>(nw, 0x24) = At<uint32_t>(old, 0x24);
    Game_AnimRemoveCallback(old, aim);
    Game_AnimSetBlend(old, -kSwapBlend);
    Game_AnimSetBlend(nw, kSwapBlend);
    At<void*>(aim, 0x64) = nw;
    At<int>(aim, 0x4C) = group; At<int>(aim, 0x50) = anim;
    At<int>(aim, 0x54) = group; At<int>(aim, 0x58) = anim;
    return true;
}

// ---- NPCs react to being aimed at from cover --------------------------------------------
//  The on-foot "gun aimed at" event is only sent while an on-foot aiming flag is set, which
//  cover doesn't set. Same logic as the drive-by mod: aim line -> target -> event.
struct alignas(16) Vec4 { float x, y, z, w; };
static bool  (__thiscall* Game_GetAimPoints)(void*, void*, void*, Vec4*, Vec4*) = nullptr;
static void* (__cdecl*    Game_FindAimTarget)(void*, Vec4*, Vec4*, bool, bool) = nullptr;
static bool  (__thiscall* Game_HasActiveEvent)(void*, int*, bool*) = nullptr;
static bool  (__thiscall* Game_CanSeePosition)(void*, Vec4*, float, float, float, float) = nullptr;
static void* (__thiscall* Game_GunAimedAt_ctor)(void*, void*) = nullptr;
static void  (__thiscall* Game_GunAimedAt_dtor)(void*) = nullptr;
static void  (__thiscall* Game_AddEvent)(void*, void*, bool, bool) = nullptr;
static void* (__thiscall* Game_GetPedGroup)(void* ped) = nullptr;
static bool  (__cdecl*    Game_IsInGroupOf)(void* ped, void* player) = nullptr;
static void* g_aimTarget = nullptr;
static float g_aimTimer  = 0.0f;
static uint32_t g_reactFrame = 0;
static int EntityType(void* e) { return (At<uint32_t>(e, 0x28) >> 6) & 0xF; }
static void UpdateAimReactions(void* ped) {
    if (!Game_AddEvent || !g_cfg.npcReactions || g_reactFrame == g_frame) return;
    g_reactFrame = g_frame;
    void* weaponObj = At<void*>(ped, 0x2C4);
    void* weapon    = Game_GetCurrentWeapon((uint8_t*)ped + 0x2B0);
    void* pIntel    = At<void*>(ped, 0x224);
    uint8_t* pm     = At<uint8_t*>(ped, 0x20);
    if (!pIntel || !pm) return;
    void* target = At<void*>(ped, 0x398);
    if (!target || (EntityType(target) != 3 && EntityType(target) != 6)) {
        Vec4 st = {}, en = {};
        if (!weapon || !weaponObj || !Game_GetAimPoints(weapon, ped, At<void*>(weaponObj, 0x20), &st, &en)) { g_aimTarget = nullptr; g_aimTimer = 0.0f; return; }
        target = Game_FindAimTarget(ped, &st, &en, false, true);
        if (!target) { g_aimTarget = nullptr; g_aimTimer = 0.0f; return; }
    }
    // The aim line alternates between the ped and things around it (cars, objects) from
    // frame to frame; only a ped counts, and other hits don't reset the timer.
    if ((At<uint32_t>(target, 0x28) & 0x3C0) != 0xC0) return;
    if (target != g_aimTarget) { g_aimTarget = target; g_aimTimer = 0.0f; Log("aim: new target, type ", EntityType(target)); return; }
    int evType = 0; bool evFlag = false;
    static int lastWhy = 0;
    auto why = [&](int w, const char* m) { if (w != lastWhy) { lastWhy = w; Log(m); } };
    // (The on-foot code skips while the player has an active event; in cover there
    //  always is one, so that check is left out here.)
    (void)evType; (void)evFlag; (void)pIntel;
    if ((At<uint32_t>(target, 0x28) & 0x3C0) != 0xC0) { why(2, "aim: target is not a ped"); return; }
    if (At<uint8_t>(target, 0x268) & 1) return;
    void* tIntel = At<void*>(target, 0x224);
    if (!tIntel) return;
    Vec4* ppos = (Vec4*)(pm + 0x30);
    uint8_t* tm = At<uint8_t*>(target, 0x20);
    Vec4* tpos = tm ? (Vec4*)(tm + 0x30) : (Vec4*)((uint8_t*)target + 0x10);
    float dx = tpos->x - ppos->x, dy = tpos->y - ppos->y, dz = tpos->z - ppos->z;
    float t = (Sqrt(dx * dx + dy * dy + dz * dz) - 5.0f) * 0.06666667f;
    float delay = (t < 0.0f ? 0.0f : t) * 1.8f + 0.2f;
    if (void* grp = Game_GetPedGroup(target))
        if (Game_IsInGroupOf(target, ped) && At<void*>(grp, 0x8 + 0x3C) == ped) return;
    if (!Game_CanSeePosition(tIntel, ppos, 12.0f, -1.0f, 20.0f, 0.0f)) { why(3, "aim: target can't see the player"); return; }
    why(4, "aim: target sees the player, waiting for the reaction delay");
    g_aimTimer += *g_timeStep;
    if (g_aimTimer <= delay) return;
    alignas(16) uint8_t ev[0x40];
    Game_GunAimedAt_ctor(ev, ped);
    Game_AddEvent((uint8_t*)tIntel + 0x84, ev, false, true);
    Game_GunAimedAt_dtor(ev);
    g_aimTimer = 0.0f;
    static void* lastSent = nullptr;
    if (target != lastSent) { lastSent = target; Log("aim: 'gun aimed at' sent to target"); }
}

// ===================================================================================
//  Drive-by (formerly GTAIVDriveByPreAim)
//    Controller: hold LB to aim, press RB to fire.  Mouse: hold right to aim, left to fire.
//    1. While aim is held in a vehicle the game simply sees its own drive-by button held,
//       so the drive-by starts and stays up exactly like vanilla.
//    2. The drive-by task's single "shoot" call (0xC60A9A -> 0xC5FBE0): its "firing" flag
//       (+0x49) is cleared unless fire is held. No fire = aim only.
//    3. RB is the trigger while LB is held, not the handbrake (A still is).
//    4. Look behind (R3) works while aiming, like GTA V (0xB09C10 answers "no").
//    5. Thrown weapons (slot 8): aim readies, fire cocks, releasing fire throws.
//    6. NPCs react to being aimed at from a vehicle.
//    Left vanilla: piloting helicopters/planes, driving vehicles with a mounted gun.
// ===================================================================================
static const int INPUT_VEH_ATTACK = 38, INPUT_VEH_ATTACK2 = 39, INPUT_VEH_HANDBRAKE = 44, INPUT_VEH_LOOK_BEHIND = 50;
static uint8_t* g_scriptModeFlag = nullptr;           // a script-controlled mode; drive-by stays vanilla while set
static int  (__cdecl* Game_IsMouseButtonPressed)(int) = nullptr;
static int  (__cdecl* Game_IsPadButtonPressed)(int, int) = nullptr;
static void (__thiscall* Orig_DriveByFire)(void* task, void* ped, int a, int b, int c) = nullptr;
static bool (__thiscall* Orig_DriveByProcess)(void* task, void* ped) = nullptr;
static bool (__cdecl* Orig_WantsDriveByCam)() = nullptr;
static Vec4* g_camFront = nullptr;                     // camera direction, fallback aim line
static bool g_dbAim = false, g_dbFire = false;         // drive-by aim / fire held this frame
static void* g_dbTarget = nullptr;
static float g_dbTimer = 0.0f;
static void DbResetReaction() { g_dbTarget = nullptr; g_dbTimer = 0.0f; }
static bool g_dbCooking = false;                       // thrown weapon cocked with fire
static bool g_driveByOk = false;                       // drive-by hooks installed
static bool DbPad(int b)   { return Game_IsPadButtonPressed && Game_IsPadButtonPressed(0, b) > 0x40; }
static bool DbMouse(int b) { return Game_IsMouseButtonPressed && Game_IsMouseButtonPressed(b) != 0; }
static bool DbPadCfg(int b)   { return b >= 0 && DbPad(b); }
static bool DbMouseCfg(int b) { return b > 0 && DbMouse(b); }
static bool DriveByAimHeld() { return g_dbAim || DbMouseCfg(g_cfg.dbAimMouse) || DbPadCfg(g_cfg.dbAimPad); }
static bool ShoulderButtonHeld() { return DbPadCfg(g_cfg.shoulderPad) || KeyHeld(g_cfg.shoulderKey); }

// Called right after the game reads input, before anything else this frame.
static void DriveByControls() {
    g_dbAim = g_dbFire = false;
    if (!g_cfg.driveBy || !g_driveByOk || !Game_IsPadButtonPressed || !Game_IsMouseButtonPressed) return;
    if (g_scriptModeFlag && *g_scriptModeFlag) { DbResetReaction(); g_dbCooking = false; return; }
    if (Game_GetPlayerControls() != g_controls) { DbResetReaction(); return; }
    void* ped = Game_FindPlayerPed(0);
    void* veh = (ped && (At<uint8_t>(ped, 0x26C) & 4)) ? At<void*>(ped, 0xB30) : nullptr;
    if (!veh) { DbResetReaction(); g_dbCooking = false; return; }
    int kind = At<int>(veh, 0x1304);                   // 0 car, 1 bike, 2 boat, 3 train, 4 heli, 5 plane
    bool isDriver = At<void*>(veh, 0xF50) == ped;
    bool aircraft = kind == 4 || kind == 5;
    bool automobileBased = kind == 0 || aircraft;      // only these have the +0x1490 field
    if (isDriver && (aircraft || (automobileBased && At<void*>(veh, 0x1490) != nullptr))) { DbResetReaction(); return; }

    bool padAim = DbPadCfg(g_cfg.dbAimPad);
    bool padFire = DbPadCfg(g_cfg.dbFirePad);
    g_dbAim  = padAim || DbMouseCfg(g_cfg.dbAimMouse);
    g_dbFire = padFire || DbMouseCfg(g_cfg.dbFireMouse);
    // Thrown weapon being cooked: from the moment fire was pressed, only fire matters.
    // Letting go of aim no longer throws; letting go of fire does.
    bool& cooking = g_dbCooking;
    if (cooking) {
        if (At<uint32_t>(ped, 0x2B0) != 8) cooking = false;
        else {
            SetInputValue(INPUT_VEH_ATTACK, g_dbFire ? 0xFF : 0);
            if (padFire && g_cfg.dbFirePad == 6) SetInputValue(INPUT_VEH_HANDBRAKE, 0);
            g_dbAim = true;                             // still "drive-by aiming" for the rest of the mod
            if (!g_dbFire) { cooking = false; Log("drive-by: thrown weapon released"); }
            DbResetReaction();
            return;
        }
    }
    if (!g_dbAim) { DbResetReaction(); return; }       // not aiming: pure vanilla
    if (padAim && g_cfg.dbFirePad == 6) SetInputValue(INPUT_VEH_HANDBRAKE, 0); // RB is the trigger while aiming

    if (g_cfg.driveByThrown && At<uint32_t>(ped, 0x2B0) == 8) {   // thrown weapon: aim readies, fire cocks
        SetInputValue(INPUT_VEH_ATTACK, g_dbFire ? 0xFF : 0);
        if (g_dbFire) { cooking = true; Log("drive-by: thrown weapon cocked"); }
        DbResetReaction();
        return;
    }
    SetInputValue(INPUT_VEH_ATTACK, 0xFF);             // keep the game's drive-by up
    if (kind == 4) SetInputValue(INPUT_VEH_ATTACK2, 0xFF);   // helicopters (passenger)
}
static void __fastcall Hook_DriveByFire(void* task, void*, void* ped, int a, int b, int c) {
    if (g_dbAim && !g_dbFire && ped == Game_FindPlayerPed(0)) At<uint8_t>(task, 0x49) = 0;   // aim only
    Orig_DriveByFire(task, ped, a, b, c);
}
static void DriveByAimReactions(void* ped) {
    if (!g_dbAim || !Game_AddEvent || !g_cfg.npcReactions) { DbResetReaction(); return; }
    void* weaponObj = At<void*>(ped, 0x2C4);
    void* weapon    = Game_GetCurrentWeapon((uint8_t*)ped + 0x2B0);
    void* pIntel    = At<void*>(ped, 0x224);
    uint8_t* pm     = At<uint8_t*>(ped, 0x20);
    if (!pIntel || !pm) return;
    void* target = At<void*>(ped, 0x398);              // lock-on target first
    if (!target || (EntityType(target) != 3 && EntityType(target) != 6)) {
        Vec4 st = {}, en = {};
        bool haveRay = weapon && weaponObj && Game_GetAimPoints(weapon, ped, At<void*>(weaponObj, 0x20), &st, &en);
        if (!haveRay && g_camFront) {                  // fallback: along the camera, 60 m
            Vec4* p = (Vec4*)(pm + 0x30);
            st.x = p->x; st.y = p->y; st.z = p->z + 0.6f;
            en.x = st.x + g_camFront->x * 60.0f; en.y = st.y + g_camFront->y * 60.0f; en.z = st.z + g_camFront->z * 60.0f;
            haveRay = true;
        }
        if (!haveRay) { DbResetReaction(); return; }
        target = Game_FindAimTarget(ped, &st, &en, false, true);
        if (!target) { DbResetReaction(); return; }
    }
    if (target != g_dbTarget) { g_dbTarget = target; g_dbTimer = 0.0f; return; }
    int evType = 0; bool evFlag = false;
    if (Game_HasActiveEvent(pIntel, &evType, &evFlag) && evType != 0x12) return;
    if ((At<uint32_t>(target, 0x28) & 0x3C0) != 0xC0) return;   // peds only
    if (At<uint8_t>(target, 0x268) & 1) return;                  // alive only
    void* tIntel = At<void*>(target, 0x224);
    if (!tIntel) return;
    Vec4* ppos = (Vec4*)(pm + 0x30);
    uint8_t* tm = At<uint8_t*>(target, 0x20);
    Vec4* tpos = tm ? (Vec4*)(tm + 0x30) : (Vec4*)((uint8_t*)target + 0x10);
    float dx = tpos->x - ppos->x, dy = tpos->y - ppos->y, dz = tpos->z - ppos->z;
    float t = (Sqrt(dx * dx + dy * dy + dz * dz) - 5.0f) * 0.06666667f;
    float delay = (t < 0.0f ? 0.0f : t) * 1.8f + 0.2f;           // game's own timing
    if (void* grp = Game_GetPedGroup(target))                    // never the player's own group
        if (Game_IsInGroupOf(target, ped) && At<void*>(grp, 0x8 + 0x3C) == ped) return;
    if (!Game_CanSeePosition(tIntel, ppos, 12.0f, -1.0f, 20.0f, 0.0f)) return;
    g_dbTimer += *g_timeStep;
    if (g_dbTimer <= delay) return;
    alignas(16) uint8_t ev[0x40];
    Game_GunAimedAt_ctor(ev, ped);
    Game_AddEvent((uint8_t*)tIntel + 0x84, ev, false, true);
    Game_GunAimedAt_dtor(ev);
    g_dbTimer = 0.0f;
}
static bool __fastcall Hook_DriveByProcess(void* task, void*, void* ped) {
    bool r = Orig_DriveByProcess(task, ped);
    if (ped && ped == Game_FindPlayerPed(0) && !(g_scriptModeFlag && *g_scriptModeFlag)) DriveByAimReactions(ped);
    return r;
}
// Look behind while aiming. Called from the camera manager's vehicle look code, manager in EBX.
static bool __cdecl Hook_WantsDriveByCam() {
    uint8_t* mgr;
    __asm__ volatile("movl %%ebx, %0" : "=r"(mgr));
    bool r = Orig_WantsDriveByCam();
    if (r && g_dbAim && g_cfg.driveByLookBehind && InputValue(INPUT_VEH_LOOK_BEHIND) > 0x7F) {
        // The manager remembers "drive-by camera" in +0x1C5 bit 0x8 and zeroes the
        // look-behind request while it's set.
        if (mgr) mgr[0x1C5] &= ~0x08;
        return false;                                  // let the look-behind camera run
    }
    return r;
}
static void InitDriveBy(uintptr_t see) {
    if (uintptr_t f = FindPattern("C6 05 ? ? ? ? ? C6 05 ? ? ? ? ? C7 05 ? ? ? ? ? ? ? ? 8B 01"))
        g_scriptModeFlag = *(uint8_t**)(f + 2);
    *(uintptr_t*)&Game_IsMouseButtonPressed = FindPattern("8B 4C 24 04 A1 ? ? ? ? 49 83 F9 07 77 47 FF 24 8D");
    *(uintptr_t*)&Game_IsPadButtonPressed   = FindPattern("FF 74 24 04 E8 ? ? ? ? 83 C4 04 85 C0 74 60");
    // VEH_ATTACK is read at +0x28FE/+0x28FC (0x2698 + 38*16).
    if (!Game_IsMouseButtonPressed || !Game_IsPadButtonPressed || !FindPattern("8A 86 FE 28 00 00 32 86 FC 28 00 00 3C 7F")) {
        g_driveByOk = false; Log("drive-by: input functions not found, disabled"); return;
    }
    uintptr_t fire = FindPattern("83 8E A0 02 00 00 02 FF 74 24 2C 8B CF FF 74 24 60 FF 74 24 5C 56 E8 ? ? ? ? 32 C0");
    if (!fire || !(*(uintptr_t*)&Orig_DriveByFire = CallDest(fire + 22))) {
        g_driveByOk = false; Log("drive-by: fire site not found, disabled"); return;
    }
    PatchCall(fire + 22, (void*)&Hook_DriveByFire);
    g_driveByOk = true;
    if (uintptr_t lb = FindPattern("80 A3 C5 01 00 00 FB E8 ? ? ? ? 84 C0 74 0C 80 8B C5 01 00 00 08")) {
        *(uintptr_t*)&Orig_WantsDriveByCam = CallDest(lb + 7);
        if (Orig_WantsDriveByCam) PatchCall(lb + 7, (void*)&Hook_WantsDriveByCam);
    }
    if (uintptr_t c = FindPattern("F3 0F 10 25 ? ? ? ? 8B 47 20 F3 0F 10 15 ? ? ? ? F3 0F 10 1D"))
        g_camFront = *(Vec4**)(c + 4);
    (void)see;
    if (Game_AddEvent) {   // NPC reactions share the cover mod's functions
        if (uintptr_t vt = FindPattern("56 8B F1 E8 ? ? ? ? 8D 4E 14 C7 06 ? ? ? ? E8 ? ? ? ? 8B 44 24 08 F3 0F 10 44 24 10")) {
            uintptr_t* slot = &(*(uintptr_t**)(vt + 13))[17];   // CTaskSimpleNewGangDriveBy::ProcessPed
            *(uintptr_t*)&Orig_DriveByProcess = *slot;
            PatchPtr(slot, (uintptr_t)&Hook_DriveByProcess);
        }
    }
    Log(Orig_WantsDriveByCam ? "drive-by enabled (LB/RB, look behind, thrown weapons)" : "drive-by enabled (no look behind)");
}


// The popped-out stance comes from the cover slide-out clip (state 8), held at its last
// frame while aiming; the aim clip itself is the weapon's generic one (cover state 9 has no
// clip of its own). So swap that held body clip: start the other stance's slide-out clip at
// the same (end) phase and crossfade. The aim task is left alone.
static void  (__thiscall* Game_SetDucking)(void* ped, bool duck, int time) = nullptr;     // 0x9F2270
__attribute__((unused)) static bool SwapBasePose(void* ped, bool wasTall) {
    void* ct = g_useTask; void* blender = At<void*>(ped, 0x78);
    if (!ct || !blender) { Log("pose swap: no cover task / blender"); return false; }
    uint8_t f = At<uint8_t>(ct, 0x35);
    bool side = (f >> 1) & 1, corner = Game_CoverCorner(ct, ped), over = (f & 0x30) != 0;
    int gO = -1, aO = -1, gN = -1, aN = -1;
    Game_CoverAnims(ped, &gO, &aO, 8, false, wasTall, side, corner, over);
    Game_CoverAnims(ped, &gN, &aN, 8, false, !wasTall, side, corner, over);
    if (gO < 0 || aO < 0 || gN < 0 || aN < 0) { Log("pose swap: no slide-out clip, group ", gN, " anim ", aN); return false; }
    void* old = Game_FindAnim(blender, gO, aO);
    if (!old) { Log("pose swap: held clip not found, group ", gO, " anim ", aO); return false; }
    void* nw = Game_StartAnim(blender, gN, aN, kSwapBlend, -1);
    if (!nw || nw == old) { Log("pose swap: could not start group ", gN, " anim ", aN); return false; }
    At<uint32_t>(nw, 4) = At<uint32_t>(old, 4);              // same play flags (hold at end etc.)
    Game_AnimSetPhase(nw, At<float>(old, 0x4C));
    Game_AnimSetBlend(nw, kSwapBlend);
    Game_AnimSetBlend(old, -kSwapBlend);
    Log("pose swap: group ", gO, " -> ", gN);
    return true;
}

static void* __fastcall Hook_GunControl(void* t, void*, void* ped) {
    if (!PlayerCoverGun(ped)) return Orig_GunControl(t, ped);
    void* sub = At<void*>(t, 0x8);
    int subType = TaskType(sub);
    if (ClipEmpty(ped) && !g_realAim) return Orig_GunControl(t, ped);
    SetGunPlan(t);

    void* r;
    {
        // Fire rate: while aiming, don't start the next shot before the
        // weapon's own time between shots has passed since the last one.
        ScopedNoFire nf;
        if (subType == TASK_AIM_GUN && Game_GetPlayerControls() == g_controls &&
            g_time - g_lastShotTime < TimeBetweenShots(ped))
            nf.Engage();
        r = Orig_GunControl(t, ped);
    }
    // Stance swap while aiming: new aim animation, then end this aim step; the gun task's
    // next step (still aiming) starts with the new pose.
    if (g_swapWanted && g_realAim && sub && r == sub && subType == TASK_AIM_GUN && g_coverState == 9) {
        g_swapWanted = false;
        bool wasTall = !g_crouched;
        if (SwapStance(t, ped)) {
            // The crouched/standing aim pose is the ped's ducking state (the aim picks the
            // crouched or standing weapon aim clip from it). The cover task only changes
            // ducking after a 0.25 s delay, then restarts the aim. Change it right away.
            bool before = Orig_IsDucking(ped);
            StartDump(before ? "COVER stand-up while aiming, " : "COVER crouch while aiming, ");
            DumpAnims(ped, "COVER before swap, ");
            Game_SetDucking(ped, g_crouched, g_crouched ? 1000 : -1);
            if (!g_crouched) StartStandFix(ped);
            if (g_useTask) At<float>(g_useTask, 0x44) = 0.0f;
            Log("swap: ducking ", before ? 1 : 0, " -> ", Orig_IsDucking(ped) ? 1 : 0);
            // Passive aim (state 18) stance changes look vanilla because the cover task
            // blends its body clip to the new stance (0xD1C860, which skips states 8/9/17).
            // Run that same blend while popped out by presenting the state as 18 for the call.
            if (void* ct = g_useTask) {
                int saved = At<int>(ct, 0x38);
                void* bodyBefore = At<void*>(ct, 0x10);
                At<int>(ct, 0x38) = 18;
                Game_CoverRefreshIdle(ct, ped);
                At<int>(ct, 0x38) = saved;
                Log(At<void*>(ct, 0x10) != bodyBefore ? "swap: body clip blended to new stance" : "swap: body clip unchanged");
            }
            (void)wasTall;
            if (true) {
                LogT("swap: handed to the game's own ducking change");
            } else {
                g_swapBlend = kSwapBlend;
                At<float>(sub, 0x60) = -kSwapBlend;
                g_swapOldAim = sub;
                Game_AimStop(sub, true);
                LogT("swap: old aim stopped (fallback)");
            }
        }
    }
    // Aim released while the aim step is running: stop it, so the gun task
    // ends and the game takes you back into cover.
    if (!g_realAim && sub && r == sub && subType == TASK_AIM_GUN) {
        static void* lastStopped = nullptr;
        Game_AimStop(sub, true);
        if (sub != lastStopped) { lastStopped = sub; Log("aim released: stopping aim"); }
    }
    return r;
}

// ---- Camera on leaving cover while aiming ---------------------------------------------
//  0xC25080 = CCamInterp::Init(src, dst, durationMs, startOffset, flag): camera blends
//  between two cameras. 0xA86C40 = camera factory (type 9 = CCamAimWeapon, 3 = CCamInterp).
static uintptr_t g_exeBase = 0;
static uintptr_t Rebase(uintptr_t a) { return a - 0x400000 + g_exeBase; }
static const char* CamName(void* cam) {
    if (!cam) return "none";
    uintptr_t vt = *(uintptr_t*)cam;
    if (vt == Rebase(0xE9B98C)) return "AimWeapon";
    if (vt == Rebase(0xE9B4A8)) return "FollowPed";
    if (vt == Rebase(0xEC57C0)) return "Interp";
    if (vt == Rebase(0xEAA588)) return "Game";
    if (vt == Rebase(0xEE7550)) return "WeaponAiming";
    if (vt == Rebase(0xE9AF3C)) return "FollowVehicle";
    if (vt == Rebase(0xEE69E4)) return "AimWeaponVeh";
    return "other";
}
// Leaving cover while aiming, the game blends cover-aim camera -> follow camera (750 ms)
// and only then creates the on-foot aim camera (200 ms blend). Until that second camera is
// live there is no reticle and the player doesn't face the aim. Make the detour short.
static const int kExitCamHopMs = 60;
static void (__thiscall* Orig_CamInterpInit)(void* self, void* src, void* dst, int dur, int ofs, bool flag) = nullptr;
static bool InVehicleNow() { void* p = Game_FindPlayerPed(0); return p && (At<uint32_t>(p, 0x26C) & 4); }
static void __fastcall Hook_CamInterpInit(void* self, void*, void* src, void* dst, int dur, int ofs, bool flag) {
    // Sniper swaps: the scope <-> over-the-shoulder changes are cuts, never a pull-out.
    if (g_time - g_cutInterpT < 0.5f) {
        char line[120]; char* p = line;
        const char* a = "sniper swap camera blend "; while (*a) *p++ = *a++;
        for (const char* q = CamName(src); *q; ++q) *p++ = *q;
        a = " -> "; while (*a) *p++ = *a++;
        for (const char* q = CamName(dst); *q; ++q) *p++ = *q;
        a = ", ms "; while (*a) *p++ = *a++;
        *p = 0; Log(line, dur);
        if (dur > 1) { dur = 1; Log("   made instant"); }
    }
    if (InExitWindow()) {
        char line[120]; char* p = line;
        const char* a = "camera blend "; while (*a) *p++ = *a++;
        for (const char* q = CamName(src); *q; ++q) *p++ = *q;
        a = " -> "; while (*a) *p++ = *a++;
        for (const char* q = CamName(dst); *q; ++q) *p++ = *q;
        a = ", ms "; while (*a) *p++ = *a++;
        *p = 0;
        LogX(line, dur);
        bool aimHeld = InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F;
        if (g_exitFromCover && aimHeld && g_time - g_exitT < 0.6f &&
            CamName(dst)[0] == 'F' && CamName(src)[0] == 'A' && dur > kExitCamHopMs) {
            dur = kExitCamHopMs; LogX("exit camera detour shortened to ms ", dur);
        }
    }
    Orig_CamInterpInit(self, src, dst, dur, ofs, flag);
}
static void* (__thiscall* Orig_CamCreate)(void* mgr, int type, int a, int b) = nullptr;
static void* __fastcall Hook_CamCreate(void* mgr, void*, int type, int a, int b) {
    void* r = Orig_CamCreate(mgr, type, a, b);
    if (InExitWindow()) LogX("camera created, type ", type);
    return r;
}

// ---- Weapon switching while aiming (on foot / cover) --------------------------------------
//  The player's weapon-select code (0xA2C500) skips next/prev weapon while the aim button
//  (0xA72820) or fire (0xA72850) is held. Its aim check is answered "not aiming".
static bool g_switchAimSeen = false;
static bool (__cdecl* Orig_SwitchAimCheck)(int) = nullptr;
static bool __cdecl Hook_SwitchAimCheck(int a) {
    g_switchAimSeen = true;
    if (!g_cfg.weaponSwitchAiming && Orig_SwitchAimCheck) return Orig_SwitchAimCheck(a);
    return false;
}
// Diagnostic: does the weapon-select code run while aiming, and do the inputs arrive?
static void (__thiscall* Orig_WeaponSelect)(void* ped) = nullptr;   // 0xA2C500
static bool DriveByAimHeld();
static bool DbPadCfg(int b);
static void __fastcall Hook_WeaponSelect(void* ped, void*) {
    bool aim = InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F;
    bool nxt = InputValue(8) > 0x7F, prv = InputValue(9) > 0x7F;
    g_switchAimSeen = false;
    uint32_t before = At<uint32_t>(ped, 0x2B0);
    // The aim / gun tasks set "no weapon switching" (+0x2A0 bit 0x10) every frame. While
    // the player aims on foot or in cover, lift it for this call only.
    uint32_t flags = At<uint32_t>(ped, 0x2A0);
    uint32_t f264 = At<uint32_t>(ped, 0x264);
    // In a vehicle, aiming is the drive-by button (input 38) as well.
    bool inVeh = (At<uint32_t>(ped, 0x26C) & 4) != 0;
    bool vehAim = inVeh && (aim || InputValue(38) > 0x7F || DriveByAimHeld());
    bool lift = IsPlayer(ped) && ((aim && !inVeh && g_cfg.weaponSwitchAiming) || (vehAim && g_cfg.weaponSwitchVehicle));
    if (vehAim) {
        aim = true;
        nxt = nxt || InputValue(112) > 0x7F;      // vehicle weapon next / prev (0x2D98 / 0x2DA8)
        prv = prv || InputValue(113) > 0x7F;
        static int lastMask = 0;
        int mask = (InputValue(8) > 0x7F ? 1 : 0) | (InputValue(9) > 0x7F ? 2 : 0) | (InputValue(42) > 0x7F ? 4 : 0) |
                   (InputValue(112) > 0x7F ? 8 : 0) | (InputValue(113) > 0x7F ? 16 : 0);
        if (mask && mask != lastMask) Log("drive-by: weapon inputs next+prev*2+in42*4+vehNext*8+vehPrev*16 = ", mask);
        lastMask = mask;
    }
    if (lift) At<uint32_t>(ped, 0x2A0) = flags & ~0x10u;
    // Aiming on foot / in cover: next / previous weapon (inputs 8 / 9) only counts when it
    // comes from the controller's d-pad. The mouse wheel feeds the same inputs and is left
    // to zoom, unless the ini allows it. A hard rule per press, not per device, so mixed
    // input (switching devices, Steam controller) behaves. Number keys (100+) always work.
    uint8_t* in8 = Input(8); uint8_t* in9 = Input(9);
    uint8_t keep8 = in8[6], keep9 = in9[6];
    bool dpad = DbPadCfg(8) || DbPadCfg(9) || DbPadCfg(10) || DbPadCfg(11);   // d-pad up/down/left/right
    // Only when this mod is the one allowing switches while aiming (SwitchWhileAiming=1);
    // with it off, ScrollSwitchWhileAiming is ignored and the wheel stays fully vanilla.
    bool blockWheel = g_cfg.weaponSwitchAiming && lift && !inVeh && !g_cfg.scrollSwitchAiming && !dpad;
    if (blockWheel) { in8[6] = in8[4]; in9[6] = in9[4]; }   // value 0 for this call
    Orig_WeaponSelect(ped);
    if (blockWheel) { in8[6] = keep8; in9[6] = keep9; }
    if (lift && (flags & 0x10)) At<uint32_t>(ped, 0x2A0) |= 0x10;
    static bool lastPress = false;
    bool press = aim && (nxt || prv);
    // Switched while aiming: keep the aim camera through the swap. Not for the sniper
    // (slot 6): its scope is a first-person camera and must switch normally.
    uint32_t after = At<uint32_t>(ped, 0x2B0);
    if (inVeh && aim && after != before) Log("drive-by weapon switch: slot ", (int)before, " -> ", (int)after);
    if (lift && aim && after != before && !inVeh) {
        if (IsMeleeSlot(ped) && g_cfg.meleeSwitchDropsAim) {
            // Fists / melee: back to the normal on-foot camera, no reticle.
            g_aimSuppress = true; g_holdAimCamT = -100.0f;
            Log("switched to melee while aiming: dropping aim");
        } else {
            // Keep the over-the-shoulder aim camera through the swap.
            g_holdAimCamT = g_time; g_holdAimCamLen = 2.5f;
            if (!g_cfg.sniperSwitchCamera && (after == 6 || before == 6)) g_holdAimCamT = -100.0f;   // vanilla sniper switch
            else if (after == 6)  { g_toSniperT = g_time;   g_sniperSwapT = g_time; Log("switch to sniper while aiming: holding aim camera until it's raised"); }
            if (g_cfg.sniperSwitchCamera && before == 6) { g_fromSniperT = g_time; g_sniperSwapT = g_time; Log("switch from sniper while aiming: scope -> aim camera"); }
        }
    }
    if (press && !lastPress) {
        Log("weapon select while aiming: block flag ", (flags & 0x10) ? 1 : 0, ", other block flag ", (f264 >> 31) & 1);
        Log("   reached aim check ", g_switchAimSeen ? 1 : 0, ", slot changed ", At<uint32_t>(ped, 0x2B0) != before ? 1 : 0);
    }
    lastPress = press;
}

// ---- Keep the aim camera while aim is held ---------------------------------------------
//  0xB0A530 is the camera manager's "aim camera -> follow camera" switch (750 ms blend);
//  it's called from two places. (0xB09F40, hooked in v43, is the switch *back* to the aim
//  camera, which is why v43 made things worse.) Leaving cover or switching weapons drops the
//  aim for a moment; while the aim button is still held shortly after one of those, skip the
//  switch so the aim camera simply stays.
static void (__thiscall* Orig_AimToFollow)(void* mgr, void* arg) = nullptr;
static void* (__thiscall* Game_FollowToAim)(void* mgr) = nullptr;          // 0xB09F40
static void* (__thiscall* Game_MgrGetCam)(void* mgr, int type, int idx) = nullptr;  // 0xA7CD30
static void* (__thiscall* Orig_CreateScopeCam)(void* mgr) = nullptr;       // 0xB0A490 (CCamFpsWeapon)
static const int CAM_FPS_WEAPON = 0x1B, CAM_AIM_WEAPON = 9;
static void (__thiscall* Game_CamRelease)(void* cam, bool now) = nullptr;   // 0xA7C900
static float g_directScopeT = -100.0f;    // we switched aim camera -> scope ourselves
// The sniper is in hand once the game's "current weapon" (weapon in hand that matches the
// selected slot) is no longer empty.
static bool SniperRaised(void* ped) {
    return ped && At<uint32_t>(ped, 0x2B0) == 6 && Game_GetCurrentWeapon((uint8_t*)ped + 0x2B0);
}

// The sniper scope is its own camera (CCamFpsWeapon). Don't let it come back while
// switching away from the sniper.
static void* __fastcall Hook_CreateScopeCam(void* mgr, void*) {
    void* ped = Game_FindPlayerPed(0);
    if (ped && g_time - g_fromSniperT < 2.5f && At<uint32_t>(ped, 0x2B0) != 6) {
        static float lastLog = -100.0f;
        if (g_time - lastLog > 0.5f) { lastLog = g_time; Log("scope camera blocked (switching away from sniper)"); }
        return nullptr;
    }
    void* r = Orig_CreateScopeCam(mgr);
    if (g_time - g_sniperSwapT < 3.0f) Log("scope camera on");
    return r;
}
// The scope's render setup (0xC054B7) reads the weapon in hand while the scope flag is still
// on. Mid-swap the "current weapon" is empty (the old one is still in hand), which crashed
// when going scope -> aim camera. Fall back to the weapon in hand, or an unarmed stand-in.
static uint8_t g_dummyWeapon[0x100];   // weapon type +0x18 = 0 (unarmed)
static void* __fastcall Hook_ScopeRenderWeapon(void* wd, void*) {
    if (void* w = Game_GetCurrentWeapon(wd)) return w;
    void* obj = At<void*>(wd, 0x14);
    if (obj && At<void*>(obj, 0x25C)) return At<void*>(obj, 0x25C);
    static bool logged = false;
    if (!logged) { logged = true; Log("scope render: no weapon in hand, used a stand-in"); }
    return g_dummyWeapon;
}
static void __fastcall Hook_AimToFollow(void* mgr, void*, void* arg) {
    bool aimHeld = InputValue(INPUT_AIM) > kAimThreshold || InputValue(INPUT_FREE_AIM) > 0x7F;
    void* ped = Game_FindPlayerPed(0);
    if (aimHeld && !InVehicleNow() && ped && !g_aimSuppress) {
        // Right after our direct switch to the scope, the manager may still try to leave
        // the (old) aim camera; that would kill the new scope. Ignore it.
        if (g_time - g_directScopeT < 0.4f && At<uint32_t>(ped, 0x2B0) == 6 && Game_MgrGetCam && Game_MgrGetCam(mgr, CAM_FPS_WEAPON, 0)) {
            static float lastLog = -100.0f;
            if (g_time - lastLog > 1.0f) { lastLog = g_time; Log("kept the scope (ignored aim -> follow)"); }
            return;
        }
        // Away from the sniper: drop the scope, then go straight to the over-the-shoulder
        // aim camera (instant), and keep it for the rest of the swap.
        void* scopeCam = (g_time - g_fromSniperT < 2.5f && Game_FollowToAim && Game_MgrGetCam) ? Game_MgrGetCam(mgr, CAM_FPS_WEAPON, 0) : nullptr;
        if (scopeCam) {
            // Keep where the scope was looking: its heading/pitch (+0x144/+0x148) become the
            // new aim camera's (+0x218/+0x21C). Without this the pitch resets to level.
            float scopePitch = At<float>(scopeCam, 0x144), scopeHeading = At<float>(scopeCam, 0x148);
            Orig_AimToFollow(mgr, arg);
            g_cutInterpT = g_time;
            Game_FollowToAim(mgr);
            if (void* aimCam = Game_MgrGetCam(mgr, CAM_AIM_WEAPON, 0)) {
                // The follow -> aim switch hands the new aim camera a starting orientation
                // (+0x22C = pitch, +0x230 = heading, +0x234 = "use it"), 0xA25270. The heading
                // comes from the current view, the pitch from the follow camera, which is
                // level after the scope: that was the snap. Give it the scope's pitch.
                Log("   start pitch x1000 ", (int)(At<float>(aimCam, 0x22C) * 1000), " -> ", (int)(scopePitch * 1000));
                Log("   start heading x1000 ", (int)(At<float>(aimCam, 0x230) * 1000), ", scope heading x1000 ", (int)(scopeHeading * 1000));
                At<float>(aimCam, 0x22C) = scopePitch;          // scope +0x144 = pitch
                if (!At<uint8_t>(aimCam, 0x234)) { At<float>(aimCam, 0x230) = scopeHeading; At<uint8_t>(aimCam, 0x234) = 1; }
            }
            g_holdAimCamT = g_time; g_holdAimCamLen = 2.5f;
            Log("scope off -> aim camera (cut)");
            return;
        }
        // To the sniper: keep the aim camera only until the sniper is raised, then let the
        // game switch to the scope.
        if (g_time - g_toSniperT < 2.5f && g_time - g_holdAimCamT < g_holdAimCamLen) {
            if ((SniperRaised(ped) && g_time - g_toSniperT > 0.15f) || g_time - g_toSniperT > 1.5f) {
                // The aim -> follow blend keeps its normal length: the scope replaces it a
                // frame later, so what you see stays the aim view until the scope cuts in.
                g_holdAimCamT = -100.0f; g_toSniperT = -100.0f;
                Log(SniperRaised(ped) ? "sniper raised: aim camera -> scope" : "sniper hold timed out: aim camera -> scope");
                // Go straight to the scope: vanilla goes aim -> follow camera -> (next frame)
                // scope, and that follow camera frame was the flash.
                void* aimCam = Game_MgrGetCam ? Game_MgrGetCam(mgr, CAM_AIM_WEAPON, 0) : nullptr;
                void* scope = (Orig_CreateScopeCam && Game_CamRelease) ? Orig_CreateScopeCam(mgr) : nullptr;
                if (scope) {
                    if (aimCam && aimCam != scope) Game_CamRelease(aimCam, false);
                    g_directScopeT = g_time;
                    Log("   scope created directly");
                    return;
                }
                Log("   direct scope failed, vanilla switch");
                Orig_AimToFollow(mgr, arg);
                return;
            }
        }
    }
    if (aimHeld && g_time - g_holdAimCamT < g_holdAimCamLen && !InVehicleNow() && !g_aimSuppress) {
        static float lastLog = -100.0f;
        if (g_time - lastLog > 0.5f) { lastLog = g_time; Log("kept the aim camera (aim still held)"); }
        return;
    }
    Orig_AimToFollow(mgr, arg);
}

// ---- Setup --------------------------------------------------------------------------------
static bool HookVtable(uintptr_t* vt, TaskFn* next, TaskFn* first, TaskFn* control, void* hn, void* hf, void* hc) {
    if (!vt) return false;
    *(uintptr_t*)next = vt[18]; *(uintptr_t*)first = vt[19]; *(uintptr_t*)control = vt[20];
    PatchPtr(&vt[18], (uintptr_t)hn); PatchPtr(&vt[19], (uintptr_t)hf); PatchPtr(&vt[20], (uintptr_t)hc);
    return true;
}

static void Init() {
    *(uintptr_t*)&Game_FindPlayerPed     = FindPattern("8B 44 24 04 85 C0 75 18 A1 ? ? ? ? 83 F8 FF 74 0B 8B 04 85 ? ? ? ? 85 C0 75 03 33 C0 C3 8B 80 98 05 00 00 C3");
    *(uintptr_t*)&Game_GetPlayerControls = FindPattern("A1 ? ? ? ? 83 F8 FF 74 31 8B 04 85 ? ? ? ? 85 C0 74 26 83 B8 C8 04 00 00 00");
    // The gun task's own "is the fire button pressed" call (0xA72850).
    uintptr_t fireCall = FindPattern("E8 ? ? ? ? 88 44 24 1A 8B 44 24 40 85 C0 74 16 8A 88 6E 28 00 00");
    if (fireCall) *(uintptr_t*)&Game_IsFirePressed = CallDest(fireCall);
    if (!Game_FindPlayerPed || !Game_GetPlayerControls || !Game_IsFirePressed) { Log("ERROR: game functions not found"); return; }

    // Input layout sanity checks (ATTACK at +0x26DE, AIM at +0x26FE).
    if (!FindPattern("8A 86 DE 26 00 00 32 86 DC 26 00 00 3C 7F") ||
        !FindPattern("8A 88 FE 26 00 00 32 88 FC 26 00 00 80 F9 7F")) { Log("ERROR: input layout check failed"); return; }

    uintptr_t see  = FindPattern("50 8B 44 24 20 8B 88 24 02 00 00 E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? F3 0F 10 46 38 F3 0F 58 05");
    uintptr_t ctrl = FindPattern("68 ? ? ? ? B9 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? F6 D8 1A C0 20 05");
    uintptr_t vtA  = FindPattern("56 8B F1 E8 ? ? ? ? 8A 44 24 08 88 46 18 C7 06 ? ? ? ? C7 46 14 00 00 00 00 C6 46 19 00");      // PlayerInCover
    uintptr_t vtB  = FindPattern("56 57 8B F1 E8 ? ? ? ? 8B 54 24 10 C7 06 ? ? ? ? 8B 02 89 46 20 8B 42 04 89 46 24 8B 42 08 89 46 28"); // NewUseCover
    uintptr_t vtC  = FindPattern("88 46 73 8B 45 18 C7 06 ? ? ? ? 89 0A F3 0F 11 46 40 C6 46 72 05");                                   // ComplexGun
    uintptr_t s8   = FindPattern("6A 01 E8 ? ? ? ? 83 C4 04 84 C0 0F 84 ? ? ? ? E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? F3 0F 10 46 48");
    uintptr_t gf   = FindPattern("6A 00 6A 01 57 8B CE E8 ? ? ? ? 5B 5F 5E 5D 59 C2 04 00 6A 01 57 8B CE E8");   // first step = fire
    uintptr_t ga   = FindPattern("84 C0 0F 84 ? ? ? ? 6A 01 57 8B CE E8 ? ? ? ? 5B 5F 5E 5D 59 C2 04 00");       // first step = aim
    if (uintptr_t w = FindPattern("8B 87 C4 02 00 00 8D 8F B0 02 00 00 89 44 24 10 E8"))
        *(uintptr_t*)&Game_GetCurrentWeapon = CallDest(w + 16);
    uintptr_t as   = FindPattern("8B 4C 24 40 6A 01 E8 ? ? ? ? 8B 46 08 5F 5E 8B E5 5D C2 04 00");             // stop aiming
    if (as) *(uintptr_t*)&Game_AimStop = CallDest(as + 6);
    if (!see || !ctrl || !vtA || !vtB || !vtC || !s8 || !gf || !ga || !Game_AimStop || !Game_GetCurrentWeapon) { Log("ERROR: a pattern was not found"); return; }
    *(uintptr_t*)&Orig_GunCreateFire = CallDest(gf + 7);
    *(uintptr_t*)&Game_GunCreateReload = CallDest(gf + 25);
    if (uintptr_t wi = FindPattern("FF 70 18 E8 ? ? ? ? 83 C4 04 83 78 08 03 89 44 24 18"))
        *(uintptr_t*)&Game_GetWeaponInfo = CallDest(wi + 3);
    *(uintptr_t*)&Game_GunCreateAim  = CallDest(ga + 13);
    if (!Orig_GunCreateFire || !Game_GunCreateAim || !Game_GunCreateReload) { Log("ERROR: gun step functions not found"); return; }
    *(uintptr_t*)&Game_CoverRefreshIdle = FindPattern("83 EC 08 56 57 8B 7C 24 14 8B F1 83 BF 68 0D 00 00 00 0F 84 ? ? ? ? 8B 4E 38 83 F9 11");
    *(uintptr_t*)&Game_CoverCorner = FindPattern("8B 44 24 04 8B 90 68 0D 00 00 85 D2 75 05 32 C0 C2 04 00 80 B8 19 02 00 00 00 75 0C 8B 02 C1 E8 03 83 E0 03 3C 03");
    uintptr_t tooHigh = FindPattern("8B 44 24 04 85 C0 74 18 8B 80 68 0D 00 00 85 C0 74 0E 8B 00 83 E0 18 3C 18 75 05 B0 01 C2 04 00");
    uintptr_t spread  = FindPattern("8B CE F3 0F 11 44 24 08 E8 ? ? ? ? 84 C0 75 09 F6 86 6C 02 00 00 04 74 10 F3 0F 10 05");
    uintptr_t reticle = FindPattern("8B CF E8 ? ? ? ? 84 C0 74 1C 8B 8F 24 02 00 00 6A 00 68 16 04 00 00 81 C1 E0 02 00 00 E8 ? ? ? ? 84 C0 74 09 F6 87 6C 02 00 00 04");
    if (spread)  *(uintptr_t*)&Orig_IsDucking = CallDest(spread + 8);
    if (reticle) *(uintptr_t*)&Orig_FindTask  = CallDest(reticle + 30);
    if (!spread || !reticle || !Orig_IsDucking || !Orig_FindTask || CallDest(reticle + 2) != (uintptr_t)Orig_IsDucking) { Log("ERROR: crouch accuracy sites not found"); return; }
    *(uintptr_t*)&Game_CoverAnims = FindPattern("53 55 56 8B 74 24 1C 57 83 FE 0B 74 0E 83 FE 0C 74 09 83 FE 0D 74 04 32 DB EB 02 B3 01 83 FE 08");
    if (!Game_CoverAnims) { Log("ERROR: cover anim picker not found"); return; }
    uintptr_t sa  = FindPattern("F3 0F 10 47 5C 6A FF 51 8B 4D 78 F3 0F 11 04 24 53 56 E8");
    uintptr_t ph  = FindPattern("F3 0F 10 46 78 8B 4E 64 F3 0F 11 04 24 E8");
    uintptr_t ev  = FindPattern("8B 77 64 8B CE FF 76 40 E8 ? ? ? ? 8B 46 64");
    uintptr_t exd = FindPattern("8B CE E8 ? ? ? ? 84 C0 74 16 80 7F 19 00 74 10 6A FF 6A 01");
    if (!sa || !exd) { Log("ERROR: aim anim / exit sites not found"); return; }
    *(uintptr_t*)&Game_StartAnim      = CallDest(sa + 18);
    if (uintptr_t fa = FindPattern("8B 4D 78 53 56 E8 ? ? ? ? 89 47 64 85 C0 75 1A")) *(uintptr_t*)&Game_FindAnim = CallDest(fa + 5);
    if (!Game_FindAnim) { Log("ERROR: find-anim not found"); return; }
    if (uintptr_t sd = FindPattern("68 E8 03 00 00 6A 01 E8 ? ? ? ? 5F 5E 5B C2 04 00 6A FF 6A 00 E8")) *(uintptr_t*)&Game_SetDucking = CallDest(sd + 7);
    if (!Game_SetDucking) { Log("ERROR: set-ducking not found"); return; }
    if (ph) *(uintptr_t*)&Game_AnimSetPhase   = CallDest(ph + 13);   // only used by unused helpers
    if (ev) *(uintptr_t*)&Game_AnimLoadEvents = CallDest(ev + 8);
    uintptr_t rel = FindPattern("8B 4E 64 85 C9 74 1D 51 C7 04 24 00 00 7A C4 E8 ? ? ? ? 8B 4E 64 56 E8");
    uintptr_t cc  = FindPattern("50 56 F3 0F 11 44 24 6C E8");
    uintptr_t uc  = FindPattern("8D 44 24 20 50 56 8B CF E8 ? ? ? ? 8D 44 24 20 50");
    if (!rel || !cc || !uc) { Log("ERROR: anim release / cover compute sites not found"); return; }
    *(uintptr_t*)&Game_AnimSetBlend       = CallDest(rel + 15);
    *(uintptr_t*)&Game_AnimRemoveCallback = CallDest(rel + 24);
    *(uintptr_t*)&Orig_ComputeCover = CallDest(cc + 8);
    *(uintptr_t*)&Orig_UpdateCover  = CallDest(uc + 8);
    uintptr_t su1 = FindPattern("8D 44 24 24 50 56 8B CF E8 ? ? ? ? C7 47 38 02 00 00 00");
    uintptr_t su2 = FindPattern("8D 44 24 24 50 56 8B CF E8 ? ? ? ? C7 47 38 0F 00 00 00");
    uintptr_t su3 = FindPattern("8D 44 24 24 50 56 8B CF E8 ? ? ? ? C7 47 38 03 00 00 00");
    uintptr_t su4 = FindPattern("50 FF 75 18 8B CE 57 E8 ? ? ? ? 8B 8F 68 0D 00 00 57 E8");
    uintptr_t ot2 = FindPattern("84 C0 0F 84 ? ? ? ? 57 8B CE E8 ? ? ? ? B0 01 5F 5E 8B E5 5D C2 14 00");
    if (su1) *(uintptr_t*)&Orig_SetupCover = CallDest(su1 + 8);
    if (!su1 || !su2 || !su3 || !su4 || !ot2 || !Orig_SetupCover ||
        CallDest(su2 + 8) != (uintptr_t)Orig_SetupCover || CallDest(su3 + 8) != (uintptr_t)Orig_SetupCover ||
        CallDest(su4 + 7) != (uintptr_t)Orig_SetupCover) { Log("ERROR: cover setup sites not found"); return; }
    if (!Game_CoverRefreshIdle || !Game_CoverCorner || !tooHigh) { Log("ERROR: cover crouch functions not found"); return; }
    *(uintptr_t*)&Orig_State8Fire = CallDest(s8 + 18);
    if (!Orig_State8Fire) { Log("ERROR: slide-out check not found"); return; }

    g_timeStep = *(float**)(see + 33);
    g_controls = *(uint8_t**)(ctrl + 1);
    *(uintptr_t*)&Orig_AfterControls = CallDest(ctrl + 15);
    if (!Orig_AfterControls) { Log("ERROR: controls hook target missing"); return; }

    HookVtable(*(uintptr_t**)(vtA + 17), &Orig_CoverNext, &Orig_CoverFirst, &Orig_CoverControl,
               (void*)&Hook_CoverNext, (void*)&Hook_CoverFirst, (void*)&Hook_CoverControl);
    HookVtable(*(uintptr_t**)(vtB + 15), &Orig_UseNext, &Orig_UseFirst, &Orig_UseControl,
               (void*)&Hook_UseNext, (void*)&Hook_UseFirst, (void*)&Hook_UseControl);
    HookVtable(*(uintptr_t**)(vtC + 8), &Orig_GunNext, &Orig_GunFirst, &Orig_GunControl,
               (void*)&Hook_GunNext, (void*)&Hook_GunFirst, (void*)&Hook_GunControl);
    PatchCall(s8 + 18, (void*)&Hook_State8Fire);
    PatchCall(gf + 7, (void*)&Hook_GunCreateFire);
    PatchCall(ctrl + 15, (void*)&Hook_AfterControls);
    { uint32_t prot; Unprotect((void*)tooHigh, 5, &prot);
      *(uint8_t*)tooHigh = 0xE9; *(int32_t*)(tooHigh + 1) = (int32_t)((uintptr_t)&Hook_TooHigh - (tooHigh + 5));
      Reprotect((void*)tooHigh, 5, prot); }
    *(uintptr_t*)&Orig_PickOverTop = CallDest(s8 + 0x34);
    if (!Orig_PickOverTop || CallDest(ot2 + 11) != (uintptr_t)Orig_PickOverTop) { Log("ERROR: over-the-top sites not found"); return; }
    PatchCall(s8 + 0x34, (void*)&Hook_PickOverTop);
    PatchCall(ot2 + 11, (void*)&Hook_PickOverTop);
    PatchCall(su1 + 8, (void*)&Hook_SetupCover); PatchCall(su2 + 8, (void*)&Hook_SetupCover);
    PatchCall(su3 + 8, (void*)&Hook_SetupCover); PatchCall(su4 + 7, (void*)&Hook_SetupCover);
    g_exeBase = (uintptr_t)GetModuleHandleA(nullptr);
    {
        uintptr_t a1 = FindPattern("FF 74 24 2C 8B CD E8 ? ? ? ? EB 55");
        uintptr_t a2 = FindPattern("FF 74 24 2C 8B CD E8 ? ? ? ? EB 3D");
        if (a1 && a2 && CallDest(a1 + 6) == CallDest(a2 + 6)) {
            *(uintptr_t*)&Orig_AimToFollow = CallDest(a1 + 6);
            PatchCall(a1 + 6, (void*)&Hook_AimToFollow);
            PatchCall(a2 + 6, (void*)&Hook_AimToFollow);
            Log("aim camera keep hook installed");
            uintptr_t g1 = CallDest((uintptr_t)Orig_AimToFollow + 0x20), g2 = CallDest((uintptr_t)Orig_AimToFollow + 0x2D);
            uintptr_t fa = FindPattern("80 7C 24 11 00 74 17 8B CD E8 ? ? ? ? 85 C0 74 0C 6A 00 6A 00 56");
            uintptr_t s1 = FindPattern("85 F6 0F 85 ? ? ? ? 8B CD E8 ? ? ? ? 85 C0 0F 84 ? ? ? ? FF 74 24 38");
            uintptr_t s2 = FindPattern("84 F6 74 1B 84 DB 75 17 8B CD E8 ? ? ? ? 85 C0 74 2A");
            if (g1 && g1 == g2 && fa && s1 && s2 && CallDest(s1 + 10) == CallDest(s2 + 10)) {
                *(uintptr_t*)&Game_MgrGetCam = g1;
                *(uintptr_t*)&Game_FollowToAim = CallDest(fa + 9);
                *(uintptr_t*)&Orig_CreateScopeCam = CallDest(s1 + 10);
                uintptr_t rel = CallDest((uintptr_t)Orig_AimToFollow + 0x1FA);
                if (rel && *(uint32_t*)rel == 0x013C818A) *(uintptr_t*)&Game_CamRelease = rel;
                Log(Game_CamRelease ? "camera release found" : "camera release not found");
                PatchCall(s1 + 10, (void*)&Hook_CreateScopeCam);
                PatchCall(s2 + 10, (void*)&Hook_CreateScopeCam);
                Log("sniper scope swap hooks installed");
                if (uintptr_t rw = FindPattern("6A 00 E8 ? ? ? ? 83 C4 04 8D 88 B0 02 00 00 E8 ? ? ? ? FF 70 18 E8 ? ? ? ? 83 C4 04 8B C8 E8")) {
                    if (CallDest(rw + 16) == (uintptr_t)Game_GetCurrentWeapon) { PatchCall(rw + 16, (void*)&Hook_ScopeRenderWeapon); Log("scope render guard installed"); }
                    if (uintptr_t ff = FindPattern("8A 46 08 24 01 A2 ? ? ? ?")) { g_scopeFxFlag = *(uint8_t**)(ff + 6); Log("scope overlay flag found"); }
                    else Log("scope render guard: unexpected call");
                } else Log("scope render guard site not found");
            } else Log("sniper scope sites not found");
        } else Log("aim camera switch sites not found");
    }
    *(uintptr_t*)&Orig_CamInterpInit = FindPattern("8A 44 24 14 53 56 57 8B F9 84 C0 88 87 40 01 00 00 8B 1D");
    *(uintptr_t*)&Orig_CamCreate     = FindPattern("53 8B D9 56 33 F6 83 3B 3C 7C 13 6A 01 E8");
    if (Orig_CamInterpInit) {
        int a = PatchAllCallsTo((uintptr_t)Orig_CamInterpInit, (void*)&Hook_CamInterpInit);
        Log("camera blend hook: calls ", a);
    } else Log("camera blend hook not found");
    (void)&Hook_CamCreate;
    int nc = PatchAllCallsTo((uintptr_t)Orig_ComputeCover, (void*)&Hook_ComputeCover);
    int nu = PatchAllCallsTo((uintptr_t)Orig_UpdateCover, (void*)&Hook_UpdateCover);
    Log("cover compute calls hooked: ", nc, " / ", nu);
    if (uintptr_t sw = FindPattern("0F BE 87 11 02 00 00 0F BE 8F 14 02 00 00 80 A7 00 02 00 00 FE 03 C8 8D 04 89 8D 04 C5"))
        InitShoulderSwap(sw);
    else Log("shoulder swap: camera site not found");
    if (uintptr_t ap = FindPattern("56 8B F1 8B 46 14 C7 06 ? ? ? ? 83 F8 FF 74 09 50 E8")) {
        g_animPlayerVt = *(uintptr_t*)(ap + 8);
        uintptr_t* slot = (uintptr_t*)g_animPlayerVt + 2;
        *(uintptr_t*)&Orig_ClipStep = *slot;
        PatchPtr(slot, (uintptr_t)&Hook_ClipStep);
        Log("cover animation speed hook installed");
    } else Log("cover animation speed: anim player not found");
    PatchCall(exd + 2, (void*)&Hook_ExitIsDucking);
    if (uintptr_t ar = FindPattern("8D 44 24 44 50 56 8B CF E8 ? ? ? ? 84 C0 74 0E F3 0F 10 47 40")) {
        *(uintptr_t*)&Orig_CoverArrived = CallDest(ar + 8);
        int n = Orig_CoverArrived ? PatchAllCallsTo((uintptr_t)Orig_CoverArrived, (void*)&Hook_CoverArrived) : 0;
        Log("cover entry arrival hooks: ", n);
    } else Log("cover entry arrival site not found");
    PatchCall(spread + 8, (void*)&Hook_SpreadDucking);
    PatchCall(reticle + 2, (void*)&Hook_ReticleDucking);
    PatchCall(reticle + 30, (void*)&Hook_ReticleHasCoverTask);
    {   // NPC reactions (optional)
        uintptr_t aim = FindPattern("8B 4C 24 0C 8D 44 24 20 50 8D 44 24 34 50 52 57 E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? 6A 01 6A 00 8D 44 24 28 50 8D 44 24 3C 50 57 E8");
        uintptr_t bsy = FindPattern("8B 8F 24 02 00 00 8D 44 24 0B 50 8D 44 24 18 50 E8 ? ? ? ? 84 C0 74 0B 83 7C 24 14 12");
        uintptr_t evt = FindPattern("57 8D 4C 24 34 E8 ? ? ? ? 6A 01 6A 00 8D 44 24 38 50 8B 44 24 18 8B 88 24 02 00 00 81 C1 84 00 00 00 E8 ? ? ? ? 8D 4C 24 30 C7 46 38 00 00 00 00 E8");
        uintptr_t grp = FindPattern("E8 ? ? ? ? 85 C0 0F 84 ? ? ? ? 57 FF 74 24 10 E8 ? ? ? ? 83 C4 08 84 C0");
        if (aim && bsy && evt && grp) {
            *(uintptr_t*)&Game_GetPedGroup     = CallDest(grp);
            *(uintptr_t*)&Game_IsInGroupOf     = CallDest(grp + 18);
            *(uintptr_t*)&Game_GetAimPoints    = CallDest(aim + 16);
            *(uintptr_t*)&Game_FindAimTarget   = CallDest(aim + 44);
            *(uintptr_t*)&Game_HasActiveEvent  = CallDest(bsy + 16);
            *(uintptr_t*)&Game_CanSeePosition  = CallDest(see + 11);
            *(uintptr_t*)&Game_GunAimedAt_ctor = CallDest(evt + 5);
            *(uintptr_t*)&Game_GunAimedAt_dtor = CallDest(evt + 51);
            uintptr_t add = CallDest(evt + 35);
            if (Game_GetPedGroup && Game_IsInGroupOf && Game_GetAimPoints && Game_FindAimTarget && Game_HasActiveEvent &&
                Game_CanSeePosition && Game_GunAimedAt_ctor && Game_GunAimedAt_dtor && add) {
                *(uintptr_t*)&Game_AddEvent = add; Log("NPC aim reactions enabled");
            }
        }
        if (!Game_AddEvent) Log("NPC aim reactions not available");
    }
    InitDriveBy(see);
    if (uintptr_t ws = FindPattern("6A 01 E8 ? ? ? ? 83 C4 04 84 C0 0F 85 ? ? ? ? E8 ? ? ? ? 84 C0 0F 85 ? ? ? ? 8D 8F 18 27 00 00")) {
        *(uintptr_t*)&Orig_SwitchAimCheck = CallDest(ws + 2);
        PatchCall(ws + 2, (void*)&Hook_SwitchAimCheck);
        Log("weapon switching while aiming enabled (on foot / cover)");
        if (uintptr_t cs = FindPattern("80 3D ? ? ? ? 00 74 09 E8 ? ? ? ? 84 C0 74 07 8B CE E8")) {
            *(uintptr_t*)&Orig_WeaponSelect = CallDest(cs + 20);
            PatchCall(cs + 20, (void*)&Hook_WeaponSelect);
        }
    } else Log("weapon switch site not found");
    Log("hooks installed");
}

extern "C" int __stdcall DllMain(void* hModule, uint32_t reason, void*) {
    if (reason == 1) { DisableThreadLibraryCalls(hModule); LoadConfig(hModule); InitLog(); Init(); }
    return 1;
}
