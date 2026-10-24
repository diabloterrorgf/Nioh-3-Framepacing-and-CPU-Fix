#pragma once
#include <stdint.h>
#include <stddef.h>
enum { A_GETFPS, A_CLOCK, A_P1, A_H2, A_P2, A_TAKE, A_HELP, A_SCENE,
       A_FRAME, A_S_FIELDS, A_ONLINE, A_ONLINE2, A_ONLINE1, A_COUNT };
typedef struct { const char *name, *pattern; int hookoff; uint32_t hint; } anchor_def;
typedef struct {
    uintptr_t match[A_COUNT], hook[A_COUNT];
    int hits[A_COUNT];
    uintptr_t clock_global, scene_global, online_global, c60, help_jobs;
    uint32_t fps, actual_fps, req, eff, pause_a, pause_b, s_fps, s_req, s_eff;
    uint32_t online_offset;
    int core, heartbeat, gate, online;
} resolved_t;
extern resolved_t g_resolved;
int resolve_image(uint8_t *image, size_t size, resolved_t *out);
