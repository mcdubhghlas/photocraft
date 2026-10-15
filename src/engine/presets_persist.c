/* Preset persistence: PresetState <-> the preferences "presets" blob (port of
 * crates/engine/src/presets/mod.rs PresetState::to_json / Session::load_presets_json, E2b).
 *
 * This uses the serde representation of the preset types (distinct from the command JSON): a
 * gradient is {name, stops:[[loc, "foreground"|"background"|{rgb:[..]}]], opacity:[[loc, 0..1]]};
 * a swatch colour is {model, values} in raw 0..1 units; a group is {name, items:[..]}.
 *
 * Covered: gradients, the current gradient, pattern groups, tool presets, layer_defaults and
 * swatches. Styles and shapes (Effect/Path serde) and the File > New documents/custom_shapes sets
 * are DEFERRED to their owners (E6/E11 serde, E3 documents, E4 custom_shapes): a missing key keeps
 * its built-in default, as the reference does. A malformed blob is ignored (never refuse to
 * start). Never crashes. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine_internal.h"
#include "presets.h"

/* ---- gradient serde ----------------------------------------------------------------------- */

static yyjson_mut_val *stop_color_to(yyjson_mut_doc *d, const pc_stop_color *c)
{
    if (c->kind == PC_STOP_FOREGROUND) {
        return yyjson_mut_strcpy(d, "foreground");
    }
    if (c->kind == PC_STOP_BACKGROUND) {
        return yyjson_mut_strcpy(d, "background");
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (int i = 0; i < 3; i++) {
        yyjson_mut_arr_add_real(d, arr, (double)c->rgb[i]);
    }
    yyjson_mut_obj_add_val(d, o, "rgb", arr);
    return o;
}

static yyjson_mut_val *gradient_to(yyjson_mut_doc *d, const pc_gradient_preset *g)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "name", g->name == NULL ? "" : g->name);
    yyjson_mut_val *stops = yyjson_mut_arr(d);
    for (size_t i = 0; i < g->nstops; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, pair, (double)g->stops[i].loc);
        yyjson_mut_arr_append(pair, stop_color_to(d, &g->stops[i].color));
        yyjson_mut_arr_append(stops, pair);
    }
    yyjson_mut_obj_add_val(d, o, "stops", stops);
    yyjson_mut_val *op = yyjson_mut_arr(d);
    for (size_t i = 0; i < g->nopacity; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, pair, (double)g->opacity[i].loc);
        yyjson_mut_arr_add_real(d, pair, (double)g->opacity[i].opacity);
        yyjson_mut_arr_append(op, pair);
    }
    yyjson_mut_obj_add_val(d, o, "opacity", op);
    return o;
}

static bool stop_color_from(yyjson_val *v, pc_stop_color *out)
{
    size_t sl = 0;
    const char *s = pc_json_as_str(v, &sl);
    if (s != NULL) {
        if (sl == 10 && memcmp(s, "foreground", 10) == 0) {
            out->kind = PC_STOP_FOREGROUND;
            return true;
        }
        if (sl == 10 && memcmp(s, "background", 10) == 0) {
            out->kind = PC_STOP_BACKGROUND;
            return true;
        }
        return false;
    }
    yyjson_val *rgb = pc_json_get(v, "rgb");
    if (rgb == NULL || !yyjson_is_arr(rgb) || yyjson_arr_size(rgb) != 3) {
        return false;
    }
    out->kind = PC_STOP_RGB;
    for (int i = 0; i < 3; i++) {
        double x = 0.0;
        (void)pc_json_as_f64(yyjson_arr_get(rgb, (size_t)i), &x);
        out->rgb[i] = (float)x;
    }
    return true;
}

static bool gradient_from(yyjson_val *v, pc_gradient_preset *out)
{
    memset(out, 0, sizeof *out);
    size_t nl = 0;
    const char *nm = pc_json_as_str(pc_json_get(v, "name"), &nl);
    out->name = malloc(nl + 1);
    if (out->name == NULL) {
        return false;
    }
    memcpy(out->name, nm != NULL ? nm : "", nm != NULL ? nl : 0);
    out->name[nm != NULL ? nl : 0] = '\0';
    yyjson_val *stops = pc_json_get(v, "stops");
    if (stops == NULL || !yyjson_is_arr(stops)) {
        pc_gradient_preset_fini(out);
        return false;
    }
    size_t ns = yyjson_arr_size(stops);
    out->stops = ns > 0 ? malloc(ns * sizeof *out->stops) : NULL;
    if (ns > 0 && out->stops == NULL) {
        pc_gradient_preset_fini(out);
        return false;
    }
    size_t idx, max;
    yyjson_val *el;
    yyjson_arr_foreach(stops, idx, max, el) {
        if (!yyjson_is_arr(el) || yyjson_arr_size(el) != 2) {
            pc_gradient_preset_fini(out);
            return false;
        }
        double loc = 0.0;
        (void)pc_json_as_f64(yyjson_arr_get(el, 0), &loc);
        pc_stop_color sc;
        if (!stop_color_from(yyjson_arr_get(el, 1), &sc)) {
            pc_gradient_preset_fini(out);
            return false;
        }
        out->stops[out->nstops].loc = (float)loc;
        out->stops[out->nstops].color = sc;
        out->nstops++;
    }
    yyjson_val *op = pc_json_get(v, "opacity");
    if (op != NULL && yyjson_is_arr(op)) {
        size_t no = yyjson_arr_size(op);
        out->opacity = no > 0 ? malloc(no * sizeof *out->opacity) : NULL;
        if (no > 0 && out->opacity == NULL) {
            pc_gradient_preset_fini(out);
            return false;
        }
        yyjson_arr_foreach(op, idx, max, el) {
            if (!yyjson_is_arr(el) || yyjson_arr_size(el) != 2) {
                continue;
            }
            double loc = 0.0, o = 1.0;
            (void)pc_json_as_f64(yyjson_arr_get(el, 0), &loc);
            (void)pc_json_as_f64(yyjson_arr_get(el, 1), &o);
            out->opacity[out->nopacity].loc = (float)loc;
            out->opacity[out->nopacity].opacity = (float)o;
            out->nopacity++;
        }
    }
    return true;
}

/* ---- swatch serde ------------------------------------------------------------------------- */

static yyjson_mut_val *swatch_color_raw(yyjson_mut_doc *d, const pc_swatch_color *c)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "model", pc_swatch_model_id(c->model));
    if (c->model == PC_SWATCH_GRAY) {
        yyjson_mut_obj_add_real(d, o, "values", (double)c->v[0]);
    } else if (c->model == PC_SWATCH_BOOK) {
        yyjson_mut_val *bo = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, bo, "space", c->book_space);
        yyjson_mut_val *bv = yyjson_mut_arr(d);
        for (int i = 0; i < 4; i++) {
            yyjson_mut_arr_add_uint(d, bv, c->book_values[i]);
        }
        yyjson_mut_obj_add_val(d, bo, "values", bv);
        yyjson_mut_obj_add_val(d, o, "values", bo);
    } else {
        int n = c->model == PC_SWATCH_CMYK ? 4 : 3;
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        for (int i = 0; i < n; i++) {
            yyjson_mut_arr_add_real(d, arr, (double)c->v[i]);
        }
        yyjson_mut_obj_add_val(d, o, "values", arr);
    }
    return o;
}

static yyjson_mut_val *swatch_to(yyjson_mut_doc *d, const pc_swatch *sw)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "name", sw->name == NULL ? "" : sw->name);
    yyjson_mut_obj_add_val(d, o, "color", swatch_color_raw(d, &sw->color));
    if (sw->kind != PC_SWATCH_PROCESS) {
        yyjson_mut_obj_add_strcpy(d, o, "kind", pc_swatch_kind_id(sw->kind));
    }
    return o;
}

static bool swatch_color_from(yyjson_val *v, pc_swatch_color *out)
{
    memset(out, 0, sizeof *out);
    size_t ml = 0;
    const char *m = pc_json_as_str(pc_json_get(v, "model"), &ml);
    yyjson_val *vals = pc_json_get(v, "values");
    if (m == NULL) {
        return false;
    }
    if (ml == 4 && memcmp(m, "gray", 4) == 0) {
        double x = 0.0;
        if (!pc_json_as_f64(vals, &x)) {
            return false;
        }
        out->model = PC_SWATCH_GRAY;
        out->v[0] = (float)x;
        return true;
    }
    if (ml == 4 && memcmp(m, "book", 4) == 0) {
        out->model = PC_SWATCH_BOOK;
        uint64_t sp = 0;
        (void)pc_json_as_u64(pc_json_get(vals, "space"), &sp);
        out->book_space = (uint16_t)sp;
        yyjson_val *bv = pc_json_get(vals, "values");
        for (int i = 0; i < 4 && bv != NULL && yyjson_is_arr(bv) && (size_t)i < yyjson_arr_size(bv); i++) {
            uint64_t x = 0;
            (void)pc_json_as_u64(yyjson_arr_get(bv, (size_t)i), &x);
            out->book_values[i] = (uint16_t)x;
        }
        return true;
    }
    if (vals == NULL || !yyjson_is_arr(vals)) {
        return false;
    }
    int n;
    if (ml == 3 && memcmp(m, "rgb", 3) == 0) {
        out->model = PC_SWATCH_RGB;
        n = 3;
    } else if (ml == 3 && memcmp(m, "hsb", 3) == 0) {
        out->model = PC_SWATCH_HSB;
        n = 3;
    } else if (ml == 4 && memcmp(m, "cmyk", 4) == 0) {
        out->model = PC_SWATCH_CMYK;
        n = 4;
    } else if (ml == 3 && memcmp(m, "lab", 3) == 0) {
        out->model = PC_SWATCH_LAB;
        n = 3;
    } else {
        return false;
    }
    for (int i = 0; i < n; i++) {
        double x = 0.0;
        (void)pc_json_as_f64(yyjson_arr_get(vals, (size_t)i), &x);
        out->v[i] = (float)x;
    }
    return true;
}

static bool swatch_from(yyjson_val *v, pc_swatch *out)
{
    memset(out, 0, sizeof *out);
    size_t nl = 0;
    const char *nm = pc_json_as_str(pc_json_get(v, "name"), &nl);
    out->name = malloc(nl + 1);
    if (out->name == NULL) {
        return false;
    }
    memcpy(out->name, nm != NULL ? nm : "", nm != NULL ? nl : 0);
    out->name[nm != NULL ? nl : 0] = '\0';
    if (!swatch_color_from(pc_json_get(v, "color"), &out->color)) {
        pc_swatch_fini(out);
        return false;
    }
    out->kind = PC_SWATCH_PROCESS;
    size_t kl = 0;
    const char *k = pc_json_as_str(pc_json_get(v, "kind"), &kl);
    if (k != NULL) {
        char kb[16];
        if (kl < sizeof kb) {
            memcpy(kb, k, kl);
            kb[kl] = '\0';
            (void)pc_swatch_kind_parse(kb, &out->kind);
        }
    }
    return true;
}

/* ---- Group<T> serde ----------------------------------------------------------------------- */

typedef yyjson_mut_val *(*item_to_fn)(yyjson_mut_doc *, const void *);
typedef bool (*item_from_fn)(yyjson_val *, void *);

static yyjson_mut_val *groups_to(yyjson_mut_doc *d, const pc_preset_groups *g, size_t size, item_to_fn to)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < g->n; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, o, "name", g->v[i].name);
        yyjson_mut_val *items = yyjson_mut_arr(d);
        for (size_t j = 0; j < g->v[i].n; j++) {
            yyjson_mut_arr_append(items, to(d, (char *)g->v[i].items + j * size));
        }
        yyjson_mut_obj_add_val(d, o, "items", items);
        yyjson_mut_arr_append(arr, o);
    }
    return arr;
}

/* Replace *dst with the groups parsed from `v` (an array of {name, items}); on any failure leaves
 * *dst untouched and returns false. */
static bool groups_from(yyjson_val *v, pc_preset_groups *dst, const pc_preset_item_ops *ops,
                        item_from_fn from)
{
    if (v == NULL || !yyjson_is_arr(v)) {
        return false;
    }
    pc_preset_groups g;
    pc_preset_groups_init(&g);
    size_t idx, max;
    yyjson_val *el;
    yyjson_arr_foreach(v, idx, max, el) {
        size_t nl = 0;
        const char *nm = pc_json_as_str(pc_json_get(el, "name"), &nl);
        char nbuf[1024];
        size_t cn = nl < sizeof nbuf - 1 ? nl : sizeof nbuf - 1;
        if (nm != NULL) {
            memcpy(nbuf, nm, cn);
        }
        nbuf[nm != NULL ? cn : 0] = '\0';
        if (pc_preset_groups_push(&g, nbuf, NULL) == NULL) {
            pc_preset_groups_fini(&g, ops);
            return false;
        }
        size_t gidx = g.n - 1;
        yyjson_val *items = pc_json_get(el, "items");
        if (items == NULL || !yyjson_is_arr(items)) {
            continue;
        }
        size_t i2, m2;
        yyjson_val *it;
        yyjson_arr_foreach(items, i2, m2, it) {
            void *slot = malloc(ops->size);
            if (slot == NULL) {
                pc_preset_groups_fini(&g, ops);
                return false;
            }
            if (!from(it, slot)) {
                free(slot);
                pc_preset_groups_fini(&g, ops);
                return false;
            }
            if (pc_preset_group_push(&g, gidx, slot, ops, NULL) != PC_OK) {
                ops->fini(slot);
                free(slot);
                pc_preset_groups_fini(&g, ops);
                return false;
            }
            free(slot);
        }
    }
    pc_preset_groups_fini(dst, ops);
    *dst = g;
    return true;
}

static yyjson_mut_val *grad_item_to(yyjson_mut_doc *d, const void *it)
{
    return gradient_to(d, it);
}
static bool grad_item_from(yyjson_val *v, void *out)
{
    return gradient_from(v, out);
}
static yyjson_mut_val *sw_item_to(yyjson_mut_doc *d, const void *it)
{
    return swatch_to(d, it);
}
static bool sw_item_from(yyjson_val *v, void *out)
{
    return swatch_from(v, out);
}
static yyjson_mut_val *str_item_to(yyjson_mut_doc *d, const void *it)
{
    const char *s = *(const char *const *)it;
    return yyjson_mut_strcpy(d, s != NULL ? s : "");
}
static bool str_item_from(yyjson_val *v, void *out)
{
    size_t sl = 0;
    const char *s = pc_json_as_str(v, &sl);
    if (s == NULL) {
        return false;
    }
    char *p = malloc(sl + 1);
    if (p == NULL) {
        return false;
    }
    memcpy(p, s, sl);
    p[sl] = '\0';
    *(char **)out = p;
    return true;
}

/* ---- public API --------------------------------------------------------------------------- */

pc_status pc_engine_presets_to_json(const pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    const pc_preset_state *st = &s->presets;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "presets");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, "gradients",
                           groups_to(d, &st->gradients, sizeof(pc_gradient_preset), grad_item_to));
    yyjson_mut_obj_add_val(d, root, "gradient", gradient_to(d, &st->gradient));
    yyjson_mut_obj_add_val(d, root, "pattern_groups",
                           groups_to(d, &st->pattern_groups, sizeof(char *), str_item_to));
    yyjson_mut_val *tools = yyjson_mut_arr(d);
    for (size_t i = 0; i < st->ntool_presets; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, o, "name", st->tool_presets[i].name);
        yyjson_mut_obj_add_strcpy(d, o, "tool", st->tool_presets[i].tool);
        yyjson_mut_val *opt = pc_json_value_root(&st->tool_presets[i].options);
        yyjson_mut_obj_add_val(d, o, "options",
                               opt != NULL ? yyjson_mut_val_mut_copy(d, opt) : yyjson_mut_obj(d));
        yyjson_mut_arr_append(tools, o);
    }
    yyjson_mut_obj_add_val(d, root, "tool_presets", tools);
    yyjson_mut_val *ld = pc_json_value_root(&st->layer_defaults);
    yyjson_mut_obj_add_val(d, root, "layer_defaults",
                           ld != NULL ? yyjson_mut_val_mut_copy(d, ld) : yyjson_mut_obj(d));
    yyjson_mut_obj_add_val(d, root, "swatches", groups_to(d, &st->swatches, sizeof(pc_swatch), sw_item_to));
    pc_engine_document_presets_to_json(st, d, root); /* E3: documents + recent_documents */
    yyjson_mut_doc_set_root(d, root);
    pc_status rc = pc_json_value_from_mut(root, out, err);
    yyjson_mut_doc_free(d);
    return rc;
}

void pc_engine_load_presets_json(pc_engine_session *s, yyjson_val *v)
{
    if (v == NULL || !yyjson_is_obj(v)) {
        return;
    }
    pc_preset_state *st = &s->presets;
    yyjson_val *g = pc_json_get(v, "gradients");
    if (g != NULL) {
        (void)groups_from(g, &st->gradients, pc_gradient_preset_ops(), grad_item_from);
    }
    yyjson_val *cur = pc_json_get(v, "gradient");
    if (cur != NULL) {
        pc_gradient_preset ng;
        if (gradient_from(cur, &ng)) {
            pc_gradient_preset_fini(&st->gradient);
            st->gradient = ng;
        }
    }
    yyjson_val *pg = pc_json_get(v, "pattern_groups");
    if (pg != NULL) {
        (void)groups_from(pg, &st->pattern_groups, pc_preset_string_ops(), str_item_from);
    }
    yyjson_val *tp = pc_json_get(v, "tool_presets");
    if (tp != NULL && yyjson_is_arr(tp)) {
        /* read entry by entry; skip bad entries / duplicate names; cap. */
        pc_tool_preset *nv = NULL;
        size_t nn = 0, cap = 0;
        size_t idx, max;
        yyjson_val *el;
        yyjson_arr_foreach(tp, idx, max, el) {
            if (nn >= PC_TOOL_PRESETS_MAX) {
                break;
            }
            size_t nl = 0, tl = 0;
            const char *nm = pc_json_as_str(pc_json_get(el, "name"), &nl);
            const char *tool = pc_json_as_str(pc_json_get(el, "tool"), &tl);
            if (nm == NULL || tool == NULL || nl == 0 || tl == 0) {
                continue;
            }
            bool dup = false;
            for (size_t i = 0; i < nn; i++) {
                if (strlen(nv[i].name) == nl && memcmp(nv[i].name, nm, nl) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if (nn == cap) {
                size_t nc = cap == 0 ? 8 : cap * 2;
                pc_tool_preset *g2 = realloc(nv, nc * sizeof *g2);
                if (g2 == NULL) {
                    break;
                }
                nv = g2;
                cap = nc;
            }
            pc_tool_preset tpr;
            memset(&tpr, 0, sizeof tpr);
            tpr.name = malloc(nl + 1);
            tpr.tool = malloc(tl + 1);
            if (tpr.name == NULL || tpr.tool == NULL) {
                free(tpr.name);
                free(tpr.tool);
                break;
            }
            memcpy(tpr.name, nm, nl);
            tpr.name[nl] = '\0';
            memcpy(tpr.tool, tool, tl);
            tpr.tool[tl] = '\0';
            yyjson_val *opt = pc_json_get(el, "options");
            if (opt != NULL && yyjson_is_obj(opt)) {
                (void)pc_json_value_from_val(opt, &tpr.options, NULL);
            } else {
                /* default to {} */
                yyjson_mut_doc *od = yyjson_mut_doc_new(NULL);
                if (od != NULL) {
                    yyjson_mut_val *o = yyjson_mut_obj(od);
                    yyjson_mut_doc_set_root(od, o);
                    (void)pc_json_value_from_mut(o, &tpr.options, NULL);
                    yyjson_mut_doc_free(od);
                }
            }
            nv[nn++] = tpr;
        }
        for (size_t i = 0; i < st->ntool_presets; i++) {
            pc_tool_preset_fini(&st->tool_presets[i]);
        }
        free(st->tool_presets);
        st->tool_presets = nv;
        st->ntool_presets = nn;
        st->cap_tool_presets = cap;
    }
    yyjson_val *ld = pc_json_get(v, "layer_defaults");
    if (ld != NULL && yyjson_is_obj(ld)) {
        pc_json_value nv = PC_JSON_VALUE_INIT;
        if (pc_json_value_from_val(ld, &nv, NULL) == PC_OK) {
            pc_json_value_fini(&st->layer_defaults);
            st->layer_defaults = nv;
        }
    }
    yyjson_val *sw = pc_json_get(v, "swatches");
    if (sw != NULL) {
        if (groups_from(sw, &st->swatches, pc_swatch_ops(), sw_item_from)) {
            pc_engine_swatches_cap_library(&st->swatches);
        }
    }
    pc_engine_document_presets_load(st, v); /* E3: documents + recent_documents */
    st->rev++;
}
