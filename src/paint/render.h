/* Dab rasterisation and stroke compositing (port of crates/paint/src/render.rs, plus the small
 * native-channel helpers of retouch.rs that the renderer needs: alpha index, "over", footprint).
 *
 * A stroke paints into a sparse, tiled stroke buffer (coverage plus, with Color Dynamics, a
 * premultiplied colour), like a private stroke layer: dabs build up with flow up to their
 * opacity ceiling, stroke-level masks (Dual Brush, Texture) apply when the buffer is composited,
 * and the whole buffer composites onto the pre-stroke pixels at the stroke opacity. Because the
 * buffer is sparse and compositing always starts from the pre-stroke pixels, strokes can be fed
 * in chunks and re-composited incrementally (pc_paint_renderer).
 *
 * Limits (never crash): one dab may cover at most PC_PAINT_MAX_DAB_PIXELS pixels (the largest
 * legal brush, 5000 px with a rotated sampled tip, needs ~50M); a larger dab from a direct caller
 * is PC_ERR_LIMIT where the reference would abort on allocation. Dense outputs over PC_MAX_ALLOC
 * are PC_ERR_LIMIT too.
 *
 * Thread safety: a context is immutable after init (its tip mips and texture are shared,
 * reference counted); a renderer is single-owner. Compositing reads `pre`/`selection` and writes
 * `target`, which must not be in use elsewhere. */
#ifndef PC_PAINT_RENDER_H
#define PC_PAINT_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "color/color.h"
#include "geom/geom.h"
#include "paint/brush.h"
#include "paint/dynamics.h"
#include "paint/tile.h"
#include "raster/surface.h"

/* Edge of a stroke-buffer tile. */
#define PC_PAINT_COV_TILE 64
#define PC_PAINT_MAX_DAB_PIXELS ((size_t)1 << 26)

/* Where an aliased (Pencil) dab of `diameter` px near (x, y) lands on the pixel grid: the centre
 * of the pixel holding the point for odd diameters, the nearest pixel corner for even ones. */
void pc_paint_grid_center(double x, double y, float diameter, float *cx, float *cy);
/* The whole-pixel square {x0, y0, x1, y1} such a dab can touch (the Pencil's cursor). */
void pc_paint_grid_square(double x, double y, float diameter, double out[4]);

/* Combine brush coverage v with a mask value t (texture/dual tip) at depth d. Every mode maps
 * v = 0 to 0. */
float pc_paint_mask_combine(pc_paint_mask_mode mode, float v, float t, float d);

/* The pixel rectangle `reach` (+1 px anti-aliasing slack) around a dab's centre. Casts and
 * arithmetic saturate, so a far or non-finite centre or reach gives a (possibly empty) rect.
 * The retouch tools' dab_rect is pc_paint_rect_around(d, d->radius). */
pc_rect pc_paint_rect_around(const pc_paint_dab *d, float reach);

/* A growable float buffer (rasterisation output). Zero-initialised = empty. */
typedef struct pc_paint_fbuf {
    float *v;
    size_t len, cap;
} pc_paint_fbuf;
/* Resize to n zeroes (keeps the allocation when it is big enough). */
pc_status pc_paint_fbuf_zeroed(pc_paint_fbuf *b, size_t n, pc_error *err);
void pc_paint_fbuf_fini(pc_paint_fbuf *b);

/* A stroke-ready brush: bounded settings plus prepared tip mips and texture. */
typedef struct pc_paint_context {
    pc_paint_brush brush;            /* bounded_for_render copy; read-only after init */
    pc_paint_mips *tip;              /* NULL unless a valid sampled tip */
    pc_paint_mips *dual_tip;         /* NULL unless dual brush on with a valid sampled tip */
    pc_paint_pattern_image *texture; /* NULL unless texture on (and a valid tile) */
} pc_paint_context;

pc_status pc_paint_context_init(pc_paint_context *ctx, const pc_paint_brush *brush, pc_error *err);
void pc_paint_context_fini(pc_paint_context *ctx); /* NULL-safe */
/* *dst = a copy sharing src's prepared data. Never fails. */
void pc_paint_context_copy(pc_paint_context *dst, const pc_paint_context *src);
/* Texture value at a document pixel (bilinear, tiled); 1 without a texture. */
float pc_paint_context_texture_at(const pc_paint_context *ctx, int32_t x, int32_t y);
/* Bounding rectangle of a dab (rotated sampled tips need the corner reach). */
pc_rect pc_paint_context_dab_rect(const pc_paint_context *ctx, const pc_paint_dab *d, bool dual);
/* Rasterise a dab over `rect` into out (w * h values, row-major): tip x noise x per-tip texture x
 * wet edges x flow. PC_ERR_LIMIT when rect exceeds PC_PAINT_MAX_DAB_PIXELS. */
pc_status pc_paint_context_rasterize(const pc_paint_context *ctx, const pc_paint_dab *d, bool dual,
                                     pc_rect rect, pc_paint_fbuf *out, pc_error *err);
/* Stroke-level masks applied to accumulated coverage c at a pixel (dual: has_dual + value). */
float pc_paint_context_stroke_mask(const pc_paint_context *ctx, float c, bool has_dual, float dual, int32_t x,
                                   int32_t y);

/* One dab's footprint: coverage over its bounding rectangle (for the sequential retouch tools). */
typedef struct pc_paint_footprint {
    pc_paint_dab dab;
    size_t index; /* position in the stroke */
    pc_rect rect;
    float *cov; /* rect.w * rect.h values, owned (malloc) */
    size_t len;
} pc_paint_footprint;

pc_status pc_paint_context_footprint(const pc_paint_context *ctx, const pc_paint_dab *d, size_t index,
                                     pc_paint_footprint *out, pc_error *err);
void pc_paint_footprint_fini(pc_paint_footprint *f); /* NULL-safe */
/* Coverage at a document pixel; 0 outside rect. */
float pc_paint_footprint_at(const pc_paint_footprint *f, int32_t x, int32_t y);

/* Index of the alpha channel, if the format has one. */
bool pc_paint_alpha_index(pc_pixel_format fmt, size_t *index);
/* Normal-mode "source over destination" at coverage k in native channels (any model, depth).
 * dst and src hold the format's channel count. */
void pc_paint_over_native(pc_pixel_format fmt, float *dst, const float *src, float k, bool lock_transparency);

/* ---- incremental stroke renderer ---- */

typedef struct pc_paint_renderer pc_paint_renderer;

/* fmt = target pixel format (needed for per-dab colour), NULL for none; zoom for smoothing. */
pc_status pc_paint_renderer_new(const pc_paint_brush *brush, const pc_pixel_format *fmt, float zoom,
                                pc_paint_renderer **out, pc_error *err);
void pc_paint_renderer_free(pc_paint_renderer *r); /* NULL-safe */
/* Keep a copy of every primary dab from now on (tests and sequential tools). */
void pc_paint_renderer_record_dabs(pc_paint_renderer *r);
/* Recorded dabs (borrowed until the next push/finish); NULL/0 when not recording. */
const pc_paint_dab *pc_paint_renderer_dabs(const pc_paint_renderer *r, size_t *n);
size_t pc_paint_renderer_dab_count(const pc_paint_renderer *r);
/* Union of the dab rectangles so far. */
pc_rect pc_paint_renderer_bounds(const pc_paint_renderer *r);
const pc_paint_context *pc_paint_renderer_context(const pc_paint_renderer *r);
/* Feed input points (any chunking gives the same result). */
pc_status pc_paint_renderer_push(pc_paint_renderer *r, const pc_paint_point *pts, size_t n, pc_error *err);
/* End of stroke (smoothing catch-up, a lone first point). */
pc_status pc_paint_renderer_finish(pc_paint_renderer *r, pc_error *err);
/* What finishing now would add, for live previews: a renderer holding copies of the coverage
 * tiles the tail touches with the tail rendered in. *out is NULL when finishing adds nothing. */
pc_status pc_paint_renderer_tail_preview(const pc_paint_renderer *r, pc_paint_renderer **out, pc_error *err);
/* Mark the coverage tiles over rect for the next composite. */
void pc_paint_renderer_mark_dirty(pc_paint_renderer *r, pc_rect rect);
/* The tiles touched since the last call (or composite) as one rect clipped to the bounds; forgets them. */
pc_rect pc_paint_renderer_take_dirty_rect(pc_paint_renderer *r);
/* Final stroke coverage at a pixel (stroke masks applied, before opacity/selection). */
float pc_paint_renderer_coverage_at(const pc_paint_renderer *r, int32_t x, int32_t y);
/* Final coverage over rect, one value per pixel in rows (free()); *out NULL when empty. */
pc_status pc_paint_renderer_coverage_in(const pc_paint_renderer *r, pc_rect rect, float **out, size_t *n,
                                        pc_error *err);
/* Dense final coverage over the stroke bounds (free()). */
pc_status pc_paint_renderer_dense_coverage(const pc_paint_renderer *r, pc_rect *rect, float **out, size_t *n,
                                           pc_error *err);
/* Composite the stroke buffer onto target, reading original pixels from pre (the pre-stroke
 * surface). all = false processes only tiles touched since the last composite. *damage gets the
 * damaged rect. On failure target may be partly written. selection may be NULL. */
pc_status pc_paint_renderer_composite(pc_paint_renderer *r, const pc_surface *pre, pc_surface *target,
                                      const pc_surface *selection, bool lock_transparency, bool all,
                                      pc_rect *damage, pc_error *err);
/* Composite the union of this stroke and another pass (a mirrored one) as one stroke. */
pc_status pc_paint_renderer_composite_union(const pc_paint_renderer *r, const pc_paint_renderer *other,
                                            const pc_surface *pre, pc_surface *target,
                                            const pc_surface *selection, bool lock_transparency,
                                            pc_rect *damage, pc_error *err);

/* Render a whole stroke onto target (one-shot). *damage = the damaged rect (empty for no
 * points). */
pc_status pc_paint_render_stroke(pc_surface *target, const pc_paint_brush *brush, const pc_paint_point *pts,
                                 size_t n, const pc_surface *selection, bool lock_transparency, float zoom,
                                 pc_rect *damage, pc_error *err);
/* lib.rs apply_stroke: render_stroke at zoom 1. */
pc_status pc_paint_apply_stroke(pc_surface *target, const pc_paint_stroke *stroke,
                                const pc_surface *selection, bool lock_transparency, pc_rect *damage,
                                pc_error *err);

#endif
