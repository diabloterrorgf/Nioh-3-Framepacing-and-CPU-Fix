/* Consumer fixes (S2C-F, from nioh3fps): systems that assume 120 updates/s read GetFps or fixed
   1/120 constants. While a scaled tick is armed, each known consumer gets a rate matching the
   real step: GetFps callers by return address (direct, or through one helper), and five inline
   sites (X leaf, three multipliers, one divisor). Everything is all-or-nothing per session. */
#include "common.h"
#include "consumer_fix.h"
#include "lockstep.h"
#include "resolve.h"
#include "MinHook.h"
#include <string.h>
#include <stdlib.h>
typedef struct { uintptr_t match,site; int active; uint32_t caller_offset; } cp_context;
#include "consumer_anchors.h"
#define CP_KNOWN (sizeof cp_known_aobs/sizeof *cp_known_aobs)
static uintptr_t known[CP_KNOWN],helper_ret;
static unsigned helper_frame;
static void *fps_tramp;
extern void cf_fps_entry(void),cf_mid_leaf(void),cf_x_entry(void);
static uint32_t u32(uintptr_t p) { uint32_t v; memcpy(&v,(void *)p,4); return v; }
static uintptr_t relative(uintptr_t p,int disp,int end) { return p+end+(int32_t)u32(p+disp); }
static uintptr_t unique(uintptr_t text,size_t n,const char *pattern)
{
    aob_t a; uintptr_t p=0;
    return !aob_parse(pattern,&a) && aob_scan((void *)text,n,&a,&p,1)==1 ? p : 0;
}
static uintptr_t caller_return(uintptr_t text,size_t n,const char *pattern,uintptr_t target)
{
    uintptr_t p=unique(text,n,pattern);
    return p && *(const uint8_t *)p==0xe8 && relative(p,1,5)==target ? p+5 : 0;
}
/* Clone active host unwind operations, dropping language handlers. Chained
   body regions are flattened in unwind order; leaf functions use an empty record.
   Derive the caller stack displacement from ALLOC/PUSH codes, never a fixed frame. */
static int unwind(uint8_t *image,size_t size,uintptr_t site,uint8_t *dst,uint32_t *frame)
{
    IMAGE_NT_HEADERS64 *nt=(void *)(image+((IMAGE_DOS_HEADER *)image)->e_lfanew);
    IMAGE_DATA_DIRECTORY d=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (d.VirtualAddress>size || d.Size>size-d.VirtualAddress) return 0;
    RUNTIME_FUNCTION *f=(void *)(image+d.VirtualAddress); uintptr_t base=(uintptr_t)image;
    *frame=0; memcpy(dst,"\1\0\0\0",4);
    for (unsigned j=0;j<d.Size/sizeof *f;j++) if (site>=base+f[j].BeginAddress && site<base+f[j].EndAddress) {
        uint32_t info_rva=f[j].UnwindData; unsigned total=0;
        for (unsigned depth=0;depth<16;depth++) {
          if (info_rva>size || size-info_rva<4) return 0;
          const uint8_t *u=image+info_rva; size_t len=stubs_unwind_len(u);
          if ((u[0]&7)!=1 || len>size-info_rva || total+u[2]>250 ||
              (!depth && site-base-f[j].BeginAddress<u[1])) return 0;
          if (u[3] && dst[3] && u[3]!=dst[3]) return 0;
          if (u[3]) dst[3]=u[3];
          memcpy(dst+4+2*total,u+4,2*u[2]);
          for (unsigned k=0;k<u[2];) {
            unsigned op=u[5+2*k]&15,info=u[5+2*k]>>4,ns=1;
            dst[4+2*(total+k)]=0;
            if (op==0) *frame+=8;
            else if (op==2) *frame+=8*info+8;
            else if (op==1) {
                ns=info ? 3 : 2;
                if (k+ns>u[2]) return 0;
                uint32_t a=(uint32_t)u[6+2*k]|((uint32_t)u[7+2*k]<<8);
                *frame+=info ? a|((uint32_t)u[8+2*k]<<16)|((uint32_t)u[9+2*k]<<24) : a*8;
            } else if (op==4 || op==8) ns=2;
            else if (op==5 || op==9) ns=3;
            else if (op!=3) return 0;
            if (k+ns>u[2]) return 0;
            k+=ns;
          }
          total+=u[2]; dst[2]=(uint8_t)total;
          if (!(u[0]&0x20)) {
              if (total&1) memset(dst+4+2*total,0,2);
              return 1;
          }
          if (len+12>size-info_rva) return 0;
          memcpy(&info_rva,u+len+8,4);
        }
        return 0; /* cyclic or excessively deep chain */
    }
    return 1;
}
static uint8_t *near_alloc(uintptr_t base)
{
    for (uintptr_t at=(base+0x10000)&~(uintptr_t)0xffff;at<base+0x60000000;at+=0x10000) {
        MEMORY_BASIC_INFORMATION m;
        if (VirtualQuery((void *)at,&m,sizeof m) && m.State==MEM_FREE) {
            uint8_t *p=VirtualAlloc((void *)at,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
            if (p) return p;
        }
    }
    return NULL;
}
static int place_mid_at(uint8_t *image,size_t size,cp_context *c,const char *name,unsigned id,void *leaf)
{
    uint8_t uw[512];
    if (!unwind(image,size,c->site,uw,&c->caller_offset)) { log_printf("STATUS: PASSIVE %s (host unwind)",name); return 0; }
    uint8_t *a=near_alloc((uintptr_t)image); void *tramp=NULL;
    if (!a) { log_printf("STATUS: PASSIVE %s (near allocation)",name); return 0; }
    /* Call preserving helper, then JE/JNE to the original trampoline. Conditional
       transfers avoid the virtual unwinder's tail-jump epilogue heuristic. */
    memcpy(a,"\xff\x15\x0c\0\0\0\x0f\x84\0\0\0\0\x0f\x85\0\0\0\0",18);
    uintptr_t fn=(uintptr_t)leaf; memcpy(a+18,&fn,8); memcpy(a+26,&id,4);
    memcpy(a+0x100,uw,stubs_unwind_len(uw));
    RUNTIME_FUNCTION *rf=(void *)(a+0x300); *rf=(RUNTIME_FUNCTION){0,18,0x100};
    DWORD old;
    /* MinHook requires both target and detour to be executable at creation. */
    if (!VirtualProtect(a,4096,PAGE_EXECUTE_READ,&old)) { VirtualFree(a,0,MEM_RELEASE); return 0; }
    MH_STATUS result=MH_CreateHook((void *)c->site,a,&tramp);
    if (result!=MH_OK) { log_printf("STATUS: PASSIVE %s (%s)",name,MH_StatusToString(result)); VirtualFree(a,0,MEM_RELEASE); return 0; }
    int64_t d1=(intptr_t)tramp-(intptr_t)(a+12),d2=(intptr_t)tramp-(intptr_t)(a+18);
    if (d1<INT32_MIN || d1>INT32_MAX || d2<INT32_MIN || d2>INT32_MAX) { log_printf("STATUS: PASSIVE %s (trampoline distance)",name); goto fail; }
    if (!VirtualProtect(a,4096,PAGE_READWRITE,&old)) goto fail;
    int32_t b=(int32_t)d1; memcpy(a+8,&b,4); b=(int32_t)d2; memcpy(a+14,&b,4);
    if (!VirtualProtect(a,4096,PAGE_EXECUTE_READ,&old) || !RtlAddFunctionTable(rf,1,(DWORD64)(uintptr_t)a)) { log_printf("STATUS: PASSIVE %s (protect/register)",name); goto fail; }
    FlushInstructionCache(GetCurrentProcess(),a,4096);
    vfr_track_hook((void *)c->site); return 1;
fail:
    MH_RemoveHook((void *)c->site); VirtualFree(a,0,MEM_RELEASE); return 0;
}
static int entry(cp_context *c,void *thunk,void **tramp)
{
    if (MH_CreateHook((void *)c->site,thunk,tramp)!=MH_OK) return 0;
    vfr_track_hook((void *)c->site); return 1;
}

typedef struct { unsigned index,policy,core; const char *name; } cf_direct;
typedef struct { const char *name,*pattern; unsigned policy,core; } cf_outer;
typedef struct { const char *name,*pattern; unsigned offset,kind; } cf_patch;
#include "consumer_fix_anchors.h"
#define CF_OUTERS (sizeof cf_outers/sizeof *cf_outers)
#define CF_PATCHES (sizeof cf_patches/sizeof *cf_patches)
static cf_site direct_sites[CP_KNOWN],outer_sites[CF_OUTERS];
static unsigned direct_count,outer_count;
static cp_context fix_contexts[CF_PATCHES];
static void *x_tramp;
int g_consumers_ready;
static int site_order(const void *a,const void *b)
{
    uintptr_t x=((const cf_site *)a)->ret,y=((const cf_site *)b)->ret;
    return x<y ? -1 : x>y;
}
static int fix_armed(cf_tick tick)
{
    /* TLS matches P1/P2 on re-entrant/overlapping native calls. */
    return g_consumers_ready && tick.armed && g_l3_tick && gate_tls_get()!=1;
}
static int effective_fps(uintptr_t ret,uintptr_t outer,int native,cf_tick tick)
{
    if (!fix_armed(tick) || native!=120) return native;
    const cf_site *s=NULL; int i;
    if (ret==helper_ret) {
        i=cf_find(outer_sites,outer_count,outer);
        if (i>=0) s=&outer_sites[i];
    } else {
        i=cf_find(direct_sites,direct_count,ret);
        if (i>=0) s=&direct_sites[i];
    }
    return s ? cf_tick_rate(tick,s->ordinal,s->policy) : native;
}
/* These preserving assembly entries return through the real caller stack.
   Calling the original leaf retains null-clock and native fallback behaviour. */
int consumer_fps_value(const uintptr_t *stack)
{
    cf_tick tick=consumer_fix_snapshot();
    int native=((int (*)(void))fps_tramp)();
    uintptr_t ret=*stack,outer=cp_outer_return(stack,helper_ret,helper_frame);
    return effective_fps(ret,outer,native,tick);
}
float consumer_x_value(void)
{
    cf_tick tick=consumer_fix_snapshot();
    float native=((float (*)(void))x_tramp)();
    if (!fix_armed(tick) || !g_clock_global) return native;
    const uint8_t *clk=*(const uint8_t *volatile *)g_clock_global;
    return clk ? cf_x_speed(*(const float *)(clk+g_resolved.eff),
                           *(const float *)(clk+g_resolved.req),1) : native;
}
float consumer_mid_factor(unsigned id)
{
    cf_tick tick=consumer_fix_snapshot();
    if (id>=CF_PATCHES || !fix_armed(tick)) return 0;
    /* Match GetFps's decoded native FPS field, not the governor's setting. */
    const uint8_t *clk=g_clock_global ? *(const uint8_t *volatile *)g_clock_global : NULL;
    if (!clk || *(const int32_t *)(clk+g_resolved.actual_fps)!=120) return 1.0f;
    return id==4 ? 1.0f/tick.r : tick.r;
}
int consumer_fix_install(uint8_t *image,size_t size)
{
    uintptr_t ts=0; size_t tn=0; unsigned char placed[CP_KNOWN]={0};
    IMAGE_NT_HEADERS64 *nt=(void *)(image+((IMAGE_DOS_HEADER *)image)->e_lfanew);
    IMAGE_SECTION_HEADER *sec=IMAGE_FIRST_SECTION(nt);
    for (unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) if (!memcmp(sec[i].Name,".text",6)) {
        ts=(uintptr_t)image+sec[i].VirtualAddress; tn=sec[i].Misc.VirtualSize;
    }
    if (!ts || ts-(uintptr_t)image>size || tn>size-(ts-(uintptr_t)image)) {
        log_printf("STATUS: S2C-F core PASSIVE (no .text)"); return 0;
    }
    /* GetFps entry hook and the helper whose caller identifies the real consumer. */
    cp_context getfps={0};
    uintptr_t helper=unique(ts,tn,cp_helper_aob);
    getfps.match=unique(ts,tn,cp_getfps_aob); getfps.site=getfps.match;
    if (helper && getfps.match && !cp_helper_decode((void *)helper,9,helper,getfps.match,&helper_ret,&helper_frame))
        helper_ret=0;
    if (getfps.match) for (unsigned i=0;i<CP_KNOWN;i++) known[i]=caller_return(ts,tn,cp_known_aobs[i],getfps.match);
    int ok=getfps.match && helper_ret && helper_frame && entry(&getfps,(void *)cf_fps_entry,&fps_tramp);
    if (!ok) log_printf("STATUS: PASSIVE S2C-F core GetFps/helper (AOB, operand or hook)");
    direct_count=CP_KNOWN;
    for (unsigned i=0;i<CP_KNOWN;i++) {
        direct_sites[i]=(cf_site){known[i],i==37 || i==38 ? CF_DENY : CF_KNOWN,0,i};
        const char *name="known native caller";
        for (unsigned j=0;j<sizeof cf_directs/sizeof *cf_directs;j++) if (cf_directs[j].index==i) {
            direct_sites[i].policy=cf_directs[j].policy; direct_sites[i].core=cf_directs[j].core;
            name=cf_directs[j].name;
        }
        if ((i==37 || i==38) && direct_sites[i].policy!=CF_DENY) {
            log_printf("STATUS: PASSIVE S2C-F (mode denylist policy mismatch at site %u; L2/L3 disabled)",i);
            ok=0;
        }
        placed[i]=known[i]!=0;
        if (direct_sites[i].policy<=CF_FLOAT && !placed[i]) log_printf("STATUS: PASSIVE S2C-F %s %s",
            direct_sites[i].core ? "core" : "rare",name);
    }
    ok=cf_core_ok(direct_sites,placed,CP_KNOWN) && ok;
    for (unsigned i=0;i<direct_count;i++) if (direct_sites[i].ret && direct_sites[i].policy<=CF_FLOAT) {
        if ((known[37] && direct_sites[i].ret==known[37]) ||
            (known[38] && direct_sites[i].ret==known[38])) {
            log_printf("STATUS: PASSIVE S2C-F (fix aliases a mode denylist caller; L2/L3 disabled)");
            ok=0;
        }
    }
    qsort(direct_sites,direct_count,sizeof *direct_sites,site_order);
    outer_count=CF_OUTERS;
    for (unsigned i=0;i<CF_OUTERS;i++) {
        uintptr_t p=helper ? caller_return(ts,tn,cf_outers[i].pattern,helper) : 0;
        outer_sites[i]=(cf_site){p,cf_outers[i].policy,cf_outers[i].core,CP_KNOWN+i};
        if (!p && cf_outers[i].core) ok=0;
        if (!p) log_printf("STATUS: PASSIVE S2C-F %s %s",cf_outers[i].core ? "core" : "rare",cf_outers[i].name);
    }
    qsort(outer_sites,outer_count,sizeof *outer_sites,site_order);
    for (unsigned i=0;i<CF_PATCHES;i++) {
        cp_context *c=&fix_contexts[i]; c->match=unique(ts,tn,cf_patches[i].pattern);
        c->site=c->match ? c->match+cf_patches[i].offset : 0;
        if (c->match && i==0) {
            /* The X leaf must read the same clock/effective field as P1. */
            if (relative(c->match,3,7)!=g_clock_global || u32(c->match+16)!=g_resolved.eff) c->match=0;
        }
        c->active=c->match && (i==0 ? entry(c,(void *)cf_x_entry,&x_tramp) :
            place_mid_at(image,size,c,cf_patches[i].name,i,(void *)cf_mid_leaf));
        if (!c->active) ok=0;
        if (!c->active) log_printf("STATUS: PASSIVE S2C-F core %s",cf_patches[i].name);
    }
    /* This immutable session flag and L2 readiness make a partial core entirely passive. */
    g_consumers_ready=ok;
    log_printf("STATUS: S2C-F core %s (all-or-nothing)",ok ? "READY" : "PASSIVE; L2/L3 disabled for session");
    return ok;
}
