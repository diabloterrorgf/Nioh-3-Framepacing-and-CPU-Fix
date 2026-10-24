// Fix E: frame pacer at the simulation stage.
// Nioh 3 simulates in fixed 1/120 s ticks and starts each frame whenever the pipeline lets it. Below
// 120 fps a frame therefore advances 1, 2 or 3 ticks depending on when it happened to start, so motion
// jumps several times a second even at a steady fps. We hold the Game thread's once-per-frame clock
// read until the next slot of an absolute timeline at a rate that divides 120 (120/60/40/30): every
// frame then advances the same number of ticks. Game logic and game speed are unchanged.
//
// Auto mode picks the fastest such rate the PC can hold and, when it can, one that also lasts a whole
// number of display refreshes, so frames are shown evenly too. Pacing hides how long frames really take, so
// GPU load comes from the kernel's per-process GPU running time (what Task Manager shows) and CPU load
// from how long the Game thread needs between its release and its next frame start.
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dwmapi.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include "nioh3fix.h"
#include "pacer_rates.h"

// Only the "120 (fixed)" cap is paced: at 60 every frame is already one tick and the game's own
// limiter releases frames on its 60 Hz schedule.
static int g_rateIdx = 1;                 // auto mode starts at 60 and climbs if there is headroom
static std::atomic<int> g_displayHz{ 0 }; // refresh rate of the monitor showing the game (0 = unknown)
static HANDLE g_timer;

// The game's tick rate: its fps mode as read at each update (0 = not known yet).
static int TickHz() { return g_h1_fps; }

// ---------------------------------------------------------------- GPU utilisation (kernel statistics)
// Per-process GPU engine running time from the WDDM kernel (what Task Manager shows), for every adapter,
// so it works the same on any GPU vendor and on hybrid (integrated + discrete) systems.
static std::atomic<float> g_gpuUtil{ -1.f };    // busiest engine's busy fraction over the last ~1 s

struct Engine { LUID luid; ULONG node; };
static Engine g_eng[64];
static int g_nEng;

static void FindEngines() {
    D3DKMT_ENUMADAPTERS2 ea = {};
    if (D3DKMTEnumAdapters2(&ea) != 0 || !ea.NumAdapters) return;
    D3DKMT_ADAPTERINFO info[16] = {};
    ea.NumAdapters = std::min<ULONG>(ea.NumAdapters, 16); ea.pAdapters = info;
    if (D3DKMTEnumAdapters2(&ea) != 0) return;
    for (ULONG a = 0; a < ea.NumAdapters; a++) {
        D3DKMT_QUERYSTATISTICS qs = {};
        qs.Type = D3DKMT_QUERYSTATISTICS_ADAPTER; qs.AdapterLuid = info[a].AdapterLuid;
        if (D3DKMTQueryStatistics(&qs) == 0)
            for (ULONG n = 0; n < qs.QueryResult.AdapterInformation.NodeCount && g_nEng < 64; n++) g_eng[g_nEng++] = { info[a].AdapterLuid, n };
        D3DKMT_CLOSEADAPTER ca = { info[a].hAdapter }; D3DKMTCloseAdapter(&ca);
    }
}

static int64_t EngineRunningTime(const Engine& e) {
    D3DKMT_QUERYSTATISTICS qs = {};
    qs.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE; qs.AdapterLuid = e.luid; qs.hProcess = GetCurrentProcess();
    qs.QueryProcessNode.NodeId = e.node;
    return D3DKMTQueryStatistics(&qs) == 0 ? qs.QueryResult.ProcessNodeInformation.RunningTime.QuadPart : 0;
}

// ---------------------------------------------------------------- display refresh rate
// The current mode of the monitor the game window is on (exclusive fullscreen included).
static BOOL CALLBACK LargestGameWindow(HWND w, LPARAM out) {
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    RECT r;
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER) || !GetWindowRect(w, &r)) return TRUE;
    auto best = (std::pair<HWND, int64_t>*)out;
    int64_t area = (int64_t)(r.right - r.left) * (r.bottom - r.top);
    if (area > best->second) *best = { w, area };
    return TRUE;
}
static int DetectDisplayHz() {
    std::pair<HWND, int64_t> best{ nullptr, 0 };
    EnumWindows(LargestGameWindow, (LPARAM)&best);
    if (!best.first) return 0;
    MONITORINFOEXW mi = {}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(best.first, MONITOR_DEFAULTTONEAREST), &mi)) return 0;
    DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? (int)dm.dmDisplayFrequency : 0;   // 0/1 = "hardware default"
}
static void UpdateDisplayHz() {
    int hz = g_cfg.pacerDisplayHz > 0 ? g_cfg.pacerDisplayHz : DetectDisplayHz();
    if (!hz || hz == g_displayHz) return;
    g_displayHz = hz;
    char fits[64] = ""; int n = 0;
    for (int i = 0; i < kNumRates; i++)
        if (RateAllowed(i, hz, g_cfg.pacerFitDisplay != 0)) n += sprintf_s(fits + n, sizeof(fits) - n, "%s%d", n ? "/" : "", kRates[i]);
    Log("pacer: display %d Hz%s, auto rates %s", hz, g_cfg.pacerDisplayHz > 0 ? " (PacerDisplayHz)" : "", fits);
}

// Samples GPU load every 250 ms and the display refresh rate every 2 s.
static DWORD WINAPI MonitorThread(void*) {
    FindEngines();
    Log("pacer: %d GPU engines on all adapters", g_nEng);
    const int kWin = 4;                      // 4 x 250 ms window
    static int64_t run[kWin][64], wall[kWin];
    for (int i = 0;; i++, Sleep(250)) {
        if (i % 8 == 0) UpdateDisplayHz();
        if (!g_nEng) continue;
        int s = i % kWin;
        wall[s] = Qpc();
        for (int e = 0; e < g_nEng; e++) run[s][e] = EngineRunningTime(g_eng[e]);
        if (i < kWin) continue;
        int o = (i + 1) % kWin;              // oldest sample
        double dWall100ns = (double)(wall[s] - wall[o]) * 1e7 / g_qpf.QuadPart;
        double best = 0;
        for (int e = 0; e < g_nEng; e++) best = std::max(best, (double)(run[s][e] - run[o][e]));
        g_gpuUtil = (float)std::min(1.0, best / dWall100ns);   // RunningTime is in 100 ns units
    }
}

// ---------------------------------------------------------------- pacing at frame start
static float g_cpuMs[240]; static int g_cpuN, g_cpuPos;
static int64_t g_deadline, g_lastRelease, g_lastRateChange, g_lastVblankQuery, g_vblank, g_refresh, g_lastStat;
static int g_late[120], g_lateSum, g_latePos, g_statFrames, g_statLate;

static float Pct(const float* a, int n, double p) {
    if (!n) return 0; float t[240]; std::copy(a, a + n, t); std::sort(t, t + n);
    return t[std::min(n - 1, (int)(n * p))];
}

static void WaitUntil(int64_t t) {
    int64_t now = Qpc(), spin = UsToTicks(g_cfg.pacerSpinUs);
    if (t - now > spin) {
        if (!g_timer) g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        LARGE_INTEGER due; due.QuadPart = -(t - spin - now) * 10000000 / g_qpf.QuadPart;
        if (g_timer && SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, INFINITE);
    }
    while (Qpc() < t) YieldProcessor();
}

// Evaluated once a second. Sustained load decides the rate; loads, deaths and streaming spikes do not.
// Climbing is conservative (estimates at a lower rate are optimistic) and backs off after a failed try.
static int g_upVotes, g_downVotes;
static int64_t g_lastEval, g_lastUpshift, g_upBanUntil, g_upBackoffSec = 30;
static void ChooseRate(int64_t now) {
    int hz = g_displayHz; bool fit = g_cfg.pacerFitDisplay != 0;
    if (!RateAllowed(g_rateIdx, hz, fit)) {   // display changed: move to a rate that fits it
        int to = SlowerRate(g_rateIdx, hz, fit);
        if (to < 0) to = FasterRate(g_rateIdx, hz, fit);
        Log("pacer: %d -> %d fps (fits the %d Hz display)", kRates[g_rateIdx], kRates[to], hz);
        g_rateIdx = to; g_cpuN = 0; g_lateSum = 0; g_upVotes = g_downVotes = 0; g_deadline = 0;
        for (int& l : g_late) l = 0;
        return;
    }
    if (now - g_lastEval < g_qpf.QuadPart || g_cpuN < 60) return;
    g_lastEval = now;
    int faster = FasterRate(g_rateIdx, hz, fit), slower = SlowerRate(g_rateIdx, hz, fit);
    double period = 1000.0 / kRates[g_rateIdx];
    float util = g_gpuUtil;
    float gpu = util < 0 ? 0.f : (float)(util * period);   // GPU ms per frame at the current rate
    float cpu = Pct(g_cpuMs, g_cpuN, 0.90);
    float need = std::max(gpu, cpu);
    bool overload = g_lateSum > 120 * 40 / 100;
    // only real missed slots count against the current rate: under back-pressure the CPU estimate
    // approaches the period without any frame actually being late
    g_downVotes = g_lateSum > 120 / 5 ? g_downVotes + 1 : 0;
    g_upVotes = faster >= 0 && util >= 0 && g_lateSum == 0 && now > g_upBanUntil &&
                need < 1000.0 / kRates[faster] * 0.75 ? g_upVotes + 1 : 0;
    int next = g_rateIdx;
    if ((overload || g_downVotes >= 3) && slower >= 0) {
        next = slower;
        if (g_lastUpshift && now - g_lastUpshift < g_qpf.QuadPart * 30) {     // the last climb failed
            g_upBanUntil = now + g_qpf.QuadPart * g_upBackoffSec;
            g_upBackoffSec = std::min<int64_t>(g_upBackoffSec * 2, 600);
        }
    } else if (g_upVotes >= 5) {
        next = faster; g_lastUpshift = now;
    }
    if (next != g_rateIdx) {
        Log("pacer: %d -> %d fps (late %d/120, gpu %.2f ms/frame, p90 cpu %.2f ms)", kRates[g_rateIdx], kRates[next], g_lateSum, gpu, cpu);
        g_rateIdx = next; g_lastRateChange = now; g_cpuN = 0; g_lateSum = 0; g_upVotes = g_downVotes = 0;
        for (int& l : g_late) l = 0;
    }
}

// ---------------------------------------------------------------- hybrid with the variable-timestep core
// The ported nioh3fps core (vfr\) runs one game update per rendered frame, scaled by real time, which is
// smooth at any rate down to ~60 fps; below that its step is capped at 2x and the game would slow down.
// So: variable timestep while the frame rate holds VfrMinFps, divisor pacing (40/30, full speed) below it,
// and divisor pacing whenever the variable-timestep gate cannot arm (online without AllowOnline, menus...).
static bool g_divMode;
static int64_t g_hyWinStart, g_hyArmedSeen;
static int g_hyFrames, g_hyLowSecs, g_hyHighSecs;

// True when variable timestep is in charge of this frame (no divisor pacing).
static bool Hybrid(int64_t now) {
    g_vfr_allow_online = g_cfg.vfrAllowOnline;
    if (!g_cfg.vfr || !g_l2_ready) { g_vfr_allow = 0; return false; }
    if (g_l3_tick) g_hyArmedSeen = now;
    if (!g_cfg.pacer && g_divMode) { g_divMode = false; g_hyLowSecs = 0; }   // no pacing to fall back to
    // Count real frames with the core's render heartbeat (one per packet taken): under variable timestep the
    // render loop polls for the next packet and passes this clock read many times per frame.
    static int64_t s_seq0;
    if (!g_hyWinStart) { g_hyWinStart = now; s_seq0 = g_render_seq; }
    if (now - g_hyWinStart >= g_qpf.QuadPart) {                 // once a second
        int64_t seq = g_render_seq;
        g_hyFrames = (int)(seq - s_seq0); s_seq0 = seq;
        double fps = g_hyFrames * (double)g_qpf.QuadPart / (double)(now - g_hyWinStart);
        static int s_logSecs;
        if (++s_logSecs % 10 == 0)
            Log("hybrid: %.1f fps, gate %s, mode %s, VfrMinFps %d", fps, now - g_hyArmedSeen < g_qpf.QuadPart ? "armed" : "idle",
                g_divMode ? "divisor" : "variable", g_cfg.vfrMinFps);
        if (!g_divMode) {
            bool armed = now - g_hyArmedSeen < g_qpf.QuadPart;
            g_hyLowSecs = armed && g_cfg.pacer && fps < g_cfg.vfrMinFps ? g_hyLowSecs + 1 : 0;
            if (g_hyLowSecs >= 2) {
                g_divMode = true; g_hyHighSecs = 0;
                // the fastest rate the PC actually holds (and that fits the display)
                g_rateIdx = RateForFps(fps, g_displayHz, g_cfg.pacerFitDisplay != 0);
                g_cpuN = 0; g_deadline = 0;
                Log("hybrid: %.1f fps < %d -> divisor pacing at %d fps", fps, g_cfg.vfrMinFps, kRates[g_rateIdx]);
            }
        } else {
            double period = 1000.0 / kRates[g_rateIdx];
            float util = g_gpuUtil;
            double need = std::max(util < 0 ? 0.0 : util * period, (double)Pct(g_cpuMs, g_cpuN, 0.90));
            g_hyHighSecs = util >= 0 && g_cpuN >= 30 && need > 0 && 1000.0 / need > g_cfg.vfrMinFps + 8 ? g_hyHighSecs + 1 : 0;
            if (g_hyHighSecs >= 3) {
                g_divMode = false; g_hyLowSecs = 0;
                Log("hybrid: capacity ~%.0f fps -> variable timestep", 1000.0 / need);
            }
        }
        g_hyFrames = 0; g_hyWinStart = now;
    }
    g_vfr_allow = !g_divMode;
    return !g_divMode && now - g_hyArmedSeen < g_qpf.QuadPart / 2;
}

// Called on the render thread at its once-per-frame clock read, before the clock is actually read.
void Pacer_FrameStart() {
    if (Hybrid(Qpc())) { g_deadline = 0; g_lastRelease = 0; return; }
    // Pace only at the "120 (fixed)" cap; at 60 the game's own limiter already gives one tick per frame.
    if (!g_cfg.pacer || TickHz() != 120) { g_deadline = 0; return; }
    int fps = g_cfg.pacer == 1 ? kRates[g_rateIdx] : g_cfg.pacer;
    if (fps < 10 || 120 % fps) return;      // a fixed rate must divide 120
    int64_t now = Qpc(), period = g_qpf.QuadPart / fps;

    if (g_lastRelease) { g_cpuMs[g_cpuPos] = (float)((now - g_lastRelease) * 1000.0 / g_qpf.QuadPart); g_cpuPos = (g_cpuPos + 1) % 240; g_cpuN = std::min(g_cpuN + 1, 240); }
    if (!g_deadline || now > g_deadline + period) g_deadline = now;     // resync after a stall or a menu

    // keep slots at a fixed phase of the display's vblank when the period is a whole number of refreshes
    bool vblankAligned = false;
    if (g_cfg.pacerVblank) {
        if (now - g_lastVblankQuery > g_qpf.QuadPart) {
            DWM_TIMING_INFO ti = { sizeof(ti) };
            if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod) { g_vblank = ti.qpcVBlank; g_refresh = ti.qpcRefreshPeriod; }
            g_lastVblankQuery = now;
        }
        if (g_refresh) {
            double k = (double)period / g_refresh;
            if (std::fabs(k - std::floor(k + 0.5)) < 0.02) {
                int64_t rel = (g_deadline - g_vblank - UsToTicks(g_cfg.pacerPhaseUs)) % g_refresh;
                if (rel < 0) rel += g_refresh;
                g_deadline += rel > g_refresh / 2 ? g_refresh - rel : -rel;
                vblankAligned = true;
            }
        }
    }
    // Guard against the update thread's tick boundaries (its schedule is the native pacer's timing struct):
    // a frame starting right as a tick publishes races it and sees 1 or 3 ticks instead of 2. If the slot is
    // within 1.5 ms of a boundary, move the whole schedule by one refresh (or half a tick), which keeps the
    // vblank phase and lands well clear; both clocks are stable, so this rarely has to happen again.
    if (uintptr_t pt = g_pacer_timing) {
        int tf = *(volatile int32_t*)(pt + 0x30);
        int64_t prev = *(volatile int64_t*)pt;
        if ((tf == 120 || tf == 60) && prev > 0 && now - prev < g_qpf.QuadPart) {
            int64_t tp = g_qpf.QuadPart / tf;
            int64_t rel = (g_deadline - prev) % tp;
            if (rel < 0) rel += tp;
            if (std::min(rel, tp - rel) < UsToTicks(1500))
                g_deadline += vblankAligned && g_refresh < tp ? g_refresh : tp / 2;
        }
    }
    bool late = now > g_deadline + UsToTicks(500);
    if (!late) WaitUntil(g_deadline);
    g_lastRelease = Qpc();
    g_deadline += period;
    g_lateSum += late - g_late[g_latePos]; g_late[g_latePos] = late; g_latePos = (g_latePos + 1) % 120;
    g_statFrames++; g_statLate += late;

    if (g_cfg.pacer == 1) ChooseRate(g_lastRelease);
    if (g_lastRelease - g_lastStat > g_qpf.QuadPart * 10) {
        Log("pacer: %d fps, %d frames, %d late, gpu busy %.0f%%, p95 cpu %.2f ms", fps, g_statFrames, g_statLate,
            (float)g_gpuUtil * 100.f, Pct(g_cpuMs, g_cpuN, 0.95));
        g_statFrames = g_statLate = 0; g_lastStat = g_lastRelease;
    }
}

void Pacer_Init() {
    CloseHandle(CreateThread(nullptr, 0, MonitorThread, nullptr, 0, nullptr));
}
