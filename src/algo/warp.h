/* Mesh warps (Edit > Transform > Warp, smart-object warps): resampling a surface through any
 * forward map, such as a pc_warp (bicubic Bezier patches or a preset) (port of
 * crates/algo/src/warp.rs).
 *
 * The source rectangle is subdivided into a grid (cells of 4 source px, at most 512 per axis)
 * whose vertices are mapped forward; each cell's two triangles are rasterized in destination
 * space with the source position interpolated barycentrically, then the source is sampled there
 * (premultiplied, nearest / bilinear / Catmull-Rom bicubic, like Free Transform). Affine maps
 * (the identity included) are reproduced exactly. Where the warp folds over itself the later
 * (lower-right) cell wins. Work is split per destination tile on the global pool; results do not
 * depend on the thread count.
 *
 * Outputs are initialised on success only (caller frees with pc_surface_fini). */
#ifndef PC_ALGO_WARP_H
#define PC_ALGO_WARP_H

#include <stddef.h>

#include "algo/params.h"
#include "algo/transform.h"
#include "base/status.h"
#include "geom/geom.h"
#include "geom/warp.h"
#include "raster/surface.h"

/* A forward map (source document -> destination document coordinates). Must be thread-safe. */
typedef void (*pc_algo_map_fn)(void *ctx, double x, double y, double *ox, double *oy);

/* One mesh vertex: destination and source position in document pixels. */
typedef struct pc_algo_warp_vertex {
    double dst[2], src[2];
} pc_algo_warp_vertex;

/* Warps the content of src inside src_rect through f. Output has src's format with alpha added;
 * pixels outside the warped area are transparent. */
pc_status pc_algo_warp_mesh_surface(const pc_surface *src, pc_rect src_rect, pc_algo_map_fn f, void *ctx,
                                    pc_algo_interp interp, pc_surface *out, pc_error *err);

/* Rasterizes textured triangles (indices into verts; later triangles draw over earlier ones),
 * sampling src restricted to src_rect. Out-of-range indices are PC_ERR_INVALID. */
pc_status pc_algo_warp_triangles(const pc_surface *src, pc_rect src_rect, const pc_algo_warp_vertex *verts,
                                 size_t nverts, const size_t (*tris)[3], size_t ntris, pc_algo_interp interp,
                                 pc_surface *out, pc_error *err);

/* warp_mesh_surface for single-channel surfaces without alpha whose outside reads as their default
 * (layer masks, selections): the content is warped with alpha and flattened onto the default.
 * Other formats are PC_ERR_INVALID. */
pc_status pc_algo_warp_mesh_gray(const pc_surface *s, pc_algo_map_fn f, void *ctx, pc_algo_interp interp,
                                 pc_surface *out, pc_error *err);

/* Places a smart object's source image (pixels in src_rect) in the document: through `warp`
 * (source space, NULL or identity = none) then the affine t in one resampling pass; an exact
 * shift for whole-pixel translations, bicubic otherwise. */
pc_status pc_algo_place_source(const pc_surface *src, pc_rect src_rect, const pc_affine *t,
                               const pc_warp *warp, pc_surface *out, pc_error *err);
/* The same through a projective map h (Distort, Perspective). */
pc_status pc_algo_place_source_projective(const pc_surface *src, pc_rect src_rect,
                                          const pc_algo_homography *h, const pc_warp *warp, pc_surface *out,
                                          pc_error *err);

#endif
