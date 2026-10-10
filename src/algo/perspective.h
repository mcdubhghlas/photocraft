/* Edit > Perspective Warp (port of crates/algo/src/perspective.rs): planes (quads) drawn in
 * Layout mode whose corners then move in Warp mode; the image follows a piecewise-homography map
 * made continuous across shared edges (each shared edge's mismatch is averaged and faded across
 * the plane). Corners that coincide within half a pixel in the source are linked: they share one
 * destination, as Photoshop's snapped planes do.
 *
 * Plain data plus one opaque prepared map; reentrant, the map is read-only once built. */
#ifndef PC_ALGO_PERSPECTIVE_H
#define PC_ALGO_PERSPECTIVE_H

#include <stdbool.h>
#include <stddef.h>

#include "algo/transform.h"
#include "base/status.h"

/* One plane: source quad (Layout mode) and destination quad (Warp mode), corners clockwise from
 * top-left, document pixels. */
typedef struct pc_algo_persp_plane {
    double src[4][2];
    double dst[4][2];
} pc_algo_persp_plane;

/* Straighten modes (Photoshop's Warp-mode buttons). */
typedef enum pc_algo_persp_straighten {
    PC_ALGO_PERSP_HORIZONTAL,
    PC_ALGO_PERSP_VERTICAL,
    PC_ALGO_PERSP_AUTO
} pc_algo_persp_straighten;

/* Straighten::parse ("h"/"horizontal"/"level", "v"/"vertical", "auto"/"both"; ASCII case
 * insensitive). False when unknown. */
bool pc_algo_persp_straighten_parse(const char *s, size_t len, pc_algo_persp_straighten *out);

pc_algo_persp_plane pc_algo_persp_plane_identity(const double q[4][2]);

/* Groups of linked corners: group_of[p * 4 + c] receives the group of corner c of plane p (groups
 * numbered in order of first appearance; members of a group in (plane, corner) order); *n_groups
 * their count. group_of holds n * 4 entries. */
void pc_algo_persp_linked_corners(const pc_algo_persp_plane *planes, size_t n, size_t *group_of,
                                  size_t *n_groups);
/* Snaps linked corners to one source and one destination position (their means). */
pc_status pc_algo_persp_unify(pc_algo_persp_plane *planes, size_t n, pc_error *err);
/* Makes near-horizontal destination edges horizontal and / or near-vertical ones vertical,
 * moving linked corners together. */
pc_status pc_algo_persp_straighten_planes(pc_algo_persp_plane *planes, size_t n,
                                          pc_algo_persp_straighten mode, pc_error *err);

typedef struct pc_algo_persp_map pc_algo_persp_map;

/* Prepares the forward map (source -> destination). PC_ERR_INVALID when there are no planes or
 * a quad is degenerate (the reference's None). Free with pc_algo_persp_map_free. */
pc_status pc_algo_persp_map_new(const pc_algo_persp_plane *planes, size_t n, pc_algo_persp_map **out,
                                pc_error *err);
void pc_algo_persp_map_free(pc_algo_persp_map *m);
/* Maps a source point to the destination. */
void pc_algo_persp_map_apply(const pc_algo_persp_map *m, double x, double y, double *ox, double *oy);
/* Whether every plane maps onto itself. */
bool pc_algo_persp_is_identity(const pc_algo_persp_plane *planes, size_t n);

/* Corners of the unit square in plane order. */
void pc_algo_persp_unit_corners(double out[4][2]);
/* A point of plane p at plane coordinates (u, v), of its destination quad when dst (else source);
 * the quad's first corner when it is degenerate. */
void pc_algo_persp_plane_point(const pc_algo_persp_plane *p, double u, double v, bool dst, double out[2]);

#endif
