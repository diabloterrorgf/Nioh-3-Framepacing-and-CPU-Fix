#include "lockstep.h"
#include "resolve.h"
#include <string.h>
int g_l2_ready;
volatile LONG g_vfr_allow=1, g_vfr_allow_online=0; /* written by Nioh3Fix (hybrid controller / ini) */
volatile LONG g_l3_tick, g_factor_epoch;
void *frame_tramp;
typedef uint64_t (*fn4)(void *,void *,void *,void *);
static gate_state G;
static volatile LONG h2_count, h2_tid;
static volatile LONG last_h1_tid;
static DWORD last_update_tid(void) { return (DWORD)__atomic_load_n(&last_h1_tid,__ATOMIC_ACQUIRE); }
static uint64_t hb_read;
static int64_t first_beat, last_beat, intervals[16];
static int64_t last_latched_beat; /* liveness survives median/overflow resets */
static unsigned interval_n, interval_pos;
typedef struct {
    uintptr_t mailbox;
    DWORD tid;
    int64_t first, last;
    uint64_t count;
    uint64_t packet;
    uint32_t frame;
    int64_t recent[21]; /* last 20 intervals, so old bursts cannot mask decay */
} hb_candidate;
static hb_candidate candidates[8], latched_health;
static LONG64 seen_ovl_h1,seen_ovl_h2;
static void median_reset(void)
{
    interval_n=interval_pos=0; first_beat=last_beat=0;
    memset(intervals,0,sizeof intervals);
}
void lockstep_init(void)
{
    gate_init(&G); median_reset(); memset(candidates,0,sizeof candidates);
    memset(&latched_health,0,sizeof latched_health);
    last_latched_beat=0;
    hb_read=(uint64_t)__atomic_load_n(&g_hb_write,__ATOMIC_ACQUIRE);
    h2_count=h2_tid=last_h1_tid=0;
    seen_ovl_h1=InterlockedCompareExchange64(&g_ovl_h1_total,0,0);
    seen_ovl_h2=InterlockedCompareExchange64(&g_ovl_h2_total,0,0);
}
/* Called only by the non-overlapping H1 reader. The leaf records all pairs;
   H1 owns latch decisions, candidate timing, history and gate resets. */
static void candidate_observe(hb_candidate *c,const hb_rec *r,int reset_on_gap)
{
    /* Repeat flags in the leaf are global to the selected pair. Independently
       track each take pair's frame progress, using packets only when both
       frames are unavailable. */
    if (c->count && ((c->frame || r->frame_no) ?
        c->frame==r->frame_no : c->packet==r->packet)) return;
    /* Candidates require a continuous run. A latched pair keeps its recent
       progress across hitches; only silence or recent-rate failure replaces it. */
    if (!c->count || r->qpc<=c->last || (reset_on_gap && r->qpc-c->last>=g_qpf/10)) {
        c->first=r->qpc; c->count=0;
    }
    c->packet=r->packet; c->frame=r->frame_no;
    c->last=r->qpc; c->recent[c->count++%21]=r->qpc;
    if (c->count>=21 && c->last-c->recent[(c->count-21)%21]>g_qpf)
        c->first=c->last; /* require a fresh one-second run after rate decay */
}
static int candidate_rate_ok(const hb_candidate *c,int64_t latest)
{
    /* The latch uses its latest raw beat, so repeats age its progress window.
       Candidates use their latest progressing beat and must also be fresh. */
    return c->count>=21 && latest>=c->last &&
        latest-c->recent[(c->count-21)%21]<=g_qpf;
}
static void latch_observe(const hb_rec *r)
{
    uintptr_t latched=__atomic_load_n(&g_expected_mailbox,__ATOMIC_ACQUIRE);
    DWORD tid=(DWORD)__atomic_load_n(&g_render_tid,__ATOMIC_ACQUIRE);
    if (!r->tid || !r->mailbox || !r->packet || (r->flags&4)) return;
    if (r->mailbox==latched && r->tid==tid) {
        candidate_observe(&latched_health,r,0); return;
    }
    hb_candidate *c=NULL, *oldest=&candidates[0];
    for (unsigned j=0;j<8;j++) {
        hb_candidate *p=&candidates[j];
        if (p->mailbox==r->mailbox && p->tid==r->tid) { c=p; break; }
        if (p->last<oldest->last) oldest=p;
    }
    if (!c) { c=oldest; memset(c,0,sizeof *c); c->mailbox=(uintptr_t)r->mailbox; c->tid=r->tid; }
    /* Track the recent rate, allowing jitter; a 100 ms gap or lost raw
       records breaks continuous health, just as it does for gate arming. */
    candidate_observe(c,r,1);
}
static void latch_try(void)
{
    int64_t now=qpc_now();
    DWORD tid=(DWORD)__atomic_load_n(&g_render_tid,__ATOMIC_ACQUIRE);
    if (tid && (!last_latched_beat || now<last_latched_beat)) return;
    if (tid && now-last_latched_beat<g_qpf/2 &&
        candidate_rate_ok(&latched_health,last_latched_beat)) return;
    hb_candidate *c=NULL;
    for (unsigned j=0;j<8;j++) {
        hb_candidate *p=&candidates[j]; int64_t span=p->last-p->first;
        if (!p->tid || !p->mailbox || span<g_qpf || now<p->last ||
            now-p->last>=g_qpf/10 || !candidate_rate_ok(p,p->last)) continue;
        int64_t recent_span=p->last-p->recent[(p->count-21)%21];
        /* All pairs prove their own full second. Among qualified take pairs,
           prefer the highest recent rate, never an unqualified short burst. */
        if (!c || recent_span<c->last-c->recent[(c->count-21)%21]) c=p;
    }
    if (!c) return;
    uintptr_t latched=__atomic_load_n(&g_expected_mailbox,__ATOMIC_ACQUIRE), next=c->mailbox;
    DWORD next_tid=c->tid;
    last_latched_beat=c->last;
    latched_health=*c;
    if (!tid) {
        __atomic_store_n(&g_expected_mailbox,next,__ATOMIC_RELEASE);
        __atomic_store_n(&g_render_tid,(LONG)next_tid,__ATOMIC_RELEASE);
        memset(candidates,0,sizeof candidates);
        log_printf("HB LATCH source=take mailbox=%p tid=%lu",(void *)next,(unsigned long)next_tid);
        return;
    }
    /* No CAS capture remains in the leaf. A transient mixed observation may
       record an extra beat; history/seq are reset and arming is held below. */
    __atomic_store_n(&g_render_tid,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_expected_mailbox,next,__ATOMIC_RELEASE);
    __atomic_store_n(&g_previous_packet,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_previous_frame,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_render_qpc,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_render_conflict,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_render_tid,(LONG)next_tid,__ATOMIC_RELEASE);
    InterlockedIncrement64(&g_hb_relatches);
    median_reset(); memset(candidates,0,sizeof candidates);
    G.armed=G.baseline_ok=0; G.baseline_start=0;
    G.last_seq=(uint64_t)__atomic_load_n(&g_render_seq,__ATOMIC_ACQUIRE);
    if (G.blocked_until<now+g_qpf) G.blocked_until=now+g_qpf;
    InterlockedIncrement(&g_factor_epoch);
    log_printf("HB RELATCH mailbox=%p TID=%lu -> mailbox=%p TID=%lu",
               (void *)latched,(unsigned long)tid,(void *)next,(unsigned long)next_tid);
}
static int hb_read_one(hb_rec *ring,uint64_t index,hb_rec *o)
{
    hb_rec *r=&ring[index&(HB_COUNT-1)];
    uint64_t stamp=__atomic_load_n(&r->stamp,__ATOMIC_ACQUIRE);
    if (stamp!=index+1) return 0;
    memcpy(o,r,sizeof *o);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&r->stamp,__ATOMIC_ACQUIRE)==stamp;
}
static int64_t heartbeat_median(void)
{
    hb_rec *ring=g_hb_ring;
    uint64_t *read=&hb_read;
    uint64_t end=(uint64_t)__atomic_load_n(&g_hb_write,__ATOMIC_ACQUIRE);
    hb_rec r; int64_t sorted[16];
    if (end-*read>HB_COUNT) {
        *read=end-HB_COUNT; median_reset(); memset(candidates,0,sizeof candidates);
        memset(&latched_health,0,sizeof latched_health);
    }
    while (*read<end) {
        if (!hb_read_one(ring,*read,&r)) break;
        (*read)++;
        if (r.flags&4) continue;
        latch_observe(&r);
        if (r.mailbox!=__atomic_load_n(&g_expected_mailbox,__ATOMIC_ACQUIRE) ||
            r.tid!=(DWORD)__atomic_load_n(&g_render_tid,__ATOMIC_ACQUIRE)) continue;
        if (r.qpc>last_latched_beat) last_latched_beat=r.qpc;
        if (!first_beat) first_beat=r.qpc;
        if (last_beat && r.qpc>last_beat) {
            intervals[interval_pos++%16]=r.qpc-last_beat;
            if (interval_n<16) interval_n++;
        }
        last_beat=r.qpc;
    }
    /* Evaluate only after draining the batch, so the old pair's entire recent
       run can veto replacement. An incomplete batch cannot prove failure,
       since its uncommitted slot could belong to the old pair. */
    if (*read==end) latch_try();
    memcpy(sorted,intervals,interval_n*sizeof *sorted);
    for (unsigned j=1;j<interval_n;j++) {
        int64_t v=sorted[j]; unsigned k=j;
        while (k && sorted[k-1]>v) { sorted[k]=sorted[k-1]; k--; } sorted[k]=v;
    }
    if (!interval_n) return 0;
    return interval_n&1 ? sorted[interval_n/2] : (sorted[interval_n/2-1]+sorted[interval_n/2])/2;
}
static int64_t gate_clock(void *ctx) { (void)ctx; return qpc_now(); }
static uint64_t gate_seq(void *ctx) { (void)ctx; return (uint64_t)__atomic_load_n(&g_render_seq,__ATOMIC_ACQUIRE); }
static void gate_help(void *ctx) { ((void (*)(void *,uint8_t))g_resolved.help_jobs)(ctx,0); }
int lockstep_h1(uint8_t *clk,void *worker,DWORD tid)
{
    int64_t median=heartbeat_median();
    LONG pairs=InterlockedExchange(&h2_count,0), ptid=InterlockedExchange(&h2_tid,0);
    LONG64 ov1=InterlockedCompareExchange64(&g_ovl_h1_total,0,0), ov2=InterlockedCompareExchange64(&g_ovl_h2_total,0,0);
    if (ov1!=seen_ovl_h1 || ov2!=seen_ovl_h2) { pairs=-1; ptid=0; }
    seen_ovl_h1=ov1; seen_ovl_h2=ov2;
    gate_input in={0}; gate_result out;
    gate_io io={worker,gate_clock,gate_seq,gate_help,NULL};
    in.enabled=g_l2_ready && g_resolved.online && g_toggle && g_vfr_allow;
    /* As with the clock/scene reads, guard the resolved global and null
       subsystem pointer, then take one plain volatile dword sample per H1. */
    if (g_resolved.online && g_resolved.online_global) {
        uintptr_t g=*(uintptr_t volatile *)g_resolved.online_global;
        if (g) in.online_flag=*(volatile int32_t *)(g+g_resolved.online_offset);
        if (g_vfr_allow_online) in.online_flag=0;
    }
    int basic=in.enabled && worker && *(uint8_t *volatile *)g_clock_global==clk && *(int32_t *)(clk+g_resolved.fps)==120 &&
        !clk[g_resolved.pause_a] && !clk[g_resolved.pause_b] &&
        *(void **)g_resolved.scene_global && !((uint8_t (*)(void))g_resolved.hook[A_SCENE])();
    int64_t hb=__atomic_load_n(&g_render_qpc,__ATOMIC_ACQUIRE);
    int64_t conflict=__atomic_load_n(&g_render_conflict,__ATOMIC_ACQUIRE);
    /* Read now AFTER shared timestamps: a concurrent heartbeat must not be
       mistaken for a future/unhealthy timestamp because median/scene work took time. */
    int64_t now=qpc_now();
    in.conditions=basic && hb>0 && now>=hb && now-hb<g_qpf/10 && median>0 && g_render_tid &&
        first_beat && last_beat-first_beat>=2*g_qpf && (!conflict || now-conflict>=2*g_qpf);
    in.tid=tid; in.paired_tid=(uint32_t)ptid;
    in.previous_h2=last_update_tid()==(DWORD)ptid ? (int)pairs : 0;
    in.now=now; in.qpf=g_qpf; in.median_interval=median;
    gate_tls_set(1);
    gate_run(&G,&in,&io,&out);
    if (out.online_edge>0) log_printf("ONLINE: gate disarmed (flag=%d)",in.online_flag);
    else if (out.online_edge<0) log_printf("ONLINE: cleared");
    /* An overlap on another worker while helping invalidates this tick too.
       Recursive help calls on this worker were already excluded by native TLS. */
    if (ov1!=InterlockedCompareExchange64(&g_ovl_h1_total,0,0) ||
        ov2!=InterlockedCompareExchange64(&g_ovl_h2_total,0,0)) {
        G.armed=G.baseline_ok=0; G.baseline_start=0; G.bad_pair=qpc_now();
        out.armed=0; out.reset=1;
    }
    gate_tls_set(2); /* H1 task continues through the native frame */
    InterlockedExchange(&last_h1_tid,(LONG)tid);
    if (out.reset) InterlockedIncrement(&g_factor_epoch);
    if (out.tid_change) log_printf("GATE: update TID recaptured as %u (H1 now %lu); disarmed for at least 1 s",G.update_tid,(unsigned long)tid);
    if (out.backoff) {
        log_printf("GATE BACKOFF: three timeouts within 1 s; disarmed for 2 s, factors reset");
        if (G.session_off) log_printf("STATUS: L2/L3 PASSIVE (five backoffs in 60 s; session disabled)");
    }
    return out.armed && !out.backoff;
}
int lockstep_h2(DWORD tid)
{
    int bypass=gate_tls_get()==3;
    InterlockedExchange(&h2_tid,bypass ? 0 : (LONG)tid);
    LONG count=InterlockedIncrement(&h2_count);
    return !bypass && count==1 && tid==last_update_tid() && g_l3_tick;
}
/* Nioh3Fix: the native pacer's timing struct, [[frame_arg+0x60]+0x28]: +0 = previous tick QPC, +0x30 = target fps.
   Published only after a sanity check so the deadline-aware nap can use the game's own schedule. */
volatile uintptr_t g_pacer_timing;
static void publish_pacer_timing(void *a1)
{
    if (g_pacer_timing || !a1) return;
    uintptr_t o=*(uintptr_t volatile *)((uint8_t *)a1+0x60);
    int ok=o>=0x10000 && o<0x800000000000ull && !(o&7);
    uintptr_t t=ok ? *(uintptr_t volatile *)(o+0x28) : 0;
    ok=t>=0x10000 && t<0x800000000000ull && !(t&7);
    int32_t fps=ok ? *(int32_t volatile *)(t+0x30) : -1;
    int64_t prev=ok ? *(int64_t volatile *)t : -1, now=qpc_now();
    if (ok && (fps==30 || fps==60 || fps==120) && prev>0 && prev<=now && now-prev<g_qpf) {
        g_pacer_timing=t; log_printf("NIOH3FIX: native pacer timing struct found (fps=%d)",fps);
    }
}
uint64_t frame_detour(void *a1,void *a2,void *a3,void *a4)
{
    int tls=gate_tls_get();
    publish_pacer_timing(a1);
    if (tls==1) return ((fn4)frame_tramp)(a1,a2,a3,a4);
    if (tls!=2) gate_tls_set(3); /* a frame without H1 cannot use a stale enabled tick */
    uint64_t ret=((fn4)frame_tramp)(a1,a2,a3,a4);
    gate_tls_set(tls==3 ? 3 : 0);
    return ret;
}
