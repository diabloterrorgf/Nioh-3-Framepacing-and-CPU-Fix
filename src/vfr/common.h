/* Shared declarations for the variable-timestep core (derived from nioh3fps, MIT). C only. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "factor.h"
#include "stubs.h"

/* ---------------- config (config.c) ---------------- */
typedef struct { int vk, ctrl, shift, alt; } keycombo;

typedef struct {
    int        start_enabled;  /* StartEnabled: initial F10 toggle state */
    keycombo   toggle_key;
    factor_cfg fc;             /* TimingMode, AvgFrames, SmoothK, ScaleMin, ScaleMax */
} config_t;

extern config_t g_cfg;
void config_defaults(config_t *c);
void config_load(const wchar_t *path, config_t *c);
void config_log(const config_t *c);
const char *keycombo_str(const keycombo *k, char *buf, size_t n);

/* ---------------- globals (init.c) ---------------- */
extern HMODULE        g_self;
extern uintptr_t      g_base;            /* game exe base */
extern int64_t        g_qpf;
extern wchar_t        g_dlldir[MAX_PATH]; /* folder of this DLL (ini) */
extern volatile LONG  g_toggle;          /* runtime enabled state (F10), latched at hook entry */
extern volatile LONG64 g_frame;          /* H1 entry counter */
extern stubs_layout   g_stubs;
extern uintptr_t      g_clock_global;    /* G = base + rva derived from the GetFps operand */

static inline int64_t qpc_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* ---------------- log (logring.c), drained by vfr_service (service.c) ---------------- */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
typedef void (*log_sink_fn)(const char *line, void *ctx);
int  logring_drain(log_sink_fn fn, void *ctx);   /* service thread only */
void vfr_service(void (*sink)(const char *line));
void vfr_track_hook(void *target);               /* init.c: the core's hooks are enabled together */

/* ---------------- hooks (hooks.c) ---------------- */
uint64_t h1_detour(uint8_t *clk, void *a2, void *a3, void *a4);
uint64_t h2_detour(void *self, uint8_t *S, void *a3, void *a4);
extern void *h1_tramp, *h2_tramp;
extern volatile LONG64 g_ovl_h1_total, g_ovl_h2_total;
/* writer statistics: updates, updates with a scaled step, and the sum of applied factors x1000 */
extern volatile LONG64 g_stat_updates, g_stat_scaled, g_stat_r_milli;

/* ---------------- AOB (aob.c) ---------------- */
typedef struct { uint8_t b[128]; uint8_t m[128]; int n; } aob_t;
int aob_parse(const char *s, aob_t *a);
int aob_match_at(const uint8_t *p, const aob_t *a);
int aob_scan(const uint8_t *start, size_t len, const aob_t *a, uintptr_t *hits, int maxhits);
