/* Internals of the non-local completion shared by nonlocal.c and nonlocal_seams.c. Not a public
 * API. */
#ifndef PC_ALGO_NONLOCAL_INT_H
#define PC_ALGO_NONLOCAL_INT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/inpaint_int.h"
#include "base/status.h"
#include "raster/interrupt.h"

/* One pyramid level: cha channels per cell (the colour, then the two texture features). */
typedef struct nl_level {
    size_t w, h;
    float *img;
    bool *hole;
} nl_level;

/* Per-level search and reconstruction state. */
typedef struct nl_solver {
    size_t w, h, cha, ch; /* ch: the colour channels (the first ch of cha) */
    int32_t r;
    float *img;         /* owned, w * h * cha */
    const bool *hole;   /* borrowed */
    bool *valid;        /* owned */
    pc_ip_pt *valid_list; /* owned */
    size_t nvalid;
    bool *targets;      /* owned: patch centres whose patch overlaps the hole */
    size_t *hole_list;  /* owned: the hole cells in raster order */
    size_t nhole;
    const pc_interrupt *ctl;
} nl_solver;

/* A hole cell, the source cell it copied, and that patch's cost. */
typedef struct nl_copied {
    size_t p, s;
    float c;
} nl_copied;

bool pc_nl_is_valid(const nl_solver *s, int32_t x, int32_t y);
/* Hide the colour seam along the hole's edge (gradient-domain blending). */
pc_status pc_nl_blend_seams(nl_solver *s, const nl_copied *copied, size_t n, float texture_scale, pc_error *err);
/* Spread visible steps between areas copied from different places (iters red-black SOR sweeps). */
pc_status pc_nl_fuse_seams(nl_solver *s, const nl_copied *copied, size_t n, float texture_scale, size_t iters,
                           pc_error *err);

#endif
