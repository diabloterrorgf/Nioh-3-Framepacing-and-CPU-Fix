/* Installer for the variable-timestep core: unique AOBs and decoded operands; anything missing leaves
   the game untouched (fail-passive). Runs on its own thread once SteamStub has decrypted the code. */
#include "common.h"
#include "resolve.h"
#include "lockstep.h"
#include "consumer_fix.h"
#include "MinHook.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
HMODULE g_self;
uintptr_t g_base, g_clock_global;
int64_t g_qpf;
wchar_t g_dlldir[MAX_PATH];
volatile LONG g_toggle;
volatile LONG64 g_frame;
stubs_layout g_stubs;
int g_vfr_active;
uintptr_t g_pacer_help_ret;
void hooks_init_state(void);
static size_t image_size;
static void *tracked[64];
static int ntracked;
void vfr_track_hook(void *target) { if (ntracked < 64) tracked[ntracked++] = target; }
static void passive(const char *why) { log_printf("STATUS: PASSIVE (%s)",why); }
static int readable(uintptr_t p,size_t n)
{
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery((void *)p,&m,sizeof m) && m.State==MEM_COMMIT &&
        !(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) && p+n<= (uintptr_t)m.BaseAddress+m.RegionSize;
}
static int text_readable(void)
{
    IMAGE_NT_HEADERS64 *nt=(void *)(g_base+((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s=IMAGE_FIRST_SECTION(nt);
    for (int i=0;i<nt->FileHeader.NumberOfSections;i++) if (!memcmp(s[i].Name,".text",6)) {
        uintptr_t at=g_base+s[i].VirtualAddress, end=at+s[i].Misc.VirtualSize;
        while (at<end) {
            MEMORY_BASIC_INFORMATION m;
            if (!VirtualQuery((void *)at,&m,sizeof m) || m.State!=MEM_COMMIT ||
                (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
                !(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return 0;
            at=(uintptr_t)m.BaseAddress+m.RegionSize;
        }
        return 1;
    }
    return 0;
}
static int wait_ready(void)
{
    DWORD start=GetTickCount(), last=0;
    for (;;) {
        DWORD now=GetTickCount();
        if (!last || now-last>=1000) {
            last=now;
            if (text_readable()) {
                resolve_image((void *)g_base,image_size,&g_resolved);
                if (g_resolved.core) {
                    log_printf("READY: unique decrypted core after %lu ms",(unsigned long)(now-start)); return 1;
                }
            }
        }
        if (now-start>=120000) { log_printf("READY: game code not recognised within 120 s"); return 1; }
        Sleep(50);
    }
}
static const uint8_t *host_unwind(uintptr_t site,char *why,size_t n)
{
    IMAGE_NT_HEADERS64 *nt=(void *)(g_base+((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY d=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (d.VirtualAddress>=image_size || d.Size>image_size-d.VirtualAddress) return NULL;
    const RUNTIME_FUNCTION *rf=(void *)(g_base+d.VirtualAddress);
    for (size_t i=0;i<d.Size/sizeof *rf;i++) if (g_base+rf[i].BeginAddress<=site && site<g_base+rf[i].EndAddress) {
        if (rf[i].UnwindData>=image_size || image_size-rf[i].UnwindData<64) break;
        const uint8_t *ui=(void *)(g_base+rf[i].UnwindData); uint8_t dst[64]; size_t len;
        if (stubs_unwind_len(ui)>sizeof dst || stubs_transform_unwind(ui,dst,&len,why,n)) return NULL;
        return ui;
    }
    snprintf(why,n,"no supported .pdata entry for matched site"); return NULL;
}
static uint8_t *alloc_near(uintptr_t base,size_t n)
{
    uintptr_t a=(base+image_size+0xffff)&~(uintptr_t)0xffff;
    for (;a<base+0x70000000;a+=0x10000) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)a,&m,sizeof m)) break;
        if (m.State==MEM_FREE) { uint8_t *p=VirtualAlloc((void *)a,n,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); if (p) return p; }
    }
    return NULL;
}
static int finalize(uint8_t *a,RUNTIME_FUNCTION *t,DWORD count)
{
    DWORD old;
    if (!VirtualProtect(a,STUB_PAGE,PAGE_EXECUTE_READ,&old)) return 0;
    FlushInstructionCache(GetCurrentProcess(),a,STUB_PAGE);
    return RtlAddFunctionTable(t,count,(DWORD64)(uintptr_t)a) ? 1 : 0;
}
static int hook(const char *name,uintptr_t target,void *detour,void **tramp)
{
    MH_STATUS st=MH_CreateHook((void *)target,detour,tramp);
    if (st!=MH_OK) log_printf("HOOK %s RVA 0x%llX: %s",name,(unsigned long long)(target-g_base),MH_StatusToString(st));
    else vfr_track_hook((void *)target);
    return st==MH_OK;
}
/* The render heartbeat: a mid-function hook at the render thread's packet take. */
static int heartbeat_hook(char *why,size_t n)
{
    uintptr_t site=g_resolved.hook[A_TAKE];
    const uint8_t *ui=host_unwind(site,why,n);
    uint8_t *a=ui ? alloc_near(g_base,STUB_PAGE) : NULL;
    RUNTIME_FUNCTION *t; void *unused=NULL;
    if (!a) return 0;
    if (heartbeat_build(a,site,ui,&t,why,n) || !finalize(a,t,1)) return 0;
    return hook("packet take",site,a,&unused);
}
static int enable_all(void)
{
    for (int i=0;i<ntracked;i++) {
        MH_STATUS st=MH_QueueEnableHook(tracked[i]);
        if (st!=MH_OK) { log_printf("HOOK enable: %s",MH_StatusToString(st)); return 0; }
    }
    MH_STATUS st=MH_ApplyQueued();
    if (st!=MH_OK) log_printf("HOOK apply: %s",MH_StatusToString(st));
    return st==MH_OK;
}
static DWORD WINAPI init_thread(LPVOID arg)
{
    wchar_t ini[MAX_PATH],*slash; char why[256]="setup failed";
    stubs_params sp={0}; void *unused=NULL; LARGE_INTEGER freq; (void)arg;
    QueryPerformanceFrequency(&freq); g_qpf=freq.QuadPart;
    g_base=(uintptr_t)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt=(void *)(g_base+((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    image_size=nt->OptionalHeader.SizeOfImage;
    GetModuleFileNameW(g_self,g_dlldir,MAX_PATH);
    slash=wcsrchr(g_dlldir,L'\\'); if (slash) *slash=0;
    _snwprintf(ini,MAX_PATH,L"%ls\\nioh3fix.ini",g_dlldir); ini[MAX_PATH-1]=0;
    config_load(ini,&g_cfg); g_toggle=g_cfg.start_enabled;
    config_log(&g_cfg);
    if (!wait_ready()) return 0;
    if (!g_resolved.core) { passive("game code not recognised (unsupported game version?); game untouched"); return 0; }
    if (!g_resolved.online) { passive("online flag unresolved; game untouched"); return 0; }
    g_clock_global=g_resolved.clock_global;
    g_pacer_help_ret=g_resolved.hook[A_HELP] ? g_resolved.hook[A_HELP]+5 : 0;
    if (!readable(g_clock_global,8)) { passive("decoded clock global unreadable"); return 0; }
    if (!readable(g_resolved.online_global,8)) { passive("decoded online global unreadable"); return 0; }
    DWORD start=GetTickCount();
    while (!*(void *volatile *)g_clock_global) {
        if (GetTickCount()-start>=15u*60*1000) { passive("clock object still null after 15 min"); return 0; } Sleep(50);
    }
    if (g_gate_tls==TLS_OUT_OF_INDEXES) g_gate_tls=TlsAlloc();
    if (g_gate_tls>=64) { passive("no inline native TLS slot (0..63); cannot guard re-entrancy"); return 0; }
    sp.p1_host_unwind=host_unwind(g_resolved.hook[A_P1],why,sizeof why);
    sp.p2_host_unwind=host_unwind(g_resolved.hook[A_P2],why,sizeof why);
    if (!sp.p1_host_unwind || !sp.p2_host_unwind) { passive(why); return 0; }
    sp.alloc=alloc_near(g_base,2*STUB_PAGE); if (!sp.alloc) { passive("no nearby stub pages"); return 0; }
    sp.p1_return=g_resolved.hook[A_P1]+16; sp.p2_return=g_resolved.hook[A_P2]+17;
    if (stubs_build(&sp,&g_stubs,why,sizeof why)) { passive(why); return 0; }
    stubs_s2l(&sp,&g_stubs,g_resolved.req,g_resolved.eff,g_gate_tls);
    if (!finalize(sp.alloc,g_stubs.table,g_stubs.count)) { passive("stub protection/unwind registration failed"); return 0; }
    hooks_init_state();
    MH_STATUS st=MH_Initialize(); if (st==MH_ERROR_ALREADY_INITIALIZED) st=MH_OK; if (st!=MH_OK) { passive(MH_StatusToString(st)); return 0; }
    if (!hook("H1",g_resolved.hook[A_CLOCK],(void *)h1_detour,&h1_tramp) ||
        !hook("H2",g_resolved.hook[A_H2],(void *)h2_detour,&h2_tramp) ||
        !hook("P1",g_resolved.hook[A_P1],g_stubs.p1_stub,&unused) ||
        !hook("P2",g_resolved.hook[A_P2],g_stubs.p2_stub,&unused)) {
        for (int i=0;i<ntracked;i++) MH_RemoveHook(tracked[i]);
        passive("required core hook creation failed"); return 0;
    }
    int hb=g_resolved.heartbeat && heartbeat_hook(why,sizeof why);
    if (!hb) log_printf("STATUS: render heartbeat PASSIVE (%s)",why);
    /* The native frame hook also publishes the native pacer's timing struct (used by UpdateNap and the pacer). */
    int frame=hook("native frame",g_resolved.hook[A_FRAME],(void *)frame_detour,&frame_tramp);
    g_l2_ready=hb && frame && g_resolved.gate;
    if (!consumer_fix_install((void *)g_base,image_size)) g_l2_ready=0;
    if (!enable_all()) {
        for (int i=0;i<ntracked;i++) MH_DisableHook(tracked[i]);
        g_l2_ready=0; passive("hook enable failed"); return 0;
    }
    g_vfr_active=1;
    log_printf("STATUS: ACTIVE, variable timestep %s",g_l2_ready ? "available (arms in gameplay)" : "PASSIVE (dependency missing; factors stay 1)");
    return 0;
}

void vfr_start(HMODULE self)
{
    HANDLE h;
    g_self = self;
    h = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
