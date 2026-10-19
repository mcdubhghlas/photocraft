#include "engine/style_editor.h"
#include "engine/engine_internal.h"
#include "engine/layer_style.h"
#include "engine/layer_cmds.h"
#include "engine/brush_cmds.h"
#include "engine/channel_display.h"
#include "doc/json.h"
#include "compose/effects_util.h"
#include "color/convert.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
pc_status pc_engine_style_contour_view(yyjson_val *param, yyjson_mut_doc *d, yyjson_mut_val **levels,
                                       yyjson_mut_val **corners, float samples[256], pc_error *err)
{
    if (!d || !levels || !corners || !samples)
        return pc_fail(err, PC_ERR_INVALID, "missing contour output");
    pc_contour curve = {0};
    if (!pc_engine_contour_from_param(param, &curve))
        (void)pc_engine_builtin_contour("Linear", &curve);
    yyjson_mut_val *points = yyjson_mut_arr(d), *flags = yyjson_mut_arr(d);
    bool ok = points && flags;
    size_t count = curve.points.len ? curve.points.len : 2;
    for (size_t i = 0; ok && i < count; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        double x = curve.points.len ? curve.points.pts[i].input : (double)i,
               y = curve.points.len ? curve.points.pts[i].output : (double)i;
        ok = pair && yyjson_mut_arr_add_real(d, pair, x * 255) && yyjson_mut_arr_add_real(d, pair, y * 255) &&
             yyjson_mut_arr_add_val(points, pair) &&
             yyjson_mut_arr_add_bool(d, flags, pc_contour_is_corner(&curve, i));
    }
    float *lut = NULL;
    pc_status status =
        ok ? pc_compose_fx_contour_lut(&curve, &lut, err) : pc_fail(err, PC_ERR_NOMEM, "contour points");
    if (status == PC_OK) {
        for (size_t i = 0; i < 256; i++) {
            size_t index = (i * (PC_COMPOSE_FX_LUT_SIZE - 1) + 127) / 255;
            samples[i] = lut ? lut[index] : (float)i / 255;
        }
        *levels = points;
        *corners = flags;
    }
    free(lut);
    pc_contour_fini(&curve);
    return status;
}
static void number(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, double value)
{
    (void)yyjson_mut_obj_add_real(d, o, key, value);
}
static void boolean(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, bool value)
{
    (void)yyjson_mut_obj_add_bool(d, o, key, value);
}
static void string(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, const char *value)
{
    (void)yyjson_mut_obj_add_strcpy(d, o, key, value);
}
static void color(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, const pc_color *c)
{
    float rgb[3];
    pc_color_to_rgb(c, rgb);
    yyjson_mut_val *array = yyjson_mut_arr(d);
    if (!array)
        return;
    for (size_t i = 0; i < 3; i++)
        if (!yyjson_mut_arr_add_real(d, array, rgb[i]))
            return;
    (void)yyjson_mut_obj_add_val(d, o, key, array);
}
static void common(yyjson_mut_doc *d, yyjson_mut_val *o, const pc_fx_common *c)
{
    boolean(d, o, "enabled", c->enabled);
    number(d, o, "opacity", (double)c->opacity * 100);
    string(d, o, "blend", pc_blend_label(c->blend));
}
static void contour(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, const pc_contour *c)
{
    yyjson_mut_val *v = NULL;
    if (pc_engine_contour_param(c, d, &v, NULL) == PC_OK)
        (void)yyjson_mut_obj_add_val(d, o, key, v);
}
static void gradient(yyjson_mut_doc *d, yyjson_mut_val *o, const pc_gradient *g)
{
    const char *styles[] = {"linear", "radial", "angle", "reflected", "diamond"};
    size_t index = (size_t)g->style;
    string(d, o, "style", index < 5 ? styles[index] : "linear");
    number(d, o, "angle", g->angle);
    number(d, o, "scale", (double)g->scale * 100);
    boolean(d, o, "reverse", g->reverse);
    if (g->nstops) {
        color(d, o, "from", &g->stops[0].color);
        color(d, o, "to", &g->stops[g->nstops - 1].color);
    }
}
static void paint(yyjson_mut_doc *d, yyjson_mut_val *o, const pc_fx_paint *p)
{
    if (p->kind == PC_FX_PAINT_COLOR)
        color(d, o, "color", &p->u.color);
    else if (p->kind == PC_FX_PAINT_GRADIENT)
        gradient(d, o, &p->u.gradient);
    else {
        string(d, o, "pattern", pc_str_c(&p->u.pattern.name));
        number(d, o, "scale", (double)p->u.pattern.scale * 100);
    }
}
static yyjson_mut_val *values(yyjson_mut_doc *d, const pc_effect *fx)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (!o)
        return NULL;
    switch (fx->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW: {
        const pc_shadow *s = &fx->u.shadow;
        common(d, o, &s->common);
        color(d, o, "color", &s->color);
        number(d, o, "angle", s->angle);
        boolean(d, o, "useGlobalLight", s->use_global_light);
        number(d, o, "distance", s->distance);
        number(d, o, fx->kind == PC_FX_DROP_SHADOW ? "spread" : "choke", (double)s->spread * 100);
        number(d, o, "size", s->size);
        contour(d, o, "contour", &s->contour);
        boolean(d, o, "antiAlias", s->anti_alias);
        number(d, o, "noise", (double)s->noise * 100);
        boolean(d, o, "knocksOut", s->knocks_out);
        break;
    }
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW: {
        const pc_glow *g = &fx->u.glow;
        common(d, o, &g->common);
        paint(d, o, &g->paint);
        string(d, o, "technique", g->technique == PC_GLOW_PRECISE ? "precise" : "softer");
        number(d, o, fx->kind == PC_FX_OUTER_GLOW ? "spread" : "choke", (double)g->spread * 100);
        number(d, o, "size", g->size);
        contour(d, o, "contour", &g->contour);
        boolean(d, o, "antiAlias", g->anti_alias);
        number(d, o, "range", (double)g->range * 100);
        number(d, o, "noise", (double)g->noise * 100);
        string(d, o, "source", g->source == PC_GLOW_SOURCE_CENTER ? "center" : "edge");
        break;
    }
    case PC_FX_STROKE: {
        const pc_stroke_fx *s = &fx->u.stroke;
        common(d, o, &s->common);
        number(d, o, "size", s->size);
        string(d, o, "position",
               s->position == PC_STROKE_INSIDE   ? "inside"
               : s->position == PC_STROKE_CENTER ? "center"
                                                 : "outside");
        paint(d, o, &s->paint);
        break;
    }
    case PC_FX_COLOR_OVERLAY:
        common(d, o, &fx->u.color_overlay.common);
        color(d, o, "color", &fx->u.color_overlay.color);
        break;
    case PC_FX_GRADIENT_OVERLAY:
        common(d, o, &fx->u.gradient_overlay.common);
        gradient(d, o, &fx->u.gradient_overlay.gradient);
        boolean(d, o, "dither", fx->u.gradient_overlay.dither);
        break;
    case PC_FX_PATTERN_OVERLAY:
        common(d, o, &fx->u.pattern_overlay.common);
        string(d, o, "pattern", pc_str_c(&fx->u.pattern_overlay.name));
        number(d, o, "scale", (double)fx->u.pattern_overlay.scale * 100);
        number(d, o, "angle", fx->u.pattern_overlay.angle);
        boolean(d, o, "link", fx->u.pattern_overlay.link);
        number(d, o, "phaseX", fx->u.pattern_overlay.phase[0]);
        number(d, o, "phaseY", fx->u.pattern_overlay.phase[1]);
        break;
    case PC_FX_SATIN: {
        const pc_satin *s = &fx->u.satin;
        common(d, o, &s->common);
        color(d, o, "color", &s->color);
        number(d, o, "angle", s->angle);
        number(d, o, "distance", s->distance);
        number(d, o, "size", s->size);
        contour(d, o, "contour", &s->contour);
        boolean(d, o, "antiAlias", s->anti_alias);
        boolean(d, o, "invert", s->invert);
        break;
    }
    case PC_FX_BEVEL_EMBOSS: {
        const pc_bevel *b = &fx->u.bevel;
        const char *styles[] = {"outer", "inner", "emboss", "pillow", "stroke"},
                   *tech[] = {"smooth", "chiselHard", "chiselSoft"};
        string(d, o, "style", (size_t)b->style < 5 ? styles[b->style] : "inner");
        string(d, o, "technique", (size_t)b->technique < 3 ? tech[b->technique] : "smooth");
        boolean(d, o, "enabled", b->enabled);
        number(d, o, "depth", (double)b->depth * 100);
        string(d, o, "direction", b->up ? "up" : "down");
        number(d, o, "size", b->size);
        number(d, o, "soften", b->soften);
        number(d, o, "angle", b->angle);
        number(d, o, "altitude", b->altitude);
        boolean(d, o, "useGlobalLight", b->use_global_light);
        contour(d, o, "glossContour", &b->gloss_contour);
        boolean(d, o, "contour", b->has_contour);
        if (b->has_contour) {
            contour(d, o, "contourName", &b->contour.contour);
            number(d, o, "contourRange", (double)b->contour.range * 100);
            boolean(d, o, "contourAntiAlias", b->contour.anti_alias);
        }
        boolean(d, o, "textureEnabled", b->has_texture);
        if (b->has_texture) {
            string(d, o, "texture", pc_str_c(&b->texture.name));
            number(d, o, "textureScale", (double)b->texture.scale * 100);
            number(d, o, "textureDepth", (double)b->texture.depth * 100);
            boolean(d, o, "textureInvert", b->texture.invert);
            boolean(d, o, "textureLink", b->texture.link);
        }
        string(d, o, "highlightBlend", pc_blend_label(b->highlight.blend));
        number(d, o, "highlightOpacity", (double)b->highlight.opacity * 100);
        color(d, o, "highlightColor", &b->highlight_color);
        string(d, o, "shadowBlend", pc_blend_label(b->shadow.blend));
        number(d, o, "shadowOpacity", (double)b->shadow.opacity * 100);
        color(d, o, "shadowColor", &b->shadow_color);
        break;
    }
    default:
        return NULL;
    }
    return o;
}
pc_status pc_engine_style_editor_entry(const char *kind, const pc_effect *effect, uint64_t id,
                                       yyjson_mut_doc *d, yyjson_mut_val **out, pc_error *err)
{
    pc_effect fresh = {0};
    bool owns = false;
    if (!effect) {
        if (!pc_engine_effect_from_params(kind, NULL, &fresh))
            return pc_fail(err, PC_ERR_INVALID, "unknown or unavailable effect");
        effect = &fresh;
        owns = true;
    }
    const char *actual = pc_engine_effect_kind_of(effect);
    if (!kind)
        kind = actual;
    if (strcmp(kind, actual)) {
        if (owns)
            pc_effect_fini(&fresh);
        return pc_fail(err, PC_ERR_INVALID, "effect kind mismatch");
    }
    char name[40];
    (void)snprintf(name, sizeof name, "fx%llu", (unsigned long long)id);
    yyjson_mut_val *entry = yyjson_mut_obj(d), *params = yyjson_mut_obj(d), *display = values(d, effect),
                   *snapshot = owns ? NULL : pc_effect_to_json(d, effect, 0);
    bool ok = entry && params && display && yyjson_mut_obj_add_strcpy(d, entry, "id", name) &&
              yyjson_mut_obj_add_strcpy(d, entry, "kind", kind) &&
              yyjson_mut_obj_add_bool(d, entry, "on", pc_effect_enabled(effect)) &&
              yyjson_mut_obj_add_val(d, entry, "params", params) &&
              yyjson_mut_obj_add_val(d, entry, "values", display) &&
              (!snapshot || yyjson_mut_obj_add_val(d, entry, "fx", snapshot));
    if (!owns && !snapshot)
        ok = false;
    if (owns)
        pc_effect_fini(&fresh);
    if (!ok)
        return pc_fail(err, PC_ERR_NOMEM, "effect editor entry");
    *out = entry;
    return PC_OK;
}
pc_status pc_engine_style_editor_new_entry(const pc_engine_session *s, const char *kind, uint64_t id,
                                           yyjson_mut_doc *d, yyjson_mut_val **out, pc_error *err)
{
    pc_status status = pc_engine_style_editor_entry(kind, NULL, id, d, out, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_val *saved = yyjson_mut_obj_get(pc_json_value_root(&s->presets.layer_defaults), kind);
    if (yyjson_mut_is_obj(saved)) {
        yyjson_mut_val *params = yyjson_mut_val_mut_copy(d, saved), *key = yyjson_mut_str(d, "params");
        if (!params || !key || !yyjson_mut_obj_put(*out, key, params))
            return pc_fail(err, PC_ERR_NOMEM, "Layer Style default params");
    }
    return PC_OK;
}
static yyjson_mut_val *blend_values(yyjson_mut_doc *d, const pc_document *doc, const pc_layer *layer)
{
    yyjson_mut_val *o = yyjson_mut_obj(d), *channels = yyjson_mut_arr(d), *ranges = yyjson_mut_arr(d);
    if (!o || !channels || !ranges)
        return NULL;
    number(d, o, "opacity", (double)layer->opacity * 100);
    number(d, o, "fillOpacity", (double)layer->fill_opacity * 100);
    string(d, o, "blend", pc_blend_label(layer->blend));
    string(d, o, "knockout", pc_knockout_name(layer->advanced.knockout));
    boolean(d, o, "blendInteriorEffectsAsGroup", layer->advanced.blend_interior);
    boolean(d, o, "blendClippedLayersAsGroup", layer->advanced.blend_clipped);
    boolean(d, o, "transparencyShapesLayer", layer->advanced.transparency_shapes);
    boolean(d, o, "layerMaskHidesEffects", layer->advanced.layer_mask_hides_effects);
    boolean(d, o, "vectorMaskHidesEffects", layer->advanced.vector_mask_hides_effects);
    size_t count = pc_engine_color_count(doc);
    if (count > 32)
        count = 32;
    for (size_t i = 0; i < count; i++)
        if (!yyjson_mut_arr_add_bool(d, channels, (layer->excluded_channels & (UINT32_C(1) << i)) == 0))
            return NULL;
    for (size_t i = 0; i < layer->blend_if.nranges; i++) {
        yyjson_mut_val *entry = yyjson_mut_obj(d), *src = yyjson_mut_arr(d), *dst = yyjson_mut_arr(d);
        if (!entry || !src || !dst)
            return NULL;
        uint8_t a[4], b[4];
        pc_blend_range_to_bytes(layer->blend_if.ranges[i][0], a);
        pc_blend_range_to_bytes(layer->blend_if.ranges[i][1], b);
        for (size_t j = 0; j < 4; j++)
            if (!yyjson_mut_arr_add_uint(d, src, a[j]) || !yyjson_mut_arr_add_uint(d, dst, b[j]))
                return NULL;
        if (!yyjson_mut_obj_add_uint(d, entry, "channel", i) ||
            !yyjson_mut_obj_add_val(d, entry, "thisLayer", src) ||
            !yyjson_mut_obj_add_val(d, entry, "underlying", dst) || !yyjson_mut_arr_add_val(ranges, entry))
            return NULL;
    }
    if (!yyjson_mut_obj_add_val(d, o, "channels", channels) ||
        !yyjson_mut_obj_add_val(d, o, "blendIf", ranges))
        return NULL;
    return o;
}
static pc_status initial_inner(const pc_engine_session *s, const char *select, yyjson_mut_doc *d,
                               yyjson_mut_val **out, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    if (!state || !state->has_active_layer)
        return pc_fail(err, PC_ERR_INVALID, "Layer Style needs an active layer");
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const pc_layer *layer = pc_document_layer(doc, state->active_layer);
    if (!layer)
        return pc_fail(err, PC_ERR_NOT_FOUND, "the style's layer was removed");
    if (layer->effects.nitems > 256)
        return pc_fail(err, PC_ERR_LIMIT, "at most 256 effects");
    yyjson_mut_val *o = yyjson_mut_obj(d), *effects = yyjson_mut_arr(d), *blend = yyjson_mut_obj(d),
                   *light = yyjson_mut_obj(d), *light_values = yyjson_mut_obj(d),
                   *blend_display = blend_values(d, doc, layer);
    bool ok = o && effects && blend && light && light_values && blend_display &&
              yyjson_mut_obj_add_uint(d, o, "layer", layer->id) &&
              yyjson_mut_obj_add_val(d, o, "effects", effects) &&
              yyjson_mut_obj_add_val(d, o, "blending", blend) &&
              yyjson_mut_obj_add_val(d, o, "blendingValues", blend_display) &&
              yyjson_mut_obj_add_val(d, o, "globalLight", light) &&
              yyjson_mut_obj_add_val(d, o, "globalLightValues", light_values);
    if (!ok)
        return pc_fail(err, PC_ERR_NOMEM, "Layer Style");
    number(d, light_values, "angle", doc->global_light.angle);
    number(d, light_values, "altitude", doc->global_light.altitude);
    const char *selected = "blendingOptions";
    bool found = false;
    for (size_t i = 0; i < layer->effects.nitems; i++) {
        yyjson_mut_val *entry = NULL;
        pc_status status =
            pc_engine_style_editor_entry(NULL, &layer->effects.items[i], i + 1, d, &entry, err);
        if (status != PC_OK)
            return status;
        if (!yyjson_mut_arr_add_val(effects, entry))
            return pc_fail(err, PC_ERR_NOMEM, "Layer Style effects");
        const char *kind = pc_engine_effect_kind_of(&layer->effects.items[i]);
        if (select && !found && !strcmp(select, kind)) {
            selected = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "id"));
            (void)yyjson_mut_obj_remove_str(entry, "on");
            boolean(d, entry, "on", true);
            found = true;
        }
    }
    if (select && !found && strcmp(select, "blendingOptions")) {
        if (layer->effects.nitems >= 256)
            return pc_fail(err, PC_ERR_LIMIT, "at most 256 effects");
        yyjson_mut_val *entry = NULL;
        pc_status status =
            pc_engine_style_editor_new_entry(s, select, layer->effects.nitems + 1, d, &entry, err);
        if (status != PC_OK)
            return status;
        if (!yyjson_mut_arr_add_val(effects, entry))
            return pc_fail(err, PC_ERR_NOMEM, "Layer Style effects");
        selected = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "id"));
    }
    boolean(d, o, "enabled", layer->effects.enabled);
    string(d, o, "selected", selected);
    *out = o;
    return PC_OK;
}
pc_status pc_engine_style_editor_initial(const pc_engine_session *s, const char *select, yyjson_mut_doc *doc,
                                         yyjson_mut_val **out, pc_error *err)
{
    pc_cmyk_space *space = pc_engine_doc_cmyk_space(s), *prior = space ? pc_cmyk_space_enter(space) : NULL;
    pc_status status = initial_inner(s, select, doc, out, err);
    if (space) {
        pc_cmyk_space_leave(prior);
        pc_cmyk_space_release(space);
    }
    return status;
}
typedef struct style_apply {
    yyjson_val *params;
    pc_layer_id layer;
} style_apply;
static pc_status execute(pc_engine_session *s, const char *command, yyjson_val *p, pc_layer_id layer,
                         pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = d ? yyjson_val_mut_copy(d, p) : NULL;
    pc_buf json = PC_BUF_INIT;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status =
        params && yyjson_mut_obj_put(params, yyjson_mut_str(d, "layer"), yyjson_mut_uint(d, layer))
            ? pc_json_write(params, 0, &json, err)
            : pc_fail(err, PC_ERR_NOMEM, "style parameters");
    if (status == PC_OK)
        status = pc_engine_session_execute_json(s, command, pc_buf_cstr(&json), &out, err);
    pc_buf_fini(&json);
    pc_json_value_fini(&out);
    yyjson_mut_doc_free(d);
    return status;
}
static pc_status set_enabled(pc_document *doc, bool *has_active, pc_layer_id *active, void *context,
                             pc_error *err)
{
    (void)has_active;
    (void)active;
    style_apply *c = context;
    pc_layer *layer = pc_document_layer_mut(doc, c->layer);
    if (!layer)
        return pc_engine_err_no_layer(err, c->layer);
    layer->effects.enabled = yyjson_get_bool(pc_json_get(c->params, "enabled"));
    return PC_OK;
}
static pc_status apply(pc_engine_session *s, void *context, pc_error *err)
{
    style_apply *c = context;
    yyjson_val *p = c->params, *blend = pc_json_get(p, "blending"), *light = pc_json_get(p, "globalLight");
    pc_status status = PC_OK;
    if (blend && yyjson_obj_size(blend))
        status = execute(s, "layer.layerStyle.blendingOptions", blend, c->layer, err);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *request = d ? yyjson_mut_obj(d) : NULL, *effects = d ? yyjson_mut_arr(d) : NULL;
    bool ok = request && effects && yyjson_mut_obj_add_uint(d, request, "layer", c->layer) &&
              yyjson_mut_obj_add_val(d, request, "effects", effects);
    yyjson_val *entries = pc_json_get(p, "effects"), *entry;
    size_t index, max;
    yyjson_arr_foreach(entries, index, max, entry)
    {
        const char *kind = pc_json_as_str(pc_json_get(entry, "kind"), NULL);
        yyjson_val *params = pc_json_get(entry, "params"), *on = pc_json_get(entry, "on"),
                   *snapshot = pc_json_get(entry, "fx");
        if (!kind || !yyjson_is_obj(entry) || (params && !yyjson_is_obj(params)) ||
            (on && !yyjson_is_bool(on))) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid Layer Style effect");
            break;
        }
        yyjson_mut_val *e = d ? yyjson_mut_obj(d) : NULL,
                       *attrs = d ? (params ? yyjson_val_mut_copy(d, params) : yyjson_mut_obj(d)) : NULL;
        ok = ok && e && attrs && yyjson_mut_obj_add_strcpy(d, e, "kind", kind) &&
             yyjson_mut_obj_add_val(d, e, "params", attrs) &&
             (!on || yyjson_mut_obj_add_bool(d, attrs, "enabled", yyjson_get_bool(on))) &&
             (!snapshot || yyjson_mut_obj_add_val(d, e, "fx", yyjson_val_mut_copy(d, snapshot))) &&
             yyjson_mut_arr_add_val(effects, e);
    }
    pc_buf json = PC_BUF_INIT;
    pc_json_value out = PC_JSON_VALUE_INIT;
    if (status == PC_OK)
        status =
            ok ? pc_json_write(request, 0, &json, err) : pc_fail(err, PC_ERR_NOMEM, "Layer Style effects");
    if (status == PC_OK)
        status = pc_engine_session_execute_json(s, "layer.layerStyle.replace", pc_buf_cstr(&json), &out, err);
    if (status == PC_OK && pc_json_get(p, "enabled"))
        status = pc_engine_session_edit(s, "Layer Style", set_enabled, c, err);
    if (status == PC_OK && light && yyjson_obj_size(light))
        status = execute(s, "layer.layerStyle.globalLight", light, c->layer, err);
    pc_buf_fini(&json);
    pc_json_value_fini(&out);
    yyjson_mut_doc_free(d);
    return status;
}
static pc_status run(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_val *effects = pc_json_get(p, "effects"), *blend = pc_json_get(p, "blending"),
               *light = pc_json_get(p, "globalLight");
    yyjson_val *enabled = pc_json_get(p, "enabled");
    if ((enabled && !yyjson_is_bool(enabled)) || !yyjson_is_arr(effects) || yyjson_arr_size(effects) > 256 ||
        (blend && !yyjson_is_obj(blend)) || (light && !yyjson_is_obj(light)))
        return pc_fail(err, PC_ERR_INVALID, "invalid Layer Style groups");
    style_apply context = {.params = p};
    pc_status status = pc_engine_layer_param(s, p, &context.layer, err);
    if (status != PC_OK)
        return status;
    size_t journal = s->njournal;
    status = pc_engine_compound(s, "Layer Style", apply, &context, err);
    for (size_t i = journal; i < s->njournal; i++) {
        free(s->journal[i].id);
        pc_json_value_fini(&s->journal[i].params);
    }
    s->njournal = journal;
    return status == PC_OK ? pc_engine_result_null(out, err) : status;
}
static pc_status defaults_enabled(const pc_engine_session *s, pc_error *err)
{
    (void)s;
    (void)err;
    return PC_OK;
}
static pc_status defaults_kind(yyjson_val *p, const char **kind, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "kind");
    *kind = yyjson_get_str(v);
    pc_effect fx = {0};
    if (!*kind || strlen(*kind) != yyjson_get_len(v) || !pc_engine_effect_from_params(*kind, NULL, &fx))
        return pc_fail(err, PC_ERR_INVALID, "unknown Layer Style kind");
    pc_effect_fini(&fx);
    return PC_OK;
}
static pc_status make_default(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *kind = NULL;
    pc_status status = defaults_kind(p, &kind, err);
    yyjson_val *params = pc_json_get(p, "params");
    if (status != PC_OK)
        return status;
    if (!yyjson_is_obj(params))
        return pc_fail(err, PC_ERR_INVALID, "Layer Style defaults need an object");
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *old = pc_json_value_root(&s->presets.layer_defaults),
                   *root = d ? (yyjson_mut_is_obj(old) ? yyjson_mut_val_mut_copy(d, old) : yyjson_mut_obj(d))
                             : NULL,
                   *key = d ? yyjson_mut_strcpy(d, kind) : NULL,
                   *value = d ? yyjson_val_mut_copy(d, params) : NULL;
    pc_json_value next = PC_JSON_VALUE_INIT;
    if (!root || !key || !value || !yyjson_mut_obj_put(root, key, value))
        status = pc_fail(err, PC_ERR_NOMEM, "Layer Style defaults");
    else
        status = pc_json_value_from_mut(root, &next, err);
    if (status == PC_OK) {
        pc_json_value_fini(&s->presets.layer_defaults);
        s->presets.layer_defaults = next;
        pc_engine_presets_changed(s);
        status = pc_engine_result_null(out, err);
    }
    yyjson_mut_doc_free(d);
    return status;
}
static pc_status default_for(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *kind = NULL;
    pc_status status = defaults_kind(p, &kind, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL,
                   *stored = yyjson_mut_obj_get(pc_json_value_root(&s->presets.layer_defaults), kind),
                   *params = NULL;
    bool user = yyjson_mut_is_obj(stored);
    if (d && user)
        params = yyjson_mut_val_mut_copy(d, stored);
    else if (d)
        (void)pc_engine_effect_defaults(kind, d, &params);
    if (!root || !params || !yyjson_mut_obj_add_val(d, root, "params", params) ||
        !yyjson_mut_obj_add_bool(d, root, "user", user))
        status = pc_fail(err, PC_ERR_NOMEM, "Layer Style defaults");
    else
        status = pc_json_value_from_mut(root, out, err);
    yyjson_mut_doc_free(d);
    return status;
}
const pc_engine_command *pc_engine_style_editor_specs(size_t *count)
{
    static const pc_engine_command commands[] = {
        {"layer.layerStyle.edit", "Edit Layer Style", NULL, 0, NULL,
         "{\"layer\":id?,\"effects\":json,\"blending\":json,\"globalLight\":json}", pc_engine_has_layer, run,
         true},
        {"layer.layerStyle.makeDefault", "Make Default Layer Effect", NULL, 0, NULL,
         "{\"kind\":str,\"params\":json}", defaults_enabled, make_default, true},
        {"layer.layerStyle.defaultFor", "Layer Effect Defaults", NULL, 0, NULL, "{\"kind\":str}",
         defaults_enabled, default_for, false}};
    *count = sizeof commands / sizeof commands[0];
    return commands;
}
