/* Layer effects (port of crates/compose/src/effects.rs).
 *
 * C1 (fx_basics.c) implements the queries the compositor core needs: pc_compose_has_effects,
 * pc_compose_effects_margin, and (in fill_layout.c) gradient_units / gradient_t. Chunk C3
 * implements the rest of effects.rs in its own files, keeping the declarations below, and
 * deletes stub_c3.c (which returns PC_ERR_UNSUPPORTED from build and composite).
 *
 * Effect maps (FxMaps) are opaque to the core: it builds them once per layer state through
 * pc_compose_fx_build_maps and shares them between tiles and renders through its own reference
 * counted cache (fx_cache.c), so C3 needs no reference counting: build allocates, free releases,
 * and a built map is only read (from several threads at once) until it is freed. */
#ifndef PC_COMPOSE_EFFECTS_H
#define PC_COMPOSE_EFFECTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doc/doc.h"

struct pc_compose_buffer;
struct pc_compose_patterns;

/* Maximum distance (px) an effect is rendered from the layer's shape. */
#define PC_COMPOSE_FX_MAX_REACH 512.0f

/* At least one enabled effect and the master switch on. */
bool pc_compose_has_effects(const pc_layer *layer);
/* Pixels the enabled effects can reach beyond the layer's shape: ceil(clamp(max reach, 0,
 * MAX_REACH)) + 2. */
int32_t pc_compose_effects_margin(const pc_layer *layer);

/* Whether every enabled effect's maps are local: each map pixel depends only on the shape within
 * the margin of it (shifts, chamfer distances, tent blurs), so maps built over part of the
 * layer's region padded by twice the margin equal the whole-region maps over that part (#1909).
 * Not local: gradient strokes, Precise glows, chiselled bevels and noise. */
bool pc_compose_fx_maps_are_local(const pc_layer *layer);
/* The distance the two satin copies are offset by: Distance 0 draws as 1 px. */
float pc_compose_fx_satin_distance(const pc_satin *s);

/* The effect maps of one layer over a region (C3 defines the struct). */
typedef struct pc_compose_fx_maps pc_compose_fx_maps;

/* effects::TextureCtx: what Bevel textures and linked patterns are laid out from. */
typedef struct pc_compose_texture_ctx {
    pc_rect rect;
    const pc_pattern *patterns; /* the document's patterns (borrowed) */
    size_t npatterns;
    double anchor[2];
} pc_compose_texture_ctx;

/* effects::VectorStroke: a stroked shape's vector stroke (drawn above interior effects). */
typedef struct pc_compose_vector_stroke {
    const struct pc_compose_buffer *stroke;
    const float *mask; /* NULL = none; else stroke->len values */
} pc_compose_vector_stroke;

/* effects::build_maps_prepared. `shape` (region w * h values, row-major; NULL when the region is
 * empty) is taken over (freed by the callee, also on failure). */
pc_status pc_compose_fx_build_maps(const pc_layer *layer, float *shape, size_t shape_len, pc_rect region,
                                   const pc_global_light *light, const pc_compose_texture_ctx *tex,
                                   const struct pc_compose_patterns *patterns, pc_compose_fx_maps **out,
                                   pc_error *err);
/* Bytes held by the maps (cache accounting). NULL-safe. */
size_t pc_compose_fx_maps_bytes(const pc_compose_fx_maps *maps);
void pc_compose_fx_maps_free(pc_compose_fx_maps *maps); /* NULL-safe */
/* FxMaps::stroke_frame: false (out untouched) when st isn't one of the mapped gradient strokes. */
bool pc_compose_fx_maps_stroke_frame(const pc_compose_fx_maps *maps, const pc_stroke_fx *st, pc_rect *out);
/* effects::composite_with_effects_prepared: the layer's content with its effects onto
 * backdrop. vstroke may be NULL. */
pc_status pc_compose_fx_composite(const pc_layer *layer, const struct pc_compose_buffer *content,
                                  struct pc_compose_buffer *backdrop, const pc_compose_fx_maps *maps,
                                  pc_rect layer_bounds, const struct pc_compose_patterns *patterns,
                                  const pc_compose_vector_stroke *vstroke, pc_error *err);

#endif
