/* Pattern paint: Pattern Overlay, pattern strokes and pattern fill layers (port of
 * crates/compose/src/pattern.rs).
 *
 * A pattern tiles the plane from an origin (the layer's frame when linked, else the canvas
 * origin, plus the phase), scaled and rotated about that origin. Samples are bilinear with
 * wrap-around on premultiplied colour, so integer placements at 100 % reproduce the tile exactly
 * and scaled ones stay seamless.
 *
 * pc_compose_patterns is the reference's PreparedPatterns: demand-prepared pattern tiles owned by
 * one render call (at most `budget` bytes and 64 patterns are kept; beyond that every lookup
 * converts afresh), shared by every tile of that call. Lookups are thread-safe; a pattern is
 * converted once even when several threads ask for it at the same time.
 *
 * Tiles are reference counted (the reference's Arc<Tile>): every tile handed out must be
 * released with pc_compose_pattern_tile_release; a released tile stays valid for other holders,
 * and cached tiles live until both their last holder and the patterns object are gone. Sampling
 * a tile only reads it (any number of threads). */
#ifndef PC_COMPOSE_PATTERN_H
#define PC_COMPOSE_PATTERN_H

#include <stdbool.h>
#include <stddef.h>

#include "doc/doc.h"

struct pc_compose_buffer;

#define PC_COMPOSE_PREPARED_PATTERN_BYTES ((size_t)64 << 20)

typedef struct pc_compose_patterns pc_compose_patterns;

/* A pattern converted once to premultiplied RGBA for sampling (opaque). */
typedef struct pc_compose_pattern_tile pc_compose_pattern_tile;

/* Borrows patterns[0 .. n) for the object's lifetime (they must not change meanwhile). */
pc_status pc_compose_patterns_new(const pc_pattern *patterns, size_t n, size_t budget,
                                  pc_compose_patterns **out, pc_error *err);
void pc_compose_patterns_free(pc_compose_patterns *p); /* NULL-safe */
/* PreparedPatterns::in_mode: patterns painted into a document of colour mode (RGB by default; see
 * pc_compose_pattern_rgba). Set before the first lookup. NULL-safe. */
void pc_compose_patterns_set_mode(pc_compose_patterns *p, pc_color_mode mode);
/* The borrowed source slice (PreparedPatterns::source); NULL / 0 for a NULL object. */
const pc_pattern *pc_compose_patterns_source(const pc_compose_patterns *p, size_t *n);
/* PreparedPatterns::get: the tile of the pattern found by id, else stored name, else display
 * name (pc_pattern_find), as a new reference in *out; *out = NULL when there is no such pattern
 * or it isn't paintable (pc_pattern_is_paintable: empty, or too large to paint; nothing is
 * allocated for those). Fails only on PC_ERR_NOMEM. p may be NULL (nothing found). */
pc_status pc_compose_patterns_get(const pc_compose_patterns *p, const char *id, size_t id_len,
                                  const char *name, size_t name_len, pc_compose_pattern_tile **out,
                                  pc_error *err);

/* pattern::pattern_rgba: a pattern's pixels (straight RGBA, row-major, malloc'd) as they look
 * painted into a document of colour mode: in a Grayscale, Bitmap, Duotone or CMYK document a
 * pattern of another mode paints that mode's colours. *out = NULL, *len = 0 for a pattern that
 * isn't paintable. */
pc_status pc_compose_pattern_rgba(const pc_pattern *p, pc_color_mode mode, float (**out)[4], size_t *len,
                                  pc_error *err);
/* Tile::new / Tile::new_in: *out = NULL for a pattern that isn't paintable. The tile starts with
 * one reference. */
pc_status pc_compose_pattern_tile_new(const pc_pattern *p, pc_compose_pattern_tile **out, pc_error *err);
pc_status pc_compose_pattern_tile_new_in(const pc_pattern *p, pc_color_mode mode,
                                         pc_compose_pattern_tile **out, pc_error *err);
pc_compose_pattern_tile *pc_compose_pattern_tile_retain(pc_compose_pattern_tile *t); /* NULL-safe */
void pc_compose_pattern_tile_release(pc_compose_pattern_tile *t);                    /* NULL-safe */
/* Bilinear, wrapping sample at continuous tile coordinates (pixel centres at i + 0.5), as
 * straight alpha. */
void pc_compose_pattern_tile_sample(const pc_compose_pattern_tile *t, double u, double v, float out[4]);

/* Where a pattern sits: document point -> tile coordinates. */
typedef struct pc_compose_placement {
    double origin[2];
    double cs[2]; /* inverse rotation (cos, sin) */
    double inv_scale;
} pc_compose_placement;

/* frame: the layer's frame (used when link; an empty frame anchors at (0, 0)); phase in
 * pixels; scale as a fraction (<= 1e-3 or non-finite counts as 1); angle in degrees
 * counter-clockwise. */
pc_compose_placement pc_compose_placement_new(pc_rect frame, bool link, const float phase[2], float scale,
                                              float angle);
/* The same with the linked origin given as a point (layer effects anchor linked patterns at
 * the effects reference point). */
pc_compose_placement pc_compose_placement_anchored(const double anchor[2], bool link, const float phase[2],
                                                   float scale, float angle);
/* Tile coordinates of the document point (x, y). */
void pc_compose_placement_map(const pc_compose_placement *p, double x, double y, double *u, double *v);

/* pattern::render: t over rect (row-major straight RGBA) into a fresh buffer over rect. */
pc_status pc_compose_pattern_render(const pc_compose_pattern_tile *t, const pc_compose_placement *place,
                                    pc_rect rect, struct pc_compose_buffer *out, pc_error *err);

/* A pattern fill (f->kind == PC_FILL_PATTERN) over rect, laid out from frame
 * (Placement::new(frame, link, phase, scale, angle)); transparent when the pattern is missing.
 * out is a fresh buffer over rect. */
pc_status pc_compose_pattern_fill_render(const pc_compose_patterns *patterns, const pc_fill *f, pc_rect frame,
                                         pc_rect rect, struct pc_compose_buffer *out, pc_error *err);

#endif
