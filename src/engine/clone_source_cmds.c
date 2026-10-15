/* Window > Clone Source: the five clone source slots and the overlay options
 * (port of crates/engine/src/presets/clone_source.rs commands, E2b).
 *
 * The slot data drives paint.cloneStamp / paint.healingBrush (E10). These commands only read and
 * write the session's clone-source state; no document is touched. Never crashes. */
#include <math.h>
#include <string.h>

#include "engine_internal.h"
#include "presets.h"

/* A [x,y] pair param. */
static bool pt_param(yyjson_val *p, const char *key, double *x, double *y)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL || !yyjson_is_arr(v) || yyjson_arr_size(v) < 2) {
        return false;
    }
    return pc_json_as_f64(yyjson_arr_get(v, 0), x) && pc_json_as_f64(yyjson_arr_get(v, 1), y);
}

static void add_pt_or_null(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, bool has,
                           const double v[2])
{
    if (!has) {
        yyjson_mut_obj_add_val(d, o, key, yyjson_mut_null(d));
        return;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, arr, v[0]);
    yyjson_mut_arr_add_real(d, arr, v[1]);
    yyjson_mut_obj_add_val(d, o, key, arr);
}

static yyjson_mut_val *slot_json(yyjson_mut_doc *d, const pc_clone_source *c, size_t i)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "index", i);
    add_pt_or_null(d, o, "source", c->has_source, c->source);
    add_pt_or_null(d, o, "anchor", c->has_anchor, c->anchor);
    if (c->has_source && c->has_anchor) {
        double off[2] = {c->source[0] - c->anchor[0], c->source[1] - c->anchor[1]};
        add_pt_or_null(d, o, "offset", true, off);
    } else {
        yyjson_mut_obj_add_val(d, o, "offset", yyjson_mut_null(d));
    }
    if (c->has_layer) {
        yyjson_mut_obj_add_uint(d, o, "layer", c->layer);
    } else {
        yyjson_mut_obj_add_val(d, o, "layer", yyjson_mut_null(d));
    }
    yyjson_mut_obj_add_real(d, o, "width", c->scale[0]);
    yyjson_mut_obj_add_real(d, o, "height", c->scale[1]);
    yyjson_mut_obj_add_bool(d, o, "linked", c->linked);
    yyjson_mut_obj_add_real(d, o, "rotation", c->rotation);
    yyjson_mut_obj_add_bool(d, o, "flipH", c->flip_h);
    yyjson_mut_obj_add_bool(d, o, "flipV", c->flip_v);
    return o;
}

static yyjson_mut_val *overlay_json(yyjson_mut_doc *d, const pc_clone_overlay *o)
{
    yyjson_mut_val *v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, v, "show", o->show);
    yyjson_mut_obj_add_real(d, v, "opacity", (double)o->opacity);
    yyjson_mut_obj_add_bool(d, v, "clipped", o->clipped);
    yyjson_mut_obj_add_bool(d, v, "auto_hide", o->auto_hide);
    yyjson_mut_obj_add_bool(d, v, "invert", o->invert);
    yyjson_mut_obj_add_strcpy(d, v, "blend", pc_clone_overlay_blend_name(o->blend));
    return v;
}

/* Resolve the targeted slot index: `index` (0..SLOTS) or the active one. */
static pc_status slot_index(const pc_engine_session *s, yyjson_val *p, const char *cmd, size_t *out,
                            pc_error *err)
{
    uint64_t i;
    if (pc_json_as_u64(pc_json_get(p, "index"), &i)) {
        if (i >= PC_CLONE_SLOTS) {
            return pc_engine_err_bad_params(err, cmd, "index %llu out of range (0..%d)",
                                            (unsigned long long)i, PC_CLONE_SLOTS);
        }
        *out = (size_t)i;
        return PC_OK;
    }
    *out = s->presets.clone.active;
    return PC_OK;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    const pc_clone_sources *c = &s->presets.clone;
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "active", c->active);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < PC_CLONE_SLOTS; i++) {
        yyjson_mut_arr_append(arr, slot_json(d, &c->slots[i], i));
    }
    yyjson_mut_obj_add_val(d, root, "sources", arr);
    yyjson_mut_obj_add_val(d, root, "overlay", overlay_json(d, &c->overlay));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_select(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "cloneSource.select";
    uint64_t i;
    if (!pc_json_as_u64(pc_json_get(p, "index"), &i)) {
        return pc_engine_err_bad_params(err, cmd, "missing `index`");
    }
    if (i >= PC_CLONE_SLOTS) {
        return pc_engine_err_bad_params(err, cmd, "index %llu out of range (0..%d)",
                                        (unsigned long long)i, PC_CLONE_SLOTS);
    }
    s->presets.clone.active = (size_t)i;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(d, slot_json(d, &s->presets.clone.slots[i], (size_t)i));
    return PC_OK;
}

static double rem_euclid(double a, double m)
{
    double r = fmod(a, m);
    if (r < 0.0) {
        r += m;
    }
    return r;
}

static pc_status run_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "cloneSource.set";
    size_t i = 0;
    if (slot_index(s, p, cmd, &i, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    bool have_active_layer = false;
    uint64_t active_layer = 0;
    const pc_engine_doc_state *ds = pc_engine_session_active(s);
    if (ds != NULL && ds->has_active_layer) {
        have_active_layer = true;
        active_layer = ds->active_layer;
    }
    pc_clone_source *slot = &s->presets.clone.slots[i];
    double x, y;
    if (pt_param(p, "source", &x, &y)) {
        slot->has_source = true;
        slot->source[0] = x;
        slot->source[1] = y;
        slot->has_anchor = false;
        uint64_t lv;
        if (pc_json_as_u64(pc_json_get(p, "layer"), &lv)) {
            slot->has_layer = true;
            slot->layer = lv;
        } else if (have_active_layer) {
            slot->has_layer = true;
            slot->layer = active_layer;
        } else {
            slot->has_layer = false;
        }
    }
    if (pt_param(p, "offset", &x, &y)) {
        if (!slot->has_anchor && !slot->has_source) {
            return pc_engine_err_bad_params(err, cmd, "set a `source` before an offset");
        }
        double a0, a1;
        if (slot->has_anchor) {
            a0 = slot->anchor[0];
            a1 = slot->anchor[1];
        } else {
            a0 = slot->source[0];
            a1 = slot->source[1];
        }
        slot->has_anchor = true;
        slot->anchor[0] = a0;
        slot->anchor[1] = a1;
        slot->has_source = true;
        slot->source[0] = a0 + x;
        slot->source[1] = a1 + y;
    }
    yyjson_val *sc = pc_json_get(p, "scale");
    if (sc != NULL && yyjson_is_arr(sc)) {
        double s0 = 100.0, s1 = 100.0;
        (void)pc_json_as_f64(yyjson_arr_get(sc, 0), &s0);
        if (yyjson_arr_size(sc) > 1) {
            (void)pc_json_as_f64(yyjson_arr_get(sc, 1), &s1);
        }
        slot->scale[0] = s0;
        slot->scale[1] = s1;
    }
    bool bl;
    if (pc_json_as_bool(pc_json_get(p, "linked"), &bl)) {
        slot->linked = bl;
    }
    double before0 = slot->scale[0], before1 = slot->scale[1];
    double w, h;
    bool have_w = pc_json_as_f64(pc_json_get(p, "width"), &w);
    bool have_h = pc_json_as_f64(pc_json_get(p, "height"), &h);
    if (have_w) {
        slot->scale[0] = w;
    }
    if (have_h) {
        slot->scale[1] = h;
    }
    if (slot->linked) {
        if (have_w && !have_h && fabs(before0) > 1e-9) {
            slot->scale[1] = before1 * w / before0;
        } else if (!have_w && have_h && fabs(before1) > 1e-9) {
            slot->scale[0] = before0 * h / before1;
        }
    }
    for (int k = 0; k < 2; k++) {
        double a = fabs(slot->scale[k]);
        if (!(a >= 1.0 && a <= 1000.0)) {
            slot->scale[0] = slot->scale[0] < 1.0 ? 1.0 : (slot->scale[0] > 1000.0 ? 1000.0 : slot->scale[0]);
            slot->scale[1] = slot->scale[1] < 1.0 ? 1.0 : (slot->scale[1] > 1000.0 ? 1000.0 : slot->scale[1]);
            break;
        }
    }
    double r;
    if (pc_json_as_f64(pc_json_get(p, "rotation"), &r)) {
        slot->rotation = rem_euclid(r + 180.0, 360.0) - 180.0;
    }
    if (pc_json_as_bool(pc_json_get(p, "flipH"), &bl)) {
        slot->flip_h = bl;
    }
    if (pc_json_as_bool(pc_json_get(p, "flipV"), &bl)) {
        slot->flip_v = bl;
    }
    if (pc_json_as_bool(pc_json_get(p, "clear"), &bl) && bl) {
        pc_clone_source_default(slot);
    }
    bool select = true;
    if (pc_json_as_bool(pc_json_get(p, "select"), &bl)) {
        select = bl;
    }
    if (select) {
        s->presets.clone.active = i;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(d, slot_json(d, &s->presets.clone.slots[i], i));
    return PC_OK;
}

static pc_status run_reset_transform(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t i = 0;
    if (slot_index(s, p, "cloneSource.resetTransform", &i, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    pc_clone_source *slot = &s->presets.clone.slots[i];
    slot->scale[0] = 100.0;
    slot->scale[1] = 100.0;
    slot->rotation = 0.0;
    slot->flip_h = false;
    slot->flip_v = false;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(d, slot_json(d, slot, i));
    return PC_OK;
}

static pc_status run_overlay(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_clone_overlay *o = &s->presets.clone.overlay;
    bool b;
    if (pc_json_as_bool(pc_json_get(p, "show"), &b)) {
        o->show = b;
    }
    double v;
    if (pc_json_as_f64(pc_json_get(p, "opacity"), &v)) {
        float f = (float)v;
        o->opacity = f < 0.0f ? 0.0f : (f > 100.0f ? 100.0f : f);
    }
    if (pc_json_as_bool(pc_json_get(p, "clipped"), &b)) {
        o->clipped = b;
    }
    if (pc_json_as_bool(pc_json_get(p, "autoHide"), &b)) {
        o->auto_hide = b;
    }
    if (pc_json_as_bool(pc_json_get(p, "invert"), &b)) {
        o->invert = b;
    }
    size_t mlen = 0;
    const char *m = pc_json_as_str(pc_json_get(p, "blend"), &mlen);
    if (m != NULL) {
        char buf[32];
        if (mlen >= sizeof buf) {
            return pc_engine_err_bad_params(err, "cloneSource.overlay",
                                            "unknown overlay mode `%.*s` (normal|darken|lighten|difference)",
                                            (int)mlen, m);
        }
        memcpy(buf, m, mlen);
        buf[mlen] = '\0';
        int bl;
        if (!pc_clone_overlay_blend_parse(buf, &bl)) {
            return pc_engine_err_bad_params(err, "cloneSource.overlay",
                                            "unknown overlay mode `%s` (normal|darken|lighten|difference)", buf);
        }
        o->blend = bl;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(d, overlay_json(d, o));
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"cloneSource.list", "Clone Sources", NULL, 0, NULL,
     "{} -> {active,sources:[{index,source,anchor,offset,layer,width,height,linked,rotation,flipH,flipV}],overlay}",
     pc_engine_always, run_list, false},
    {"cloneSource.select", "Select Clone Source", NULL, 0, NULL, "{\"index\":0..4}", pc_engine_always,
     run_select, true},
    {"cloneSource.set", "Set Clone Source", NULL, 0, NULL,
     "{\"index\":0..4?=active,\"source\":[x,y]? (option-click; re-pairs with the next "
     "stroke),\"offset\":[dx,dy]?,\"width\":%?,\"height\":%?|\"scale\":[w%,h%]?,\"linked\":bool? (W/H "
     "link: a lone width or height scales the other by the same "
     "factor),\"rotation\":deg?,\"flipH\":bool?,\"flipV\":bool?,\"layer\":id?,\"clear\":bool?,\"select\":bool=true}",
     pc_engine_always, run_set, true},
    {"cloneSource.resetTransform", "Reset Transform", NULL, 0, NULL, "{\"index\":0..4?=active}",
     pc_engine_always, run_reset_transform, true},
    {"cloneSource.overlay", "Clone Source Overlay", NULL, 0, NULL,
     "{\"show\":bool?,\"opacity\":0..100?,\"clipped\":bool?,\"autoHide\":bool?,\"invert\":bool?,\"blend\":"
     "\"normal|darken|lighten|difference\"?}",
     pc_engine_always, run_overlay, true},
};

const pc_engine_command *pc_engine_clone_source_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
