/* Internals of the patch-based completion (inpaint.c, inpaint_solver.c, inpaint_peel.c,
 * inpaint_synth.c), shared by nonlocal.c and exposed to the tests (the reference's tests reach
 * into the module). Not a public API. */
#ifndef PC_ALGO_INPAINT_INT_H
#define PC_ALGO_INPAINT_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/inpaint.h"
#include "base/status.h"
#include "raster/interrupt.h"

/* Below this many cells a pass runs on the calling thread (same results either way). */
#define PC_IP_PAR_MIN ((size_t)64 * 64)
/* PC_IP_PAR_MIN for the passes that match or vote a patch per cell. */
#define PC_IP_PAR_MIN_PATCHES ((size_t)16 * 16)
/* Most channels whose moments the colour adaptation fits. */
#define PC_IP_MOM_CH 8

/* ---- SplitMix64 (inpaint::Rng) ---- */
typedef struct pc_ip_rng {
    uint64_t s;
} pc_ip_rng;

static inline pc_ip_rng pc_ip_rng_new(uint64_t seed)
{
    return (pc_ip_rng){seed ^ 0x9E3779B97F4A7C15ull};
}
static inline uint64_t pc_ip_rng_next(pc_ip_rng *r)
{
    uint64_t z;
    r->s += 0x9E3779B97F4A7C15ull;
    z = r->s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
/* The same state as after n calls of next. */
static inline void pc_ip_rng_skip(pc_ip_rng *r, uint64_t n)
{
    r->s += n * 0x9E3779B97F4A7C15ull;
}
/* Uniform integer in lo..=hi. */
static inline int32_t pc_ip_rng_range(pc_ip_rng *r, int32_t lo, int32_t hi)
{
    uint64_t span;
    if (hi <= lo) {
        return lo;
    }
    span = (uint64_t)((int64_t)hi - (int64_t)lo + 1);
    return (int32_t)((int64_t)lo + (int64_t)(pc_ip_rng_next(r) % span));
}

typedef struct pc_ip_pt {
    int32_t x, y;
} pc_ip_pt;

/* The half-open box [x0, x1) x [y0, y1) of a grid. */
typedef struct pc_ip_bounds {
    size_t x0, y0, x1, y1;
} pc_ip_bounds;

static inline size_t pc_ip_bw(pc_ip_bounds b)
{
    return b.x1 > b.x0 ? b.x1 - b.x0 : 0;
}
static inline size_t pc_ip_bh(pc_ip_bounds b)
{
    return b.y1 > b.y0 ? b.y1 - b.y0 : 0;
}
static inline size_t pc_ip_barea(pc_ip_bounds b)
{
    return pc_ip_bw(b) * pc_ip_bh(b);
}

/* Summed-area table of a boolean grid ((w+1) x (h+1), free()); NULL when out of memory. */
uint32_t *pc_ip_integral(size_t w, size_t h, const bool *m);
/* Count of set cells in the clipped window [x0, x1) x [y0, y1) of a gw x gh table. */
uint32_t pc_ip_window_count(const uint32_t *s, size_t gw, size_t gh, int64_t x0, int64_t y0, int64_t x1,
                            int64_t y1);
/* Bounding box of the set cells (b: x0, y0, x1, y1); false when none. */
bool pc_ip_hole_bbox(size_t w, size_t h, const bool *hole, size_t b[4]);
/* Bounds of the PC_ALGO_PX_HOLE cells; false when none. */
bool pc_ip_hole_bounds(size_t w, size_t h, const uint8_t *px, pc_ip_bounds *out);

/* Summed-area table restricted to the hole's bounds (inpaint::proximity_integral::HoleIntegral). */
typedef struct pc_ip_hole_integral {
    int64_t x0, y0;
    size_t w, h, len;
    uint32_t *data; /* (w+1) x (h+1), owned */
} pc_ip_hole_integral;

/* The table over b (x0, y0, x1, y1) of a hole mask with row stride `stride` (hole_len entries).
 * PC_OK with out->data NULL when it can't be built (inverted bounds, bounds outside the mask,
 * overflow: the reference's None); PC_ERR_NOMEM. */
pc_status pc_ip_hole_integral_new(size_t stride, const bool *hole, size_t hole_len, const size_t b[4],
                                  pc_ip_hole_integral *out, pc_error *err);
void pc_ip_hole_integral_free(pc_ip_hole_integral *t);
/* Set cells in the window [x0, x1) x [y0, y1) of the full grid. */
uint32_t pc_ip_hole_integral_count(const pc_ip_hole_integral *t, int64_t x0, int64_t y0, int64_t x1, int64_t y1);

/* Valid source centres (the whole 2r+1 patch inside the grid and PC_ALGO_PX_SOURCE): *out holds
 * w * h bools (free()). PC_ERR_CANCELLED when ctl was cancelled. */
pc_status pc_ip_valid_sources(size_t w, size_t h, size_t r, const uint8_t *px, const pc_interrupt *ctl,
                              bool **out, pc_error *err);

/* The valid centres in row-major order, indexed by counts per row and per 64-column block. */
typedef struct pc_ip_sources {
    size_t *row_start;     /* h + 1 entries */
    uint32_t *block_start; /* per row, the centres in it before each block */
    size_t blocks, h;
} pc_ip_sources;

pc_status pc_ip_sources_new(const bool *valid, size_t w, size_t h, pc_ip_sources *out, pc_error *err);
void pc_ip_sources_free(pc_ip_sources *s);
size_t pc_ip_sources_len(const pc_ip_sources *s);
/* The k-th valid centre (k < len), (0, 0) otherwise. */
pc_ip_pt pc_ip_sources_nth(const pc_ip_sources *s, size_t k, const bool *valid, size_t w);
/* All of them in order (*out: len points, free()). */
pc_status pc_ip_sources_all(const pc_ip_sources *s, const bool *valid, size_t w, pc_ip_pt **out,
                            pc_error *err);

/* Per cell of tb (hb grown by r): is it a target centre (its patch overlaps the hole)? tb-area
 * bools (free()), NULL when out of memory. */
bool *pc_ip_targets_around(size_t w, size_t r, const uint8_t *px, pc_ip_bounds hb, pc_ip_bounds tb);

/* Per-level PatchMatch / EM state (inpaint::Solver). */
typedef struct pc_ip_solver {
    size_t w, h, ch;
    int32_t r;
    float *img;        /* borrowed, w * h * ch, the level's pixels (hole pixels change) */
    const uint8_t *px; /* borrowed */
    bool *valid;       /* owned, w * h */
    pc_ip_bounds hole_box, tbox;
    bool *targets;  /* owned, tbox area */
    bool *full;     /* owned, tbox area: the whole patch inside the grid and not void */
    float *tmom;    /* owned, tbox area * c8 * 2 (with adaptation), else NULL */
    float *smom;    /* owned, w * h * c8 * 2 when kept, else NULL */
    pc_algo_adapt adapt;
    const pc_interrupt *ctl;
} pc_ip_solver;

/* Frees the owned tables (valid, targets, full, tmom, smom). */
void pc_ip_solver_fini(pc_ip_solver *s);
pc_status pc_ip_solver_prepare(pc_ip_solver *s, bool keep_source_moments, pc_error *err);
bool pc_ip_solver_is_valid(const pc_ip_solver *s, int32_t x, int32_t y);
float pc_ip_dist(const pc_ip_solver *s, pc_ip_pt t, size_t k, pc_ip_pt src, float cutoff);
float pc_ip_dist_clipped(const pc_ip_solver *s, pc_ip_pt t, pc_ip_pt src, float cutoff);
float pc_ip_dist_full(const pc_ip_solver *s, pc_ip_pt t, pc_ip_pt src, float cutoff);
/* m: c8 x [sum t, sum t^2, sum s, sum s^2, sum ts]; returns the sample count. */
size_t pc_ip_moments(const pc_ip_solver *s, pc_ip_pt t, pc_ip_pt src, float (*m)[5], size_t c8);
void pc_ip_moments_full(const pc_ip_solver *s, size_t k, pc_ip_pt t, pc_ip_pt src, float (*m)[5], size_t c8);
float pc_ip_dist_adapted(const pc_ip_solver *s, pc_ip_pt t, pc_ip_pt src);
float pc_ip_dist_adapted_full(const pc_ip_solver *s, size_t k, pc_ip_pt t, pc_ip_pt src);
void pc_ip_patchmatch(const pc_ip_solver *s, pc_ip_pt *nnf, float *cost, size_t pass, uint64_t seed,
                      int32_t search_radius);
/* Recompute every target's cost (and with adaptation the full targets' moments). */
pc_status pc_ip_costs(pc_ip_solver *s, const pc_ip_pt *nnf, float *cost, pc_error *err);
/* Re-estimate the hole pixels by the similarity-weighted vote (nothing when cancelled). */
pc_status pc_ip_vote(pc_ip_solver *s, const pc_ip_pt *nnf, const float *cost, pc_error *err);

/* *out = the k-th smallest (k < n) of v[0..n) in f32::total_cmp order (select_nth_unstable_by);
 * v is not modified. */
pc_status pc_ip_nth_total(const float *v, size_t n, size_t k, float *out, pc_error *err);
/* sigma^2 of the vote: the 75th percentile of the n costs, at least 1e-6 (1 when n is 0). */
pc_status pc_ip_sigma2(const float *costs, size_t n, float *out, pc_error *err);

/* The least sum e >= 0 with e / n > v (strict) or >= v in float arithmetic. */
float pc_ip_least_sum(float v, float n, bool strict);
/* Fill the hole from its edge inwards, ring by ring (onion peel): sources in row-major order. */
pc_status pc_ip_onion_peel(size_t w, size_t h, size_t ch, int32_t r, float *img, const uint8_t *px,
                           pc_ip_bounds hb, const pc_ip_pt *sources, size_t nsources, const pc_interrupt *ctl,
                           pc_error *err);

#endif
