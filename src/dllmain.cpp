// Nioh3Fix - dinput8.dll proxy that fixes Nioh 3's frame-pacing judder and busy-wait CPU burn.
// Windows APIs are hooked by export; game code is found by byte signature at run time (after SteamStub
// has decrypted it) and anything not found stays untouched.
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "MinHook.h"
#include "nioh3fix.h"

Config g_cfg;
LARGE_INTEGER g_qpf;
static wchar_t g_dir[MAX_PATH];
static uintptr_t g_exeBase, g_exeEnd, g_textBase, g_textEnd;

static inline bool FromGame(const void* p) { auto a = (uintptr_t)p; return a >= g_exeBase && a < g_exeEnd; }
// True when [p, p+n) lies inside the game's .text, so signature bytes there can be read.
static inline bool InText(const void* p, size_t n) { auto a = (uintptr_t)p; return a >= g_textBase && a <= g_textEnd && n <= g_textEnd - a; }

// ---------------------------------------------------------------- logging (buffered, written by the service thread)
static SRWLOCK g_logLock = SRWLOCK_INIT;
static std::string g_logBuf;
static FILE* g_log;

void Log(const char* fmt, ...) {
    char line[1024];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = _snprintf_s(line, _TRUNCATE, "%02u:%02u:%02u.%03u %5lu ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt);
    vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    AcquireSRWLockExclusive(&g_logLock);
    if (g_logBuf.size() < (1u << 20)) { g_logBuf += line; g_logBuf += '\n'; }
    ReleaseSRWLockExclusive(&g_logLock);
}

static FILE* OpenLog() {
    wchar_t p[MAX_PATH];
    swprintf_s(p, L"%s\\nioh3fix.log", g_dir);
    if (FILE* f = _wfsopen(p, L"w", _SH_DENYWR)) return f;
    // game folder not writable (e.g. under Program Files): use %LOCALAPPDATA%\Nioh3Fix
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", p, MAX_PATH)) return nullptr;
    wcscat_s(p, L"\\Nioh3Fix"); CreateDirectoryW(p, nullptr);
    wcscat_s(p, L"\\nioh3fix.log");
    return _wfsopen(p, L"w", _SH_DENYWR);
}

// ---------------------------------------------------------------- high-resolution nap
static thread_local HANDLE t_timer;
static HANDLE ThreadTimer() {
    if (!t_timer) t_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return t_timer;
}
void NapUs(int us) {
    HANDLE t = ThreadTimer();
    LARGE_INTEGER due; due.QuadPart = -(int64_t)us * 10;
    if (t && SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(t, INFINITE);
    else SwitchToThread();
}

// ---------------------------------------------------------------- Fix F: update thread waits
// The update thread (a job worker, usually KIDSTaskScheduler0) burns a core in two Sleep(0) waits.
//  - Variable-timestep gate (waiting for the render heartbeat): the next update is scaled by measured real
//    time, so a short nap costs no game time. Nap in small slices.
//  - Native 120/60 Hz pacer (while (now - prev < period) help_jobs/Sleep(0); prev = now): it schedules
//    relative to its actual wake-up, so any lateness is lost game time. Nap only until 1 ms before the tick
//    is due (read from the game's own timing struct) and let the original spin hit it exactly. Only the
//    pacer's own call path is touched (identified by the help call's return address), never the job waits
//    inside an update.
static bool UpdateThreadNap() {
    if (!g_cfg.updateNap || (DWORD)g_h1_tid != GetCurrentThreadId()) return false;
    if (gate_tls_get() == 1) {                      // inside the variable-timestep gate
        if (!g_l3_tick) return false;
        NapUs(g_cfg.gateNapUs);
        return true;
    }
    void* fr[2];
    uintptr_t timing = g_pacer_timing;
    if (!timing || !g_pacer_help_ret || RtlCaptureStackBackTrace(1, 2, fr, nullptr) < 2 || (uintptr_t)fr[1] != g_pacer_help_ret) return false;
    int fps = *(volatile int32_t*)(timing + 0x30);
    if (fps != 120 && fps != 60 && fps != 30) return false;
    int64_t now = Qpc(), due = *(volatile int64_t*)timing + g_qpf.QuadPart / fps - UsToTicks(1000);
    if (now >= due) return false;
    int64_t us = (due - now) * 1000000 / g_qpf.QuadPart;
    NapUs((int)(us < 300 ? us : 300));
    return true;
}

// ---------------------------------------------------------------- Fix G: render thread GPU-fence poll
// The render thread polls its GPU fence with Sleep(0) (Sleep(0); if (!fence_done()) loop), a full core while
// GPU-bound. After a short spin, nap in small slices. Matched by the code at the return address:
// mov rdx,rdi ; mov rcx,rsi ; call fence_done ; test al,al ; je loop
static bool IsFenceWaitSite(const BYTE* r) {
    static thread_local const BYTE* known;
    if (r == known) return true;
    bool ok = InText(r, 14) && r[0] == 0x48 && r[1] == 0x8B && r[2] == 0xD7 && r[3] == 0x48 && r[4] == 0x8B &&
              r[5] == 0xCE && r[6] == 0xE8 && r[11] == 0x84 && r[12] == 0xC0 && r[13] == 0x74;
    if (ok && !known) { known = r; Log("render fence wait found at exe+0x%llX", (uintptr_t)r - g_exeBase); }
    return ok;
}
static thread_local int64_t t_fenceStart, t_fenceLast;
static bool FenceNap(const BYTE* ret) {
    if (!g_cfg.fenceNap || !IsFenceWaitSite(ret)) return false;
    int64_t now = Qpc();
    if (now - t_fenceLast > UsToTicks(50)) t_fenceStart = now;      // a new wait
    t_fenceLast = now;
    if (now - t_fenceStart < UsToTicks(g_cfg.fenceSpinUs)) return false;
    NapUs(g_cfg.fenceNapUs);
    t_fenceLast = Qpc();
    return true;
}

typedef void(WINAPI* Sleep_t)(DWORD);
static Sleep_t oSleep;
static void WINAPI hkSleep(DWORD ms) {
    const BYTE* ret = (const BYTE*)_ReturnAddress();
    if (ms == 0 && FromGame(ret)) {
        DWORD le = GetLastError();
        bool napped = UpdateThreadNap() || FenceNap(ret);
        SetLastError(le);
        if (napped) return;
    }
    oSleep(ms);
}

// ---------------------------------------------------------------- Fix H: render thread queue drain
// The render thread drains its command queue with a pure busy loop (do { busy = queue_has_work(q); }
// while (busy); - no pause, no yield), ~1.7 ms per frame. We hook only the queue check (a leaf) and, when
// called from that loop and still busy after a short spin, nap a slice.
typedef bool(__fastcall* QueueBusy_t)(void*);
static QueueBusy_t oQueueBusy;
static const BYTE* g_drainRet;                   // return address inside the drain loop
static thread_local int64_t t_drainStart, t_drainLast;
static bool __fastcall hkQueueBusy(void* q) {
    bool busy = oQueueBusy(q);
    if (busy && g_cfg.drainNap && _ReturnAddress() == g_drainRet) {
        int64_t now = Qpc();
        if (now - t_drainLast > UsToTicks(50)) t_drainStart = now;   // a new drain wait
        if (now - t_drainStart >= UsToTicks(g_cfg.drainSpinUs)) NapUs(g_cfg.drainNapUs);
        else YieldProcessor();
        t_drainLast = Qpc();
    }
    return busy;
}

// Finds `mov rcx,rbx ; call queue_busy ; mov [g],al ; movzx eax,[g] ; test al,al ; jne loop` in .text.
// Returns false while the code may still be encrypted (no match yet), true once decided.
static bool InstallDrainHook() {
    static const BYTE leaf[] = { 0x8B, 0x41, 0x28, 0x39, 0x41, 0x24, 0x74, 0x03, 0xB0, 0x01, 0xC3 };  // queue_busy
    const BYTE* p = (const BYTE*)g_textBase; size_t n = g_textEnd - g_textBase;
    const BYTE* hit = nullptr; int hits = 0;
    for (size_t k = 0; k + 24 <= n; k++) {
        const BYTE* s = p + k;
        if (s[0] == 0x48 && s[1] == 0x8B && s[2] == 0xCB && s[3] == 0xE8 && s[8] == 0x88 && s[9] == 0x05 &&
            s[14] == 0x0F && s[15] == 0xB6 && s[16] == 0x05 && s[21] == 0x84 && s[22] == 0xC0 && s[23] == 0x75) {
            const BYTE* target = s + 8 + *(const int32_t*)(s + 4);
            if (InText(target, sizeof(leaf)) && !memcmp(target, leaf, sizeof(leaf))) { hit = s; hits++; }
        }
    }
    if (hits == 0) return false;
    if (hits != 1) { Log("drain loop: %d matches, Fix H passive", hits); return true; }
    BYTE* target = (BYTE*)hit + 8 + *(const int32_t*)(hit + 4);
    g_drainRet = hit + 8;
    MH_STATUS a = MH_CreateHook(target, (void*)hkQueueBusy, (void**)&oQueueBusy);
    MH_STATUS b = a == MH_OK ? MH_EnableHook(target) : a;
    Log("drain loop at exe+0x%llX, queue check exe+0x%llX hooked: %s", (uintptr_t)g_drainRet - g_exeBase,
        (uintptr_t)target - g_exeBase, MH_StatusToString(b));
    return true;
}

static bool TextExecutable() {
    for (uintptr_t at = g_textBase; at < g_textEnd;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void*)at, &m, sizeof(m)) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            !(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
        at = (uintptr_t)m.BaseAddress + m.RegionSize;
    }
    return true;
}

// SteamStub decrypts .text after we load; game-code fixes are found once it is readable.
static DWORD WINAPI LateInitThread(void*) {
    for (int i = 0; i < 120; i++, Sleep(1000)) {
        if (!TextExecutable()) continue;
        if (InstallDrainHook()) return 0;
    }
    Log("drain loop not found, Fix H passive");
    return 0;
}

// ---------------------------------------------------------------- Fix D / Fix E: render thread clock reads
// Fix D: the render thread polls "may the next frame start?" and, while not, busy-waits exactly 1 ms with
// `while ((QPC() - start) / freq < 0.001f) {}` (start in rbx, the wait in xmm6) before re-checking - a whole
// core at priority 15. We nap one short slice and report the wait as elapsed, so the work queue is
// re-checked ~10x sooner and the core is free in between.
// Fix E: the render thread's once-per-frame clock read is where the pacer / hybrid controller runs.
typedef BOOL(WINAPI* QPC_t)(LARGE_INTEGER*);
static QPC_t oQPC;
extern "C" BOOL WINAPI QpcHookStub(LARGE_INTEGER*);

// mov rcx,[rbp-60h] ; ... sub rcx,rbx at +0xE ; ... comiss xmm6,xmm0 at +0x65
static bool IsLimiterSite(const BYTE* ret) {
    static thread_local const BYTE* known; static thread_local const BYTE* bad[8]; static thread_local int nbad;
    if (ret == known) return true;
    for (int i = 0; i < nbad; i++) if (bad[i] == ret) return false;
    bool ok = InText(ret, 0x68) &&
        ret[0] == 0x48 && ret[1] == 0x8B && ret[2] == 0x4D && ret[3] == 0xA0 &&
        ret[0xE] == 0x48 && ret[0xF] == 0x2B && ret[0x10] == 0xCB &&
        ret[0x65] == 0x0F && ret[0x66] == 0x2F && ret[0x67] == 0xF0;
    if (ok) { if (!known) Log("render wait loop found at exe+0x%llX", (uintptr_t)ret - g_exeBase); known = ret; }
    else if (nbad < 8) bad[nbad++] = ret;
    return ok;
}

// mov rbx,[rsp+68h] ; mov [rsp+38h],rbx ; jmp +12h ; call <timeGetTime path> ; movsxd rbx,eax
static bool IsFrameStartSite(const BYTE* ret) {
    static thread_local const BYTE* known; static thread_local const BYTE* bad[16]; static thread_local int nbad;
    if (ret == known) return true;
    for (int i = 0; i < nbad; i++) if (bad[i] == ret) return false;
    static const BYTE sig[] = { 0x48, 0x8B, 0x5C, 0x24, 0x68, 0x48, 0x89, 0x5C, 0x24, 0x38, 0xEB, 0x12, 0xE8 };
    bool ok = InText(ret, 20) && !memcmp(ret, sig, sizeof(sig)) && ret[17] == 0x48 && ret[18] == 0x63 && ret[19] == 0xD8;
    if (ok) { if (!known) Log("frame start found at exe+0x%llX", (uintptr_t)ret - g_exeBase); known = ret; }
    else if (nbad < 16) bad[nbad++] = ret;
    return ok;
}

extern "C" BOOL WINAPI QpcHookC(LARGE_INTEGER* out, const BYTE* ret, int64_t start, uint32_t waitBits) {
    if (!FromGame(ret)) return oQPC(out);
    if (IsFrameStartSite(ret)) Pacer_FrameStart();
    float wait; memcpy(&wait, &waitBits, 4);
    if (g_cfg.gatePollUs > 0 && wait > 0.f && wait < 0.5f && IsLimiterSite(ret)) {
        DWORD le = GetLastError();
        NapUs(g_cfg.gatePollUs);
        SetLastError(le);
        BOOL r = oQPC(out);
        int64_t due = start + (int64_t)((double)wait * g_qpf.QuadPart) + 1;
        if (out->QuadPart < due) out->QuadPart = due;
        return r;
    }
    return oQPC(out);
}

// ---------------------------------------------------------------- Fix B: busy message pump
// The main thread loops PeekMessageA(PM_REMOVE) forever with no wait. When the queue is empty we wait (up to
// pumpWaitUs) for any message/input, which wakes immediately when one arrives. The Steam overlay sits between
// the game and PeekMessageA, so the main thread is matched by id.
typedef BOOL(WINAPI* PeekMessageA_t)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessageA_t oPeekMessageA;
static thread_local int t_emptyPeeks;
static DWORD g_mainTid;

static BOOL WINAPI hkPeekMessageA(LPMSG msg, HWND hwnd, UINT mn, UINT mx, UINT rm) {
    BOOL r = oPeekMessageA(msg, hwnd, mn, mx, rm);
    if (r || !g_cfg.pumpFix || hwnd || mn || mx || !(rm & PM_REMOVE) || GetCurrentThreadId() != g_mainTid) {
        t_emptyPeeks = 0; return r;
    }
    HANDLE t = ++t_emptyPeeks >= 2 ? ThreadTimer() : nullptr;
    if (t) {
        DWORD le = GetLastError();
        LARGE_INTEGER due; due.QuadPart = -(int64_t)g_cfg.pumpWaitUs * 10;
        if (SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE))
            MsgWaitForMultipleObjectsEx(1, &t, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        SetLastError(le);
    }
    return r;
}

// ---------------------------------------------------------------- dinput8 proxy
static HMODULE g_realDinput;
static FARPROC Real(const char* name) {
    if (!g_realDinput) {
        wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat_s(p, L"\\dinput8.dll");
        g_realDinput = LoadLibraryW(p);
    }
    return g_realDinput ? GetProcAddress(g_realDinput, name) : nullptr;
}
extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, LPUNKNOWN u) {
    auto f = (HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN))Real("DirectInput8Create");
    return f ? f(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() { auto f = (HRESULT(WINAPI*)())Real("DllCanUnloadNow"); return f ? f() : S_FALSE; }
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) {
    auto f = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))Real("DllGetClassObject"); return f ? f(c, r, o) : E_FAIL;
}
HRESULT WINAPI Proxy_DllRegisterServer() { auto f = (HRESULT(WINAPI*)())Real("DllRegisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI Proxy_DllUnregisterServer() { auto f = (HRESULT(WINAPI*)())Real("DllUnregisterServer"); return f ? f() : E_FAIL; }
LPCVOID WINAPI Proxy_GetdfDIJoystick() { auto f = (LPCVOID(WINAPI*)())Real("GetdfDIJoystick"); return f ? f() : nullptr; }
}

// ---------------------------------------------------------------- config + service thread
static void LoadConfig() {
    wchar_t ini[MAX_PATH]; swprintf_s(ini, L"%s\\nioh3fix.ini", g_dir);
    auto I = [&](const wchar_t* k, int d, int lo, int hi) {
        int v = (int)GetPrivateProfileIntW(L"Nioh3Fix", k, d, ini);
        return v < lo ? lo : v > hi ? hi : v;
    };
    g_cfg.pumpFix = I(L"PumpFix", g_cfg.pumpFix, 0, 1);
    g_cfg.pumpWaitUs = I(L"PumpWaitUs", g_cfg.pumpWaitUs, 50, 16000);
    g_cfg.gatePollUs = I(L"GatePollUs", g_cfg.gatePollUs, 0, 1000);
    g_cfg.pacer = I(L"Pacer", g_cfg.pacer, 0, 120);
    g_cfg.pacerSpinUs = I(L"PacerSpinUs", g_cfg.pacerSpinUs, 0, 5000);
    g_cfg.pacerVblank = I(L"PacerVblank", g_cfg.pacerVblank, 0, 1);
    g_cfg.pacerPhaseUs = I(L"PacerPhaseUs", g_cfg.pacerPhaseUs, 0, 10000);
    g_cfg.vfr = I(L"VariableTimestep", g_cfg.vfr, 0, 1);
    g_cfg.vfrMinFps = I(L"VfrMinFps", g_cfg.vfrMinFps, 0, 120);
    g_cfg.vfrAllowOnline = I(L"VfrAllowOnline", g_cfg.vfrAllowOnline, 0, 1);
    g_cfg.updateNap = I(L"UpdateNap", g_cfg.updateNap, 0, 1);
    g_cfg.gateNapUs = I(L"GateNapUs", g_cfg.gateNapUs, 20, 1000);
    g_cfg.fenceNap = I(L"FenceNap", g_cfg.fenceNap, 0, 1);
    g_cfg.fenceSpinUs = I(L"FenceSpinUs", g_cfg.fenceSpinUs, 0, 5000);
    g_cfg.fenceNapUs = I(L"FenceNapUs", g_cfg.fenceNapUs, 20, 1000);
    g_cfg.drainNap = I(L"DrainNap", g_cfg.drainNap, 0, 1);
    g_cfg.drainSpinUs = I(L"DrainSpinUs", g_cfg.drainSpinUs, 0, 5000);
    g_cfg.drainNapUs = I(L"DrainNapUs", g_cfg.drainNapUs, 20, 1000);
}

static void VfrLogLine(const char* line) { Log("vfr: %s", line); }

// Writes the log, runs the variable-timestep core's housekeeping and re-reads nioh3fix.ini every 2 s,
// so settings apply live.
static DWORD WINAPI ServiceThread(void*) {
    for (int it = 1;; it++) {
        Sleep(50);
        vfr_service(VfrLogLine);
        if (it % 40 == 0) LoadConfig();
        if (it % 10 == 0) {
            std::string out;
            AcquireSRWLockExclusive(&g_logLock); out.swap(g_logBuf); ReleaseSRWLockExclusive(&g_logLock);
            if (!out.empty() && g_log) { fwrite(out.data(), 1, out.size(), g_log); fflush(g_log); }
        }
    }
}

// ---------------------------------------------------------------- init
static void Init(HMODULE self) {
    QueryPerformanceFrequency(&g_qpf);
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) *slash = 0;
    auto exe = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    g_exeBase = (uintptr_t)exe; g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".text", 6)) { g_textBase = g_exeBase + sec[i].VirtualAddress; g_textEnd = g_textBase + sec[i].Misc.VirtualSize; }
    LoadConfig();
    g_log = OpenLog();
    CloseHandle(CreateThread(nullptr, 0, ServiceThread, nullptr, 0, nullptr));
    Log("Nioh3Fix loaded. qpf=%lld PumpFix=%d GatePollUs=%d Pacer=%d VariableTimestep=%d VfrMinFps=%d UpdateNap=%d FenceNap=%d DrainNap=%d",
        g_qpf.QuadPart, g_cfg.pumpFix, g_cfg.gatePollUs, g_cfg.pacer, g_cfg.vfr, g_cfg.vfrMinFps, g_cfg.updateNap, g_cfg.fenceNap, g_cfg.drainNap);

    g_mainTid = GetCurrentThreadId();   // imports are resolved on the main thread
    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) { Log("MinHook: %s, nothing hooked", MH_StatusToString(mh)); return; }
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) u32 = LoadLibraryW(L"user32.dll");
    struct { const char* name; void* target; void* detour; void** orig; } hooks[] = {
        { "Sleep", (void*)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "Sleep"), (void*)hkSleep, (void**)&oSleep },
        { "PeekMessageA", u32 ? (void*)GetProcAddress(u32, "PeekMessageA") : nullptr, (void*)hkPeekMessageA, (void**)&oPeekMessageA },
        { "QueryPerformanceCounter", (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryPerformanceCounter"), (void*)QpcHookStub, (void**)&oQPC },
    };
    for (auto& h : hooks) {
        MH_STATUS s = h.target ? MH_CreateHook(h.target, h.detour, h.orig) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (s == MH_OK) s = MH_EnableHook(h.target);
        Log("hook %s: %s", h.name, MH_StatusToString(s));
    }
    Pacer_Init();
    g_gate_tls = TlsAlloc();     // reserve a low TLS slot now (the core reads slots 0..63 inline); the game takes many later
    vfr_start(self);             // its own thread waits for SteamStub to decrypt the code
    CloseHandle(CreateThread(nullptr, 0, LateInitThread, nullptr, 0, nullptr));
}

BOOL WINAPI DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); Init(h); }
    return TRUE;
}
