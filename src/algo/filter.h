/* Filters: parameters, halos, the filter registry and the tiled apply driver (port of the
 * FilterParams machinery in crates/algo/src/lib.rs).
 *
 * The reference's FilterParams is one Rust enum with a variant per filter. In C each variant is
 * a pc_algo_filter_def (a vtable: serde tag, label, parameter struct + JSON field table, halo,
 * kernel, optional whole-area override) and a pc_algo_filter pairs a def with an owned parameter
 * struct. The defs live with their algorithms in later chunks and are listed in per-chunk tables
 * (filters_l5.c, filters_l6.c, filters_l9.c), so adding filters never touches this file. The
 * variant list and labels of all 59 reference variants are fixed here, so JSON errors ("unknown
 * variant ..., expected one of ...") and labels match the reference before every def exists; a
 * known variant without a def yet is PC_ERR_UNSUPPORTED.
 *
 * apply: a filter runs over an area tile by tile (tiles of `tile` px in parallel on the global
 * pc_pool), each tile reading its input grown by the filter's halo (or the shared reference area
 * for global filters), and the result is mixed with the original by a selection's coverage.
 * Results never depend on the tiling or the thread count.
 *
 * Thread safety: defs are immutable; a pc_algo_filter is not synchronised; apply reads the
 * surfaces concurrently (const) and calls the def's kernel from several threads at once. */
#ifndef PC_ALGO_FILTER_H
#define PC_ALGO_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "algo/image.h"
#include "algo/params.h"
#include "base/buf.h"
#include "base/status.h"
#include "color/color.h"
#include "geom/geom.h"
#include "raster/interrupt.h"
#include "raster/surface.h"

/* Output tile size used by pc_algo_apply. */
#define PC_ALGO_TILE 256
/* Bytes of float filter results held at once by the tiled driver. */
#define PC_ALGO_RESULT_BUDGET ((size_t)256 << 20)
/* Number of FilterParams variants in the reference. */
#define PC_ALGO_FILTER_VARIANTS 60

/* How far outside an output tile a filter reads: a margin of `radius` pixels, or (bounds) the
 * whole reference bounds (distortions, radial blur). */
typedef struct pc_algo_halo {
    bool bounds;
    int32_t radius;
} pc_algo_halo;

static inline pc_algo_halo pc_algo_halo_radius(int32_t r)
{
    return (pc_algo_halo){false, r};
}
static inline pc_algo_halo pc_algo_halo_bounds(void)
{
    return (pc_algo_halo){true, 0};
}
static inline bool pc_algo_halo_eq(pc_algo_halo a, pc_algo_halo b)
{
    return a.bounds == b.bounds && (a.bounds || a.radius == b.radius);
}
/* Halo::Radius(v.max(0.0).ceil() as i32 + 1) — the reference's common `r(v)` helper. */
pc_algo_halo pc_algo_halo_reach(float v);

/* Context a filter runs in. */
typedef struct pc_algo_ctx {
    pc_rect bounds;     /* reference area (selection bounds, else the canvas) */
    pc_color_mode mode; /* colour model of the samples */
    bool alpha;         /* the last channel is alpha */
} pc_algo_ctx;

typedef struct pc_algo_filter_def pc_algo_filter_def;

/* A filter and its parameters (FilterParams). `params` is owned (def->params_size bytes). */
typedef struct pc_algo_filter {
    const pc_algo_filter_def *def;
    void *params;
} pc_algo_filter;

/* Arguments of the whole-area override (box blurs, FFT motion blur). */
typedef struct pc_algo_apply_args {
    const pc_surface *surface;
    const pc_algo_filter *filter;
    pc_rect area;
    pc_rect bounds;
    const pc_surface *selection; /* or NULL */
    int32_t tile;
    const pc_rect *extent;   /* or NULL */
    const pc_interrupt *ctl; /* or NULL */
} pc_algo_apply_args;

enum {
    /* Clamp the caller's tile size to 1..=2048 (Shape Blur's scratch is per tile). */
    PC_ALGO_FILTER_CLAMP_TILE = 1u,
    /* pc_algo_apply / _tiled / _in return an unchanged copy when the kernel or prepare step
     * fails with anything but PC_ERR_NOMEM (Shape Blur's reference behaviour). */
    PC_ALGO_FILTER_KEEP_ON_FAIL = 2u
};

struct pc_algo_filter_def {
    const char *tag;    /* serde tag ("gaussianBlur"); must be one of the reference variants */
    size_t params_size; /* sizeof the parameter struct (0 for unit variants) */
    /* Parameter JSON: the field table (generic read / write / copy / fini), and the defaults
     * written before reading (NULL: all zero). */
    const pc_algo_jfield *fields;
    size_t nfields;
    void (*init)(void *params);
    /* Optional extra validation/normalisation after a successful read. */
    pc_status (*check)(void *params, pc_error *err);
    /* FilterParams::halo_for: bounds is NULL for plain halo(). NULL: Radius(0). */
    pc_algo_halo (*halo)(const void *params, const pc_rect *bounds);
    /* Label override (Filter Gallery names its single effect); NULL: the table label. */
    const char *(*label)(const void *params);
    /* Optional per-run preparation (Shape Blur's rasterised kernel), shared by all tiles. */
    pc_status (*prepare)(const void *params, void **prep, pc_error *err);
    void (*prep_free)(void *prep);
    /* The kernel: writes out.w * out.h * src->ch samples (dst is zeroed) for `out` from src
     * (which covers out grown by the halo, or the bounds for global filters). */
    pc_status (*kernel)(const void *params, const void *prep, const pc_algo_image *src, pc_rect out,
                        const pc_algo_ctx *ctx, float *dst, pc_error *err);
    /* Optional whole-area path tried before tiling. Sets *handled; when handled and PC_OK, `out`
     * is initialised with the result; PC_ERR_CANCELLED when ctl cancels. */
    pc_status (*apply)(const pc_algo_apply_args *a, pc_surface *out, bool *handled, pc_error *err);
    unsigned flags;
};

/* ---- the variant table ---- */

/* The reference's serde tags in declaration order, and their labels. */
extern const char *const pc_algo_filter_tags[PC_ALGO_FILTER_VARIANTS];
extern const char *const pc_algo_filter_labels[PC_ALGO_FILTER_VARIANTS];
/* Index of a tag, or -1. */
int pc_algo_filter_tag_index(const char *tag, size_t len);
/* The registered def for a tag, or NULL (unknown tag, or not ported yet). */
const pc_algo_filter_def *pc_algo_filter_def_find(const char *tag);

/* ---- filter objects ---- */

/* A filter of `def` with a deep copy of `params` (def->params_size bytes; NULL = defaults). */
pc_status pc_algo_filter_make(const pc_algo_filter_def *def, const void *params, pc_algo_filter *out,
                              pc_error *err);
void pc_algo_filter_fini(pc_algo_filter *f); /* NULL-safe */
pc_status pc_algo_filter_clone(const pc_algo_filter *f, pc_algo_filter *out, pc_error *err);
/* serde Deserialize of FilterParams (internally tagged by "filter"), with serde_json::from_value
 * semantics: keys visited in byte order, a repeated key keeps its last value. */
pc_status pc_algo_filter_from_json(yyjson_val *v, pc_algo_filter *out, pc_error *err);
/* The same, looking tags up in `extra` (NULL-terminated, may be NULL) before the registry (tests,
 * plug-in style callers). */
pc_status pc_algo_filter_from_json_in(yyjson_val *v, const pc_algo_filter_def *const *extra,
                                      pc_algo_filter *out, pc_error *err);
pc_status pc_algo_filter_from_json_str(const char *text, size_t len, pc_algo_filter *out, pc_error *err);
/* serde Serialize: {"filter": tag, fields...}; NULL on out-of-memory. */
yyjson_mut_val *pc_algo_filter_to_json(yyjson_mut_doc *d, const pc_algo_filter *f);
/* Compact JSON text appended to *out. */
pc_status pc_algo_filter_to_json_text(const pc_algo_filter *f, pc_buf *out, pc_error *err);
/* Same def and the same serialised parameters. */
bool pc_algo_filter_eq(const pc_algo_filter *a, const pc_algo_filter *b);

pc_algo_halo pc_algo_filter_halo(const pc_algo_filter *f);
pc_algo_halo pc_algo_filter_halo_for(const pc_algo_filter *f, pc_rect bounds);
bool pc_algo_filter_is_global(const pc_algo_filter *f);
/* FilterParams::makes_transparency: the filter's job is to make pixels transparent (Color to
 * Alpha), so callers give it a layer that can hold transparency. */
bool pc_algo_filter_makes_transparency(const pc_algo_filter *f);
const char *pc_algo_filter_label(const pc_algo_filter *f);

/* ---- running ---- */

/* kernel(): the filter's output for `out` from src, newly allocated (free()). */
pc_status pc_algo_kernel(const pc_algo_filter *f, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float **data, size_t *len, pc_error *err);

/* The area a filter writes for a layer whose pixels cover `content`: global filters the bounds,
 * others content grown by the halo; clipped to selection_bounds when given (may be NULL). */
pc_rect pc_algo_output_area(const pc_algo_filter *f, pc_rect content, pc_rect bounds,
                            const pc_rect *selection_bounds);
/* Tile size for apply: wide halos use bigger tiles (results don't depend on it). */
int32_t pc_algo_auto_tile(const pc_algo_filter *f, pc_rect bounds);

/* apply(): the filter over `area`, mixed with the original by channel 0 of `selection` (NULL =
 * everywhere). `out` is initialised on success. */
pc_status pc_algo_apply(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                        const pc_surface *selection, pc_surface *out, pc_error *err);
/* apply_in(): neighbourhood filters repeat the edge pixels of `extent` (canvas plus the layer's
 * off-canvas pixels) and the output is clipped to it. */
pc_status pc_algo_apply_in(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                           const pc_surface *selection, pc_rect extent, pc_surface *out, pc_error *err);
/* apply_in_with(): cancellable (checked before each tile; PC_ERR_CANCELLED) with progress. */
pc_status pc_algo_apply_in_with(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                                const pc_surface *selection, pc_rect extent, const pc_interrupt *ctl,
                                pc_surface *out, pc_error *err);
/* apply_tiled(): explicit tile size; extent may be NULL. */
pc_status pc_algo_apply_tiled(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                              const pc_surface *selection, int32_t tile, const pc_rect *extent,
                              pc_surface *out, pc_error *err);
/* apply_tiled_with(): the driver everything above uses. */
pc_status pc_algo_apply_tiled_with(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                                   const pc_surface *selection, int32_t tile, const pc_rect *extent,
                                   const pc_interrupt *ctl, pc_surface *out, pc_error *err);

/* Mixes filtered `data` (t.w * t.h * orig->ch samples) for tile t back toward the original
 * pixels in `orig` by the selection's coverage (channel 0 of sel, clamped to 0..1). With `alpha`
 * (the last channel is alpha) a pixel whose alpha the filter changed is mixed premultiplied, so
 * hidden colours don't leak into feathered edges; coverage 0 restores the original exactly. */
void pc_algo_mix_selection(float *data, pc_rect t, const pc_surface *sel, const pc_algo_image *orig,
                           bool alpha);

#endif
