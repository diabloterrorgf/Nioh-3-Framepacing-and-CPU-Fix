// Shared config and helpers for Nioh3Fix.
#pragma once
#include <windows.h>
#include <cstdint>

struct Config {
    int pumpFix = 1;         // Fix B: the window thread waits for messages instead of polling
    int pumpWaitUs = 500;    // Fix B: max wait per idle pump iteration (wakes instantly on messages)
    int gatePollUs = 100;    // Fix D: render thread work-poll slice (0 = keep the game's 1 ms busy-wait)
    int pacer = 1;           // Fix E: 0 = off, 1 = auto (120/60/40/30), N = fixed N fps (must divide 120)
    int pacerSpinUs = 700;   // Fix E: spin (instead of timer-sleep) for the last part of each wait
    int pacerVblank = 1;     // Fix E: align frame slots to the display's vblank phase
    int pacerPhaseUs = 500;  // Fix E: release frames this long after a vblank
    int vfr = 1;             // variable timestep (ported nioh3fps core) while fps >= vfrMinFps
    int vfrMinFps = 58;      // below this, switch to divisor pacing (40/30) to keep full game speed
    int vfrAllowOnline = 0;  // keep variable timestep on in online/co-op sessions
    int updateNap = 1;       // Fix F: deadline-aware naps for the update thread's waits
    int gateNapUs = 100;     // Fix F: nap slice while waiting for the render heartbeat
    int fenceNap = 1;        // Fix G: nap in the render thread's GPU-fence Sleep(0) poll
    int fenceSpinUs = 300;   // Fix G: keep the original spin this long first
    int fenceNapUs = 100;    // Fix G: nap slice after that
    int drainNap = 1;        // Fix H: nap in the render thread's command-queue drain loop
    int drainSpinUs = 200;   // Fix H: keep the original spin this long first
    int drainNapUs = 100;    // Fix H: nap slice after that
};
extern Config g_cfg;
extern LARGE_INTEGER g_qpf;

inline int64_t Qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
inline int64_t UsToTicks(int64_t us) { return us * g_qpf.QuadPart / 1000000; }
void Log(const char* fmt, ...);
void NapUs(int us);

void Pacer_Init();
void Pacer_FrameStart();

// variable-timestep core (vfr\, C)
extern "C" {
void vfr_start(HMODULE self);
void vfr_service(void (*sink)(const char* line));   // log drain, toggle key, stats; called every ~50 ms
int gate_tls_get(void);
extern DWORD g_gate_tls;
extern volatile LONG g_vfr_allow, g_vfr_allow_online, g_l3_tick, g_h1_tid, g_h1_fps;
extern volatile uintptr_t g_pacer_timing;   // native pacer timing struct: +0 previous tick QPC, +0x30 tick rate
extern uintptr_t g_pacer_help_ret;          // return address of the native pacer's help-jobs call
extern volatile LONG64 g_render_seq;        // render packet takes = rendered frames
extern int g_l2_ready;
}
