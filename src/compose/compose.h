/* The CPU reference compositor (port of crates/compose): flattens a pc_document layer tree into
 * straight-alpha RGBA (f32) for any rectangle. This is the oracle the GPU compositor must match
 * (REWRITE_PLAN.md D5). Umbrella header of src/compose.
 *
 * It encodes Photoshop's layer semantics in one place: blend modes, opacity x fill opacity,
 * visibility, layer masks with density and feather, vector masks, clipping groups, pass-through
 * vs isolated groups, artboards, Dissolve, channel restrictions, Blend If, adjustment layers
 * (applied to the composite below), fill layers, layer effects and advanced blending.
 * Compositing happens in display RGB; CMYK layers are read through the document's CMYK profile
 * (color/convert.h scope), Lab documents mix Normal blending in CIELAB and 32-bit documents
 * don't clip Add / Divide (compose/psblend.h flags).
 *
 * Files (C1 = this chunk; C2 and C3 complete the module):
 *   compose.h       buffers, rendering, layer queries (C1: buffer.c render.c composite.c query.c
 *                   reduce.c fx_cache.c)
 *   psblend.h       Photoshop blend variants and the compositing formula (C1: psblend.c)
 *   masks.h         combined (pixel x vector, feathered) masks, cached (C1: masks.c)
 *   proxy.h         downsampled proxy documents, multichannel inks, cached content bounds
 *                   (C1: proxy.c multichannel.c bounds.c)
 *   fill_layout.h   gradient fill geometry (C1: fill_layout.c)
 *   effects.h       layer effects. C1 implements the queries the core needs (fx_basics.c:
 *                   has_effects, margin, gradient_units, gradient_t); C3 implements the effect
 *                   maps (build, bytes, free, stroke_frame, composite) in effects*.c and deletes
 *                   stub_c3.c.
 *   adjust.h        adjustment layers (C2: adjust.c adjust_tone.c)
 *   pattern.h       prepared patterns, tiles, placements and pattern fills (C2: pattern.c)
 *   gradient_fill.h gradient fill layers (C2: gradient_fill.c)
 *   shape_split.h   stroked shape layers split into fill and stroke (C2: shape_split.c)
 *   advanced.h      Advanced Blending (knockout, Blend Clipped Layers as Group off, Transparency
 *                   Shapes Layer off, masks hiding effects) (C2: advanced.c; the C2 files share
 *                   adjust_internal.h and stable_sort.h)
 *   internal.h      the render context, the scope and the compositor internals that advanced.c
 *                   and effects.c call back into (pc_cmp_*), plus the advanced-blending hooks
 *                   the core calls (also C2).
 * C2 and C3 replaced the temporary stub_c2.c / stub_c3.c with the real files.
 *
 * Conventions (REWRITE_PLAN.md §4): fallible functions return pc_status and take an optional
 * pc_error; out-params are written only on success. Nothing aborts: absurd rectangles or
 * thumbnail sizes are PC_ERR_LIMIT (buffers are capped at PC_COMPOSE_MAX_PIXELS), groups nested
 * deeper than PC_COMPOSE_MAX_DEPTH are PC_ERR_LIMIT (the reference recurses without bound), and
 * a pc_interrupt (checked per render tile) gives PC_ERR_CANCELLED.
 *
 * Thread safety: rendering only reads the document; any number of renders may run at once on
 * different threads (the caches below are mutex-protected). Tiles render on pc_pool_global();
 * results are identical for any thread count (D6). The document must not be mutated during a
 * render. */
#ifndef PC_COMPOSE_COMPOSE_H
#define PC_COMPOSE_COMPOSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "color/convert.h"
#include "compose/fill_layout.h"
#include "compose/masks.h"
#include "compose/proxy.h"
#include "compose/psblend.h"
#include "doc/doc.h"
#include "raster/interrupt.h"
#include "raster/pixel.h"
#include "raster/surface.h"

/* Tile size for parallel rendering (tile results are independent). */
#define PC_COMPOSE_RENDER_TILE 256
/* Pixels per band of pc_compose_render_bands (8 Mi). */
#define PC_COMPOSE_BAND_PIXELS ((uint64_t)8 << 20)
/* Documents above this many pixels get thumbnails from a proxy when that is faithful. */
#define PC_COMPOSE_PROXY_THUMBNAIL_PIXELS ((uint64_t)16 << 20)
/* Default effect-map cache budget in bytes. */
#define PC_COMPOSE_DEFAULT_FX_CACHE_BUDGET ((size_t)768 << 20)
/* Largest buffer one call allocates (2^28 pixels = 4 GiB of RGBA f32). */
#define PC_COMPOSE_MAX_PIXELS ((uint64_t)1 << 28)
/* Deepest group nesting the compositor descends (> PC_MAX_GROUP_DEPTH, so every legal document
 * renders); deeper trees are PC_ERR_LIMIT. */
#define PC_COMPOSE_MAX_DEPTH 128

/* Straight-alpha RGBA float buffer covering a rectangle (row-major, len = w * h). */
typedef struct pc_compose_buffer {
    pc_rect rect;
    float (*px)[4]; /* owned; NULL when len is 0 */
    size_t len;
} pc_compose_buffer;

pc_status pc_compose_buffer_transparent(pc_rect rect, pc_compose_buffer *out, pc_error *err);
pc_status pc_compose_buffer_filled(pc_rect rect, const float c[4], pc_compose_buffer *out, pc_error *err);
pc_status pc_compose_buffer_clone(const pc_compose_buffer *b, pc_compose_buffer *out, pc_error *err);
void pc_compose_buffer_fini(pc_compose_buffer *b); /* NULL-safe; leaves b empty */
/* The pixel at document (x, y), or NULL outside the rect (the reference panics). */
const float *pc_compose_buffer_get(const pc_compose_buffer *b, int32_t x, int32_t y);
/* Clamped to [0, 1], rounded to 8 bits. */
pc_status pc_compose_buffer_to_rgba8(const pc_compose_buffer *b, pc_rgba8_image *out, pc_error *err);
/* Flattened in place over an opaque background colour. */
void pc_compose_buffer_over_background(pc_compose_buffer *b, const float bg[3]);
/* Bitwise equality of rect and pixels. */
bool pc_compose_buffer_eq(const pc_compose_buffer *a, const pc_compose_buffer *b);

/* Composite the whole document over rect (tiles of PC_COMPOSE_RENDER_TILE in parallel). */
pc_status pc_compose_render(const pc_document *doc, pc_rect rect, pc_compose_buffer *out, pc_error *err);
/* pc_compose_render with an explicit tile size (< 1 counts as 1) and an optional interrupt. */
pc_status pc_compose_render_tiled(const pc_document *doc, pc_rect rect, int32_t tile,
                                  const pc_interrupt *intr, pc_compose_buffer *out, pc_error *err);
/* The full canvas. */
pc_status pc_compose_flatten(const pc_document *doc, pc_compose_buffer *out, pc_error *err);

/* Receives one band (owned by the caller of the sink; it may steal band->px by zeroing the
 * struct). A non-OK status stops the render and is returned. */
typedef pc_status (*pc_compose_band_sink)(void *ctx, pc_compose_buffer *band, pc_error *err);
/* Composite rect in horizontal bands, top to bottom, each handed to sink; peak memory is one
 * band. band_rows 0 picks about PC_COMPOSE_BAND_PIXELS per band; bands are whole multiples of
 * PC_COMPOSE_RENDER_TILE rows. */
pc_status pc_compose_render_bands(const pc_document *doc, pc_rect rect, int32_t band_rows,
                                  const pc_interrupt *intr, pc_compose_band_sink sink, void *ctx,
                                  pc_error *err);
/* Band height: requested rounded up to whole tiles, or about PC_COMPOSE_BAND_PIXELS / width. */
int32_t pc_compose_band_rows_for(uint32_t width, int32_t requested);
/* The composite as a surface in fmt (optionally over an opaque background), written in bands;
 * tiles equal to the default pixel are pruned. */
pc_status pc_compose_flatten_to_surface(const pc_document *doc, pc_pixel_format fmt, const float *background,
                                        pc_surface *out, pc_error *err);
/* A single layer, isolated (thumbnails): sRGB transfer, no document patterns. */
pc_status pc_compose_render_layer(const pc_layer *layer, pc_rect rect, pc_compose_buffer *out, pc_error *err);

/* Area-averaged RGBA8 thumbnail at most max_side on its longer side (from a proxy for large
 * documents when that is faithful). */
pc_status pc_compose_thumbnail(const pc_document *doc, uint32_t max_side, pc_rgba8_image *out, pc_error *err);
pc_status pc_compose_thumbnail_buffer(const pc_document *doc, uint32_t max_side, pc_compose_buffer *out,
                                      pc_error *err);
/* The composite area-averaged (premultiplied) to w x h (clamped to the document size), in bands.
 * The output rect is in output pixels from (0, 0). */
pc_status pc_compose_render_reduced(const pc_document *doc, uint32_t w, uint32_t h, pc_compose_buffer *out,
                                    pc_error *err);
/* The same for any area of the document plane (it may lie past the canvas). */
pc_status pc_compose_render_reduced_rect(const pc_document *doc, pc_rect rect, uint32_t w, uint32_t h,
                                         pc_compose_buffer *out, pc_error *err);
/* The output pixels of pc_compose_render_reduced(doc, w, h) whose source areas meet damage, with
 * the same values (empty rect when damage misses the document). */
pc_status pc_compose_render_reduced_damage(const pc_document *doc, uint32_t w, uint32_t h, pc_rect damage,
                                           pc_compose_buffer *out, pc_error *err);
/* Explicit band height (tests). damage NULL = the whole reduction. */
pc_status pc_compose_render_reduced_in_bands(const pc_document *doc, pc_rect area, uint32_t w, uint32_t h,
                                             const pc_rect *damage, int32_t band_rows, pc_compose_buffer *out,
                                             pc_error *err);

/* The document's own CMYK space (new reference; NULL: not CMYK, untagged or the built-in). */
pc_cmyk_space *pc_compose_cmyk_space(const pc_document *doc);
/* Steps per unit adjustment results are rounded to: 255 (U8), 32768 (U16); false for F32. */
bool pc_compose_adjustment_quantum(pc_sample_type depth, float *q);

/* ---- layer queries (query.c) ---- */

/* Bounds of a layer's own pixels (union over visible group children; the canvas for fill
 * layers and shapeless layers; an artboard's board; a filled shape with effects adds its path). */
pc_rect pc_compose_layer_bounds(const pc_layer *layer, pc_rect canvas);
/* The path a shape layer's effects are shaped by (its fill path), or NULL. Borrowed. */
const pc_path *pc_compose_effect_outline(const pc_layer *layer);
/* A shape layer's path control bounds truncated to whole pixels; false otherwise. */
bool pc_compose_paint_bounds(const pc_layer *layer, pc_rect *out);
/* The frame a fill layer's gradient is laid out in (masks' painted area or vector path, else
 * the canvas). */
pc_rect pc_compose_fill_frame(const pc_layer *layer, pc_rect canvas);
/* Channel restriction weights over display RGB; false when every channel blends. */
bool pc_compose_channel_weights(const pc_layer *layer, pc_color_mode mode, float out[3]);
/* Where the layer's composite can be non-transparent; false = anywhere (*out untouched). */
bool pc_compose_composite_bounds(const pc_layer *layer, pc_rect canvas, pc_rect *out);
/* The pixels showing/hiding/moving the layer can change; false = anywhere. */
bool pc_compose_change_bounds(const pc_layer *layer, pc_rect canvas, pc_rect *out);
/* Every surface the layer draws from has a transparent default pixel. */
bool pc_compose_transparent_outside(const pc_layer *layer);
/* The layer replaces everything beneath it in its stack (opaque Normal fill, no extras). */
bool pc_compose_occludes_below(const pc_layer *layer, pc_color_mode mode);
/* Blend If changes how the layer composites in a document of mode. */
bool pc_compose_blend_if_active(const pc_layer *layer, pc_color_mode mode);
/* The text blending gamma for type layers, else 1. */
float pc_compose_text_gamma(const pc_layer *layer);
/* A surface's pixels over rect as straight RGBA. */
pc_status pc_compose_surface_to_buffer(const pc_surface *s, pc_rect rect, pc_compose_buffer *out,
                                       pc_error *err);
/* The shape the layer's effect maps are built from over rect (row-major, malloc'd w * h floats;
 * NULL when rect is empty). */
pc_status pc_compose_layer_shape(const pc_document *doc, const pc_layer *layer, pc_rect rect, float **out,
                                 size_t *len, pc_error *err);
/* A fill layer's content over canvas laid out in its frame, masks not applied (what a PSD fill
 * layer stores). */
pc_status pc_compose_render_fill_content(const pc_layer *layer, const pc_fill *f, pc_rect canvas,
                                         const pc_pattern *patterns, size_t npatterns, pc_compose_buffer *out,
                                         pc_error *err);
/* The frame a gradient stroke of layer is laid out in, from the layer's effect maps; false when
 * st isn't a gradient stroke of the layer (needs C3). */
pc_status pc_compose_stroke_frame(const pc_document *doc, const pc_layer *layer, const pc_stroke_fx *st,
                                  bool *found, pc_rect *out, pc_error *err);

/* ---- effect-map cache (fx_cache.c) ---- */

/* Budget of the process-wide effect-map cache (at least 1 MiB); oldest entries are evicted on
 * the next build. */
void pc_compose_set_effect_cache_budget(size_t bytes);
size_t pc_compose_effect_cache_budget(void);
size_t pc_compose_effect_cache_bytes(void);
/* Drops every cached map (Edit > Purge > All); returns the bytes released. */
size_t pc_compose_purge_effect_cache(void);

#endif
