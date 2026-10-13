/* Pattern tiles used by Pattern Overlay, pattern fills and strokes, Edit > Fill and brush
 * textures (port of crates/doc/src/pattern.rs). Conventions: doc/adjust.h. */
#ifndef PC_DOC_PATTERN_H
#define PC_DOC_PATTERN_H

#include "doc/adjust.h"
#include "raster/surface.h"

typedef struct pc_pattern {
    pc_str id;   /* UUID-like string */
    pc_str name; /* as stored ("$$$/Patterns/...=Display Name" for built-ins) */
    uint32_t width;
    uint32_t height;
    pc_surface surface; /* pixels cover 0..width x 0..height */
} pc_pattern;

/* A pattern whose id is derived from name, size, pixels and format (deterministic). Takes over
 * *surface (moved on success). */
pc_status pc_pattern_new(const char *name, size_t name_len, pc_surface *surface, uint32_t width, uint32_t height,
                         pc_pattern *out, pc_error *err);
void pc_pattern_fini(pc_pattern *p);
pc_status pc_pattern_clone(const pc_pattern *p, pc_pattern *out, pc_error *err);
bool pc_pattern_eq(const pc_pattern *a, const pc_pattern *b);
pc_rect pc_pattern_rect(const pc_pattern *p);
bool pc_pattern_is_empty(const pc_pattern *p);

/* Largest pattern edge a file may declare (the PSD reader's limit; Define Pattern stops at 4000). */
#define PC_PATTERN_MAX_EDGE 30000u
/* Most pixels a pattern may have to be painted: renderers expand a pattern into a 16-byte-per-pixel
 * working copy, so this keeps that copy at 1 GiB; larger patterns paint nothing. */
#define PC_PATTERN_MAX_PAINT_PIXELS ((uint64_t)1 << 26)
/* width x height, never overflowing. */
uint64_t pc_pattern_pixel_count(const pc_pattern *p);
/* Has pixels and is small enough to paint (PC_PATTERN_MAX_PAINT_PIXELS). */
bool pc_pattern_is_paintable(const pc_pattern *p);
/* The displayed name: trailing NULs dropped, "$$$/...=Shown" shows the text after '='.
 * Returns a pointer into name (borrowed) and its length. */
const char *pc_pattern_display_name(const char *name, size_t len, size_t *out_len);
/* By id, else stored name, else display name (ASCII case-insensitive). NULL when none. */
const pc_pattern *pc_pattern_find(const pc_pattern *patterns, size_t n, const char *id, size_t id_len,
                                  const char *name, size_t name_len);

#endif
