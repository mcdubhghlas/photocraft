/* Dab-driven stroke machinery for the retouching tools (port of crates/paint/src/retouch.rs):
 * Clone Stamp, Healing Brush, Spot Healing, Dodge/Burn/Sponge, Blur/Sharpen, Smudge, History
 * Brush, plus the Mixer Brush, Color Replacement and Background Eraser of this module.
 *
 * Two stroke shapes cover every retouching tool:
 *  - accumulate, then composite (pc_paint_stroke_coverage + pc_paint_apply_coverage): the
 *    stroke's dabs build up one coverage map, and a paint image computed once from the
 *    pre-stroke state is composited through it at the stroke opacity;
 *  - sequential dabs (pc_paint_apply_dab_stroke): each dab edits a working copy in place (dab n
 *    sees the result of dab n - 1, as Dodge, Blur or Smudge need), and the finished working copy
 *    is mixed back at the stroke opacity.
 *
 * All pixel data is the surface's native channels as normalised floats (any depth, any colour
 * model); colour-space-specific work happens in the callbacks. pc_paint_alpha_index,
 * pc_paint_over_native, pc_paint_footprint and pc_paint_rect_around live in render.h.
 *
 * Never crash: where the reference indexes out of range (a paint image that doesn't cover a
 * pixel, a short coverage map, a smudge radius that can't be allocated) these functions skip the
 * pixel or return a status instead. Allocations sized by input are capped (PC_ERR_LIMIT).
 *
 * Thread safety: objects are single-owner; functions only read their const inputs. */
#ifndef PC_PAINT_RETOUCH_H
#define PC_PAINT_RETOUCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "color/blend.h"
#include "geom/geom.h"
#include "paint/brush.h"
#include "paint/render.h"
#include "raster/surface.h"

/* ---- regions ---- */

/* A rectangle of native-channel pixels: width * height * ch floats, row-major. data is owned
 * (malloc; NULL when empty). Zero-initialised = an empty region. */
typedef struct pc_paint_region {
    pc_rect rect;
    size_t ch;
    float *data;
    size_t len;
} pc_paint_region;

/* A zeroed region (PC_ERR_LIMIT past PC_MAX_ALLOC). */
pc_status pc_paint_region_new(pc_rect rect, size_t ch, pc_paint_region *out, pc_error *err);
/* rect read from a surface. */
pc_status pc_paint_region_read(const pc_surface *s, pc_rect rect, pc_paint_region *out, pc_error *err);
pc_status pc_paint_region_clone(const pc_paint_region *r, pc_paint_region *out, pc_error *err);
void pc_paint_region_fini(pc_paint_region *r); /* NULL-safe; leaves an empty region */
size_t pc_paint_region_width(const pc_paint_region *r);
size_t pc_paint_region_height(const pc_paint_region *r);
/* The pixel at document (x, y): ch floats, or NULL outside rect (the reference panics). */
float *pc_paint_region_px(pc_paint_region *r, int32_t x, int32_t y);
const float *pc_paint_region_cpx(const pc_paint_region *r, int32_t x, int32_t y);
/* A copy of the sub-rectangle r (clipped to the region). */
pc_status pc_paint_region_crop(const pc_paint_region *src, pc_rect r, pc_paint_region *out, pc_error *err);
/* Write the region into a surface (channel count must match: else PC_ERR_INVALID). */
pc_status pc_paint_region_write(const pc_paint_region *r, pc_surface *s, pc_error *err);

/* ---- footprints and coverage ---- */

/* Bounding rectangle of a dab (1 px of anti-aliasing slack): pc_paint_rect_around(d, radius). */
pc_rect pc_paint_dab_rect(const pc_paint_dab *d);
/* A plain round footprint (hardness falloff x dab alpha; retouch.rs `footprint`). PC_ERR_LIMIT
 * past PC_PAINT_MAX_DAB_PIXELS. Release with pc_paint_footprint_fini. */
pc_status pc_paint_footprint_round(const pc_paint_dab *d, size_t index, float hardness,
                                   pc_paint_footprint *out, pc_error *err);
/* Union of the dabs' rectangles. */
pc_rect pc_paint_stroke_bounds(const pc_paint_dab *ds, size_t n);
/* Accumulated coverage of a whole stroke through the full brush engine (flow builds up to each
 * dab's ceiling, stroke masks applied): the stroke bounds and width * height values (free()). */
pc_status pc_paint_stroke_coverage(const pc_paint_stroke *stroke, pc_rect *rect, float **cov, size_t *n,
                                   pc_error *err);

/* ---- compositing ---- */

/* Composite a paint image through a coverage map onto target. cov has one value per pixel of
 * rect (row-major; values past ncov count as 0); the weight is cov x opacity x selection, capped
 * at 1. Non-normal modes composite in straight RGBA (W3C formula). Pixels outside paint's rect
 * are left alone. paint->ch must be at least the target's channel count (else PC_ERR_INVALID).
 * *damage = rect (empty for an empty rect). On failure target is unchanged, except that running
 * out of memory in the final region write may leave it partly written. */
pc_status pc_paint_apply_coverage(pc_surface *target, pc_rect rect, const float *cov, size_t ncov,
                                  float opacity, const pc_surface *selection, bool lock_transparency,
                                  const pc_paint_region *paint, pc_blend_mode blend, pc_rect *damage,
                                  pc_error *err);

/* One sequential dab: edit `work` (the working copy) under the footprint. A non-OK status aborts
 * the stroke (target untouched) and is returned. */
typedef pc_status (*pc_paint_dab_effect)(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp,
                                         pc_error *err);

/* Run a sequential-dab stroke: effect(ctx, work, footprint) edits the working copy for each dab
 * in order; the result is mixed back at brush opacity x selection. The working copy covers the
 * stroke bounds grown by halo pixels (neighbourhood effects read around each dab); only the
 * stroke bounds are written back. With lock_transparency alpha is preserved. *damage = the
 * stroke bounds (empty for no dabs). A failing dab or effect leaves target unchanged; running out
 * of memory in the final region write may leave it partly written. */
pc_status pc_paint_apply_dab_stroke(pc_surface *target, const pc_paint_stroke *stroke,
                                    const pc_surface *selection, bool lock_transparency, int32_t halo,
                                    pc_paint_dab_effect effect, void *ctx, pc_rect *damage, pc_error *err);

/* Mix a sequential stroke's working pixels back over the original at opacity x selection inside
 * rect and write that area to target (the last step of pc_paint_apply_dab_stroke, shared with
 * live previews). orig and work must cover rect with the target's channel count (else
 * PC_ERR_INVALID). */
pc_status pc_paint_mix_back(pc_surface *target, const pc_paint_region *orig, const pc_paint_region *work,
                            pc_rect rect, float opacity, const pc_surface *selection, bool lock_transparency,
                            pc_error *err);

/* dst <- lerp(dst, src, k) in premultiplied space (straight in, straight out; a = alpha index).
 * Where the result is fully transparent the colour is mixed straight. Uses min(ndst, nsrc)
 * channels; an alpha index outside either leaves dst untouched. */
void pc_paint_mix_premultiplied(float *dst, size_t ndst, const float *src, size_t nsrc, size_t a, float k);

/* ---- smudge ---- */

/* Smudge state: a colour buffer carried from dab to dab (centred on the dab), laid down at
 * strength and then re-picked-up from the pixels under the dab. With an alpha index the carried
 * colour is premultiplied, so smudging into transparency never drags in the colour of
 * transparent pixels. */
typedef struct pc_paint_smudge {
    float strength; /* 0..1 */
    float *finger;  /* finger painting colour (native channels, owned), NULL for none */
    size_t finger_len;
    int32_t radius;
    bool has_alpha;
    size_t alpha;
    float *carry; /* (2r+1)^2 * ch floats once the first dab ran; NULL before */
    size_t carry_len;
} pc_paint_smudge;

/* strength is clamped to 0..1; finger (may be NULL) is copied. PC_ERR_NOMEM only. */
pc_status pc_paint_smudge_init(pc_paint_smudge *s, float strength, const float *finger, size_t finger_len,
                               float max_brush_size, pc_error *err);
/* Mix in premultiplied space using alpha channel a (the format's alpha index). */
void pc_paint_smudge_with_alpha(pc_paint_smudge *s, bool has_alpha, size_t a);
/* Apply one dab to the working copy. PC_ERR_LIMIT when the carry buffer for the radius can't be
 * allocated (a hostile max_brush_size), PC_ERR_NOMEM. */
pc_status pc_paint_smudge_dab(pc_paint_smudge *s, pc_paint_region *work, const pc_paint_footprint *fp,
                              pc_error *err);
void pc_paint_smudge_fini(pc_paint_smudge *s); /* NULL-safe */
/* A pc_paint_dab_effect running pc_paint_smudge_dab (ctx = the pc_paint_smudge). */
pc_status pc_paint_smudge_effect(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp,
                                 pc_error *err);

#endif
