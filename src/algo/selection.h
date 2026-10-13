/* Selection algorithms on coverage masks (port of crates/algo/src/selection.rs).
 *
 * A mask is w x h row-major floats in 0..=1 over an area: magic wand / flood regions, Color
 * Range (sampled colours, Lab boxes, hue families, tonal ranges), modify (expand, contract,
 * border, smooth, feather), anti-aliased lasso polygons, and combining with an existing
 * selection surface (GRAY8). A pc_algo_region is 8-bit coverage over a bounding box; the
 * segmentation and matting modules (segment.h, matting.h) and retouching return it too.
 *
 * selection::color_range itself is pc_algo_color_range in fxutil.h; smooth and feather are
 * pc_algo_selection_smooth / _feather in selection_blur.h (L5).
 *
 * Failure: functions that allocate return pc_status (PC_ERR_NOMEM, PC_ERR_LIMIT past
 * PC_MAX_ALLOC); a buffer whose length doesn't match its w x h (where the reference would panic
 * on an index) is PC_ERR_INVALID. Outputs are written only on success. Everything is
 * thread-safe on distinct outputs; large loops use the global pool (results don't depend on the
 * thread count). */
#ifndef PC_ALGO_SELECTION_H
#define PC_ALGO_SELECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* How a new selection combines with the current one (serde camelCase names). */
typedef enum pc_algo_selection_mode {
    PC_ALGO_SELECTION_REPLACE,
    PC_ALGO_SELECTION_ADD,
    PC_ALGO_SELECTION_SUBTRACT,
    PC_ALGO_SELECTION_INTERSECT
} pc_algo_selection_mode;

/* SelectionMode::parse: "add" | "subtract" | "intersect", anything else (or NULL) is Replace. */
pc_algo_selection_mode pc_algo_selection_mode_parse(const char *s);
/* "replace", "add", "subtract", "intersect". */
const char *pc_algo_selection_mode_name(pc_algo_selection_mode m);

/* A selected region: 8-bit coverage over `bbox` (row-major, bbox.w * bbox.h bytes). Functions
 * returning Option<Region> in the reference write an empty region (empty bbox, mask NULL) for
 * None: test with pc_algo_region_is_none. Owned: pc_algo_region_fini frees mask. */
typedef struct pc_algo_region {
    pc_rect bbox;
    uint8_t *mask;
    size_t len;
} pc_algo_region;

void pc_algo_region_fini(pc_algo_region *r); /* NULL-safe; leaves an empty region */
static inline bool pc_algo_region_is_none(const pc_algo_region *r)
{
    return r->mask == NULL && pc_rect_is_empty(r->bbox);
}
/* Coverage (0..1) at document pixel (x, y); 0 outside the box (or past the mask). */
float pc_algo_region_at(const pc_algo_region *r, int32_t x, int32_t y);
/* Deep copy (out initialised on success). */
pc_status pc_algo_region_clone(const pc_algo_region *r, pc_algo_region *out, pc_error *err);

/* Reads a selection surface over `area` (NULL surface: all zero) as w * h floats (channel 0),
 * newly allocated (free()); *out is NULL for an empty area. */
pc_status pc_algo_selection_mask_from_surface(const pc_surface *s, pc_rect area, float **out, size_t *len,
                                              pc_error *err);
/* A GRAY8 selection surface from a mask over `area` (missing samples read 0; tiles holding only
 * zeros are not allocated). `out` is initialised on success. */
pc_status pc_algo_selection_mask_to_surface(const float *m, size_t len, pc_rect area, pc_surface *out,
                                            pc_error *err);
/* Combines `m` (over area, missing samples 0) with `old` (may be NULL) by `mode`, quantised to
 * the 8-bit selection grid. *some is false when nothing is selected (then `out` is an empty
 * GRAY8 surface); `out` is initialised on success either way. */
pc_status pc_algo_selection_combine(const pc_surface *old, const float *m, size_t len, pc_rect area,
                                    pc_algo_selection_mode mode, pc_surface *out, bool *some, pc_error *err);

/* Magic wand over straight RGBA float pixels covering `area` (px_len == w * h): pixels whose
 * every channel is within `tolerance` levels of the seed's; `contiguous` limits to the
 * 4-connected region. A seed outside the area gives all zeros. out: w * h floats (free()). */
pc_status pc_algo_selection_magic_wand(const float (*px)[4], size_t px_len, pc_rect area, int32_t seed_x,
                                       int32_t seed_y, float tolerance, bool contiguous, bool anti_alias,
                                       float **out, size_t *len, pc_error *err);
/* Straight RGBA8 copy of `area` of a surface (w * h pixels, free()); NULL for an empty area. */
pc_status pc_algo_selection_rgba8_image(const pc_surface *s, pc_rect area, uint8_t (**out)[4], size_t *len,
                                        pc_error *err);
/* Magic wand on 8-bit pixels (scanline fill, anti-aliasing only inside the touched box). The
 * region is None when the seed lies outside `area`. */
pc_status pc_algo_selection_wand_region(const uint8_t (*img)[4], size_t img_len, pc_rect area, int32_t seed_x,
                                        int32_t seed_y, float tolerance, bool contiguous, bool anti_alias,
                                        pc_algo_region *out, pc_error *err);
/* Combines a region (NULL = None) with the current selection (NULL = none), touching only the
 * region's box. Same *some / out contract as pc_algo_selection_combine. */
pc_status pc_algo_selection_combine_region(const pc_surface *old, const pc_algo_region *region,
                                           pc_algo_selection_mode mode, pc_surface *out, bool *some,
                                           pc_error *err);

/* The Elliptical Marquee's shape (selection/marquee.rs): the ellipse inscribed in r, cut to clip,
 * as a GRAY8 selection. Anti-aliased edge pixels hold the fraction of their 4x4 sub-samples
 * inside; without anti-aliasing a pixel is selected when its centre is inside. Built tile by tile
 * on the global pool; fully selected tiles share one tile. `out` initialised on success. */
pc_status pc_algo_selection_ellipse_surface(pc_rect r, pc_rect clip, bool anti_alias, pc_surface *out,
                                            pc_error *err);
/* f(ctx, a, b) per pixel for two selection values. */
typedef float (*pc_algo_combine_fn)(void *ctx, float a, float b);
/* f per pixel over `area` for two selections, as a GRAY8 selection holding nothing outside it:
 * the same bytes as reading both over area, mapping and writing the result. GRAY8 inputs go tile
 * by tile through a table of all 256 x 256 byte pairs (f is called 65,536 times, on the calling
 * thread); a tile f leaves unchanged stays shared with its input. */
pc_status pc_algo_selection_combine_surfaces(const pc_surface *a, const pc_surface *b, pc_rect area,
                                             pc_algo_combine_fn f, void *fctx, pc_surface *out,
                                             pc_error *err);

/* ---- Color Range ---- */

/* One Sampled Colors sample: an eyedropper colour, and (has_at) where it was picked. */
typedef struct pc_algo_range_sample {
    float color[3];
    bool has_at;
    float at_x, at_y;
} pc_algo_range_sample;

/* Each pixel takes its best match over the samples (as pc_algo_color_range); with `localized`
 * (has_radius) a positioned sample fades to 0 at `radius` pixels. `w` is px's row width.
 * mask holds n values. */
void pc_algo_selection_color_range_samples(const float (*px)[4], size_t n, size_t w,
                                           const pc_algo_range_sample *samples, size_t nsamples,
                                           float fuzziness, bool has_radius, float radius, float *mask);

/* The Lab box spanned by Sampled Colors samples (CIE Lab D50). */
typedef struct pc_algo_lab_range {
    float lo[3];
    float hi[3];
} pc_algo_lab_range;

pc_algo_lab_range pc_algo_lab_range_point(const float lab[3]);
/* false (out untouched) when n == 0. */
bool pc_algo_lab_range_spanning(const float (*colors)[3], size_t n, pc_algo_lab_range *out);
pc_algo_lab_range pc_algo_lab_range_grow(pc_algo_lab_range r, const float lab[3]);
/* "Subtract from Sample" as Photoshop reshapes the box (see the reference docs). */
pc_algo_lab_range pc_algo_lab_range_subtract(pc_algo_lab_range r, const float lab[3], float fuzziness);
/* How much a Lab colour belongs to `range` at `fuzziness` (0..200). */
float pc_algo_lab_range_coverage(const float lab[3], const pc_algo_lab_range *range, float fuzziness);
/* The same on Photoshop's 8-bit Lab grid. */
float pc_algo_lab_range_coverage_8bit(const float lab[3], const pc_algo_lab_range *range, float fuzziness);
/* A Lab colour on the 8-bit Lab grid. */
void pc_algo_quantize_lab8(const float lab[3], float out[3]);
/* Color Range over Lab pixels (L, a, b, alpha; row width w): coverage per pixel, 0 where
 * transparent; with npoints > 0 (Localized Color Clusters) faded by the distance to the nearest
 * position, reaching 0 at `radius`. mask holds n values. Parallel over rows. */
pc_status pc_algo_selection_color_range_lab(const float (*lab)[4], size_t n, size_t w,
                                            const pc_algo_lab_range *range, float fuzziness, bool grid8,
                                            bool localized, float radius, const float (*points)[2],
                                            size_t npoints, float *mask, pc_error *err);
/* Reds..Magentas (`center` 0, 60, ... 300 degrees): hue closeness times chroma. */
void pc_algo_selection_hue_range(const float (*px)[4], size_t n, float center, float *mask);
/* Highlights / Midtones / Shadows: luminance (0..255) in lo..=hi, falling off over `falloff`. */
void pc_algo_selection_tone_range(const float (*px)[4], size_t n, float lo, float hi, float falloff,
                                  float *mask);

/* ---- Modify ---- */

/* Euclidean distance to the nearest true pixel (w * h; 1e10 everywhere when there is none; *out
 * NULL for an empty mask; PC_ERR_LIMIT past PC_MAX_ALLOC or a side past 2^32 - 1). Parallel for
 * large masks, identical with any thread count. */
pc_status pc_algo_selection_edt(const bool *inside, size_t w, size_t h, float **out, pc_error *err);
/* Grow / shrink by r pixels; border: Photoshop's soft band 2r wide centred on the edge (a pixel D
 * from the nearest pixel across the edge is 1 - (D - 1) / r; all zero for r <= 0). m has w * h values;
 * out is w * h (free()). */
pc_status pc_algo_selection_expand(const float *m, size_t w, size_t h, float r, float **out, pc_error *err);
pc_status pc_algo_selection_contract(const float *m, size_t w, size_t h, float r, float **out, pc_error *err);
pc_status pc_algo_selection_border(const float *m, size_t w, size_t h, float r, float **out, pc_error *err);
/* Anti-aliased even-odd polygon coverage over `area` (4 sub-rows per pixel row when
 * anti-aliased). out: w * h floats (free()). */
pc_status pc_algo_selection_polygon(const float (*points)[2], size_t npoints, pc_rect area, bool anti_alias,
                                    float **out, size_t *len, pc_error *err);

#endif
