/* DPS library (#118), C ABI for offline hosts (scripts/dps.py via ctypes). Built into build/libdps.so by `just dps`.
   Not part of DoS-Tool.dll: in game use dps.hpp. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- raw kernel: n builds as attribute rows, one compiled class x weapon x scenario (layout: src/layout.hpp) ---- */
typedef struct {
    int A, K, C;                                   /* attributes per build, abilities, damage components */
    const float *ab_cast, *ab_cd;                  /* [K] cast seconds at attack speed 0, cooldown seconds */
    const int32_t *c_ab, *c_ap, *c_magic, *c_elem; /* [C] ability index, 0 MAP 1 RAP 2 SP, magic flag, element */
    const float *c_coef, *c_hits, *c_targets, *c_dot_dur, *c_dot_per;
    const float *scen;                             /* layout.hpp S_* */
} dps_ctx;
typedef struct {                                   /* optional in-kernel assembly: base + sum of slot rows + pts @ Hm */
    int S, H;
    const int32_t *choice, *slot_off;
    const float *rows, *pts, *Hm, *base;
} dps_gear;
/* attrs [n][A] or NULL with gear; out [n]; per_ability [n][K] or NULL. Threads: env DPS_THREADS (default 1). */
void dps_eval(int n, const float *attrs, const dps_gear *gear, const dps_ctx *ctx, float *out, float *per_ability);

/* ---- model: tables file + YAML requests (formats: scripts/dps.py) ---- */
typedef struct dps_model dps_model;
dps_model *dps_open(const char *tables_path, char *err, int err_len);
void dps_close(dps_model *m);
/* request/response as YAML-free line text, see host/c_api.cpp; returns bytes written (truncated to cap) or -1 */
int dps_call(dps_model *m, const char *request, char *out, int cap);
/* one stat of a scalable crafting-bonus item: value by (spec, stat, item level, grade), -1 when the stat cannot roll */
float dps_item_stat(dps_model *m, int spec, const char *stat, int level, int grade);

#ifdef __cplusplus
}
#endif
