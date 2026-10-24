/* S2C-F: tick publication, consumer policy and pure arithmetic. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { float r; uint32_t phase:31, armed:1; uint64_t seq; } cf_tick;
enum { CF_RATE, CF_ROUND, CF_FLOAT, CF_KNOWN, CF_DENY };
typedef struct { uintptr_t ret; unsigned policy, core, ordinal; } cf_site;
int cf_rate(float r, uint64_t seq, unsigned site, unsigned policy);
int cf_tick_rate(cf_tick tick, unsigned site, unsigned policy);
float cf_x_speed(float effective, float requested, int armed);
int cf_find(const cf_site *sites, unsigned count, uintptr_t ret);
int cf_core_ok(const cf_site *sites, const unsigned char *placed, unsigned count);
void consumer_fix_publish(float r, uint64_t seq, int armed);
cf_tick consumer_fix_snapshot(void);
int consumer_fix_install(uint8_t *image, size_t size);
extern int g_consumers_ready;
int cp_helper_decode(const uint8_t *code, size_t size, uintptr_t at,
                     uintptr_t getfps, uintptr_t *ret, unsigned *frame);
uintptr_t cp_outer_return(const void *rsp, uintptr_t helper_ret, unsigned frame);
