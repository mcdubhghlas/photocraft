#include "move_mods.h"
#include "engine/preview.h"
#include "base/mem.h"
#include "engine/floating.h"
#include "tool_gesture.h"
#include "ui/shortcuts.h"
#include "engine/channel_display.h"
#include "paint/json.h"
#include "engine/move.h"
#include "ui/hold_keys.h"
#include "ui/vector_tool.h"
#include "raster/surface.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_uint_fast64_t preview_serial = 1;
static pc_status next_preview(pc_ui_tool_gesture *g, pc_error *err)
{
    uint_fast64_t value = atomic_load_explicit(&preview_serial, memory_order_relaxed);
    do {
        if (value >= UINT64_MAX / 2) {
            return pc_fail(err, PC_ERR_LIMIT, "preview generation exhausted");
        }
    } while (!atomic_compare_exchange_weak_explicit(&preview_serial, &value, value + 1, memory_order_relaxed,
                                                    memory_order_relaxed));
    g->preview_revision = (uint64_t)value;
    return PC_OK;
}

static const char *stroke_command(pc_ui_tool tool)
{
    switch (tool) {
    case PC_TOOL_BRUSH:
    case PC_TOOL_ERASER:
        return "paint.stroke";
    case PC_TOOL_PENCIL:
        return "paint.pencil";
    case PC_TOOL_MIXER_BRUSH:
        return "paint.mixerBrush";
    case PC_TOOL_COLOR_REPLACEMENT:
        return "paint.colorReplacement";
    case PC_TOOL_DODGE:
        return "paint.dodge";
    case PC_TOOL_BURN:
        return "paint.burn";
    case PC_TOOL_SPONGE:
        return "paint.sponge";
    case PC_TOOL_BLUR:
        return "paint.blur";
    case PC_TOOL_SHARPEN:
        return "paint.sharpen";
    case PC_TOOL_SMUDGE:
        return "paint.smudge";
    case PC_TOOL_SPOT_HEALING:
        return "paint.spotHealing";
    case PC_TOOL_REMOVE:
        return "paint.remove";
    case PC_TOOL_HISTORY_BRUSH:
        return "paint.historyBrush";
    case PC_TOOL_BACKGROUND_ERASER:
        return "paint.backgroundEraser";
    default:
        return NULL;
    }
}
bool pc_ui_tool_gesture_supported(pc_ui_tool tool)
{
    return (pc_ui_is_shape_tool(tool) && tool != PC_TOOL_CUSTOM_SHAPE) || stroke_command(tool) ||
           tool == PC_TOOL_MOVE || tool == PC_TOOL_RECT_MARQUEE || tool == PC_TOOL_ELLIPSE_MARQUEE ||
           tool == PC_TOOL_SINGLE_ROW_MARQUEE || tool == PC_TOOL_SINGLE_COLUMN_MARQUEE ||
           tool == PC_TOOL_LASSO || tool == PC_TOOL_CROP || tool == PC_TOOL_GRADIENT ||
           tool == PC_TOOL_PAINT_BUCKET || tool == PC_TOOL_MAGIC_WAND || tool == PC_TOOL_MAGIC_ERASER ||
           tool == PC_TOOL_RED_EYE || tool == PC_TOOL_QUICK_SELECTION ||
           tool == PC_TOOL_CLONE_STAMP || tool == PC_TOOL_HEALING || tool == PC_TOOL_PATTERN_STAMP ||
           tool == PC_TOOL_PATCH || tool == PC_TOOL_CONTENT_AWARE_MOVE;
}
/* Is the document point inside the current selection? (Patch / Content-Aware Move drag test.) */
static bool point_in_selection(const pc_document *doc, double x, double y)
{
    if (doc == NULL || !doc->has_selection || !isfinite(x) || !isfinite(y) || fabs(x) > 1e8 ||
        fabs(y) > 1e8) {
        return false;
    }
    int32_t ix = (int32_t)floor(x), iy = (int32_t)floor(y);
    return pc_surface_sample_channel(&doc->selection, ix, iy, 0) > 0.0f;
}
void pc_ui_tool_gesture_fini(pc_ui_tool_gesture *g)
{
    if (!g) {
        return;
    }
    if (g->active) {
        pc_ui_tool_options_fini(&g->options);
    }
    pc_shape_layer_fini(&g->shape_preview);
    g->has_shape_preview = false;
    free(g->selected);
    g->selected = NULL;
    g->nselected = 0;
    pc_doc_arc_release(g->move_preview);
    g->move_preview = NULL;
    pc_engine_live_stroke_free(g->live);
    pc_buf_fini(&g->brush);
    free(g->points);
    *g = (pc_ui_tool_gesture){0};
}
static void finish(pc_ui_tool_gesture *g)
{
    if (g->active) {
        pc_ui_tool_options_fini(&g->options);
    }
    pc_shape_layer_fini(&g->shape_preview);
    g->has_shape_preview = false;
    free(g->selected);
    g->selected = NULL;
    g->nselected = 0;
    pc_doc_arc_release(g->move_preview);
    g->move_preview = NULL;
    pc_engine_live_stroke_free(g->live);
    g->live = NULL;
    g->has_seed = false;
    pc_buf_truncate(&g->brush, 0);
    g->active = false;
    g->move_has_last = false;
    g->patch_dragging = false;
    g->handoff = 0;
    g->count = 0;
    g->stroke_axis = (pc_ui_stroke_axis){0};
    /* last_stroke_* persist across gestures for Shift-click line connection. */
}
static unsigned current_target(const pc_engine_session *session, const pc_ui_state *ui, size_t *alpha)
{
    bool quick = false;
    if (pc_engine_targeted_channel(session, &quick, alpha)) {
        return quick ? 2u : 3u;
    }
    return ui->mask_target || pc_engine_targeted_mask(session) ||
                   pc_engine_session_tools(session)->mask_targeted
               ? 1u
               : 0u;
}
static bool valid(pc_ui_tool_point p)
{
    return isfinite(p.x) && isfinite(p.y) && fabsf(p.x) <= 1e8f && fabsf(p.y) <= 1e8f &&
           isfinite(p.pressure) && p.pressure >= 0 && p.pressure <= 1 && isfinite(p.tilt_x) &&
           fabsf(p.tilt_x) <= 90 && isfinite(p.tilt_y) && fabsf(p.tilt_y) <= 90 && isfinite(p.rotation) &&
           isfinite(p.time_ms) && isfinite(p.wheel);
}
static pc_status append(pc_ui_tool_gesture *g, pc_ui_tool_point p, pc_error *err)
{
    if (g->count && !memcmp(&g->points[g->count - 1], &p, sizeof p)) {
        return PC_OK;
    }
    if (g->count >= 100000) {
        return pc_fail(err, PC_ERR_LIMIT, "pointer gesture exceeds 100000 samples");
    }
    if (g->count == g->capacity) {
        size_t cap = g->capacity ? g->capacity * 2 : 64;
        if (cap > 100000) {
            cap = 100000;
        }
        pc_ui_tool_point *points = realloc(g->points, cap * sizeof *points);
        if (!points) {
            return pc_fail(err, PC_ERR_NOMEM, "pointer samples");
        }
        g->points = points;
        g->capacity = cap;
    }
    g->points[g->count++] = p;
    return PC_OK;
}
static yyjson_mut_val *pair(yyjson_mut_doc *d, float x, float y)
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_float(d, a, x);
    yyjson_mut_arr_add_float(d, a, y);
    return a;
}
/* Append the gesture's samples as the 8-float `points` array the paint commands expect. */
static void add_points(yyjson_mut_doc *d, yyjson_mut_val *root, const pc_ui_tool_gesture *g)
{
    yyjson_mut_val *points = yyjson_mut_arr(d);
    for (size_t i = 0; i < g->count; i++) {
        pc_ui_tool_point p = g->points[i];
        yyjson_mut_val *sample = yyjson_mut_arr(d);
        const float values[] = {p.x, p.y, p.pressure, p.tilt_x, p.tilt_y, p.rotation, p.time_ms, p.wheel};
        for (size_t j = 0; j < 8; j++) {
            yyjson_mut_arr_add_float(d, sample, values[j]);
        }
        yyjson_mut_arr_add_val(points, sample);
    }
    yyjson_mut_obj_add_val(d, root, "points", points);
}
static pc_status commit(pc_ui_tool_gesture *g, pc_engine_session *session, bool preview, pc_json_value *out,
                        pc_error *err)
{
    if (pc_ui_is_shape_tool(g->tool)) {
        return pc_ui_shape_gesture(g, session, preview, out, err);
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) {
        return pc_fail(err, PC_ERR_NOMEM, "gesture parameters");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    pc_ui_tool_point first = g->points[0], last = g->points[g->count - 1];
    pc_ui_tool_options *o = &g->options;
    if (g->target == 3) {
        yyjson_mut_val *target = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, target, "channel", g->alpha);
        yyjson_mut_obj_add_val(d, root, "target", target);
    } else {
        yyjson_mut_obj_add_str(d, root, "target",
                               g->target == 2   ? "quickMask"
                               : g->target == 1 ? "mask"
                                                : "pixels");
    }
    const char *command = stroke_command(g->tool);
    /* Alt at the press flips Dodge/Burn and Blur/Sharpen; the tool (and its params) stay the same. */
    if (g->alt_start) {
        if (g->tool == PC_TOOL_DODGE)
            command = "paint.burn";
        else if (g->tool == PC_TOOL_BURN)
            command = "paint.dodge";
        else if (g->tool == PC_TOOL_BLUR)
            command = "paint.sharpen";
        else if (g->tool == PC_TOOL_SHARPEN)
            command = "paint.blur";
    }
    const char *modes[] = {"replace", "add", "subtract", "intersect"};
    const char *selection = modes[g->selection_mode < 4 ? g->selection_mode : 0];
    if (command) {
        yyjson_mut_val *points = yyjson_mut_arr(d);
        for (size_t i = 0; i < g->count; i++) {
            pc_ui_tool_point p = g->points[i];
            yyjson_mut_val *sample = yyjson_mut_arr(d);
            const float values[] = {p.x, p.y, p.pressure, p.tilt_x, p.tilt_y, p.rotation, p.time_ms, p.wheel};
            for (size_t j = 0; j < 8; j++) {
                yyjson_mut_arr_add_float(d, sample, values[j]);
            }
            yyjson_mut_arr_add_val(points, sample);
        }
        yyjson_mut_obj_add_val(d, root, "points", points);
        yyjson_mut_obj_add_float(d, root, "zoom", g->zoom);
        bool erase = g->tool == PC_TOOL_ERASER || (g->tool == PC_TOOL_BRUSH && o->brush_clear) ||
                     (g->tool == PC_TOOL_PENCIL && o->pencil_clear);
        if (g->tool == PC_TOOL_ERASER && strcmp(pc_str_c(&o->eraser_mode), "brush")) {
            command = "paint.pencil";
            yyjson_mut_obj_add_bool(d, root, "block", !strcmp(pc_str_c(&o->eraser_mode), "block"));
        }
        if (erase) {
            yyjson_mut_obj_add_bool(d, root, "erase", true);
        }
        if (g->tool == PC_TOOL_PENCIL) {
            yyjson_mut_obj_add_bool(d, root, "autoErase", o->pencil_auto_erase);
        }
        if (g->tool == PC_TOOL_COLOR_REPLACEMENT) {
            yyjson_mut_obj_add_strcpy(d, root, "mode", pc_str_c(&o->cr_mode));
            yyjson_mut_obj_add_strcpy(d, root, "sampling", pc_str_c(&o->cr_sampling));
            yyjson_mut_obj_add_strcpy(d, root, "limits", pc_str_c(&o->cr_limits));
            yyjson_mut_obj_add_float(d, root, "tolerance", o->cr_tolerance);
            yyjson_mut_obj_add_bool(d, root, "antiAlias", o->cr_anti_alias);
            /* Pin the foreground colour so a replayed stroke paints the same colour. */
            char hex[8];
            int chan[3];
            for (size_t c = 0; c < 3; c++) {
                float v = g->colors[0][c];
                v = isfinite(v) ? (v < 0 ? 0 : (v > 1 ? 1 : v)) : 0;
                chan[c] = (int)(v * 255 + .5f);
            }
            (void)snprintf(hex, sizeof hex, "#%02x%02x%02x", chan[0], chan[1], chan[2]);
            yyjson_mut_obj_add_strcpy(d, root, "color", hex);
        }
        if (g->tool == PC_TOOL_DODGE || g->tool == PC_TOOL_BURN) {
            yyjson_mut_obj_add_strcpy(d, root, "range", pc_str_c(&o->tone_range));
            yyjson_mut_obj_add_float(d, root, "exposure", o->exposure);
            yyjson_mut_obj_add_bool(d, root, "protectTones", o->protect_tones);
        }
        if (g->tool == PC_TOOL_SPONGE) {
            yyjson_mut_obj_add_strcpy(d, root, "mode", pc_str_c(&o->sponge_mode));
            yyjson_mut_obj_add_bool(d, root, "vibrance", o->vibrance);
        }
        if (g->tool == PC_TOOL_BLUR || g->tool == PC_TOOL_SHARPEN || g->tool == PC_TOOL_SMUDGE) {
            yyjson_mut_obj_add_float(d, root, "strength", o->strength);
            yyjson_mut_obj_add_bool(d, root, "protectDetail", o->protect_detail);
            yyjson_mut_obj_add_bool(d, root, "fingerPainting", o->finger_painting);
        }
        if (g->tool == PC_TOOL_SPOT_HEALING) {
            yyjson_mut_obj_add_strcpy(d, root, "type", pc_str_c(&o->spot_type));
        }
        yyjson_mut_obj_add_bool(d, root, "sampleAllLayers", o->sample_all_layers);
        if (g->tool == PC_TOOL_REMOVE) {
            yyjson_mut_obj_add_bool(d, root, "newLayer", o->remove_new_layer);
        }
        if (g->tool == PC_TOOL_BACKGROUND_ERASER) {
            yyjson_mut_obj_add_strcpy(d, root, "sampling", pc_str_c(&o->bg_sampling));
            yyjson_mut_obj_add_strcpy(d, root, "limits", pc_str_c(&o->bg_limits));
            yyjson_mut_obj_add_float(d, root, "tolerance", o->bg_tolerance);
            yyjson_mut_obj_add_bool(d, root, "protectForegroundColor", o->bg_protect_fg);
        }
    } else if (g->tool == PC_TOOL_MOVE) {
        if (!preview && roundf(last.x - first.x) == 0 && roundf(last.y - first.y) == 0) {
            yyjson_mut_doc_free(d);
            return PC_OK;
        }
        command = g->handoff == 1   ? "select.transformSelection"
                  : g->float_pixels ? "select.float"
                  : g->move_pixels  ? "edit.transform"
                                    : "layer.translate";
        yyjson_mut_obj_add_bool(
            d, root, "copy",
            g->move_copy && (g->float_pixels || hypot(last.x - first.x, last.y - first.y) * g->zoom >= 2));
        yyjson_mut_obj_add_float(d, root, "dx", roundf(last.x - first.x));
        yyjson_mut_obj_add_float(d, root, "dy", roundf(last.y - first.y));
        yyjson_mut_obj_add_str(d, root, "interpolation", "nearest");
    } else if (g->tool == PC_TOOL_LASSO) {
        if (g->count < 3) {
            yyjson_mut_doc_free(d);
            return PC_OK;
        }
        command = "select.lasso";
        yyjson_mut_val *points = yyjson_mut_arr(d);
        for (size_t i = 0; i < g->count; i++) {
            yyjson_mut_arr_add_val(points, pair(d, g->points[i].x, g->points[i].y));
        }
        yyjson_mut_obj_add_val(d, root, "points", points);
        yyjson_mut_obj_add_str(d, root, "mode", selection);
        yyjson_mut_obj_add_float(d, root, "feather", o->feather);
        yyjson_mut_obj_add_bool(d, root, "antiAlias", o->anti_alias);
    } else if (g->tool == PC_TOOL_GRADIENT) {
        command = "paint.gradient";
        yyjson_mut_obj_add_val(d, root, "from", pair(d, first.x, first.y));
        yyjson_mut_obj_add_val(d, root, "to", pair(d, last.x, last.y));
        yyjson_mut_obj_add_strcpy(d, root, "style", pc_str_c(&o->gradient_style));
        yyjson_mut_obj_add_bool(d, root, "reverse", o->gradient_reverse);
        yyjson_mut_obj_add_bool(d, root, "dither", o->gradient_dither);
        yyjson_mut_obj_add_float(d, root, "opacity", o->fill_opacity);
        yyjson_mut_obj_add_str(d, root, "mode", pc_blend_name(o->gradient_blend_mode));
        if (o->gradient_classic) {
            yyjson_mut_obj_add_str(d, root, "method", "classic");
        }
    } else if (g->tool == PC_TOOL_PAINT_BUCKET || g->tool == PC_TOOL_MAGIC_WAND ||
               g->tool == PC_TOOL_MAGIC_ERASER) {
        command = g->tool == PC_TOOL_PAINT_BUCKET ? "paint.bucket"
                  : g->tool == PC_TOOL_MAGIC_WAND ? "select.magicWand"
                                                  : "paint.magicEraser";
        yyjson_mut_obj_add_float(d, root, "x", first.x);
        yyjson_mut_obj_add_float(d, root, "y", first.y);
        yyjson_mut_obj_add_float(d, root, "tolerance", o->tolerance);
        yyjson_mut_obj_add_bool(d, root, "contiguous", o->contiguous);
        yyjson_mut_obj_add_bool(d, root, "antiAlias", o->anti_alias);
        yyjson_mut_obj_add_bool(d, root, "sampleAllLayers", o->sample_all_layers);
        if (g->tool == PC_TOOL_MAGIC_WAND) {
            yyjson_mut_obj_add_str(d, root, "mode", selection);
        } else {
            yyjson_mut_obj_add_float(d, root, "opacity",
                                     g->tool == PC_TOOL_MAGIC_ERASER ? o->magic_eraser_opacity
                                                                     : o->fill_opacity);
            if (g->tool == PC_TOOL_PAINT_BUCKET) {
                yyjson_mut_obj_add_str(d, root, "contents",
                                       o->bucket_fill_pattern ? "pattern" : "foreground");
            }
        }
    } else if (g->tool == PC_TOOL_RED_EYE) {
        command = "paint.redEye";
        yyjson_mut_obj_add_float(d, root, "x", first.x);
        yyjson_mut_obj_add_float(d, root, "y", first.y);
        yyjson_mut_obj_add_float(d, root, "pupilSize", fmaxf(1, fminf(100, o->red_eye_pupil_size)));
        yyjson_mut_obj_add_float(d, root, "darken", fmaxf(0, fminf(100, o->red_eye_darken)));
    } else if (g->tool == PC_TOOL_QUICK_SELECTION) {
        command = "select.quick";
        yyjson_mut_val *points = yyjson_mut_arr(d);
        for (size_t i = 0; i < g->count; i++) {
            yyjson_mut_arr_add_val(points, pair(d, g->points[i].x, g->points[i].y));
        }
        yyjson_mut_obj_add_val(d, root, "points", points);
        yyjson_mut_obj_add_float(d, root, "size", pc_engine_session_tools(session)->brush.size);
        yyjson_mut_obj_add_str(d, root, "mode", g->alt_start ? "subtract" : "add");
        yyjson_mut_obj_add_bool(d, root, "enhanceEdge", o->enhance_edge);
        yyjson_mut_obj_add_bool(d, root, "sampleAllLayers", o->sample_all_layers);
    } else if (g->tool == PC_TOOL_CLONE_STAMP || g->tool == PC_TOOL_HEALING) {
        /* Alt-click set the source (cloneSource.set); the engine owns the source + aligned pairing. */
        command = g->tool == PC_TOOL_CLONE_STAMP ? "paint.cloneStamp" : "paint.healingBrush";
        add_points(d, root, g);
        yyjson_mut_obj_add_bool(d, root, "aligned", o->clone_aligned);
        yyjson_mut_obj_add_strcpy(d, root, "sampleLayer", pc_str_c(&o->clone_sample));
        if (g->tool == PC_TOOL_CLONE_STAMP) {
            const pc_paint_brush *brush = &pc_engine_session_tools(session)->brush;
            yyjson_mut_obj_add_float(d, root, "opacity", brush->opacity * 100.0f);
            yyjson_mut_obj_add_float(d, root, "flow", brush->flow * 100.0f);
        }
    } else if (g->tool == PC_TOOL_PATTERN_STAMP) {
        command = "paint.patternStamp";
        add_points(d, root, g);
        yyjson_mut_obj_add_bool(d, root, "aligned", o->pattern_stamp_aligned);
        yyjson_mut_obj_add_bool(d, root, "impressionist", o->pattern_stamp_impressionist);
        yyjson_mut_obj_add_float(d, root, "scale", o->pattern_stamp_scale);
        yyjson_mut_obj_add_float(d, root, "angle", o->pattern_stamp_angle);
        if (o->pattern_stamp_aligned && g->has_pattern_phase && g->pattern_phase_doc == g->document) {
            yyjson_mut_obj_add_val(d, root, "phase",
                                   pair(d, (float)g->pattern_phase[0], (float)g->pattern_phase[1]));
        }
    } else if (g->tool == PC_TOOL_PATCH || g->tool == PC_TOOL_CONTENT_AWARE_MOVE) {
        if (g->patch_dragging) {
            /* Drag the selection: dispatch paint.patch / paint.contentAwareMove with the whole-pixel
             * destination offset. The source region is the current selection (owned by the engine). */
            double dx = round((double)last.x - (double)first.x);
            double dy = round((double)last.y - (double)first.y);
            if (!isfinite(dx) || !isfinite(dy) || fabs(dx) > 1e7 || fabs(dy) > 1e7) {
                yyjson_mut_doc_free(d);
                return pc_fail(err, PC_ERR_INVALID, "patch offset out of range");
            }
            if (dx == 0 && dy == 0) { /* a click inside the selection does nothing */
                yyjson_mut_doc_free(d);
                return PC_OK;
            }
            command = g->tool == PC_TOOL_PATCH ? "paint.patch" : "paint.contentAwareMove";
            yyjson_mut_obj_add_val(d, root, "offset", pair(d, (float)dx, (float)dy));
            if (g->tool == PC_TOOL_PATCH) {
                if (o->patch_content_aware) {
                    long st = lroundf(o->patch_structure), co = lroundf(o->patch_color);
                    yyjson_mut_obj_add_bool(d, root, "contentAware", true);
                    yyjson_mut_obj_add_int(d, root, "structure", st < 1 ? 1 : (st > 7 ? 7 : st));
                    yyjson_mut_obj_add_int(d, root, "color", co < 0 ? 0 : (co > 10 ? 10 : co));
                    yyjson_mut_obj_add_bool(d, root, "sampleAllLayers", o->sample_all_layers);
                } else {
                    yyjson_mut_obj_add_strcpy(d, root, "mode", pc_str_c(&o->patch_mode));
                }
            } else {
                long st = lroundf(o->cam_structure), co = lroundf(o->cam_color);
                yyjson_mut_obj_add_strcpy(d, root, "mode", pc_str_c(&o->cam_mode));
                yyjson_mut_obj_add_int(d, root, "structure", st < 1 ? 1 : (st > 7 ? 7 : st));
                yyjson_mut_obj_add_int(d, root, "color", co < 0 ? 0 : (co > 10 ? 10 : co));
                yyjson_mut_obj_add_bool(d, root, "sampleAllLayers", o->sample_all_layers);
            }
        } else {
            /* No drag: the gesture is a freehand lasso selection (feather is forced off, unlike the
             * Lasso tool, because the patch/move samples hard-edged regions). */
            if (g->count < 3) {
                yyjson_mut_doc_free(d);
                return PC_OK;
            }
            command = "select.lasso";
            yyjson_mut_val *points = yyjson_mut_arr(d);
            for (size_t i = 0; i < g->count; i++) {
                yyjson_mut_arr_add_val(points, pair(d, g->points[i].x, g->points[i].y));
            }
            yyjson_mut_obj_add_val(d, root, "points", points);
            yyjson_mut_obj_add_str(d, root, "mode", selection);
            yyjson_mut_obj_add_float(d, root, "feather", 0);
            yyjson_mut_obj_add_bool(d, root, "antiAlias", o->anti_alias);
        }
    } else {
        bool crop = g->tool == PC_TOOL_CROP;
        command = crop ? "image.crop" : "select.rect";
        float x = floorf(fminf(first.x, last.x)), y = floorf(fminf(first.y, last.y));
        float w = ceilf(fmaxf(first.x, last.x)) - x, h = ceilf(fmaxf(first.y, last.y)) - y;
        if (g->tool == PC_TOOL_SINGLE_ROW_MARQUEE) {
            h = 1;
        }
        if (g->tool == PC_TOOL_SINGLE_COLUMN_MARQUEE) {
            w = 1;
        }
        if (!crop && !strcmp(pc_str_c(&o->marquee_style), "fixedSize")) {
            w = o->marquee_width;
            h = o->marquee_height;
        }
        if (!isfinite(w) || !isfinite(h) || w > 1e8f || h > 1e8f) {
            yyjson_mut_doc_free(d);
            return pc_fail(err, PC_ERR_INVALID, "marquee dimensions out of range");
        }
        if (w < 1 || h < 1) {
            yyjson_mut_doc_free(d);
            return PC_OK;
        }
        yyjson_mut_obj_add_sint(d, root, "x", (int64_t)x);
        yyjson_mut_obj_add_sint(d, root, "y", (int64_t)y);
        yyjson_mut_obj_add_uint(d, root, "width", (uint64_t)w);
        yyjson_mut_obj_add_uint(d, root, "height", (uint64_t)h);
        if (crop) {
            yyjson_mut_obj_add_bool(d, root, "deleteCroppedPixels", o->crop_delete);
        } else {
            yyjson_mut_obj_add_str(d, root, "mode", selection);
            yyjson_mut_obj_add_bool(d, root, "ellipse", g->tool == PC_TOOL_ELLIPSE_MARQUEE);
            yyjson_mut_obj_add_bool(d, root, "antiAlias", o->anti_alias);
            yyjson_mut_obj_add_float(d, root, "feather", o->feather);
            if (g->tool == PC_TOOL_SINGLE_ROW_MARQUEE || g->tool == PC_TOOL_SINGLE_COLUMN_MARQUEE) {
                yyjson_mut_obj_add_str(d, root, "line",
                                       g->tool == PC_TOOL_SINGLE_ROW_MARQUEE ? "row" : "column");
            }
        }
    }
    if (g->has_seed) {
        yyjson_mut_obj_add_uint(d, root, "seed", g->seed);
    }
    pc_buf text = PC_BUF_INIT;
    pc_status status = pc_json_write(root, 0, &text, err);
    if (status == PC_OK) {
        if (preview) {
            yyjson_doc *params = NULL;
            status = pc_json_parse(pc_buf_cstr(&text), text.len, &params, err);
            if (status == PC_OK && g->tool == PC_TOOL_MOVE) {
                pc_doc_arc *doc = NULL;
                g->preview_layer = g->has_layer ? g->layer : 0;
                if (g->handoff == 1)
                    status = pc_engine_preview_sync(session, command, yyjson_doc_get_root(params), &doc, err);
                else if (g->float_pixels)
                    status = pc_engine_floating_preview(session, (int32_t)roundf(last.x - first.x),
                                                        (int32_t)roundf(last.y - first.y), g->move_copy,
                                                        g->target == 1, &doc, err);
                else if (g->move_pixels)
                    status = pc_engine_preview_sync(session, command, yyjson_doc_get_root(params), &doc, err);
                else
                    status = pc_engine_move_preview_layer(session, yyjson_doc_get_root(params), &doc,
                                                          &g->preview_layer, err);
                if (status == PC_OK) {
                    pc_shape_layer_fini(&g->shape_preview);
                    g->has_shape_preview = false;
                    pc_doc_arc_release(g->move_preview);
                    g->move_preview = doc;
                    status = next_preview(g, err);
                }
            } else if (status == PC_OK) {
                status =
                    pc_engine_live_stroke_begin(session, command, yyjson_doc_get_root(params), &g->live, err);
            }
            yyjson_doc_free(params);
            if (status == PC_OK && g->live) {
                g->seed = pc_engine_live_stroke_seed(g->live);
                g->has_seed = true;
                status = next_preview(g, err);
            }
        } else {
            pc_engine_session_offer_live_stroke(session, &g->live);
            status = pc_engine_session_execute_json(session, command, pc_buf_cstr(&text), out, err);
            pc_engine_session_clear_live_stroke(session);
        }
    }
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
    return status;
}
static pc_status gesture_event(pc_ui_tool_gesture *g, pc_engine_session *session, const pc_ui_state *ui,
                               pc_ui_pointer_kind kind, pc_ui_tool_point p, unsigned modifiers, float zoom,
                               unsigned handoff, pc_json_value *out, pc_error *err)
{
    if (!g || !session || !ui || !out || (unsigned)kind > PC_UI_POINTER_CANCEL) {
        return pc_fail(err, PC_ERR_INVALID, "missing pointer routing context");
    }
    if (kind == PC_UI_POINTER_CANCEL) {
        finish(g);
        return PC_OK;
    }
    if (!valid(p) || !isfinite(zoom) || zoom <= 0) {
        return pc_fail(err, PC_ERR_INVALID, "invalid pointer sample");
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state) {
        finish(g);
        return pc_fail(err, PC_ERR_INVALID, "pointer tool needs a document");
    }
    if (kind == PC_UI_POINTER_DOWN) {
        if (!pc_ui_tool_gesture_supported(ui->tool)) {
            return PC_ERR_NOT_FOUND;
        }
        if (ui->n_dialogs) {
            return pc_fail(err, PC_ERR_INVALID, "confirm or cancel the open dialog first");
        }
        /* Alt-click with Clone Stamp / Healing Brush sets the clone source; it never starts a stroke. */
        if ((ui->tool == PC_TOOL_CLONE_STAMP || ui->tool == PC_TOOL_HEALING) && (modifiers & PC_UI_MOD_ALT)) {
            finish(g);
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            if (!d) {
                return pc_fail(err, PC_ERR_NOMEM, "clone source");
            }
            double sx = round(p.x), sy = round(p.y);
            yyjson_mut_val *params = yyjson_mut_obj(d);
            yyjson_mut_obj_add_val(d, params, "source", pair(d, (float)sx, (float)sy));
            pc_buf text = PC_BUF_INIT;
            pc_status rc = pc_json_write(params, 0, &text, err);
            if (rc == PC_OK) {
                rc = pc_engine_session_execute_json(session, "cloneSource.set", pc_buf_cstr(&text), out, err);
            }
            pc_buf_fini(&text);
            yyjson_mut_doc_free(d);
            if (rc == PC_OK) {
                g->has_clone_source = true;
                g->clone_source_doc = pc_doc_arc_get(state->doc)->id;
                g->clone_source[0] = sx;
                g->clone_source[1] = sy;
            }
            return rc;
        }
        if (ui->tool == PC_TOOL_MOVE && handoff != 1 && handoff != 3 && (modifiers & PC_UI_MOD_ALT) &&
            pc_engine_floating_can_move(session) && pc_engine_floating_offset(session, NULL)) {
            pc_status dropped = pc_engine_session_execute_json(session, "select.drop", "{}", out, err);
            pc_json_value_fini(out);
            if (dropped != PC_OK)
                return dropped;
            state = pc_engine_session_active(session);
        }
        if (ui->tool == PC_TOOL_MOVE && !pc_doc_arc_get(state->doc)->has_selection && !ui->mask_target &&
            !pc_engine_targeted_mask(session) && ui->tool_options.move_auto_select) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            if (!d) {
                return pc_fail(err, PC_ERR_NOMEM, "Move auto-selection");
            }
            yyjson_mut_val *params = yyjson_mut_obj(d);
            yyjson_mut_obj_add_float(d, params, "x", p.x);
            yyjson_mut_obj_add_float(d, params, "y", p.y);
            yyjson_mut_obj_add_strcpy(d, params, "target", pc_str_c(&ui->tool_options.move_target));
            yyjson_mut_obj_add_str(d, params, "mode", (modifiers & PC_UI_MOD_SHIFT) ? "toggle" : "replace");
            pc_buf text = PC_BUF_INIT;
            pc_status pick = pc_json_write(params, 0, &text, err);
            if (pick == PC_OK) {
                pick = pc_engine_session_execute_json(session, "layer.pickAt", pc_buf_cstr(&text), out, err);
            }
            yyjson_mut_doc_free(d);
            pc_buf_fini(&text);
            pc_json_value_fini(out);
            if (pick != PC_OK) {
                return pick;
            }
            state = pc_engine_session_active(session);
        }
        finish(g);
        pc_status status = pc_ui_tool_options_clone(&ui->tool_options, &g->options, err);
        if (status != PC_OK) {
            return status;
        }
        g->active = true;
        g->handoff = handoff;
        g->tool = ui->tool;
        if (state->nselected) {
            g->selected = pc_malloc_array(state->nselected, sizeof *g->selected);
            if (!g->selected) {
                finish(g);
                return pc_fail(err, PC_ERR_NOMEM, "gesture layer selection");
            }
            memcpy(g->selected, state->selected_layers, state->nselected * sizeof *g->selected);
            g->nselected = state->nselected;
        }
        g->document = pc_doc_arc_get(state->doc)->id;
        g->layer = state->active_layer;
        g->has_layer = state->has_active_layer;
        g->move_pixels =
            handoff == 2 || (handoff != 1 && handoff != 3 && pc_engine_floating_can_move(session));
        g->revision = state->revision;
        g->zoom = zoom;
        g->alpha = 0;
        g->color = pc_engine_targeted_color(session);
        g->target = current_target(session, ui, &g->alpha);
        g->float_pixels = g->move_pixels && g->target < 2;
        g->move_copy = (modifiers & PC_UI_MOD_ALT) != 0;
        g->move_pixels = g->move_pixels || g->target >= 2;
        const pc_engine_tool_state *tools = pc_engine_session_tools(session);
        memcpy(g->colors[0], tools->foreground, sizeof g->colors[0]);
        memcpy(g->colors[1], tools->background, sizeof g->colors[1]);
        status = pc_paint_json_to_string(PC_PAINT_JSON_BRUSH, &tools->brush, 0, &g->brush, err);
        if (status != PC_OK) {
            finish(g);
            return status;
        }
        g->selection_mode = (modifiers & PC_UI_MOD_SHIFT) ? ((modifiers & PC_UI_MOD_ALT) ? 3u : 1u)
                            : (modifiers & PC_UI_MOD_ALT) ? 2u
                                                          : ui->selection_mode;
        g->alt_start = (modifiers & PC_UI_MOD_ALT) != 0;
        /* Patch / Content-Aware Move: with no modifiers, pressing inside the current selection drags
         * that selection (the patch/move); otherwise the gesture builds a freehand lasso. */
        if (g->tool == PC_TOOL_PATCH || g->tool == PC_TOOL_CONTENT_AWARE_MOVE) {
            g->patch_dragging = !(modifiers & (PC_UI_MOD_SHIFT | PC_UI_MOD_ALT)) &&
                                point_in_selection(pc_doc_arc_get(state->doc), p.x, p.y);
        }
        /* Shift-click after a painting stroke connects a straight line from the previous stroke's
         * end to the click: the previous endpoint (with its pen pressure) becomes this stroke's
         * first point, so it is one command and one undo step. */
        if ((modifiers & PC_UI_MOD_SHIFT) && pc_ui_stroke_constraint_connects(g->tool) &&
            g->has_last_stroke && g->last_stroke_doc == g->document) {
            pc_ui_tool_point prev = {.x = (float)g->last_stroke_end[0],
                                     .y = (float)g->last_stroke_end[1],
                                     .pressure = g->last_stroke_pressure};
            if (valid(prev)) {
                status = append(g, prev, err);
                if (status != PC_OK) {
                    finish(g);
                    return status;
                }
            }
        }
    } else if (!g->active) {
        return PC_OK;
    }
    size_t alpha = 0;
    unsigned target = current_target(session, ui, &alpha);
    if (g->color != pc_engine_targeted_color(session) || target != g->target ||
        (target == 3 && alpha != g->alpha) || ui->n_dialogs ||
        pc_doc_arc_get(state->doc)->id != g->document || state->revision != g->revision ||
        state->nselected != g->nselected ||
        (g->nselected && memcmp(g->selected, state->selected_layers, g->nselected * sizeof *g->selected)) ||
        state->has_active_layer != g->has_layer || (g->has_layer && state->active_layer != g->layer)) {
        finish(g);
        return pc_fail(err, PC_ERR_INVALID, "document or layer changed during pointer gesture");
    }
    if (g->tool == PC_TOOL_MOVE && g->count) {
        double delta[2] = {(double)p.x - g->points[0].x, (double)p.y - g->points[0].y};
        double constrained[2] = {delta[0], delta[1]};
        if (modifiers & PC_UI_MOD_SHIFT)
            pc_ui_move_constrain(delta, g->move_has_last ? g->move_last : NULL, g->float_pixels ? 8 : 4,
                                 constrained);
        memcpy(g->move_last, constrained, sizeof constrained);
        g->move_has_last = true;
        p.x = (float)(g->points[0].x + constrained[0]);
        p.y = (float)(g->points[0].y + constrained[1]);
    }
    /* Shift straight-line (0/45/90 degrees) for painting tools; Shift 45 degree snap for the
     * Gradient tool. Never constrains the press itself (a connecting Shift-click keeps its exact
     * endpoint). */
    if (kind != PC_UI_POINTER_DOWN && pc_ui_stroke_constraint_applies(g->tool) && g->count) {
        double start[2] = {g->points[0].x, g->points[0].y};
        double last[2] = {g->points[g->count - 1].x, g->points[g->count - 1].y};
        double pin[2] = {p.x, p.y}, snapped[2];
        pc_ui_stroke_constrain(g->tool, &g->stroke_axis, start, last, pin,
                               (modifiers & PC_UI_MOD_SHIFT) != 0, g->zoom, snapped);
        if (isfinite(snapped[0]) && isfinite(snapped[1])) {
            p.x = (float)snapped[0];
            p.y = (float)snapped[1];
        }
    }
    size_t old_count = g->count;
    pc_status status = append(g, p, err);
    if (status != PC_OK) {
        finish(g);
        return status;
    }
    g->modifiers = modifiers;
    bool brush = g->tool == PC_TOOL_BRUSH || g->tool == PC_TOOL_PENCIL || g->tool == PC_TOOL_ERASER;
    if (pc_ui_is_shape_tool(g->tool)) {
        status = commit(g, session, true, out, err);
        if (status == PC_OK) {
            status = next_preview(g, err);
        }
    } else if (g->tool == PC_TOOL_MOVE) {
        status = commit(g, session, true, out, err);
    } else if (brush && !g->live) {
        status = commit(g, session, true, out, err);
    } else if (g->live && g->count != old_count) {
        pc_paint_point point = pc_paint_point_make(p.x, p.y, p.pressure);
        point.tilt_x = p.tilt_x;
        point.tilt_y = p.tilt_y;
        point.rotation = p.rotation;
        point.time = p.time_ms;
        point.wheel = p.wheel;
        status = pc_engine_live_stroke_push(g->live, &point, 1, NULL, err);
        if (status == PC_OK) {
            status = next_preview(g, err);
        }
    }
    if (status != PC_OK) {
        finish(g);
        return status;
    }
    if (kind == PC_UI_POINTER_UP) {
        const pc_engine_tool_state *tools = pc_engine_session_tools(session);
        pc_buf current = PC_BUF_INIT;
        status = pc_paint_json_to_string(PC_PAINT_JSON_BRUSH, &tools->brush, 0, &current, err);
        if (status == PC_OK &&
            (current.len != g->brush.len || memcmp(current.data, g->brush.data, current.len) ||
             memcmp(g->colors[0], tools->foreground, sizeof g->colors[0]) ||
             memcmp(g->colors[1], tools->background, sizeof g->colors[1]))) {
            status = pc_fail(err, PC_ERR_INVALID, "brush or colours changed during pointer gesture");
        }
        pc_buf_fini(&current);
        if (status == PC_OK) {
            /* A preview commit above (shape/move/brush) already wrote `out`; drop it before the
             * final commit overwrites it, so the preview result does not leak. */
            pc_json_value_fini(out);
            status = commit(g, session, false, out, err);
        }
        /* Remember where a painting stroke ended, so the next Shift-click connects a line to it. */
        if (status == PC_OK && pc_ui_stroke_constraint_connects(g->tool) && g->count) {
            g->has_last_stroke = true;
            g->last_stroke_doc = g->document;
            g->last_stroke_end[0] = g->points[g->count - 1].x;
            g->last_stroke_end[1] = g->points[g->count - 1].y;
            g->last_stroke_pressure = g->points[g->count - 1].pressure;
        }
        /* Pattern Stamp keeps the tile phase across aligned strokes (read back from the result). */
        if (status == PC_OK && g->tool == PC_TOOL_PATTERN_STAMP) {
            g->has_pattern_phase = false;
            yyjson_mut_val *root = out ? pc_json_value_root(out) : NULL;
            yyjson_mut_val *phase = root ? yyjson_mut_obj_get(root, "phase") : NULL;
            if (g->options.pattern_stamp_aligned && phase && yyjson_mut_arr_size(phase) == 2) {
                g->pattern_phase[0] = yyjson_mut_get_num(yyjson_mut_arr_get(phase, 0));
                g->pattern_phase[1] = yyjson_mut_get_num(yyjson_mut_arr_get(phase, 1));
                g->has_pattern_phase = true;
                g->pattern_phase_doc = g->document;
            }
        }
        finish(g);
    }
    return status;
}

pc_status pc_ui_tool_gesture_event(pc_ui_tool_gesture *g, pc_engine_session *session, const pc_ui_state *ui,
                                   pc_ui_pointer_kind kind, pc_ui_tool_point point, unsigned modifiers,
                                   float zoom, pc_json_value *out, pc_error *err)
{
    return gesture_event(g, session, ui, kind, point, modifiers, zoom, 0, out, err);
}
pc_status pc_ui_tool_gesture_selection_event(pc_ui_tool_gesture *g, pc_engine_session *session,
                                             const pc_ui_state *ui, unsigned handoff, pc_ui_pointer_kind kind,
                                             pc_ui_tool_point point, unsigned modifiers, float zoom,
                                             pc_json_value *out, pc_error *err)
{
    if (!ui || !g || handoff < 1 || handoff > 3)
        return pc_fail(err, PC_ERR_INVALID, "invalid selection handoff");
    if (kind != PC_UI_POINTER_DOWN && g->active && ui->tool != g->handoff_tool) {
        finish(g);
        return pc_fail(err, PC_ERR_INVALID, "the selection tool changed during its drag");
    }
    pc_ui_state temporary = *ui;
    temporary.tool = PC_TOOL_MOVE;
    if (handoff < 3)
        temporary.tool_options.move_auto_select = false;
    pc_status status = gesture_event(g, session, &temporary, kind, point, modifiers, zoom, handoff, out, err);
    if (kind == PC_UI_POINTER_DOWN && status == PC_OK)
        g->handoff_tool = ui->tool;
    return status;
}
