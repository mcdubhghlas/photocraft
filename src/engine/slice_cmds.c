/* Web slices: the Slice tool (slice.new, Slices From Guides), the Slice Select tool (slice.set,
 * promote, divide, delete), Layer > New Layer Based Slice, View > Lock/Clear Slices. Port of
 * crates/engine/src/slice_cmds.rs (E12). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "base/num.h"
#include "compose/effects.h"
#include "doc/comps.h"
#include "doc/doc.h"
#include "doc/slices.h"
#include "engine_internal.h"
#include "slice_ui.h"
#include "layer_cmds.h" /* pc_engine_layer_param */

/* ---- predicates --------------------------------------------------------------------------- */

static pc_status slice_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

static pc_status slice_unlocked(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = slice_has_doc(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    if (s->slices_locked) {
        return pc_fail(reason, PC_ERR_INVALID, "slices are locked (View \xE2\x80\xBA Lock Slices)");
    }
    return PC_OK;
}

static pc_status slice_has_slices(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = slice_unlocked(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    if (pc_slices_is_empty(&doc->slices)) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no user or layer-based slices");
    }
    return PC_OK;
}

static pc_status slice_has_layer(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = slice_unlocked(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (!d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind == PC_LAYER_ADJUSTMENT) {
        return pc_fail(reason, PC_ERR_INVALID, "adjustment layers have no bounds to slice");
    }
    if (pc_document_is_background(doc, d->active_layer)) {
        return pc_fail(reason, PC_ERR_INVALID, "the Background layer can't have a layer-based slice");
    }
    for (size_t i = 0; i < doc->slices.len; i++) {
        if (doc->slices.list[i].has_layer && doc->slices.list[i].layer == d->active_layer) {
            return pc_fail(reason, PC_ERR_INVALID, "the layer already has a layer-based slice");
        }
    }
    return PC_OK;
}

/* ---- geometry ----------------------------------------------------------------------------- */

static pc_rect slice_layer_bounds(const pc_layer *l);

static pc_rect group_children_union(const pc_layer *l)
{
    pc_rect acc = PC_RECT_EMPTY;
    for (size_t i = 0; i < l->content.u.group.nchildren; i++) {
        const pc_layer *c = &l->content.u.group.children[i];
        if (!c->visible) {
            continue;
        }
        pc_rect b = slice_layer_bounds(c);
        if (pc_rect_is_empty(b)) {
            continue;
        }
        acc = pc_rect_is_empty(acc) ? b : pc_rect_union(acc, b);
    }
    return acc;
}

static pc_rect slice_layer_bounds(const pc_layer *l)
{
    pc_rect own;
    if (l->content.kind == PC_LAYER_GROUP) {
        if (l->content.u.group.has_artboard) {
            own = l->content.u.group.artboard.rect;
        } else {
            own = group_children_union(l);
        }
    } else {
        own = pc_comp_position_bounds(l);
    }
    if (pc_rect_is_empty(own) || !pc_compose_has_effects(l)) {
        return own;
    }
    return pc_rect_inflate(own, pc_compose_effects_margin(l));
}

/* layer_slice_rect: the stored slice's layer bounds plus outsets; false when the layer is gone. */
static bool layer_slice_rect(const pc_document *doc, const pc_slice *sl, pc_rect *out)
{
    if (!sl->has_layer) {
        return false;
    }
    const pc_layer *l = pc_document_layer(doc, sl->layer);
    if (l == NULL) {
        return false;
    }
    pc_rect b = slice_layer_bounds(l);
    if (pc_rect_is_empty(b)) {
        *out = b;
        return true;
    }
    int32_t t = sl->outsets[0], le = sl->outsets[1], bo = sl->outsets[2], r = sl->outsets[3];
    pc_rect nr;
    nr.x0 = (int32_t)((int64_t)b.x0 - le < INT32_MIN ? INT32_MIN : (int64_t)b.x0 - le);
    nr.y0 = (int32_t)((int64_t)b.y0 - t < INT32_MIN ? INT32_MIN : (int64_t)b.y0 - t);
    int64_t x1 = (int64_t)b.x1 + r, y1 = (int64_t)b.y1 + bo;
    nr.x1 = (int32_t)(x1 > INT32_MAX ? INT32_MAX : x1);
    nr.y1 = (int32_t)(y1 > INT32_MAX ? INT32_MAX : y1);
    *out = nr;
    return true;
}

/* refresh: re-fit layer-based slices to their layers; drop ones whose layer was deleted. */
void pc_engine_slices_refresh(pc_engine_session *s)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return;
    }
    const pc_document *cdoc = pc_doc_arc_get(st->doc);
    bool any_layer = false;
    for (size_t i = 0; i < cdoc->slices.len; i++) {
        if (cdoc->slices.list[i].origin == PC_SLICE_LAYER) {
            any_layer = true;
            break;
        }
    }
    if (!any_layer) {
        return;
    }
    /* decide whether anything changes before cloning */
    bool changes = false;
    for (size_t i = 0; i < cdoc->slices.len && !changes; i++) {
        const pc_slice *sl = &cdoc->slices.list[i];
        if (sl->origin != PC_SLICE_LAYER) {
            continue;
        }
        pc_rect r;
        if (!layer_slice_rect(cdoc, sl, &r)) {
            changes = true;
        } else if (r.x0 != sl->rect.x0 || r.y0 != sl->rect.y0 || r.x1 != sl->rect.x1 || r.y1 != sl->rect.y1) {
            changes = true;
        }
    }
    if (!changes) {
        return;
    }
    pc_document *doc = NULL;
    if (pc_doc_arc_make_mut(&st->doc, &doc, NULL) != PC_OK) {
        return;
    }
    size_t keep = 0;
    for (size_t i = 0; i < doc->slices.len; i++) {
        pc_slice *sl = &doc->slices.list[i];
        if (sl->origin == PC_SLICE_LAYER) {
            pc_rect r;
            if (!layer_slice_rect(doc, sl, &r)) {
                pc_slice_fini(sl);
                continue; /* drop */
            }
            sl->rect = r;
        }
        doc->slices.list[keep++] = *sl;
    }
    doc->slices.len = keep;
}

/* ---- params ------------------------------------------------------------------------------- */

static bool f_i32(yyjson_val *v, int32_t *out)
{
    double d;
    if (v == NULL || !pc_json_as_f64(v, &d) || !isfinite(d)) {
        return false;
    }
    d = round(d);
    if (d < (double)INT32_MIN || d > (double)INT32_MAX) {
        return false;
    }
    *out = (int32_t)d;
    return true;
}

static bool mk_rect(int32_t x, int32_t y, int32_t w, int32_t h, pc_rect *out)
{
    if (w <= 0 || h <= 0) {
        return false;
    }
    int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if (x1 > INT32_MAX || y1 > INT32_MAX) {
        return false;
    }
    *out = pc_rect_make(x, y, (int32_t)x1, (int32_t)y1);
    return true;
}

/* rect_param: "rect":[x,y,w,h] or x/y/width/height; false if absent/invalid. */
static bool rect_param(yyjson_val *p, pc_rect *out)
{
    if (p == NULL) {
        return false;
    }
    yyjson_val *ra = pc_json_get(p, "rect");
    if (ra != NULL && yyjson_is_arr(ra) && yyjson_arr_size(ra) == 4) {
        int32_t v[4];
        for (size_t i = 0; i < 4; i++) {
            if (!f_i32(yyjson_arr_get(ra, i), &v[i])) {
                return false;
            }
        }
        return mk_rect(v[0], v[1], v[2], v[3], out);
    }
    int32_t x, y, w, h;
    if (f_i32(pc_json_get(p, "x"), &x) && f_i32(pc_json_get(p, "y"), &y) &&
        f_i32(pc_json_get(p, "width"), &w) && f_i32(pc_json_get(p, "height"), &h)) {
        return mk_rect(x, y, w, h, out);
    }
    return false;
}

static bool has_rect_key(yyjson_val *p)
{
    const char *keys[] = {"rect", "x", "y", "width", "height"};
    for (size_t i = 0; i < 5; i++) {
        if (p != NULL && pc_json_get(p, keys[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static bool hex_color(const char *s, size_t len, uint8_t out[4])
{
    if (len > 0 && s[0] == '#') {
        s++;
        len--;
    }
    if (len != 6) {
        return false;
    }
    for (size_t i = 0; i < 6; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    unsigned r, g, b;
    char buf[7];
    memcpy(buf, s, 6);
    buf[6] = '\0';
    if (sscanf(buf, "%2x%2x%2x", &r, &g, &b) != 3) {
        return false;
    }
    out[0] = 255;
    out[1] = (uint8_t)r;
    out[2] = (uint8_t)g;
    out[3] = (uint8_t)b;
    return true;
}

/* apply_options: fields present in p applied to sl. */
static pc_status apply_options(pc_slice *sl, yyjson_val *p, const char *cmd, pc_error *err)
{
    size_t sl_len;
    const char *sv;
    yyjson_val *v;
    if ((v = pc_json_get(p, "name")) != NULL && (sv = pc_json_as_str(v, &sl_len)) != NULL) {
        if (pc_str_set(&sl->name, sv, sl_len, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    v = pc_json_get(p, "kind");
    if (v == NULL) {
        v = pc_json_get(p, "type");
    }
    if (v != NULL && (sv = pc_json_as_str(v, &sl_len)) != NULL) {
        pc_slice_kind k;
        if (!pc_slice_kind_from_id(sv, sl_len, &k)) {
            return pc_engine_err_bad_params(err, cmd, "unknown slice type `%.*s` (image|noImage|table)",
                                            (int)sl_len, sv);
        }
        sl->kind = k;
    }
    struct {
        const char *key;
        pc_str *slot;
    } texts[] = {{"url", &sl->url},
                 {"target", &sl->target},
                 {"message", &sl->message},
                 {"alt", &sl->alt},
                 {"cellText", &sl->cell_text}};
    for (size_t i = 0; i < 5; i++) {
        if ((v = pc_json_get(p, texts[i].key)) != NULL && (sv = pc_json_as_str(v, &sl_len)) != NULL) {
            if (pc_str_set(texts[i].slot, sv, sl_len, err) != PC_OK) {
                return PC_ERR_NOMEM;
            }
        }
    }
    bool b;
    if ((v = pc_json_get(p, "cellTextIsHtml")) != NULL && pc_json_as_bool(v, &b)) {
        sl->cell_text_is_html = b;
    }
    struct {
        const char *key;
        uint32_t *slot;
    } aligns[] = {{"horizontalAlign", &sl->horizontal_align}, {"verticalAlign", &sl->vertical_align}};
    for (size_t i = 0; i < 2; i++) {
        int64_t iv;
        if (pc_engine_param_int(p, aligns[i].key, &iv) && iv >= 0) {
            *aligns[i].slot = (uint32_t)(iv > 4 ? 4 : iv);
        }
    }
    v = pc_json_get(p, "background");
    if (v != NULL && (sv = pc_json_as_str(v, &sl_len)) != NULL) {
        if (sl_len == 0 || (sl_len == 4 && memcmp(sv, "none", 4) == 0)) {
            sl->has_background = false;
        } else {
            uint8_t c[4];
            if (!hex_color(sv, sl_len, c)) {
                return pc_engine_err_bad_params(err, cmd, "background is \"none\" or \"#rrggbb\"");
            }
            memcpy(sl->background, c, 4);
            sl->has_background = true;
        }
    }
    v = pc_json_get(p, "outsets");
    if (v != NULL && yyjson_is_arr(v) && yyjson_arr_size(v) == 4) {
        for (size_t i = 0; i < 4; i++) {
            int64_t ov = 0;
            (void)pc_json_as_i64(yyjson_arr_get(v, i), &ov);
            if (ov < -10000) {
                ov = -10000;
            }
            if (ov > 10000) {
                ov = 10000;
            }
            sl->outsets[i] = (int32_t)ov;
        }
    }
    pc_rect r;
    if (rect_param(p, &r)) {
        if (sl->origin == PC_SLICE_LAYER) {
            sl->origin = PC_SLICE_USER;
            sl->has_layer = false;
        }
        sl->rect = r;
    }
    return PC_OK;
}

/* Target: a stored slice id, or an auto slice (its rect, not stored yet). */
typedef struct {
    bool stored;
    uint32_t id;  /* stored */
    pc_rect rect; /* auto */
} slice_target;

static pc_status find_target(const pc_engine_session *s, yyjson_val *p, const char *cmd, slice_target *out,
                             pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    yyjson_val *sv = pc_json_get(p, "slice");
    if (sv != NULL && !yyjson_is_null(sv)) {
        uint32_t id = 0;
        if (pc_engine_param_u32_id(cmd, "slice", sv, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        const pc_slice *sl = pc_slices_get(&doc->slices, id);
        if (sl == NULL) {
            return pc_engine_err_bad_params(err, cmd, "no slice with id %u", id);
        }
        out->stored = true;
        out->id = sl->id;
        return PC_OK;
    }
    int64_t n = 0;
    if (!pc_engine_param_int(p, "number", &n) || n <= 0) {
        return pc_engine_err_bad_params(err, cmd, "give \"slice\" (id) or \"number\"");
    }
    pc_resolved_slice *rs = NULL;
    size_t nrs = 0;
    if (pc_slices_resolve(doc, &rs, &nrs, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < nrs; i++) {
        if (rs[i].number == (size_t)n) {
            if (rs[i].has_id) {
                out->stored = true;
                out->id = rs[i].id;
            } else {
                out->stored = false;
                out->rect = rs[i].rect;
            }
            pc_resolved_slices_free(rs, nrs);
            return PC_OK;
        }
    }
    pc_resolved_slices_free(rs, nrs);
    return pc_engine_err_bad_params(err, cmd, "no slice number %lld", (long long)n);
}

/* Store a target: an auto slice is promoted to a user slice in doc; returns its id. */
static pc_status store_target(pc_document *doc, slice_target t, uint32_t *out, pc_error *err)
{
    if (t.stored) {
        *out = t.id;
        return PC_OK;
    }
    uint32_t id = 0;
    if (!pc_slices_next_id(&doc->slices, &id)) {
        return pc_engine_err_other(err, "slice id space exhausted");
    }
    size_t sneed = doc->slices.len + 1;
    if (sneed == 0 || sneed > SIZE_MAX / sizeof *doc->slices.list) {
        return pc_fail(err, PC_ERR_LIMIT, "slice");
    }
    pc_slice *grown = realloc(doc->slices.list, sneed * sizeof *doc->slices.list);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "slice");
    }
    doc->slices.list = grown;
    pc_slice sl;
    pc_slice_default(&sl);
    sl.id = id;
    sl.rect = t.rect;
    doc->slices.list[doc->slices.len++] = sl;
    *out = id;
    return PC_OK;
}

static void add_resolved_json(yyjson_mut_doc *rd, yyjson_mut_val *o, const pc_document *doc,
                              const pc_resolved_slice *r)
{
    const pc_slice *sl = r->has_id ? pc_slices_get(&doc->slices, r->id) : NULL;
    yyjson_mut_obj_add_uint(rd, o, "number", r->number);
    if (r->has_id) {
        yyjson_mut_obj_add_uint(rd, o, "id", r->id);
    } else {
        yyjson_mut_obj_add_null(rd, o, "id");
    }
    yyjson_mut_obj_add_str(rd, o, "origin", pc_slice_origin_id(r->origin));
    yyjson_mut_obj_add_strcpy(rd, o, "name", pc_str_c(&r->name));
    yyjson_mut_val *rect = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_int(rd, rect, r->rect.x0);
    yyjson_mut_arr_add_int(rd, rect, r->rect.y0);
    yyjson_mut_arr_add_uint(rd, rect, pc_rect_width(r->rect));
    yyjson_mut_arr_add_uint(rd, rect, pc_rect_height(r->rect));
    yyjson_mut_obj_add_val(rd, o, "rect", rect);
    yyjson_mut_obj_add_str(rd, o, "kind", pc_slice_kind_id(r->kind));
    if (sl != NULL && sl->has_layer) {
        yyjson_mut_obj_add_uint(rd, o, "layer", sl->layer);
    } else {
        yyjson_mut_obj_add_null(rd, o, "layer");
    }
    yyjson_mut_obj_add_strcpy(rd, o, "url", sl != NULL ? pc_str_c(&sl->url) : "");
    yyjson_mut_obj_add_strcpy(rd, o, "target", sl != NULL ? pc_str_c(&sl->target) : "");
    yyjson_mut_obj_add_strcpy(rd, o, "message", sl != NULL ? pc_str_c(&sl->message) : "");
    yyjson_mut_obj_add_strcpy(rd, o, "alt", sl != NULL ? pc_str_c(&sl->alt) : "");
    yyjson_mut_obj_add_strcpy(rd, o, "cellText", sl != NULL ? pc_str_c(&sl->cell_text) : "");
    if (sl != NULL && sl->has_background) {
        char bg[8];
        snprintf(bg, sizeof bg, "#%02x%02x%02x", sl->background[1], sl->background[2], sl->background[3]);
        yyjson_mut_obj_add_strcpy(rd, o, "background", bg);
    } else {
        yyjson_mut_obj_add_null(rd, o, "background");
    }
}

/* ---- commands ----------------------------------------------------------------------------- */

typedef struct {
    pc_rect rect;
    yyjson_val *p;
    const char *cmd;
    uint32_t id_out;
} new_slice_ctx;

static pc_status edit_new_slice(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    new_slice_ctx *c = ctx;
    uint32_t id = 0;
    if (!pc_slices_next_id(&doc->slices, &id)) {
        return pc_engine_err_other(err, "slice id space exhausted");
    }
    size_t sneed = doc->slices.len + 1;
    if (sneed == 0 || sneed > SIZE_MAX / sizeof *doc->slices.list) {
        return pc_fail(err, PC_ERR_LIMIT, "slice");
    }
    pc_slice *grown = realloc(doc->slices.list, sneed * sizeof *doc->slices.list);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "slice");
    }
    doc->slices.list = grown;
    pc_slice sl;
    pc_slice_default(&sl);
    sl.id = id;
    sl.rect = c->rect;
    pc_status rc = apply_options(&sl, c->p, c->cmd, err);
    if (rc != PC_OK) {
        pc_slice_fini(&sl);
        return rc;
    }
    sl.rect = c->rect;
    doc->slices.list[doc->slices.len++] = sl;
    c->id_out = id;
    return PC_OK;
}

static pc_status run_new(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "slice.new";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_rect rect;
    if (!rect_param(p, &rect)) {
        return pc_engine_err_bad_params(err, CMD, "give \"rect\":[x,y,w,h] (or x, y, width, height)");
    }
    rect = pc_rect_intersect(rect, pc_document_bounds(pc_doc_arc_get(d->doc)));
    if (pc_rect_is_empty(rect)) {
        return pc_engine_err_bad_params(err, CMD, "the slice is outside the canvas");
    }
    new_slice_ctx c = {.rect = rect, .p = p, .cmd = CMD, .id_out = 0};
    pc_status rc = pc_engine_session_edit(s, "Slice", edit_new_slice, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_resolved_slice *rs = NULL;
    size_t nrs = 0;
    size_t number = 0;
    bool have = false;
    if (pc_slices_resolve(doc, &rs, &nrs, NULL) == PC_OK) {
        for (size_t i = 0; i < nrs; i++) {
            if (rs[i].has_id && rs[i].id == c.id_out) {
                number = rs[i].number;
                have = true;
                break;
            }
        }
        pc_resolved_slices_free(rs, nrs);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "slice", c.id_out);
    if (have) {
        yyjson_mut_obj_add_uint(rd, root, "number", number);
    } else {
        yyjson_mut_obj_add_null(rd, root, "number");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* Slices From Guides. */
typedef struct {
    int32_t *xs;
    size_t nxs;
    int32_t *ys;
    size_t nys;
    size_t count;
} guides_ctx;

static pc_status edit_from_guides(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    guides_ctx *g = ctx;
    for (size_t i = 0; i < doc->slices.len; i++) {
        pc_slice_fini(&doc->slices.list[i]);
    }
    doc->slices.len = 0;
    size_t total = (g->nys - 1) * (g->nxs - 1);
    size_t sneed = total;
    if (sneed == 0 || sneed > SIZE_MAX / sizeof *doc->slices.list) {
        return pc_fail(err, PC_ERR_LIMIT, "slice");
    }
    pc_slice *grown = realloc(doc->slices.list, sneed * sizeof *doc->slices.list);
    if (grown == NULL && total > 0) {
        return pc_fail(err, PC_ERR_NOMEM, "slice");
    }
    if (grown != NULL) {
        doc->slices.list = grown;
    }
    for (size_t j = 0; j + 1 < g->nys; j++) {
        for (size_t i = 0; i + 1 < g->nxs; i++) {
            uint32_t id = 0;
            if (!pc_slices_next_id(&doc->slices, &id)) {
                return pc_engine_err_other(err, "slice id space exhausted");
            }
            pc_slice sl;
            pc_slice_default(&sl);
            sl.id = id;
            sl.rect = pc_rect_make(g->xs[i], g->ys[j], g->xs[i + 1], g->ys[j + 1]);
            doc->slices.list[doc->slices.len++] = sl;
        }
    }
    g->count = total;
    return PC_OK;
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static size_t dedup_sorted(int32_t *v, size_t n)
{
    if (n == 0) {
        return 0;
    }
    size_t k = 1;
    for (size_t i = 1; i < n; i++) {
        if (v[i] != v[k - 1]) {
            v[k++] = v[i];
        }
    }
    return k;
}

static pc_status run_from_guides(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect b = pc_document_bounds(doc);
    size_t capx = doc->guides.nvertical + 2, capy = doc->guides.nhorizontal + 2;
    int32_t *xs = pc_malloc_array(capx, sizeof *xs);
    int32_t *ys = pc_malloc_array(capy, sizeof *ys);
    if (xs == NULL || ys == NULL) {
        free(xs);
        free(ys);
        return pc_fail(err, PC_ERR_NOMEM, "slice.fromGuides");
    }
    size_t nxs = 0, nys = 0;
    xs[nxs++] = b.x0;
    xs[nxs++] = b.x1;
    ys[nys++] = b.y0;
    ys[nys++] = b.y1;
    for (size_t i = 0; i < doc->guides.nvertical; i++) {
        int32_t x = (int32_t)lround(doc->guides.vertical[i]);
        if (x > b.x0 && x < b.x1) {
            xs[nxs++] = x;
        }
    }
    for (size_t i = 0; i < doc->guides.nhorizontal; i++) {
        int32_t y = (int32_t)lround(doc->guides.horizontal[i]);
        if (y > b.y0 && y < b.y1) {
            ys[nys++] = y;
        }
    }
    if (nxs == 2 && nys == 2) {
        free(xs);
        free(ys);
        return pc_engine_err_other(err, "the document has no guides inside the canvas");
    }
    qsort(xs, nxs, sizeof *xs, cmp_i32);
    qsort(ys, nys, sizeof *ys, cmp_i32);
    nxs = dedup_sorted(xs, nxs);
    nys = dedup_sorted(ys, nys);
    guides_ctx g = {.xs = xs, .nxs = nxs, .ys = ys, .nys = nys, .count = 0};
    pc_status rc = pc_engine_session_edit(s, "Slices From Guides", edit_from_guides, &g, err);
    free(xs);
    free(ys);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "slices", g.count);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* slice.set (Slice Options). */
typedef struct {
    slice_target t;
    yyjson_val *p;
    const char *cmd;
    uint32_t id_out;
} set_ctx;

static pc_status edit_set(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    set_ctx *c = ctx;
    uint32_t id = 0;
    pc_status rc = store_target(doc, c->t, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_slice *sl = pc_slices_get_mut(&doc->slices, id);
    if (sl == NULL) {
        return pc_engine_err_bad_params(err, c->cmd, "slice vanished");
    }
    rc = apply_options(sl, c->p, c->cmd, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* still resolved (on canvas)? */
    pc_resolved_slice *rs = NULL;
    size_t nrs = 0;
    bool on = false;
    if (pc_slices_resolve(doc, &rs, &nrs, NULL) == PC_OK) {
        for (size_t i = 0; i < nrs; i++) {
            if (rs[i].has_id && rs[i].id == id) {
                on = true;
                break;
            }
        }
        pc_resolved_slices_free(rs, nrs);
    }
    if (!on) {
        return pc_engine_err_bad_params(err, c->cmd, "slice is off the canvas");
    }
    c->id_out = id;
    return PC_OK;
}

static pc_status run_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "slice.set";
    pc_rect dummy;
    if (has_rect_key(p) && !rect_param(p, &dummy)) {
        return pc_engine_err_bad_params(
            err, CMD, "give a finite rectangle with positive dimensions and representable corners");
    }
    slice_target t = {0};
    pc_status rc = find_target(s, p, CMD, &t, err);
    if (rc != PC_OK) {
        return rc;
    }
    set_ctx c = {.t = t, .p = p, .cmd = CMD, .id_out = 0};
    rc = pc_engine_session_edit(s, "Slice Options", edit_set, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_resolved_slice *rs = NULL;
    size_t nrs = 0;
    if (pc_slices_resolve(doc, &rs, &nrs, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_status result = pc_engine_err_bad_params(err, CMD, "slice is off the canvas");
    for (size_t i = 0; i < nrs; i++) {
        if (rs[i].has_id && rs[i].id == c.id_out) {
            yyjson_mut_doc *rd = NULL;
            if (pc_engine_result_doc(out, &rd, err) == PC_OK) {
                yyjson_mut_val *root = yyjson_mut_obj(rd);
                add_resolved_json(rd, root, doc, &rs[i]);
                yyjson_mut_doc_set_root(rd, root);
                result = PC_OK;
            } else {
                result = PC_ERR_NOMEM;
            }
            break;
        }
    }
    pc_resolved_slices_free(rs, nrs);
    return result;
}

/* slice.promote. */
typedef struct {
    slice_target t;
    uint32_t id_out;
} promote_ctx;

static pc_status edit_promote(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    promote_ctx *c = ctx;
    uint32_t id = 0;
    pc_status rc = store_target(doc, c->t, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_slice *sl = pc_slices_get_mut(&doc->slices, id);
    if (sl != NULL) {
        sl->origin = PC_SLICE_USER;
        sl->has_layer = false;
    }
    c->id_out = id;
    return PC_OK;
}

static pc_status run_promote(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "slice.promote";
    slice_target t = {0};
    pc_status rc = find_target(s, p, CMD, &t, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool promotes = true;
    if (t.stored) {
        const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
        const pc_slice *sl = pc_slices_get(&doc->slices, t.id);
        promotes = sl != NULL && sl->origin == PC_SLICE_LAYER;
    }
    uint32_t id = t.id;
    if (!t.stored || promotes) {
        promote_ctx c = {.t = t, .id_out = 0};
        rc = pc_engine_session_edit(s, "Promote to User Slice", edit_promote, &c, err);
        if (rc != PC_OK) {
            return rc;
        }
        id = c.id_out;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "slice", id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* slice.delete. */
typedef struct {
    uint32_t *ids;
    size_t n;
} del_ctx;

static pc_status edit_delete(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    (void)err;
    del_ctx *c = ctx;
    size_t keep = 0;
    for (size_t i = 0; i < doc->slices.len; i++) {
        bool drop = false;
        for (size_t k = 0; k < c->n; k++) {
            if (doc->slices.list[i].id == c->ids[k]) {
                drop = true;
                break;
            }
        }
        if (drop) {
            pc_slice_fini(&doc->slices.list[i]);
        } else {
            doc->slices.list[keep++] = doc->slices.list[i];
        }
    }
    doc->slices.len = keep;
    return PC_OK;
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "slice.delete";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    uint32_t *ids = NULL;
    size_t nids = 0;
    yyjson_val *arr = pc_json_get(p, "slices");
    if (arr != NULL && yyjson_is_arr(arr)) {
        size_t m = yyjson_arr_size(arr);
        ids = pc_malloc_array(m > 0 ? m : 1, sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, CMD);
        }
        for (size_t i = 0; i < m; i++) {
            uint32_t id = 0;
            if (pc_engine_param_u32_id(CMD, "slices", yyjson_arr_get(arr, i), &id, err) != PC_OK) {
                free(ids);
                return err != NULL ? err->code : PC_ERR_INVALID;
            }
            if (pc_slices_get(&doc->slices, id) != NULL) {
                ids[nids++] = id;
            }
        }
        if (nids == 0) {
            free(ids);
            return pc_engine_err_bad_params(err, CMD, "none of the slices exist");
        }
    } else {
        slice_target t = {0};
        pc_status rc = find_target(s, p, CMD, &t, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (!t.stored) {
            return pc_engine_err_bad_params(err, CMD, "that is an auto slice");
        }
        ids = pc_malloc_array(1, sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, CMD);
        }
        ids[0] = t.id;
        nids = 1;
    }
    del_ctx c = {.ids = ids, .n = nids};
    pc_status rc = pc_engine_session_edit(s, "Delete Slice", edit_delete, &c, err);
    free(ids);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "deleted", nids);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* slice.divide. */
#define MAX_DIVIDE_PARTS 10000

typedef struct {
    slice_target t;
    int32_t down, across;
    uint32_t *ids_out;
    size_t n_out;
} divide_ctx;

static pc_status edit_divide(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    divide_ctx *c = ctx;
    uint32_t id = 0;
    pc_status rc = store_target(doc, c->t, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_slice *srcp = pc_slices_get(&doc->slices, id);
    if (srcp == NULL) {
        return pc_engine_err_bad_params(err, "slice.divide", "no such slice");
    }
    pc_slice src;
    if (pc_slice_clone(srcp, &src, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_rect r = src.rect;
    /* find position of id, remove it */
    size_t pos = 0;
    for (size_t i = 0; i < doc->slices.len; i++) {
        if (doc->slices.list[i].id == id) {
            pos = i;
            break;
        }
    }
    size_t keep = 0;
    for (size_t i = 0; i < doc->slices.len; i++) {
        if (doc->slices.list[i].id == id) {
            pc_slice_fini(&doc->slices.list[i]);
        } else {
            doc->slices.list[keep++] = doc->slices.list[i];
        }
    }
    doc->slices.len = keep;
    if (pos > doc->slices.len) {
        pos = doc->slices.len;
    }
    size_t nparts = (size_t)c->down * (size_t)c->across;
    uint32_t *ids = pc_malloc_array(nparts, sizeof *ids);
    pc_slice *parts = pc_malloc_array(nparts, sizeof *parts);
    if (ids == NULL || parts == NULL) {
        free(ids);
        free(parts);
        pc_slice_fini(&src);
        return pc_fail(err, PC_ERR_NOMEM, "slice.divide");
    }
    bool next_ok = true;
    uint32_t next_id = 0;
    next_ok = pc_slices_next_id(&doc->slices, &next_id);
    size_t made = 0;
    for (int32_t j = 0; j < c->down; j++) {
        for (int32_t i = 0; i < c->across; i++) {
            int32_t x0 = (int32_t)((int64_t)r.x0 + (int64_t)pc_rect_width(r) * i / c->across);
            int32_t x1 = (int32_t)((int64_t)r.x0 + (int64_t)pc_rect_width(r) * (i + 1) / c->across);
            int32_t y0 = (int32_t)((int64_t)r.y0 + (int64_t)pc_rect_height(r) * j / c->down);
            int32_t y1 = (int32_t)((int64_t)r.y0 + (int64_t)pc_rect_height(r) * (j + 1) / c->down);
            uint32_t nid;
            if (made == 0) {
                nid = id;
            } else {
                if (!next_ok) {
                    for (size_t k = 0; k < made; k++) {
                        pc_slice_fini(&parts[k]);
                    }
                    free(ids);
                    free(parts);
                    pc_slice_fini(&src);
                    return pc_engine_err_other(err, "slice id space exhausted");
                }
                nid = next_id;
                if (nid == id) {
                    nid++;
                }
                next_ok = nid < UINT32_MAX;
                next_id = (uint32_t)(nid + 1);
            }
            pc_slice sl;
            if (made == 0) {
                /* first part keeps the original's options */
                if (pc_slice_clone(&src, &sl, err) != PC_OK) {
                    free(ids);
                    free(parts);
                    pc_slice_fini(&src);
                    return PC_ERR_NOMEM;
                }
                sl.origin = PC_SLICE_USER;
                sl.has_layer = false;
            } else {
                pc_slice_default(&sl);
            }
            sl.id = nid;
            sl.rect = pc_rect_make(x0, y0, x1, y1);
            ids[made] = nid;
            parts[made] = sl;
            made++;
        }
    }
    pc_slice_fini(&src);
    /* insert parts at pos */
    size_t sneed = doc->slices.len + made;
    if (sneed == 0 || sneed > SIZE_MAX / sizeof *doc->slices.list) {
        for (size_t k = 0; k < made; k++) {
            pc_slice_fini(&parts[k]);
        }
        free(ids);
        free(parts);
        return pc_fail(err, PC_ERR_LIMIT, "slice");
    }
    pc_slice *grown = realloc(doc->slices.list, sneed * sizeof *doc->slices.list);
    if (grown == NULL) {
        for (size_t k = 0; k < made; k++) {
            pc_slice_fini(&parts[k]);
        }
        free(ids);
        free(parts);
        return pc_fail(err, PC_ERR_NOMEM, "slice");
    }
    doc->slices.list = grown;
    memmove(&grown[pos + made], &grown[pos], (doc->slices.len - pos) * sizeof *grown);
    for (size_t k = 0; k < made; k++) {
        grown[pos + k] = parts[k];
    }
    doc->slices.len += made;
    free(parts);
    c->ids_out = ids;
    c->n_out = made;
    return PC_OK;
}

static pc_status run_divide(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "slice.divide";
    slice_target t = {0};
    pc_status rc = find_target(s, p, CMD, &t, err);
    if (rc != PC_OK) {
        return rc;
    }
    int64_t down = 1, across = 1;
    if (pc_engine_param_int(p, "horizontal", &down)) {
        down = down < 1 ? 1 : down > 1000 ? 1000 : down;
    } else {
        down = 1;
    }
    if (pc_engine_param_int(p, "vertical", &across)) {
        across = across < 1 ? 1 : across > 1000 ? 1000 : across;
    } else {
        across = 1;
    }
    if (down == 1 && across == 1) {
        return pc_engine_err_bad_params(
            err, CMD, "give \"horizontal\" (slices down) and/or \"vertical\" (slices across) > 1");
    }
    if (down * across > MAX_DIVIDE_PARTS) {
        return pc_engine_err_bad_params(
            err, CMD, "%lld x %lld = %lld parts is more than one division makes (%d)", (long long)down,
            (long long)across, (long long)(down * across), MAX_DIVIDE_PARTS);
    }
    /* slice rect for size check */
    pc_rect r;
    if (t.stored) {
        const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
        const pc_slice *sl = pc_slices_get(&doc->slices, t.id);
        if (sl == NULL) {
            return pc_engine_err_bad_params(err, CMD, "no such slice");
        }
        r = sl->rect;
    } else {
        r = t.rect;
    }
    if (pc_rect_width(r) < (uint32_t)across || pc_rect_height(r) < (uint32_t)down) {
        return pc_engine_err_bad_params(err, CMD, "the slice is too small to divide that many times");
    }
    divide_ctx c = {.t = t, .down = (int32_t)down, .across = (int32_t)across, .ids_out = NULL, .n_out = 0};
    rc = pc_engine_session_edit(s, "Divide Slice", edit_divide, &c, err);
    if (rc != PC_OK) {
        free(c.ids_out);
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(c.ids_out);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < c.n_out; i++) {
        yyjson_mut_arr_add_uint(rd, arr, c.ids_out[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "slices", arr);
    yyjson_mut_obj_add_uint(rd, root, "count", c.n_out);
    yyjson_mut_doc_set_root(rd, root);
    free(c.ids_out);
    return PC_OK;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_resolved_slice *rs = NULL;
    size_t nrs = 0;
    if (pc_slices_resolve(doc, &rs, &nrs, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_resolved_slices_free(rs, nrs);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nrs; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        add_resolved_json(rd, o, doc, &rs[i]);
        yyjson_mut_arr_append(arr, o);
    }
    pc_resolved_slices_free(rs, nrs);
    yyjson_mut_obj_add_val(rd, root, "slices", arr);
    yyjson_mut_obj_add_bool(rd, root, "locked", s->slices_locked);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* layer.newLayerBasedSlice. */
typedef struct {
    pc_layer_id layer;
    yyjson_val *p;
    pc_rect rect;
    uint32_t id_out;
} lbs_ctx;

static pc_status edit_layer_based(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    lbs_ctx *c = ctx;
    uint32_t id = 0;
    if (!pc_slices_next_id(&doc->slices, &id)) {
        return pc_engine_err_other(err, "slice id space exhausted");
    }
    size_t sneed = doc->slices.len + 1;
    if (sneed == 0 || sneed > SIZE_MAX / sizeof *doc->slices.list) {
        return pc_fail(err, PC_ERR_LIMIT, "slice");
    }
    pc_slice *grown = realloc(doc->slices.list, sneed * sizeof *doc->slices.list);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "slice");
    }
    doc->slices.list = grown;
    pc_slice sl;
    pc_slice_default(&sl);
    sl.id = id;
    sl.origin = PC_SLICE_LAYER;
    sl.has_layer = true;
    sl.layer = c->layer;
    sl.rect = c->rect;
    size_t tl;
    const char *tv;
    yyjson_val *v;
    if ((v = pc_json_get(c->p, "name")) != NULL && (tv = pc_json_as_str(v, &tl)) != NULL) {
        (void)pc_str_set(&sl.name, tv, tl, NULL);
    }
    if ((v = pc_json_get(c->p, "url")) != NULL && (tv = pc_json_as_str(v, &tl)) != NULL) {
        (void)pc_str_set(&sl.url, tv, tl, NULL);
    }
    if ((v = pc_json_get(c->p, "alt")) != NULL && (tv = pc_json_as_str(v, &tl)) != NULL) {
        (void)pc_str_set(&sl.alt, tv, tl, NULL);
    }
    doc->slices.list[doc->slices.len++] = sl;
    c->id_out = id;
    return PC_OK;
}

static pc_status run_layer_based(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.newLayerBasedSlice";
    pc_layer_id layer;
    pc_status rc = pc_engine_layer_param(s, p, &layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, layer);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, layer);
    }
    for (size_t i = 0; i < doc->slices.len; i++) {
        if (doc->slices.list[i].has_layer && doc->slices.list[i].layer == layer) {
            return pc_engine_err_bad_params(err, CMD, "the layer already has a layer-based slice");
        }
    }
    pc_rect rect = slice_layer_bounds(l);
    if (pc_rect_is_empty(rect)) {
        return pc_engine_err_other(err, "Could not create a layer based slice because the layer is empty");
    }
    lbs_ctx c = {.layer = layer, .p = p, .rect = rect, .id_out = 0};
    rc = pc_engine_session_edit(s, "New Layer Based Slice", edit_layer_based, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "slice", c.id_out);
    yyjson_mut_val *ra = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_int(rd, ra, rect.x0);
    yyjson_mut_arr_add_int(rd, ra, rect.y0);
    yyjson_mut_arr_add_uint(rd, ra, pc_rect_width(rect));
    yyjson_mut_arr_add_uint(rd, ra, pc_rect_height(rect));
    yyjson_mut_obj_add_val(rd, root, "rect", ra);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_lock(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool on;
    yyjson_val *v = pc_json_get(p, "on");
    if (v == NULL || !pc_json_as_bool(v, &on)) {
        on = !s->slices_locked;
    }
    s->slices_locked = on;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "locked", on);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status edit_clear(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    (void)ctx;
    (void)err;
    for (size_t i = 0; i < doc->slices.len; i++) {
        pc_slice_fini(&doc->slices.list[i]);
    }
    doc->slices.len = 0;
    return PC_OK;
}

static pc_status run_clear(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    size_t n = d != NULL ? pc_doc_arc_get(d->doc)->slices.len : 0;
    pc_status rc = pc_engine_session_edit(s, "Clear Slices", edit_clear, NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "cleared", n);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *const MENU_LAYER[] = {"Layer"};
static const char *const MENU_VIEW[] = {"View"};

static const pc_engine_command SPECS[] = {
    {"slice.new", "Slice Tool", NULL, 0, NULL,
     "{\"rect\":[x,y,w,h] | \"x\",\"y\",\"width\",\"height\", plus Slice Options} \xE2\x86\x92 {slice, "
     "number}",
     slice_unlocked, run_new, true},
    {"slice.fromGuides", "Slices From Guides", NULL, 0, NULL,
     "{} (replaces every slice by the grid of the canvas guides) \xE2\x86\x92 {slices}", slice_unlocked,
     run_from_guides, true},
    {"slice.set", "Slice Options\xE2\x80\xA6", NULL, 0, NULL,
     "{\"slice\":id | \"number\":n (an auto slice is promoted), Slice Options, \"rect\":[x,y,w,h]?} "
     "\xE2\x86\x92 the slice",
     slice_unlocked, run_set, true},
    {"slice.promote", "Promote", NULL, 0, NULL,
     "{\"slice\":id | \"number\":n} (auto or layer-based \xE2\x86\x92 user slice) \xE2\x86\x92 {slice}",
     slice_unlocked, run_promote, true},
    {"slice.delete", "Delete Slice", NULL, 0, NULL,
     "{\"slice\":id | \"number\":n | \"slices\":[id\xE2\x80\xA6]} \xE2\x86\x92 {deleted}", slice_has_slices,
     run_delete, true},
    {"slice.divide", "Divide Slice\xE2\x80\xA6", NULL, 0, NULL,
     "{\"slice\":id | \"number\":n,\"horizontal\":n=1 (slices down),\"vertical\":n=1 (slices across); "
     "at most 10000 parts} \xE2\x86\x92 {slices}",
     slice_unlocked, run_divide, true},
    {"slice.list", "List Slices", NULL, 0, NULL,
     "{} \xE2\x86\x92 {slices:[{number,id,origin:auto|layer|user,name,rect:[x,y,w,h],kind,layer,url,alt,"
     "\xE2\x80\xA6}], locked}",
     slice_has_doc, run_list, false},
    {"layer.newLayerBasedSlice", "New Layer Based Slice", MENU_LAYER, 1, NULL,
     "{\"layer\":id?,\"name\":str?,\"url\":str?,\"alt\":str?} (follows the layer's bounds with "
     "effects) \xE2\x86\x92 {slice, rect}",
     slice_has_layer, run_layer_based, true},
    {"view.lockSlices", "Lock Slices", MENU_VIEW, 1, NULL,
     "{\"on\":bool? (default: toggle)} \xE2\x86\x92 {locked}", slice_has_doc, run_lock, true},
    {"view.clearSlices", "Clear Slices", MENU_VIEW, 1, NULL,
     "{} (deletes every user and layer-based slice) \xE2\x86\x92 {cleared}", slice_has_slices, run_clear,
     true},
};

const pc_engine_command *pc_engine_slice_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}

bool pc_engine_slices_locked(const pc_engine_session *session)
{
    return session && session->slices_locked;
}
