/* Style presets data model and built-in style groups (port of crates/engine/src/presets/styles.rs
 * data, E2b). A style is a list of layer effects plus optional blending options. The built-in
 * designs are our own, built from the same effect params as layer.layerStyle.*. Never crashes. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine_internal.h"
#include "layer_style.h"
#include "presets.h"

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

void pc_style_preset_fini(pc_style_preset *s)
{
    if (s == NULL) {
        return;
    }
    free(s->name);
    for (size_t i = 0; i < s->neffects; i++) {
        pc_effect_fini(&s->effects[i]);
    }
    free(s->effects);
    s->name = NULL;
    s->effects = NULL;
    s->neffects = 0;
}

pc_status pc_style_preset_clone(const pc_style_preset *src, pc_style_preset *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->name = dup_str(src->name);
    if (out->name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "style");
    }
    out->has_blend = src->has_blend;
    out->blend = src->blend;
    out->has_fill_opacity = src->has_fill_opacity;
    out->fill_opacity = src->fill_opacity;
    if (src->neffects > 0) {
        out->effects = calloc(src->neffects, sizeof *out->effects);
        if (out->effects == NULL) {
            pc_style_preset_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "style");
        }
        for (size_t i = 0; i < src->neffects; i++) {
            if (pc_effect_clone(&src->effects[i], &out->effects[i], err) != PC_OK) {
                out->neffects = i;
                pc_style_preset_fini(out);
                return PC_ERR_NOMEM;
            }
        }
        out->neffects = src->neffects;
    }
    return PC_OK;
}

static const char *st_get_name(const void *item)
{
    return ((const pc_style_preset *)item)->name;
}
static bool st_set_name(void *item, const char *name)
{
    char *nm = dup_str(name);
    if (nm == NULL) {
        return false;
    }
    pc_style_preset *s = item;
    free(s->name);
    s->name = nm;
    return true;
}
static void st_fini(void *item)
{
    pc_style_preset_fini(item);
}
static pc_status st_clone(const void *src, void *dst, pc_error *err)
{
    return pc_style_preset_clone(src, dst, err);
}

const pc_preset_item_ops *pc_style_preset_ops(void)
{
    static const pc_preset_item_ops ops = {sizeof(pc_style_preset), st_get_name, st_set_name, st_fini, st_clone};
    return &ops;
}

/* ---- JSON --------------------------------------------------------------------------------- */

yyjson_mut_val *pc_style_preset_to_json(yyjson_mut_doc *d, const pc_style_preset *s)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "name", s->name == NULL ? "" : s->name);
    yyjson_mut_val *fx = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->neffects; i++) {
        yyjson_mut_arr_add_strcpy(d, fx, pc_effect_label(&s->effects[i]));
    }
    yyjson_mut_obj_add_val(d, o, "effects", fx);
    if (s->has_blend) {
        yyjson_mut_obj_add_strcpy(d, o, "blend", pc_blend_label(s->blend));
    } else {
        yyjson_mut_obj_add_val(d, o, "blend", yyjson_mut_null(d));
    }
    if (s->has_fill_opacity) {
        yyjson_mut_obj_add_real(d, o, "fillOpacity", (double)lroundf(s->fill_opacity * 100.0f));
    } else {
        yyjson_mut_obj_add_val(d, o, "fillOpacity", yyjson_mut_null(d));
    }
    return o;
}

/* ---- built-in data ------------------------------------------------------------------------ */

typedef struct {
    const char *kind;
    const char *json; /* effect params, or NULL to end a style's list */
} fx_spec;

/* Build a style from its effect specs; has_fill/fill set the saved fill opacity (0..1). */
static pc_status make_style(pc_preset_groups *g, size_t gi, const char *name, const fx_spec *specs,
                            bool has_fill, float fill, pc_error *err)
{
    pc_style_preset sp;
    memset(&sp, 0, sizeof sp);
    sp.name = dup_str(name);
    if (sp.name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "style");
    }
    sp.has_fill_opacity = has_fill;
    sp.fill_opacity = fill;
    size_t cnt = 0;
    while (specs != NULL && specs[cnt].kind != NULL) {
        cnt++;
    }
    if (cnt > 0) {
        sp.effects = calloc(cnt, sizeof *sp.effects);
        if (sp.effects == NULL) {
            pc_style_preset_fini(&sp);
            return pc_fail(err, PC_ERR_NOMEM, "style");
        }
        for (size_t i = 0; i < cnt; i++) {
            yyjson_doc *jd = NULL;
            yyjson_val *root = NULL;
            if (specs[i].json != NULL) {
                pc_error pe = {0};
                if (pc_json_parse(specs[i].json, strlen(specs[i].json), &jd, &pe) == PC_OK) {
                    root = yyjson_doc_get_root(jd);
                }
            }
            pc_effect fx;
            bool ok = pc_engine_effect_from_params(specs[i].kind, root, &fx);
            if (jd != NULL) {
                yyjson_doc_free(jd);
            }
            if (!ok) {
                pc_style_preset_fini(&sp);
                return pc_fail(err, PC_ERR_INVALID, "style effect");
            }
            sp.effects[sp.neffects++] = fx;
        }
    }
    if (pc_preset_group_push(g, gi, &sp, pc_style_preset_ops(), err) != PC_OK) {
        pc_style_preset_fini(&sp);
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

#define S1(k, j) ((const fx_spec[]){{k, j}, {NULL, NULL}})
#define S2(k1, j1, k2, j2) ((const fx_spec[]){{k1, j1}, {k2, j2}, {NULL, NULL}})
#define S3(k1, j1, k2, j2, k3, j3) ((const fx_spec[]){{k1, j1}, {k2, j2}, {k3, j3}, {NULL, NULL}})
#define S4(k1, j1, k2, j2, k3, j3, k4, j4)                                                  \
    ((const fx_spec[]){{k1, j1}, {k2, j2}, {k3, j3}, {k4, j4}, {NULL, NULL}})

pc_status pc_styles_builtin(pc_preset_groups *out, pc_error *err)
{
    pc_error e = {0};
    pc_preset_groups_init(out);
#define ADD(name, specs) \
    do { if (make_style(out, out->n - 1, name, specs, false, 0.0f, &e) != PC_OK) goto oom; } while (0)

    if (pc_preset_groups_push(out, "Basics", &e) == NULL) {
        goto oom;
    }
    if (make_style(out, 0, "Default Style (None)", NULL, false, 0.0f, &e) != PC_OK) {
        goto oom;
    }
    ADD("Drop Shadow", S1("dropShadow", "{\"distance\":6,\"size\":8,\"opacity\":60}"));
    ADD("Soft Shadow", S1("dropShadow", "{\"distance\":2,\"size\":16,\"opacity\":45}"));
    ADD("Black Stroke", S1("stroke", "{\"size\":3,\"color\":\"#000000\",\"position\":\"outside\"}"));
    ADD("White Stroke", S1("stroke", "{\"size\":3,\"color\":\"#ffffff\",\"position\":\"outside\"}"));
    ADD("Inner Shadow", S1("innerShadow", "{\"distance\":4,\"size\":6,\"opacity\":55}"));
    ADD("Outer Glow", S1("outerGlow", "{\"size\":14,\"color\":\"#ffe9a8\",\"opacity\":80}"));
    ADD("Emboss", S1("bevelEmboss", "{\"style\":\"emboss\",\"size\":4,\"depth\":120}"));

    if (pc_preset_groups_push(out, "Text Effects", &e) == NULL) {
        goto oom;
    }
    ADD("Neon Blue",
        S3("colorOverlay", "{\"color\":\"#d8f4ff\"}", "innerGlow",
           "{\"color\":\"#3fb6ff\",\"size\":6,\"opacity\":90,\"blend\":\"normal\"}", "outerGlow",
           "{\"color\":\"#1e90ff\",\"size\":22,\"opacity\":90}"));
    ADD("Neon Pink",
        S3("colorOverlay", "{\"color\":\"#ffe1f3\"}", "innerGlow",
           "{\"color\":\"#ff4fb4\",\"size\":6,\"opacity\":90,\"blend\":\"normal\"}", "outerGlow",
           "{\"color\":\"#ff2d95\",\"size\":22,\"opacity\":90}"));
    ADD("Chrome",
        S3("gradientOverlay", "{\"from\":\"#3a3f47\",\"to\":\"#f4f6f8\",\"angle\":90}", "bevelEmboss",
           "{\"style\":\"inner\",\"size\":6,\"depth\":250,\"soften\":1}", "stroke",
           "{\"size\":1,\"color\":\"#2a2d33\",\"position\":\"outside\"}"));
    ADD("Gold",
        S3("gradientOverlay", "{\"from\":\"#8a5a12\",\"to\":\"#ffe08a\",\"angle\":90}", "bevelEmboss",
           "{\"style\":\"inner\",\"size\":5,\"depth\":180}", "dropShadow",
           "{\"distance\":3,\"size\":5,\"opacity\":50}"));
    ADD("Comic Outline",
        S3("colorOverlay", "{\"color\":\"#ffd23f\"}", "stroke",
           "{\"size\":4,\"color\":\"#111111\",\"position\":\"outside\"}", "dropShadow",
           "{\"distance\":6,\"size\":0,\"opacity\":100,\"angle\":135}"));
    ADD("Letterpress",
        S3("colorOverlay", "{\"color\":\"#5b6168\"}", "innerShadow",
           "{\"distance\":2,\"size\":2,\"opacity\":70}", "dropShadow",
           "{\"distance\":1,\"size\":0,\"color\":\"#ffffff\",\"opacity\":60,\"blend\":\"screen\",\"angle\":90}"));
    if (make_style(out, 1, "Hollow",
                   S1("stroke", "{\"size\":2,\"color\":\"#222222\",\"position\":\"inside\"}"), true, 0.0f,
                   &e) != PC_OK) {
        goto oom;
    }

    if (pc_preset_groups_push(out, "Buttons", &e) == NULL) {
        goto oom;
    }
    ADD("Glass",
        S4("gradientOverlay", "{\"from\":\"#2b6cb0\",\"to\":\"#90cdf4\",\"angle\":90}", "innerGlow",
           "{\"color\":\"#ffffff\",\"size\":6,\"opacity\":45,\"blend\":\"screen\"}", "bevelEmboss",
           "{\"style\":\"inner\",\"size\":8,\"depth\":100,\"soften\":4}", "dropShadow",
           "{\"distance\":3,\"size\":8,\"opacity\":40}"));
    ADD("Gel Green",
        S3("gradientOverlay", "{\"from\":\"#1f7a3a\",\"to\":\"#7be495\",\"angle\":90}", "innerShadow",
           "{\"distance\":2,\"size\":5,\"opacity\":35}", "bevelEmboss",
           "{\"style\":\"inner\",\"size\":10,\"depth\":150,\"soften\":6}"));
    ADD("Pressed",
        S3("colorOverlay", "{\"color\":\"#d7dbe0\"}", "innerShadow",
           "{\"distance\":3,\"size\":6,\"opacity\":60}", "stroke",
           "{\"size\":1,\"color\":\"#9aa1a9\",\"position\":\"inside\"}"));
    ADD("Pill Red",
        S3("gradientOverlay", "{\"from\":\"#9b1c1c\",\"to\":\"#f87171\",\"angle\":90}", "bevelEmboss",
           "{\"style\":\"pillow\",\"size\":6,\"depth\":120}", "dropShadow",
           "{\"distance\":2,\"size\":4,\"opacity\":45}"));
    ADD("Flat Shadow",
        S2("colorOverlay", "{\"color\":\"#3b82f6\"}", "dropShadow",
           "{\"distance\":5,\"size\":0,\"opacity\":35,\"angle\":90}"));
    ADD("Satin Plum",
        S3("colorOverlay", "{\"color\":\"#7e3a8c\"}", "satin",
           "{\"color\":\"#1b0420\",\"opacity\":55,\"size\":18,\"distance\":14}", "bevelEmboss",
           "{\"style\":\"inner\",\"size\":4,\"depth\":80}"));
#undef ADD
    return PC_OK;
oom:
    pc_preset_groups_fini(out, pc_style_preset_ops());
    pc_preset_groups_init(out);
    if (err != NULL) {
        *err = e;
    }
    return PC_ERR_NOMEM;
}
