#pragma once
#include "common.h"
#include "heartbeat.h"
#include "gate.h"
extern int g_l2_ready;
extern volatile LONG g_l3_tick, g_factor_epoch, g_vfr_allow, g_vfr_allow_online;
extern void *frame_tramp;
uint64_t frame_detour(void *a1, void *a2, void *a3, void *a4);
int lockstep_h1(uint8_t *clk, void *worker, DWORD tid);
int lockstep_h2(DWORD tid); /* paired first H2 only; bypass paths stay unscaled */
void lockstep_init(void);
