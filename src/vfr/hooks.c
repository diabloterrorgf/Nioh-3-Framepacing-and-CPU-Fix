/* H1 (clock entry) and H2 (main update entry) detours.
   Each computes its own real-time factor from the QPC delta between its own entries,
   guarded by an interlocked overlap counter in the stub data page. Overlapping calls change
   no state. The P1/P2 stubs read r1/r2 from the data page; they are only non-1 on ticks the
   lockstep gate armed. */
#include "common.h"
#include "lockstep.h"
#include "resolve.h"
#include "consumer_fix.h"

void *h1_tramp, *h2_tramp;
volatile LONG64 g_ovl_h1_total, g_ovl_h2_total;
volatile LONG64 g_stat_updates, g_stat_scaled, g_stat_r_milli;
/* Nioh3Fix: thread and fps mode (120/60 fixed cap) of the latest update (H1 entry) */
volatile LONG g_h1_tid, g_h1_fps;

static factor_state g_st1, g_st2;   /* touched only inside the non-overlapping section */

typedef uint64_t (*fn4_t)(void *, void *, void *, void *);

/* Reset a factor state when the gate re-armed or disarmed (epoch change), keeping its accounting. */
static void sync_epoch(factor_state *st, LONG *epoch)
{
    LONG current = InterlockedCompareExchange(&g_factor_epoch, 0, 0);
    if (*epoch != current) {
        double lost = st->time_lost, gained = st->time_gained;
        factor_init(st); st->time_lost = lost; st->time_gained = gained; *epoch = current;
    }
}

uint64_t h1_detour(uint8_t *clk, void *a2, void *a3, void *a4)
{
    if (gate_tls_get()==1) return ((fn4_t)h1_tramp)(clk,a2,a3,a4);
    DWORD le = GetLastError();
    long n = factor_gate_enter(g_stubs.ovl_h1);
    if (n==1) InterlockedExchange(&g_l3_tick,lockstep_h1(clk,a2,GetCurrentThreadId()));
    int64_t now = qpc_now();
    uint64_t frame = (uint64_t)InterlockedIncrement64(&g_frame);
    if (n > 1) {
        /* another worker is inside: change nothing, P1 keeps using the current r1 */
        InterlockedIncrement64(&g_ovl_h1_total);
    } else {
        int32_t fps = *(volatile int32_t *)(clk + g_resolved.fps);
        g_h1_tid = (LONG)GetCurrentThreadId(); g_h1_fps = fps;
        int en = g_l3_tick != 0;
        static LONG epoch;
        sync_epoch(&g_st1, &epoch);
        float rv = factor_update(&g_st1, &g_cfg.fc, now, g_qpf, fps, en, NULL);
        *g_stubs.r1 = rv;
        consumer_fix_publish(rv,frame,en);
        InterlockedIncrement64(&g_stat_updates);
        if (en) { InterlockedIncrement64(&g_stat_scaled); InterlockedAdd64(&g_stat_r_milli, (LONG64)(rv * 1000.0f)); }
    }
    SetLastError(le);
    int tls_before=gate_tls_get();
    if (n>1) gate_tls_set(1); /* original math on an overlapping worker */
    uint64_t ret = ((fn4_t)h1_tramp)(clk, a2, a3, a4);
    if (n>1) gate_tls_set(tls_before);
    factor_gate_leave(g_stubs.ovl_h1);
    return ret;
}

uint64_t h2_detour(void *self, uint8_t *S, void *a3, void *a4)
{
    if (gate_tls_get()==1) return ((fn4_t)h2_tramp)(self,S,a3,a4);
    DWORD le = GetLastError();
    int64_t now = qpc_now();
    long n = factor_gate_enter(g_stubs.ovl_h2);
    int paired_en=n==1 ? lockstep_h2(GetCurrentThreadId()) : 0;
    if (n > 1) {
        InterlockedIncrement64(&g_ovl_h2_total);
    } else {
        static LONG epoch;
        sync_epoch(&g_st2, &epoch);
        *g_stubs.r2 = factor_update(&g_st2, &g_cfg.fc, now, g_qpf, *(volatile int32_t *)(S + g_resolved.s_fps), paired_en, NULL);
    }
    SetLastError(le);
    int tls_before=gate_tls_get();
    if (n>1) gate_tls_set(1);
    uint64_t ret = ((fn4_t)h2_tramp)(self, S, a3, a4);
    if (n>1) gate_tls_set(tls_before);
    factor_gate_leave(g_stubs.ovl_h2);
    return ret;
}

void hooks_init_state(void)
{
    factor_init(&g_st1);
    factor_init(&g_st2);
    lockstep_init();
}
