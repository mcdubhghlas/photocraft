#include "gpu/plan_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "base/mem.h"
#include "compose/effects.h"
#include "compose/advanced.h"
#include "compose/shape_split.h"
#include "color/convert.h"

void pc_gpu_plan_fini(pc_gpu_plan *plan)
{
    if (plan) {
        for (size_t i = 0; i < plan->count; i++) {
            pc_gpu_adjust_fini(&plan->passes[i].adjustment);
            if (plan->passes[i].has_surface) {
                pc_surface_fini(&plan->passes[i].surface);
            }
        }
        free(plan->passes);
        free(plan->checkpoints);
        *plan = (pc_gpu_plan){0};
    }
}
/* Allocate only live slots: a destination must not alias either sampled input. Depth-bounded
 * stacks need at most two accumulators per isolated group, plus the upload slot. */
static pc_status alloc_slot(planner *p, uint32_t *out, pc_error *err)
{
    for (uint32_t i = 0; i < 256; i++) {
        if (!p->live[i]) {
            p->live[i] = 1;
            if (i >= p->plan.slots) {
                p->plan.slots = i + 1;
            }
            *out = i;
            return PC_OK;
        }
    }
    return pc_fail(err, PC_ERR_LIMIT, "GPU plan slot limit");
}
pc_status pc_gpu_plan_emit(planner *p, pc_gpu_pass pass, uint32_t *out, pc_error *err)
{
    if (p->plan.count >= 100000) {
        return pc_fail(err, PC_ERR_LIMIT, "GPU plan pass limit");
    }
    pc_status rc = alloc_slot(p, &pass.dst, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_gpu_pass *grown = pc_grow_array(p->plan.passes, &p->plan.capacity, p->plan.count + 1, sizeof *grown);
    if (!grown) {
        return pc_fail(err, PC_ERR_NOMEM, "GPU plan");
    }
    p->plan.passes = grown;
    p->plan.passes[p->plan.count++] = pass;
    if (pass.kernel != PC_GPU_CLEAR && pass.kernel != PC_GPU_CONTENT) {
        p->live[pass.a]--;
        if (pass.has_c) {
            p->live[pass.c]--;
        }
        if (pass.has_d) {
            p->live[pass.d]--;
        }
        if (pass.kernel != PC_GPU_MASK && pass.kernel != PC_GPU_ADJUST &&
            !(pass.kernel == PC_GPU_FX_INIT && pass.fx_kind != 3 && pass.fx_kind != 4 && pass.fx_kind != 5)) {
            p->live[pass.b]--;
        }
    }
    *out = pass.dst;
    return PC_OK;
}
static bool blend_supported(pc_blend_mode mode)
{
    return mode >= PC_BLEND_NORMAL && mode < PC_BLEND_COUNT;
}
static pc_status stack(planner *p, const pc_layer *layers, size_t n, unsigned depth, uint32_t backdrop,
                       uint32_t *out, pc_error *err);
pc_status pc_gpu_plan_source(planner *p, const pc_layer *layer, unsigned depth, uint32_t *source,
                             pc_error *err)
{
    if (layer->has_video) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU source advanced blending or effects");
    }
    if (p->doc->mode == PC_COLOR_MODE_CMYK && layer->content.kind == PC_LAYER_FILL &&
        layer->content.u.fill.kind != PC_FILL_PATTERN &&
        !(layer->has_fill_cache && pc_fill_eq(&layer->fill_cache.fill, &layer->content.u.fill))) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "native CMYK fill (composited on the CPU)");
    }
    pc_status rc;
    if (pc_layer_is_group(layer)) {
        rc = pc_gpu_plan_emit(p, (pc_gpu_pass){.kernel = PC_GPU_CLEAR}, source, err);
        if (rc == PC_OK) {
            rc = stack(p, layer->content.u.group.children, layer->content.u.group.nchildren, depth + 1,
                       *source, source, err);
        }
    } else if (layer->content.kind == PC_LAYER_RASTER || layer->content.kind == PC_LAYER_SMART ||
               layer->content.kind == PC_LAYER_TEXT || layer->content.kind == PC_LAYER_SHAPE ||
               layer->content.kind == PC_LAYER_FILL) {
        rc = pc_gpu_plan_emit(p, (pc_gpu_pass){.kernel = PC_GPU_CONTENT, .layer = layer}, source, err);
    } else {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU layer content");
    }
    if (rc == PC_OK && (layer->has_mask || layer->has_vector_mask)) {
        rc = pc_gpu_plan_emit(p, (pc_gpu_pass){.kernel = PC_GPU_MASK, .a = *source, .layer = layer}, source,
                              err);
    }
    return rc;
}
static bool restrictions(const planner *p, const pc_layer *layer)
{
    float weights[3];
    return pc_compose_channel_weights(layer, p->doc->mode, weights) ||
           pc_compose_blend_if_active(layer, p->doc->mode);
}
/* before has one retained reference; after is consumed. This Layer uses freshly rendered own
 * content, without the layers clipped to it, matching the CPU's Blend If test. */
static pc_status restrict_result(planner *p, const pc_layer *layer, unsigned depth, uint32_t before,
                                 uint32_t after, uint32_t *out, pc_error *err)
{
    float weights[3];
    bool bi = pc_compose_blend_if_active(layer, p->doc->mode);
    if (pc_compose_channel_weights(layer, p->doc->mode, weights)) {
        if (bi) {
            p->live[before]++;
        }
        pc_gpu_pass pass = {.kernel = PC_GPU_CHANNELS, .a = before, .b = after};
        memcpy(pass.adjustment.params[0], weights, sizeof weights);
        pass.adjustment.params[0][3] = 1;
        pc_status rc = pc_gpu_plan_emit(p, pass, &after, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    if (bi) {
        pc_gpu_pass pass = {.kernel = PC_GPU_BLEND_IF, .a = before, .b = after};
        if (layer->content.kind != PC_LAYER_ADJUSTMENT) {
            pc_status rc = pc_gpu_plan_source(p, layer, depth, &pass.c, err);
            if (rc != PC_OK) {
                return rc;
            }
            pass.has_c = true;
        }
        for (size_t entry = 0; entry < 4; entry++) {
            pc_blend_range pair[2];
            pc_blend_if_get(&layer->blend_if, entry, pair);
            for (size_t side = 0; side < 2; side++) {
                pass.adjustment.params[entry][side * 2] =
                    (float)pair[side].black[0] * 256 + pair[side].black[1];
                pass.adjustment.params[entry][side * 2 + 1] =
                    (float)pair[side].white[0] * 256 + pair[side].white[1];
            }
        }
        pc_status rc = pc_gpu_plan_emit(p, pass, &after, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    *out = after;
    return PC_OK;
}
pc_status pc_gpu_plan_surface(planner *p, pc_surface *surface, uint32_t *out, pc_error *err)
{
    pc_gpu_pass pass = {.kernel = PC_GPU_CONTENT, .has_surface = true, .surface = *surface};
    pc_status rc = pc_gpu_plan_emit(p, pass, out, err);
    if (rc == PC_OK) {
        *surface = (pc_surface){0};
    }
    return rc;
}
pc_status pc_gpu_plan_clip(planner *p, const pc_layer *layer, unsigned depth, uint32_t base, uint32_t *out,
                           pc_error *err)
{
    if (!isfinite(layer->opacity) || !isfinite(layer->fill_opacity) || !blend_supported(layer->blend) ||
        pc_compose_advanced_active(layer, NULL, 0)) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU clipped layer parameters");
    }
    bool restricted = restrictions(p, layer);
    uint32_t before = base;
    if (restricted) {
        p->live[before]++;
    }
    if (pc_compose_has_effects(layer)) {
        pc_status result = pc_gpu_plan_effects(p, layer, NULL, 0, depth, base, true, &base, err);
        if (result == PC_OK && restricted) {
            result = restrict_result(p, layer, depth, before, base, &base, err);
        }
        if (result == PC_OK) {
            *out = base;
        }
        return result;
    }
    pc_status rc;
    if (layer->content.kind == PC_LAYER_ADJUSTMENT) {
        uint32_t previous = base;
        p->live[previous]++;
        pc_gpu_pass adjust = {.kernel = PC_GPU_ADJUST, .a = base};
        rc = pc_gpu_adjust_build(&layer->content.u.adjustment, p->doc, &adjust.adjustment, err);
        if (rc == PC_OK) {
            rc = pc_gpu_plan_emit(p, adjust, &base, err);
        }
        if (rc != PC_OK) {
            pc_gpu_adjust_fini(&adjust.adjustment);
            return rc;
        }
        rc = pc_gpu_plan_emit(p,
                              (pc_gpu_pass){.kernel = PC_GPU_ADJMIX,
                                            .a = previous,
                                            .b = base,
                                            .layer = layer,
                                            .mode = layer->blend,
                                            .opacity = layer->opacity * layer->fill_opacity},
                              &base, err);
    } else {
        uint32_t pixels;
        rc = pc_gpu_plan_source(p, layer, depth, &pixels, err);
        if (rc == PC_OK) {
            rc = pc_gpu_plan_emit(p,
                                  (pc_gpu_pass){.kernel = PC_GPU_ATOP,
                                                .a = base,
                                                .b = pixels,
                                                .mode = layer->blend,
                                                .opacity = layer->opacity * layer->fill_opacity,
                                                .fill = 1,
                                                .gamma = pc_compose_text_gamma(layer)},
                                  &base, err);
        }
    }
    if (rc == PC_OK && restricted) {
        rc = restrict_result(p, layer, depth, before, base, &base, err);
    }
    if (rc == PC_OK) {
        *out = base;
    }
    return rc;
}
static pc_status stack(planner *p, const pc_layer *layers, size_t n, unsigned depth, uint32_t backdrop,
                       uint32_t *out, pc_error *err)
{
    if (depth > PC_MAX_GROUP_DEPTH) {
        return pc_fail(err, PC_ERR_LIMIT, "GPU group depth");
    }
    for (size_t i = 0; i < n;) {
        const pc_layer *layer = &layers[i];
        size_t end = i + 1;
        while (end < n && layers[end].clipped && !layer->clipped) {
            end++;
        }
        const pc_layer *clipped = &layers[i + 1];
        size_t nclipped = end - i - 1;
        i = end;
        if (!layer->visible) {
            continue;
        }
        if (!pc_compose_has_effects(layer) &&
            (layer->content.kind == PC_LAYER_TEXT || layer->content.kind == PC_LAYER_SHAPE ||
             layer->content.kind == PC_LAYER_SMART)) {
            const pc_surface *cached = pc_layer_surface(layer);
            if (!cached) {
                continue;
            }
            float pixel[PC_RASTER_MAX_CHANNELS];
            pc_surface_default_pixel(cached, pixel);
            size_t channels = pc_surface_channels(cached);
            if (!pc_surface_tile_count(cached) && pc_surface_format(cached).alpha && channels &&
                pixel[channels - 1] <= 0) {
                continue;
            }
        }
        if (!isfinite(layer->opacity) || !isfinite(layer->fill_opacity)) {
            return pc_fail(err, PC_ERR_INVALID, "non-finite GPU layer opacity");
        }
        if (layer->opacity <= 0 && layer->content.kind != PC_LAYER_ADJUSTMENT) {
            continue;
        }
        if (layer->has_video || pc_compose_advanced_active(layer, clipped, nclipped)) {
            return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU layer masks, effects or advanced blending");
        }
        bool restricted = restrictions(p, layer);
        uint32_t original = backdrop;
        if (restricted) {
            p->live[original]++;
        }
        bool board = pc_layer_is_group(layer) && layer->content.u.group.has_artboard;
        uint32_t board_before = backdrop;
        if (board) {
            p->live[board_before]++;
            pc_gpu_pass background = {.kernel = PC_GPU_CONTENT};
            if (pc_artboard_background_rgba(&layer->content.u.group.artboard.background,
                                            background.adjustment.params[0])) {
                uint32_t bg;
                pc_status result = pc_gpu_plan_emit(p, background, &bg, err);
                if (result == PC_OK) {
                    result = pc_gpu_plan_emit(p,
                                              (pc_gpu_pass){.kernel = PC_GPU_BLEND,
                                                            .a = backdrop,
                                                            .b = bg,
                                                            .mode = PC_BLEND_NORMAL,
                                                            .opacity = 1,
                                                            .fill = 1},
                                              &backdrop, err);
                }
                if (result != PC_OK) {
                    return result;
                }
            }
        }
        if (pc_compose_has_effects(layer)) {
            pc_status result =
                pc_gpu_plan_effects(p, layer, clipped, nclipped, depth, backdrop, false, &backdrop, err);
            if (result != PC_OK) {
                return result;
            }
            goto finish_layer;
        }
        if (layer->content.kind == PC_LAYER_ADJUSTMENT) {
            uint32_t before = backdrop;
            p->live[before]++;
            pc_gpu_pass adjust = {.kernel = PC_GPU_ADJUST, .a = backdrop};
            pc_status rc = pc_gpu_adjust_build(&layer->content.u.adjustment, p->doc, &adjust.adjustment, err);
            if (rc == PC_OK) {
                rc = pc_gpu_plan_emit(p, adjust, &backdrop, err);
            }
            if (rc != PC_OK) {
                pc_gpu_adjust_fini(&adjust.adjustment);
                return rc;
            }
            for (size_t c = 0; c < nclipped; c++) {
                if (!clipped[c].visible || clipped[c].opacity <= 0) {
                    continue;
                }
                rc = pc_gpu_plan_clip(p, &clipped[c], depth, backdrop, &backdrop, err);
                if (rc != PC_OK) {
                    return rc;
                }
            }
            rc = pc_gpu_plan_emit(p,
                                  (pc_gpu_pass){.kernel = PC_GPU_ADJMIX,
                                                .a = before,
                                                .b = backdrop,
                                                .layer = layer,
                                                .mode = layer->blend,
                                                .opacity = layer->opacity * layer->fill_opacity,
                                                .fill = 1},
                                  &backdrop, err);
            if (rc != PC_OK) {
                return rc;
            }
            goto finish_layer;
        }
        size_t children = 0;
        const pc_layer *items = pc_layer_children(layer, &children);
        bool group = pc_layer_is_group(layer);
        if (group && layer->blend == PC_BLEND_PASS_THROUGH && layer->fill_opacity >= 1) {
            bool clips = false;
            for (size_t c = 0; c < nclipped; c++) {
                clips = clips || clipped[c].visible;
            }
            uint32_t before = backdrop;
            p->live[before]++;
            if (clips) {
                p->live[before]++;
            }
            pc_status rc = stack(p, items, children, depth + 1, backdrop, &backdrop, err);
            if (rc != PC_OK) {
                return rc;
            }
            rc = pc_gpu_plan_emit(p,
                                  (pc_gpu_pass){.kernel = PC_GPU_LERP,
                                                .a = before,
                                                .b = backdrop,
                                                .layer = layer,
                                                .opacity = layer->opacity * layer->fill_opacity},
                                  &backdrop, err);
            if (rc != PC_OK) {
                return rc;
            }
            if (clips) {
                uint32_t isolated;
                rc = pc_gpu_plan_source(p, layer, depth, &isolated, err);
                if (rc != PC_OK) {
                    return rc;
                }
                uint32_t keep = isolated;
                p->live[keep]++;
                for (size_t c = 0; c < nclipped; c++) {
                    if (!clipped[c].visible || clipped[c].opacity <= 0) {
                        continue;
                    }
                    rc = pc_gpu_plan_clip(p, &clipped[c], depth, isolated, &isolated, err);
                    if (rc != PC_OK) {
                        return rc;
                    }
                }
                uint32_t without, with;
                p->live[before]++;
                rc = pc_gpu_plan_emit(p,
                                      (pc_gpu_pass){.kernel = PC_GPU_BLEND,
                                                    .a = before,
                                                    .b = keep,
                                                    .mode = PC_BLEND_NORMAL,
                                                    .opacity = layer->opacity * layer->fill_opacity,
                                                    .fill = 1},
                                      &without, err);
                if (rc == PC_OK) {
                    rc = pc_gpu_plan_emit(p,
                                          (pc_gpu_pass){.kernel = PC_GPU_BLEND,
                                                        .a = before,
                                                        .b = isolated,
                                                        .mode = PC_BLEND_NORMAL,
                                                        .opacity = layer->opacity * layer->fill_opacity,
                                                        .fill = 1},
                                          &with, err);
                }
                if (rc == PC_OK) {
                    rc = pc_gpu_plan_emit(
                        p,
                        (pc_gpu_pass){
                            .kernel = PC_GPU_ADD_DIFF, .a = backdrop, .b = with, .c = without, .has_c = true},
                        &backdrop, err);
                }
                if (rc != PC_OK) {
                    return rc;
                }
            }
            goto finish_layer;
        }
        if ((!blend_supported(layer->blend) && !(group && layer->blend == PC_BLEND_PASS_THROUGH))) {
            return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU blend or artboard");
        }
        uint32_t source;
        pc_status rc = PC_OK;
        pc_surface fill = {0}, stroke = {0};
        bool split = false, clips = false;
        for (size_t c = 0; c < nclipped; c++) {
            clips = clips || clipped[c].visible;
        }
        if (clips && layer->content.kind == PC_LAYER_SHAPE && layer->content.u.shape.has_stroke) {
            rc = pc_compose_shape_split(&layer->content.u.shape, pc_document_bounds(p->doc), p->doc->depth,
                                        &split, &fill, &stroke, err);
        }
        uint32_t stroke_slot = 0;
        if (rc == PC_OK && split) {
            rc = pc_gpu_plan_surface(p, &fill, &source, err);
            if (rc == PC_OK) {
                rc = pc_gpu_plan_surface(p, &stroke, &stroke_slot, err);
            }
        } else if (rc == PC_OK) {
            rc = pc_gpu_plan_source(p, layer, depth, &source, err);
        }
        pc_surface_fini(&fill);
        pc_surface_fini(&stroke);
        if (rc != PC_OK) {
            return rc;
        }
        for (size_t c = 0; c < nclipped; c++) {
            const pc_layer *clip = &clipped[c];
            if (!clip->visible || clip->opacity <= 0) {
                continue;
            }
            rc = pc_gpu_plan_clip(p, clip, depth, source, &source, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        if (split) {
            rc = pc_gpu_plan_emit(p,
                                  (pc_gpu_pass){.kernel = PC_GPU_BLEND,
                                                .a = source,
                                                .b = stroke_slot,
                                                .mode = PC_BLEND_NORMAL,
                                                .opacity = 1,
                                                .fill = 1},
                                  &source, err);
            if (rc == PC_OK && (layer->has_mask || layer->has_vector_mask)) {
                rc = pc_gpu_plan_emit(p, (pc_gpu_pass){.kernel = PC_GPU_MASK, .a = source, .layer = layer},
                                      &source, err);
            }
            if (rc != PC_OK) {
                return rc;
            }
        }
        rc = pc_gpu_plan_emit(p,
                              (pc_gpu_pass){.kernel = PC_GPU_BLEND,
                                            .a = backdrop,
                                            .b = source,
                                            .mode = layer->blend,
                                            .opacity = layer->opacity,
                                            .fill = layer->fill_opacity,
                                            .gamma = pc_compose_text_gamma(layer)},
                              &backdrop, err);
        if (rc != PC_OK) {
            return rc;
        }
    finish_layer:
        if (board) {
            pc_status result = pc_gpu_plan_emit(p,
                                                (pc_gpu_pass){.kernel = PC_GPU_COPY_RECT,
                                                              .a = board_before,
                                                              .b = backdrop,
                                                              .clip = layer->content.u.group.artboard.rect},
                                                &backdrop, err);
            if (result != PC_OK) {
                return result;
            }
        }
        if (restricted) {
            pc_status result = restrict_result(p, layer, depth, original, backdrop, &backdrop, err);
            if (result != PC_OK) {
                return result;
            }
        }
        if (depth == 0) {
            bool single = true;
            for (uint32_t slot = 0; slot < p->plan.slots; slot++) {
                if (p->live[slot] != (slot == backdrop ? 1u : 0u)) {
                    single = false;
                    break;
                }
            }
            if (single) {
                pc_gpu_checkpoint *grown = pc_grow_array(p->plan.checkpoints, &p->plan.checkpoint_capacity,
                                                         p->plan.ncheckpoints + 1, sizeof *grown);
                if (!grown) {
                    return pc_fail(err, PC_ERR_NOMEM, "GPU plan checkpoints");
                }
                p->plan.checkpoints = grown;
                grown[p->plan.ncheckpoints++] =
                    (pc_gpu_checkpoint){.layer_end = end, .pass_end = p->plan.count, .slot = backdrop};
            }
        }
    }
    *out = backdrop;
    return PC_OK;
}
pc_status pc_gpu_plan_build(const pc_document *doc, pc_gpu_plan *out, pc_error *err)
{
    if (!doc || !out) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU plan input");
    }
    if ((unsigned)doc->mode >= PC_COLOR_MODE_COUNT || doc->mode == PC_COLOR_MODE_MULTICHANNEL) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU document colour mode");
    }
    pc_cmyk_space *space = pc_compose_cmyk_space(doc);
    pc_cmyk_space *old_space = pc_cmyk_space_enter(space);
    planner p = {.doc = doc};
    uint32_t root;
    pc_status rc = pc_gpu_plan_emit(&p, (pc_gpu_pass){.kernel = PC_GPU_CLEAR}, &root, err);
    if (rc == PC_OK) {
        rc = stack(&p, doc->layers, doc->nlayers, 0, root, &root, err);
    }
    pc_cmyk_space_leave(old_space);
    pc_cmyk_space_release(space);
    if (rc != PC_OK) {
        pc_gpu_plan_fini(&p.plan);
        return rc;
    }
    p.plan.root = root;
    pc_gpu_plan_fini(out);
    *out = p.plan;
    return PC_OK;
}
pc_rect pc_gpu_effect_region(const pc_layer *layer, pc_rect canvas)
{
    int32_t margin = pc_compose_effects_margin(layer);
    pc_rect whole = pc_rect_inflate(pc_compose_effect_source_bounds(layer, canvas), margin);
    pc_rect window = pc_rect_inflate(canvas, margin);
    uint64_t area = (uint64_t)pc_rect_width(window) * pc_rect_height(window);
    uint64_t total = (uint64_t)pc_rect_width(whole) * pc_rect_height(whole);
    return total <= area + area / 4 ? whole : pc_rect_intersect(whole, window);
}
