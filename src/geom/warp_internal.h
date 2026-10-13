/* Helpers shared by warp.c and warp_edit.c (not public API). */
#ifndef PC_GEOM_WARP_INTERNAL_H
#define PC_GEOM_WARP_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "warp.h"

/* Cubic Bernstein weights at t. */
void pc_geom_bernstein(double t, double out[4]);
/* Knot counts and point count consistent: safe to index (values may still be odd). */
bool pc_geom_mesh_structural(const pc_mesh *m);
/* Which patch holds s (clamped to 0..1), and the local parameter within it. */
size_t pc_geom_mesh_locate(const double *knots, size_t nknots, double s, double *local);

#endif
