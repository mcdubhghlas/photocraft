/* Private to src/compose: the render context, the knockout scope, the compositor internals that
 * C2's advanced.c and C3's effects.c call back into (pc_cmp_*, implemented by C1 in composite.c
 * and fx_cache.c), and the Advanced Blending hooks the core calls (implemented by C2's
 * advanced.c). Not part of the public API.
 *
 * Buffers passed around are pc_compose_buffer; "mask values" are malloc'd arrays of exactly
 * rect w * h floats (row-major) or NULL for "no mask" (the reference's Option<Vec<f32>>). */
#ifndef PC_COMPOSE_INTERNAL_H
#define PC_COMPOSE_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "compose/adjust.h"
#include "compose/advanced.h"
#include "compose/compose.h"
#include "compose/effects.h"
#include "compose/gradient_fill.h"
#include "compose/pattern.h"
#include "compose/shape_split.h"
#include "vector/vector.h"

/* Vector masks compiled once per render (RenderVectorMasks), keyed by the mask's address (masks
 * are immutable for the render's lifetime). Compilation runs under the lock (it is sequential in
 * the reference too); rendering a compiled mask runs outside it. */
typedef struct pc_cmp_vmask_slot {
    const pc_vector_mask *key;
    uint64_t hash; /* content hash: copies of layers (advanced.c effects_source) can reuse an
                      address within one render, so the address alone is not identity (C2) */
    pc_vec_compiled_mask mask;
} pc_cmp_vmask_slot;

typedef struct pc_cmp_vmasks {
    pthread_mutex_t lock;
    pc_cmp_vmask_slot **slots; /* each slot heap-allocated so its address is stable */
    size_t len;
    size_t cap;
} pc_cmp_vmasks;

/* A reference-counted effect-map holder (Arc<FxMaps>). */
typedef struct pc_cmp_fx_ref pc_cmp_fx_ref;
const pc_compose_fx_maps *pc_cmp_fx_maps(const pc_cmp_fx_ref *r);
void pc_cmp_fx_ref_release(pc_cmp_fx_ref *r); /* NULL-safe */

/* The effect maps prepared for one render (Ctx::fx_maps), by cache key. */
typedef struct pc_cmp_fx_pair {
    uint64_t key;
    pc_cmp_fx_ref *ref;
} pc_cmp_fx_pair;

typedef struct pc_cmp_fx_local {
    pthread_mutex_t lock;
    pc_cmp_fx_pair *items;
    size_t len;
    size_t cap;
} pc_cmp_fx_local;

/* Rendering context shared down the tree (lib.rs Ctx). Built once per render call; the mutable
 * per-render caches sit behind pointers so tiles share them through a const context. */
typedef struct pc_cmp_ctx {
    pc_rect canvas; /* fill layers and gradients are laid out relative to it */
    pc_compose_transfer transfer;
    pc_global_light light;
    const pc_compose_patterns *patterns;
    pc_color_mode mode;
    pc_sample_type depth;
    pc_cmp_vmasks *vmasks;
    pc_cmp_fx_local *fx;
    const pc_document *doc; /* NULL for a lone layer (render_layer) */
    const pc_interrupt *intr;
} pc_cmp_ctx;

/* Owns the patterns, vector-mask and effect-map caches of a context. */
typedef struct pc_cmp_ctx_store {
    pc_cmp_ctx cx;
    pc_compose_patterns *patterns;
    pc_cmp_vmasks vmasks;
    pc_cmp_fx_local fx;
} pc_cmp_ctx_store;

/* Ctx::for_doc (init_doc), and render_layer's context for a lone layer over canvas (init_layer: sRGB
 * transfer, default light, no patterns, RGB / F32, no document). fini releases the caches. */
pc_status pc_cmp_ctx_init_doc(pc_cmp_ctx_store *s, const pc_document *doc, const pc_interrupt *intr,
                              pc_error *err);
pc_status pc_cmp_ctx_init_layer(pc_cmp_ctx_store *s, pc_rect canvas, pc_error *err);
void pc_cmp_ctx_fini(pc_cmp_ctx_store *s);

/* advanced::Scope: where a layer stack sits, for knockouts. */
typedef struct pc_cmp_scope {
    bool root;     /* the document's top-level stack */
    bool isolated; /* inside an isolated group */
} pc_cmp_scope;

#define PC_CMP_SCOPE_ROOT ((pc_cmp_scope){true, false})
#define PC_CMP_SCOPE_ISOLATED ((pc_cmp_scope){false, true})

static inline pc_cmp_scope pc_cmp_scope_pass_through(pc_cmp_scope s)
{
    return (pc_cmp_scope){false, s.isolated};
}

/* ---- C1 internals (composite.c) ---- */

/* PC_ERR_CANCELLED once the context's interrupt fires. */
pc_status pc_cmp_check(const pc_cmp_ctx *cx, pc_error *err);
/* composite_stack_at: a sibling list (bottom to top) onto backdrop. */
pc_status pc_cmp_composite_stack_at(const pc_layer *layers, size_t n, struct pc_compose_buffer *backdrop,
                                    const pc_cmp_ctx *cx, pc_cmp_scope scope, pc_error *err);
/* composite_layer: layer plus its clipping group onto backdrop; ko = knockout target or NULL. */
pc_status pc_cmp_composite_layer(const pc_layer *layer, const pc_layer *clipped, size_t nclipped,
                                 struct pc_compose_buffer *backdrop, const pc_cmp_ctx *cx,
                                 const struct pc_compose_buffer *ko, pc_cmp_scope scope, pc_error *err);
/* composite_layer_any: the same without channel restrictions / Blend If / knockout. */
pc_status pc_cmp_composite_layer_any(const pc_layer *layer, const pc_layer *clipped, size_t nclipped,
                                     struct pc_compose_buffer *backdrop, const pc_cmp_ctx *cx,
                                     pc_cmp_scope scope, pc_error *err);
/* composite_atop: layer onto base restricted to the base's alpha (clipping semantics). */
pc_status pc_cmp_composite_atop(const pc_layer *layer, struct pc_compose_buffer *base, const pc_cmp_ctx *cx,
                                pc_error *err);
/* render_content: the layer's own content, masks applied; *some = false (out untouched) for
 * adjustment layers. */
pc_status pc_cmp_render_content(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, bool *some,
                                struct pc_compose_buffer *out, pc_error *err);
/* mask_vals: the layer's effective mask over rect, or *out = NULL for none. */
pc_status pc_cmp_mask_vals(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, float **out,
                           pc_error *err);
/* multichannel::backdrop: the inks for Multichannel documents, else transparent. */
pc_status pc_cmp_backdrop(const pc_document *doc, pc_rect rect, struct pc_compose_buffer *out, pc_error *err);
/* render_fill: a fill's content over rect laid out in frame (solid here; gradient and pattern
 * fills through gradient_fill.c / pattern.c). */
pc_status pc_cmp_fill_content(const pc_fill *f, pc_rect rect, pc_rect frame,
                              const pc_compose_patterns *patterns, struct pc_compose_buffer *out,
                              pc_error *err);
/* blend_into_g: an isolated layer buffer into the backdrop (Dissolve resolved per pixel). */
void pc_cmp_blend_into(struct pc_compose_buffer *backdrop, const struct pc_compose_buffer *src,
                       pc_blend_mode mode, float opacity, float gamma);
/* effect_shape (lib.rs): the shape the layer's effect maps are built from over rect. */
pc_status pc_cmp_effect_shape(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, float **out,
                              size_t *len, pc_error *err);

/* ---- C1 effect-map cache (fx_cache.c) ---- */

/* effect_maps: the layer's maps over its whole region, built once per layer state. */
pc_status pc_cmp_effect_maps(const pc_layer *layer, const pc_cmp_ctx *cx, pc_cmp_fx_ref **out, pc_error *err);
/* prepare_effects: builds the maps of every visible layer with effects that can reach rect
 * before tiles render in parallel. */
pc_status pc_cmp_prepare_effects(const pc_layer *layers, size_t n, pc_rect rect, const pc_cmp_ctx *cx,
                                 pc_cmyk_space *cmyk, bool lab, bool hdr, pc_error *err);
void pc_cmp_fx_local_init(pc_cmp_fx_local *l);
void pc_cmp_fx_local_fini(pc_cmp_fx_local *l);
/* empty_in (composite.c): the layer contributes nothing in rect. */
bool pc_cmp_empty_in(const pc_layer *layer, pc_rect rect);

/* ---- Advanced Blending hooks (C2: advanced.c) ---- */

typedef pc_status (*pc_cmp_draw_fn)(void *ctx, struct pc_compose_buffer *b, pc_error *err);

/* A visible unclipped shallow knockout in layers (the stack keeps its starting backdrop). */
bool pc_cmp_needs_floor(const pc_layer *layers, size_t n);
/* knockout_target: *target = NULL (no knockout), floor (borrowed), or owned (then filled in and
 * owned by the caller). */
pc_status pc_cmp_knockout_target(const pc_layer *layer, const struct pc_compose_buffer *floor, pc_rect rect,
                                 const pc_cmp_ctx *cx, pc_cmp_scope scope, struct pc_compose_buffer *owned,
                                 const struct pc_compose_buffer **target, pc_error *err);
/* knockout: layer drawn by draw(dctx, backdrop) as a knockout onto target. */
pc_status pc_cmp_knockout(const pc_layer *layer, struct pc_compose_buffer *backdrop,
                          const struct pc_compose_buffer *target, bool clip_alpha, const pc_cmp_ctx *cx,
                          pc_cmp_draw_fn draw, void *dctx, pc_error *err);
/* composite_clipped: the clipped layers atop base (Blend Clipped Layers as Group on). */
pc_status pc_cmp_composite_clipped(const pc_layer *clipped, size_t n, struct pc_compose_buffer *base,
                                   const pc_cmp_ctx *cx, pc_error *err);
bool pc_cmp_clips_individually(const pc_layer *layer, const pc_layer *clipped, size_t n);
pc_status pc_cmp_composite_clipped_individually(const pc_layer *layer, const pc_layer *clipped, size_t n,
                                                struct pc_compose_buffer *backdrop, const pc_cmp_ctx *cx,
                                                const struct pc_compose_buffer *ko, pc_cmp_scope scope,
                                                pc_error *err);
bool pc_cmp_hides_effects(const pc_layer *layer);
/* effects_source: *has_owned = false: use layer itself; true: *owned is a copy without the
 * hiding masks (caller finalises). */
pc_status pc_cmp_effects_source(const pc_layer *layer, pc_layer *owned, bool *has_owned, pc_error *err);
/* hiding_mask: *out = NULL when no mask hides the effects. */
pc_status pc_cmp_hiding_mask(const pc_layer *layer, pc_rect rect, const pc_cmp_ctx *cx, float **out,
                             pc_error *err);
void pc_cmp_apply_hiding_mask(struct pc_compose_buffer *out, const struct pc_compose_buffer *before,
                              const float *mask);

/* ---- small helpers ---- */

/* w * h of rect, PC_ERR_LIMIT past PC_COMPOSE_MAX_PIXELS. */
pc_status pc_cmp_rect_pixels(pc_rect rect, size_t *out, pc_error *err);
/* Rust's `x as u32` for the non-negative widths used here. */
static inline size_t pc_cmp_w(pc_rect r)
{
    return (size_t)pc_rect_width(r);
}

#endif
