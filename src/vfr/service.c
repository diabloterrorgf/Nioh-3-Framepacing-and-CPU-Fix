/* Housekeeping for the variable-timestep core, called by Nioh3Fix's service thread every ~50 ms:
   drains the log ring, polls the toggle key (edge-detected, only while the game window is in the
   foreground) and logs a short status line every 10 s. Game threads never wait on it. */
#include "common.h"
#include "lockstep.h"

extern int g_vfr_active;   /* init.c: hooks are live */

static int game_foreground(void)
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static int combo_down(const keycombo *k)
{
    int ctrl, shift, alt;
    if (!k->vk || !(GetAsyncKeyState(k->vk) & 0x8000)) return 0;
    ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return ctrl == k->ctrl && shift == k->shift && alt == k->alt;
}

static void status_line(void)
{
    static LONG64 updates0, scaled0, r0;
    LONG64 u = g_stat_updates, s = g_stat_scaled, r = g_stat_r_milli;
    LONG64 du = u - updates0, ds = s - scaled0, dr = r - r0;
    updates0 = u; scaled0 = s; r0 = r;
    log_printf("10 s: %lld updates, %.0f%% variable-timestep (mean step x%.3f), toggle %s, allowed %ld",
               (long long)du, du ? 100.0 * ds / du : 0.0, ds ? dr / 1000.0 / ds : 1.0,
               g_toggle ? "on" : "off", (long)g_vfr_allow);
}

static void (*g_sink)(const char *line);
static void sink_adapter(const char *line, void *ctx) { (void)ctx; g_sink(line); }

void vfr_service(void (*sink)(const char *line))
{
    static int prev_key;
    static DWORD last_status;
    int key = g_vfr_active && game_foreground() && combo_down(&g_cfg.toggle_key);
    if (key && !prev_key) {
        LONG v = InterlockedXor(&g_toggle, 1) ^ 1;
        log_printf("TOGGLE: variable timestep %s", v ? "ON" : "OFF");
    }
    prev_key = key;
    if (g_vfr_active) {
        DWORD now = GetTickCount();
        if (!last_status) last_status = now;
        if (now - last_status >= 10000) { last_status = now; status_line(); }
    }
    g_sink = sink;
    logring_drain(sink_adapter, NULL);
}
