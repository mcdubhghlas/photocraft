/* Style presets data model and built-in style groups (port of crates/engine/src/presets/styles.rs
 * data, E2b). A style is a list of layer effects plus optional blending options. The built-in
 * designs are our own, built from the same effect params as layer.layerStyle.*. Never crashes. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "doc/doc.h"
#include "doc/effects.h"
#include "doc/pattern.h"

#include "brush_cmds.h"   /* pc_engine_blend_from_str */
#include "engine_internal.h"
#include "extra_cmds.h"   /* pc_engine_background_to_layer */
#include "layer_cmds.h"   /* pc_engine_layer_param, pc_engine_selected */
#include "layer_style.h"  /* pc_engine_effect_from_params */
#include "pattern_cmds.h" /* pc_engine_pattern_resolve, pc_engine_pattern_ensure_in_doc */
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

/* ================================ commands (style.presets.*) ============================== *
 * Port of crates/engine/src/presets/styles.rs commands. A style is a list of layer effects plus,
 * optionally, saved blending options. Applying one replaces the selected layers' effects (⇧-click
 * adds: "add": true). Never crashes on hostile params. */

static const pc_preset_item_ops *S_OPS(void)
{
    return pc_style_preset_ops();
}

/* A trimmed non-empty string param copied into out[cap]; false when absent/empty. */
static bool str_copy(yyjson_val *p, const char *key, char *out, size_t cap)
{
    size_t len = 0;
    const char *s = pc_json_as_str(pc_json_get(p, key), &len);
    if (s == NULL) {
        return false;
    }
    size_t a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) {
        a++;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) {
        b--;
    }
    if (b == a) {
        return false;
    }
    size_t n = b - a;
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, s + a, n);
    out[n] = '\0';
    return true;
}

static void effects_free_arr(pc_effect *a, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_effect_fini(&a[i]);
    }
    free(a);
}

static pc_status effects_clone_arr(const pc_effect *src, size_t n, pc_effect **out, pc_error *err)
{
    *out = NULL;
    if (n == 0) {
        return PC_OK;
    }
    pc_effect *a = calloc(n, sizeof *a);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "effects");
    }
    for (size_t i = 0; i < n; i++) {
        if (pc_effect_clone(&src[i], &a[i], err) != PC_OK) {
            effects_free_arr(a, i);
            return PC_ERR_NOMEM;
        }
    }
    *out = a;
    return PC_OK;
}

/* ---- list ---- */

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *groups = yyjson_mut_arr(d);
    const pc_preset_item_ops *ops = S_OPS();
    for (size_t i = 0; i < s->presets.styles.n; i++) {
        const pc_preset_group *g = &s->presets.styles.v[i];
        yyjson_mut_val *go = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, go, "name", g->name);
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        for (size_t j = 0; j < g->n; j++) {
            yyjson_mut_arr_append(
                arr, pc_style_preset_to_json(d, (const pc_style_preset *)((char *)g->items + j * ops->size)));
        }
        yyjson_mut_obj_add_val(d, go, "presets", arr);
        yyjson_mut_arr_append(groups, go);
    }
    yyjson_mut_obj_add_val(d, root, "groups", groups);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- apply ---- */

/* Target layers: "layers":[ids], "layer":id, else every selected layer. Owned array in *out. */
static pc_status apply_targets(pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n,
                               pc_error *err)
{
    *out = NULL;
    *n = 0;
    yyjson_val *arr = pc_json_get(p, "layers");
    if (arr != NULL && yyjson_is_arr(arr)) {
        size_t cnt = yyjson_arr_size(arr);
        pc_layer_id *ids = cnt > 0 ? calloc(cnt, sizeof *ids) : NULL;
        if (cnt > 0 && ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "layers");
        }
        size_t k = 0;
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(arr, idx, max, v) {
            uint64_t id;
            if (pc_json_as_u64(v, &id)) {
                ids[k++] = (pc_layer_id)id;
            }
        }
        *out = ids;
        *n = k;
        return PC_OK;
    }
    yyjson_val *lv = pc_json_get(p, "layer");
    uint64_t one;
    if (lv != NULL && pc_json_as_u64(lv, &one)) {
        pc_layer_id *ids = calloc(1, sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "layer");
        }
        ids[0] = (pc_layer_id)one;
        *out = ids;
        *n = 1;
        return PC_OK;
    }
    return pc_engine_selected(s, out, n, err);
}

struct apply_ctx {
    const pc_effect *effects; /* resolved (pattern names fixed), RGB colours; borrowed */
    size_t neffects;
    const pc_pattern *pats; /* patterns to copy into the doc; borrowed */
    size_t npats;
    const pc_layer_id *ids;
    size_t nids;
    bool add;
    bool has_blend;
    pc_blend_mode blend;
    bool has_fill;
    float fill;
};

static pc_status apply_cb(pc_document *doc, bool *ha, pc_layer_id *al, void *v, pc_error *err)
{
    (void)ha;
    (void)al;
    struct apply_ctx *c = v;
    for (size_t i = 0; i < c->npats; i++) {
        if (pc_engine_pattern_ensure_in_doc(doc, &c->pats[i], err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    /* A preset's colours are RGB; the layer's are in the document's mode. Convert once. */
    pc_color_mode mode = doc->mode;
    pc_effect *conv = NULL;
    if (effects_clone_arr(c->effects, c->neffects, &conv, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < c->neffects; i++) {
        pc_effect_in_mode(&conv[i], mode);
    }
    pc_status rc = PC_OK;
    for (size_t t = 0; t < c->nids; t++) {
        pc_layer_id id = c->ids[t];
        /* Styles can't sit on the Background: Photoshop turns it into a normal layer first. */
        pc_engine_background_to_layer(doc, id);
        pc_layer *l = pc_document_layer_mut(doc, id);
        if (l == NULL) {
            rc = pc_engine_err_no_layer(err, id);
            break;
        }
        pc_effect *copy = NULL;
        if (effects_clone_arr(conv, c->neffects, &copy, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            break;
        }
        if (c->add) {
            size_t total = l->effects.nitems + c->neffects;
            pc_effect *merged = total > 0 ? realloc(l->effects.items, total * sizeof *merged) : l->effects.items;
            if (total > 0 && merged == NULL) {
                effects_free_arr(copy, c->neffects);
                rc = pc_fail(err, PC_ERR_NOMEM, "effects");
                break;
            }
            l->effects.items = merged;
            for (size_t i = 0; i < c->neffects; i++) {
                l->effects.items[l->effects.nitems + i] = copy[i]; /* move */
            }
            l->effects.nitems = total;
            free(copy); /* the pc_effects were moved, free only the array */
        } else {
            for (size_t i = 0; i < l->effects.nitems; i++) {
                pc_effect_fini(&l->effects.items[i]);
            }
            free(l->effects.items);
            l->effects.items = copy;
            l->effects.nitems = c->neffects;
            pc_blob_release(l->effects.psd_raw);
            l->effects.psd_raw = NULL;
        }
        l->effects.enabled = true;
        if (!c->add || c->has_blend) {
            if (c->has_blend) {
                l->blend = c->blend;
            }
            if (c->has_fill) {
                l->fill_opacity = c->fill;
            } else if (!c->add) {
                l->fill_opacity = 1.0f;
            }
        }
    }
    effects_free_arr(conv, c->neffects);
    return rc;
}

static pc_status run_apply(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "style.presets.apply";
    char name[512], group[512];
    if (!str_copy(p, "preset", name, sizeof name)) {
        return pc_engine_err_bad_params(err, CMD, "missing `preset`");
    }
    const pc_preset_item_ops *ops = S_OPS();
    size_t gi, ii;
    const char *grp = str_copy(p, "group", group, sizeof group) ? group : NULL;
    if (!pc_preset_find(&s->presets.styles, name, grp, ops, &gi, &ii)) {
        return pc_engine_err_bad_params(err, CMD, "no style \"%s\" (see style.presets.list)", name);
    }
    const pc_style_preset *style = (const pc_style_preset *)((char *)s->presets.styles.v[gi].items + ii * ops->size);
    bool add = false;
    (void)pc_json_as_bool(pc_json_get(p, "add"), &add);

    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (apply_targets(s, p, &ids, &nids, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (nids == 0) {
        free(ids);
        return pc_engine_err_bad_params(err, CMD, "no target layers");
    }

    /* Resolve the style's effects: a pattern overlay names a pattern we copy into the document. */
    pc_effect *effects = NULL;
    size_t neffects = 0;
    pc_pattern *pats = NULL;
    size_t npats = 0;
    pc_status rc = PC_OK;
    if (style->neffects > 0) {
        effects = calloc(style->neffects, sizeof *effects);
        if (effects == NULL) {
            free(ids);
            return pc_fail(err, PC_ERR_NOMEM, "effects");
        }
        pats = calloc(style->neffects, sizeof *pats);
        if (pats == NULL) {
            free(effects);
            free(ids);
            return pc_fail(err, PC_ERR_NOMEM, "effects");
        }
    }
    for (size_t i = 0; i < style->neffects; i++) {
        if (pc_effect_clone(&style->effects[i], &effects[neffects], err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            goto done;
        }
        pc_effect *e = &effects[neffects];
        neffects++;
        if (e->kind == PC_FX_PATTERN_OVERLAY) {
            const char *key = pc_str_c(&e->u.pattern_overlay.id);
            size_t klen = e->u.pattern_overlay.id.len;
            if (klen == 0) {
                key = pc_str_c(&e->u.pattern_overlay.name);
                klen = e->u.pattern_overlay.name.len;
            }
            pc_pattern pat;
            bool found = false;
            if (pc_engine_pattern_resolve(s, key, klen, &pat, &found, err) != PC_OK) {
                rc = PC_ERR_NOMEM;
                goto done;
            }
            if (!found) {
                rc = pc_engine_err_bad_params(err, CMD, "no pattern \"%s\" (see pattern.list)", key);
                goto done;
            }
            /* Fix the effect's name/id to the resolved pattern (free the cloned-in values first),
             * then keep the pattern to copy. */
            pc_str_fini(&e->u.pattern_overlay.name);
            pc_str_fini(&e->u.pattern_overlay.id);
            if (pc_str_clone(&pat.name, &e->u.pattern_overlay.name, err) != PC_OK ||
                pc_str_clone(&pat.id, &e->u.pattern_overlay.id, err) != PC_OK) {
                pc_pattern_fini(&pat);
                rc = PC_ERR_NOMEM;
                goto done;
            }
            pats[npats++] = pat; /* move */
        }
    }

    struct apply_ctx ctx = {
        .effects = effects, .neffects = neffects, .pats = pats, .npats = npats,
        .ids = ids, .nids = nids, .add = add,
        .has_blend = style->has_blend, .blend = style->blend,
        .has_fill = style->has_fill_opacity, .fill = style->fill_opacity,
    };
    char label[128];
    (void)snprintf(label, sizeof label, "Apply Style: %s", style->name != NULL ? style->name : "");
    rc = pc_engine_session_edit(s, label, apply_cb, &ctx, err);
    if (rc != PC_OK) {
        goto done;
    }

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto done;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *la = yyjson_mut_arr(d);
    for (size_t i = 0; i < nids; i++) {
        yyjson_mut_arr_add_uint(d, la, ids[i]);
    }
    yyjson_mut_obj_add_val(d, root, "layers", la);
    yyjson_mut_obj_add_strcpy(d, root, "style", style->name != NULL ? style->name : "");
    yyjson_mut_doc_set_root(d, root);
done:
    effects_free_arr(effects, neffects);
    for (size_t i = 0; i < npats; i++) {
        pc_pattern_fini(&pats[i]);
    }
    free(pats);
    free(ids);
    return rc;
}

/* ---- new ---- */

static pc_status run_new(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "style.presets.new";
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    bool with_fx = true, with_blend = true;
    (void)pc_json_as_bool(pc_json_get(p, "includeEffects"), &with_fx);
    (void)pc_json_as_bool(pc_json_get(p, "includeBlending"), &with_blend);
    if (!with_fx && !with_blend) {
        return pc_engine_err_bad_params(err, CMD, "include effects and/or blending options");
    }

    pc_style_preset sp;
    memset(&sp, 0, sizeof sp);
    /* An explicit effect list ([[kind, params], ...]) overrides the layer's own effects. */
    yyjson_val *listed = pc_json_get(p, "effects");
    if (listed != NULL && yyjson_is_arr(listed)) {
        size_t cnt = yyjson_arr_size(listed);
        if (cnt > 0) {
            sp.effects = calloc(cnt, sizeof *sp.effects);
            if (sp.effects == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "style");
            }
        }
        size_t idx, max;
        yyjson_val *entry;
        yyjson_arr_foreach(listed, idx, max, entry) {
            size_t klen = 0;
            const char *kind = pc_json_as_str(yyjson_arr_get(entry, 0), &klen);
            if (kind == NULL) {
                pc_style_preset_fini(&sp);
                return pc_engine_err_bad_params(err, CMD, "each effect is [kind, params]");
            }
            char kbuf[64];
            if (klen >= sizeof kbuf) {
                pc_style_preset_fini(&sp);
                return pc_engine_err_bad_params(err, CMD, "unknown effect");
            }
            memcpy(kbuf, kind, klen);
            kbuf[klen] = '\0';
            yyjson_val *prm = yyjson_arr_get(entry, 1);
            if (!pc_engine_effect_from_params(kbuf, prm, &sp.effects[sp.neffects])) {
                pc_style_preset_fini(&sp);
                return pc_engine_err_bad_params(err, CMD, "unknown effect %s", kbuf);
            }
            sp.neffects++;
        }
    } else if (with_fx) {
        if (effects_clone_arr(l->effects.items, l->effects.nitems, &sp.effects, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        sp.neffects = l->effects.nitems;
    }

    /* Blending options: an explicit override, else the layer's own (when includeBlending). */
    char blendbuf[32];
    if (str_copy(p, "blend", blendbuf, sizeof blendbuf)) {
        pc_blend_mode b;
        if (pc_engine_blend_from_str(blendbuf, strlen(blendbuf), &b)) {
            sp.has_blend = true;
            sp.blend = b;
        }
    } else if (with_blend) {
        sp.has_blend = true;
        sp.blend = l->blend;
    }
    double fo;
    if (pc_json_as_f64(pc_json_get(p, "fillOpacity"), &fo)) {
        double v = fo / 100.0;
        if (v < 0.0) {
            v = 0.0;
        }
        if (v > 1.0) {
            v = 1.0;
        }
        sp.has_fill_opacity = true;
        sp.fill_opacity = (float)v;
    } else if (with_blend) {
        sp.has_fill_opacity = true;
        sp.fill_opacity = l->fill_opacity;
    }

    char nmbuf[512];
    const char *base = str_copy(p, "name", nmbuf, sizeof nmbuf) ? nmbuf : "Style";
    char *uniq = pc_preset_unique_name(&s->presets.styles, base, S_OPS());
    if (uniq == NULL) {
        pc_style_preset_fini(&sp);
        return pc_fail(err, PC_ERR_NOMEM, "style");
    }
    free(sp.name);
    sp.name = uniq;
    char grpbuf[512];
    const char *group = str_copy(p, "group", grpbuf, sizeof grpbuf) ? grpbuf : NULL;
    size_t gi = pc_preset_group_index(&s->presets.styles, group, err);
    if (gi == SIZE_MAX) {
        pc_style_preset_fini(&sp);
        return PC_ERR_NOMEM;
    }
    char *gname = dup_str(s->presets.styles.v[gi].name);
    size_t effcount = sp.neffects;
    if (pc_preset_group_push(&s->presets.styles, gi, &sp, S_OPS(), err) != PC_OK) {
        pc_style_preset_fini(&sp);
        free(gname);
        return PC_ERR_NOMEM;
    }
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(gname);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", uniq);
    yyjson_mut_obj_add_strcpy(d, root, "group", gname != NULL ? gname : "");
    yyjson_mut_obj_add_uint(d, root, "effects", effcount);
    yyjson_mut_doc_set_root(d, root);
    free(gname);
    return PC_OK;
}

/* ---- edit ---- */

static pc_status run_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "style.presets.edit";
    size_t al = 0;
    const char *action = pc_json_as_str(pc_json_get(p, "action"), &al);
    if (action == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing `action`");
    }
    char act[32];
    if (al >= sizeof act) {
        return pc_engine_err_bad_params(err, CMD, "unknown action");
    }
    memcpy(act, action, al);
    act[al] = '\0';
    pc_status rc = pc_preset_edit_groups(&s->presets.styles, act, p, CMD, S_OPS(), out, err);
    if (rc == PC_OK) {
        pc_engine_presets_changed(s);
    }
    return rc;
}

/* ---- reset ---- */

static pc_status run_reset(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool append = false;
    (void)pc_json_as_bool(pc_json_get(p, "append"), &append);
    pc_preset_groups b;
    if (pc_styles_builtin(&b, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    const pc_preset_item_ops *ops = S_OPS();
    if (append) {
        for (size_t i = 0; i < b.n; i++) {
            char *nm = pc_preset_unique_group(&s->presets.styles, b.v[i].name);
            if (nm != NULL) {
                free(b.v[i].name);
                b.v[i].name = nm;
            }
            pc_preset_group *gr = pc_preset_groups_push(&s->presets.styles, b.v[i].name, err);
            if (gr == NULL) {
                pc_preset_groups_fini(&b, ops);
                return PC_ERR_NOMEM;
            }
            gr->items = b.v[i].items;
            gr->n = b.v[i].n;
            gr->cap = b.v[i].cap;
            b.v[i].items = NULL;
            b.v[i].n = b.v[i].cap = 0;
        }
        pc_preset_groups_fini(&b, ops);
    } else {
        pc_preset_groups_fini(&s->presets.styles, ops);
        s->presets.styles = b;
    }
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "groups", s->presets.styles.n);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static const pc_engine_command STYLE_SPECS[] = {
    {"style.presets.list", "Style Presets", NULL, 0, NULL,
     "{} -> {groups:[{name,presets:[{name,effects,blend,fillOpacity}]}]}", pc_engine_always, run_list, false},
    {"style.presets.apply", "Apply Style", NULL, 0, NULL,
     "{\"preset\":name,\"group\":name?,\"layers\":[ids]?|\"layer\":id? (default: the selected layers),"
     "\"add\":bool=false} -> {layers,style}",
     pc_engine_has_layer, run_apply, true},
    {"style.presets.new", "New Style\xE2\x80\xA6", NULL, 0, NULL,
     "{\"name\":str=\"Style\",\"group\":name?,\"layer\":id?,\"includeEffects\":bool=true,"
     "\"includeBlending\":bool=true,\"effects\":[[kind,params]]?,\"blend\":str?,\"fillOpacity\":0..100?}",
     pc_engine_has_layer, run_new, true},
    {"style.presets.edit", "Edit Style Presets", NULL, 0, NULL, GROUP_EDIT_PARAMS_DOC, pc_engine_always,
     run_edit, true},
    {"style.presets.reset", "Restore Default Styles", NULL, 0, NULL, "{\"append\":bool=false}",
     pc_engine_always, run_reset, true},
};

const pc_engine_command *pc_engine_style_preset_specs(size_t *n)
{
    *n = sizeof STYLE_SPECS / sizeof STYLE_SPECS[0];
    return STYLE_SPECS;
}
