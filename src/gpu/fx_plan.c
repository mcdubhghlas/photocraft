/* Layer-style stack: coverage maps are independent of paint and backdrop. The order and
 * straight-alpha transitions mirror the CPU oracle, including stacked outside-stroke shares. */
#include "gpu/plan_internal.h"
#include "gpu/fx.h"
#include "compose/effects.h"
#include "compose/shape_split.h"
#include <math.h>

enum { KNOCKOUT = 1, GATE = 2, REL = 4, VECTOR = 8, OUTLINE = 16, FIRST = 32, ATOP = 64, NO_LAYER = 128 };
static pc_gpu_paint color(const pc_color *c)
{
    pc_gpu_paint paint = {.color = {0, 0, 0, 1}};
    pc_color_to_rgb(c, paint.color);
    return paint;
}
static pc_gpu_paint effect_paint(planner *p, const pc_fx_paint *src, const double anchor[2], pc_rect frame)
{
    pc_gpu_paint paint = {.frame = frame};
    if (src->kind == PC_FX_PAINT_COLOR) {
        return color(&src->u.color);
    }
    if (src->kind == PC_FX_PAINT_GRADIENT) {
        paint.kind = PC_GPU_PAINT_GRADIENT;
        paint.gradient = &src->u.gradient;
    } else {
        paint.kind = PC_GPU_PAINT_PATTERN;
        paint.pattern =
            pc_pattern_find(p->doc->patterns, p->doc->npatterns, src->u.pattern.id.ptr, src->u.pattern.id.len,
                            src->u.pattern.name.ptr, src->u.pattern.name.len);
        const float phase[2] = {0};
        paint.placement = pc_compose_placement_anchored(anchor, true, phase, src->u.pattern.scale, 0);
    }
    return paint;
}
static pc_status init(planner *p, uint32_t a, uint32_t b, bool has_b, int kind, float opacity,
                      const pc_layer *layer, uint32_t *out, pc_error *err)
{
    pc_gpu_pass pass = {
        .kernel = PC_GPU_FX_INIT, .a = a, .b = b, .fx_kind = kind, .opacity = opacity, .fx_layer = layer};
    if (has_b) {
        p->live[b]++;
    }
    return pc_gpu_plan_emit(p, pass, out, err);
}
static pc_status paint(planner *p, uint32_t *dst, uint32_t content, const pc_layer *layer,
                       const pc_effect *effect, int map, float outside, pc_gpu_paint source,
                       pc_fx_common common, uint32_t flags, pc_error *err)
{
    if (source.kind == PC_GPU_PAINT_PATTERN && !source.pattern) {
        return PC_OK;
    }
    if (!isfinite(common.opacity) || common.blend < PC_BLEND_NORMAL || common.blend >= PC_BLEND_COUNT) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU effect paint parameters");
    }
    pc_gpu_pass pass = {.kernel = PC_GPU_FX_PAINT,
                        .a = *dst,
                        .b = content,
                        .fx_layer = layer,
                        .effect = effect,
                        .map_index = map,
                        .outside = outside,
                        .paint = source,
                        .mode = common.blend,
                        .opacity = common.opacity,
                        .flags = flags,
                        .knockout = 1 - fminf(1, fmaxf(0, layer->fill_opacity))};
    p->live[content]++;
    return pc_gpu_plan_emit(p, pass, dst, err);
}
static pc_status merge(planner *p, uint32_t w, uint32_t l, uint32_t backdrop, float opacity,
                       pc_blend_mode mode, float gamma, uint32_t flags, uint32_t *out, pc_error *err)
{
    p->live[backdrop]++;
    return pc_gpu_plan_emit(p,
                            (pc_gpu_pass){.kernel = PC_GPU_FX_MERGE,
                                          .a = w,
                                          .b = l,
                                          .c = backdrop,
                                          .has_c = true,
                                          .opacity = opacity,
                                          .mode = mode,
                                          .gamma = gamma,
                                          .flags = flags},
                            out, err);
}
pc_status pc_gpu_plan_effects(planner *p, const pc_layer *layer, const pc_layer *clips, size_t count,
                              unsigned depth, uint32_t backdrop, bool atop, uint32_t *out, pc_error *err)
{
    pc_rect canvas = pc_document_bounds(p->doc), frame;
    if (!pc_compose_paint_bounds(layer, &frame)) {
        frame = pc_compose_layer_bounds(layer, canvas);
    }
    double anchor[2] = {(double)frame.x0, (double)frame.y0};
    if (layer->effects.has_reference) {
        anchor[0] = layer->effects.reference[0];
        anchor[1] = layer->effects.reference[1];
    }
    bool outline = pc_compose_effect_outline(layer) != NULL;
    bool vector = layer->content.kind == PC_LAYER_SHAPE && !outline;
    /* Preflight every map, before returning a usable plan. */
    for (size_t i = 0; i < layer->effects.nitems; i++) {
        const pc_effect *effect = &layer->effects.items[i];
        if (!pc_effect_enabled(effect)) {
            continue;
        }
        pc_gpu_fx_program program = {0};
        pc_status rc = pc_gpu_fx_program_build(effect, p->doc->global_light, vector, p->doc->patterns,
                                               p->doc->npatterns, anchor, &program, err);
        pc_gpu_fx_program_fini(&program);
        if (rc != PC_OK) {
            return rc;
        }
    }
    uint32_t content = 0, stroke_slot = 0;
    bool split = false;
    pc_surface fill = {0}, stroke = {0};
    pc_status rc = PC_OK;
    if (layer->content.kind == PC_LAYER_SHAPE && layer->content.u.shape.has_stroke) {
        rc = pc_compose_shape_split(&layer->content.u.shape, canvas, p->doc->depth, &split, &fill, &stroke,
                                    err);
    }
    if (rc == PC_OK && split) {
        rc = pc_gpu_plan_surface(p, &fill, &content, err);
        if (rc == PC_OK) {
            rc = pc_gpu_plan_surface(p, &stroke, &stroke_slot, err);
        }
    } else if (rc == PC_OK) {
        rc = pc_gpu_plan_source(p, layer, depth, &content, err);
    }
    pc_surface_fini(&fill);
    pc_surface_fini(&stroke);
    for (size_t i = 0; i < count && rc == PC_OK; i++) {
        if (clips[i].visible && clips[i].opacity > 0) {
            rc = pc_gpu_plan_clip(p, &clips[i], depth, content, &content, err);
        }
    }
    if (rc != PC_OK) {
        return rc;
    }
    uint32_t lay = content;
    p->live[lay]++;
    if (split) {
        p->live[stroke_slot]++;
        rc = pc_gpu_plan_emit(p,
                              (pc_gpu_pass){.kernel = PC_GPU_BLEND,
                                            .a = content,
                                            .b = stroke_slot,
                                            .mode = PC_BLEND_NORMAL,
                                            .opacity = 1,
                                            .fill = 1},
                              &content, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (layer->has_mask || layer->has_vector_mask) {
            uint32_t *slots[] = {&lay, &content, &stroke_slot};
            for (size_t i = 0; i < 3 && rc == PC_OK; i++) {
                rc = pc_gpu_plan_emit(p, (pc_gpu_pass){.kernel = PC_GPU_MASK, .a = *slots[i], .layer = layer},
                                      slots[i], err);
            }
        }
    }
    if (rc == PC_OK && outline) {
        rc = init(p, content, 0, false, 7, 1, layer, &content, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    uint32_t w = backdrop;
    p->live[w]++;
    if (atop) {
        rc = init(p, w, 0, false, 1, 1, NULL, &w, err);
    }
    for (unsigned kind = 0; kind < 2 && rc == PC_OK; kind++) {
        for (size_t i = layer->effects.nitems; i-- > 0 && rc == PC_OK;) {
            const pc_effect *e = &layer->effects.items[i];
            if (!pc_effect_enabled(e)) {
                continue;
            }
            if (kind == 0 && e->kind == PC_FX_DROP_SHADOW) {
                rc = paint(p, &w, content, layer, e, 0, 0, color(&e->u.shadow.color), e->u.shadow.common,
                           e->u.shadow.knocks_out ? KNOCKOUT : 0, err);
            } else if (kind == 1 && e->kind == PC_FX_OUTER_GLOW) {
                pc_gpu_paint source = effect_paint(p, &e->u.glow.paint, anchor, frame);
                if (source.kind == PC_GPU_PAINT_GRADIENT) {
                    source.kind = PC_GPU_PAINT_GLOW;
                    source.gain = pc_compose_fx_glow_gradient_gain(e->u.glow.range);
                }
                rc = paint(p, &w, content, layer, e, 0, 0, source, e->u.glow.common, KNOCKOUT, err);
            }
        }
    }
    if (rc == PC_OK) {
        rc = init(p, lay, content, split || outline, split || outline ? 5 : 2, layer->fill_opacity, NULL,
                  &lay, err);
    }
    /* Blend Interior Effects as Group off premerges the layer in its own mode. */
    bool premerge = !layer->advanced.blend_interior && layer->blend != PC_BLEND_NORMAL &&
                    layer->blend != PC_BLEND_PASS_THROUGH && layer->blend != PC_BLEND_DISSOLVE;
    if (rc == PC_OK && premerge) {
        p->live[w]++;
        rc = pc_gpu_plan_emit(p,
                              (pc_gpu_pass){.kernel = PC_GPU_BLEND,
                                            .a = w,
                                            .b = lay,
                                            .mode = layer->blend,
                                            .opacity = 1,
                                            .fill = 1,
                                            .gamma = pc_compose_text_gamma(layer)},
                              &lay, err);
    }
    const pc_effect_kind order[] = {PC_FX_PATTERN_OVERLAY, PC_FX_GRADIENT_OVERLAY, PC_FX_COLOR_OVERLAY,
                                    PC_FX_SATIN,           PC_FX_INNER_GLOW,       PC_FX_INNER_SHADOW};
    for (size_t k = 0; k < sizeof order / sizeof *order && rc == PC_OK; k++) {
        for (size_t i = layer->effects.nitems; i-- > 0 && rc == PC_OK;) {
            const pc_effect *e = &layer->effects.items[i];
            if (!pc_effect_enabled(e) || e->kind != order[k]) {
                continue;
            }
            pc_gpu_paint source = {0};
            pc_fx_common common = {0};
            int map = -1;
            uint32_t flags = GATE;
            switch (e->kind) {
            case PC_FX_PATTERN_OVERLAY:
                source.kind = PC_GPU_PAINT_PATTERN;
                source.pattern =
                    pc_pattern_find(p->doc->patterns, p->doc->npatterns, e->u.pattern_overlay.id.ptr,
                                    e->u.pattern_overlay.id.len, e->u.pattern_overlay.name.ptr,
                                    e->u.pattern_overlay.name.len);
                source.placement = pc_compose_placement_anchored(
                    anchor, e->u.pattern_overlay.link, e->u.pattern_overlay.phase, e->u.pattern_overlay.scale,
                    e->u.pattern_overlay.angle);
                common = e->u.pattern_overlay.common;
                break;
            case PC_FX_GRADIENT_OVERLAY:
                source = (pc_gpu_paint){.kind = PC_GPU_PAINT_GRADIENT,
                                        .gradient = &e->u.gradient_overlay.gradient,
                                        .frame = frame};
                common = e->u.gradient_overlay.common;
                break;
            case PC_FX_COLOR_OVERLAY:
                source = color(&e->u.color_overlay.color);
                common = e->u.color_overlay.common;
                break;
            case PC_FX_SATIN:
                source = color(&e->u.satin.color);
                common = e->u.satin.common;
                map = 0;
                flags = REL;
                break;
            case PC_FX_INNER_SHADOW:
                source = color(&e->u.shadow.color);
                common = e->u.shadow.common;
                map = 0;
                flags = REL;
                break;
            case PC_FX_INNER_GLOW:
                source = effect_paint(p, &e->u.glow.paint, anchor, frame);
                if (source.kind == PC_GPU_PAINT_GRADIENT) {
                    source.kind = PC_GPU_PAINT_GLOW;
                    source.gain = pc_compose_fx_glow_gradient_gain(e->u.glow.range);
                }
                common = e->u.glow.common;
                map = 0;
                flags = REL;
                break;
            default:
                break;
            }
            rc = paint(p, &lay, content, layer, e, map, 0, source, common, flags, err);
        }
    }
    if (rc == PC_OK && split) {
        p->live[content]++;
        rc = pc_gpu_plan_emit(p,
                              (pc_gpu_pass){.kernel = PC_GPU_FX_INIT,
                                            .a = lay,
                                            .b = content,
                                            .c = stroke_slot,
                                            .has_c = true,
                                            .fx_kind = 4,
                                            .opacity = layer->fill_opacity},
                              &lay, err);
    }
    for (size_t i = layer->effects.nitems; i-- > 0 && rc == PC_OK;) {
        const pc_effect *e = &layer->effects.items[i];
        if (!pc_effect_enabled(e) || e->kind != PC_FX_STROKE) {
            continue;
        }
        float in_w, out_w;
        pc_compose_fx_stroke_widths(&e->u.stroke, &in_w, &out_w);
        if (in_w <= 0) {
            continue;
        }
        pc_rect stroke_frame = frame;
        bool found;
        rc = pc_compose_stroke_frame(p->doc, layer, &e->u.stroke, &found, &stroke_frame, err);
        if (rc == PC_OK) {
            rc = paint(p, &lay, content, layer, e, 1, fminf(1, fmaxf(0, in_w + .5f)),
                       effect_paint(p, &e->u.stroke.paint, anchor, stroke_frame), e->u.stroke.common, GATE,
                       err);
        }
    }
    for (size_t i = layer->effects.nitems; i-- > 0 && rc == PC_OK;) {
        const pc_effect *e = &layer->effects.items[i];
        if (!pc_effect_enabled(e) || e->kind != PC_FX_BEVEL_EMBOSS ||
            pc_compose_fx_bevel_geom(&e->u.bevel).paint != PC_COMPOSE_BEVEL_PAINT_INNER) {
            continue;
        }
        rc = paint(p, &lay, content, layer, e, 0, 0, color(&e->u.bevel.highlight_color), e->u.bevel.highlight,
                   REL, err);
        if (rc == PC_OK) {
            rc = paint(p, &lay, content, layer, e, 1, 0, color(&e->u.bevel.shadow_color), e->u.bevel.shadow,
                       REL, err);
        }
    }
    if (rc == PC_OK) {
        rc = init(p, lay, content, true, 3, 1, NULL, &lay, err);
    }
    uint32_t cover = 0, acc = 0;
    bool strokes = false;
    for (size_t i = 0; i < layer->effects.nitems && rc == PC_OK; i++) {
        const pc_effect *e = &layer->effects.items[i];
        if (!pc_effect_enabled(e) || e->kind != PC_FX_STROKE) {
            continue;
        }
        float in_w, out_w;
        pc_compose_fx_stroke_widths(&e->u.stroke, &in_w, &out_w);
        if (out_w <= 0) {
            continue;
        }
        if (!strokes) {
            p->live[lay]++;
            rc = init(p, lay, 0, false, 6, 1, NULL, &cover, err);
            if (rc != PC_OK) {
                break;
            }
        }
        pc_rect stroke_frame = frame;
        bool found;
        rc = pc_compose_stroke_frame(p->doc, layer, &e->u.stroke, &found, &stroke_frame, err);
        if (rc != PC_OK) {
            break;
        }
        pc_gpu_pass pass = {.kernel = PC_GPU_FX_STROKE_PAINT,
                            .a = w,
                            .b = content,
                            .c = cover,
                            .has_c = true,
                            .d = acc,
                            .has_d = strokes,
                            .fx_layer = layer,
                            .effect = e,
                            .map_index = 0,
                            .paint = effect_paint(p, &e->u.stroke.paint, anchor, stroke_frame),
                            .mode = e->u.stroke.common.blend,
                            .opacity = e->u.stroke.common.opacity,
                            .flags = (vector ? VECTOR : 0) | (outline ? OUTLINE : 0) | (strokes ? 0 : FIRST)};
        p->live[w]++;
        p->live[content]++;
        p->live[cover]++;
        rc = pc_gpu_plan_emit(p, pass, &acc, err);
        if (rc != PC_OK) {
            break;
        }
        pass.fx_kind = 1;
        pass.has_d = false;
        p->live[w]++;
        p->live[content]++;
        rc = pc_gpu_plan_emit(p, pass, &cover, err);
        strokes = true;
    }
    if (rc == PC_OK && strokes) {
        rc = pc_gpu_plan_emit(
            p, (pc_gpu_pass){.kernel = PC_GPU_FX_STROKE_END, .a = w, .b = acc, .c = cover, .has_c = true}, &w,
            err);
    }
    bool late = false;
    for (size_t i = layer->effects.nitems; i-- > 0 && rc == PC_OK;) {
        const pc_effect *e = &layer->effects.items[i];
        if (!pc_effect_enabled(e) || e->kind != PC_FX_BEVEL_EMBOSS) {
            continue;
        }
        pc_compose_bevel_paint where = pc_compose_fx_bevel_geom(&e->u.bevel).paint;
        if (where == PC_COMPOSE_BEVEL_PAINT_BOTH) {
            late = true;
        }
        if (where != PC_COMPOSE_BEVEL_PAINT_OUTER) {
            continue;
        }
        rc = paint(p, &w, content, layer, e, 0, 0, color(&e->u.bevel.highlight_color), e->u.bevel.highlight,
                   0, err);
        if (rc == PC_OK) {
            rc = paint(p, &w, content, layer, e, 1, 0, color(&e->u.bevel.shadow_color), e->u.bevel.shadow, 0,
                       err);
        }
    }
    pc_blend_mode mode = premerge || layer->blend == PC_BLEND_PASS_THROUGH ? PC_BLEND_NORMAL : layer->blend;
    uint32_t merged = 0;
    if (rc == PC_OK && !late) {
        rc = merge(p, w, lay, backdrop, layer->opacity, mode, pc_compose_text_gamma(layer), atop ? ATOP : 0,
                   &merged, err);
    }
    if (rc == PC_OK && late) {
        rc = merge(p, w, lay, w, 1, mode, pc_compose_text_gamma(layer), 0, &merged, err);
        for (size_t i = layer->effects.nitems; i-- > 0 && rc == PC_OK;) {
            const pc_effect *e = &layer->effects.items[i];
            if (!pc_effect_enabled(e) || e->kind != PC_FX_BEVEL_EMBOSS ||
                pc_compose_fx_bevel_geom(&e->u.bevel).paint != PC_COMPOSE_BEVEL_PAINT_BOTH) {
                continue;
            }
            rc = paint(p, &merged, content, layer, e, 0, 0, color(&e->u.bevel.highlight_color),
                       e->u.bevel.highlight, 0, err);
            if (rc == PC_OK) {
                rc = paint(p, &merged, content, layer, e, 1, 0, color(&e->u.bevel.shadow_color),
                           e->u.bevel.shadow, 0, err);
            }
        }
        if (rc == PC_OK) {
            p->live[merged]++;
            rc = merge(p, merged, merged, backdrop, layer->opacity, mode, 1, (atop ? ATOP : 0) | NO_LAYER,
                       &merged, err);
        }
    }
    p->live[content]--;
    if (rc == PC_OK) {
        pc_rect clip = pc_compose_transparent_outside(layer) ? pc_gpu_effect_region(layer, canvas) : canvas;
        rc = pc_gpu_plan_emit(
            p, (pc_gpu_pass){.kernel = PC_GPU_COPY_RECT, .a = backdrop, .b = merged, .clip = clip}, &merged,
            err);
    }
    if (rc == PC_OK) {
        *out = merged;
    }
    return rc;
}
