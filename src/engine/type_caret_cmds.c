/* Headless caret geometry for type layers (port of crates/engine/src/type_caret_cmds.rs, E11):
 * type.hitTest, type.caret, type.navigate. Queries (journal: false). Positions are document
 * pixels; offsets are character indices. Routes through src/text layout + navigation. */
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

#include "base/json.h"
#include "base/str.h"
#include "geom/geom.h"
#include "text/engine.h"
#include "text/layout.h"
#include "text/layout_data.h"

#include "engine_internal.h"
#include "type_cmds.h" /* byte_at / char_at helpers */

const pc_engine_command *pc_engine_type_caret_specs(size_t *n);

static pc_status bad(pc_error *err, const char *cmd, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

static pc_status need_doc(const pc_engine_session *s, const pc_document **out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    *out = pc_doc_arc_get(st->doc);
    return PC_OK;
}

static pc_status req_f64(yyjson_val *p, const char *key, const char *cmd, double *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return bad(err, cmd, "missing `%s`", key);
    }
    if (!(pc_json_as_f64(v, out) && isfinite(*out))) {
        return bad(err, cmd, "`%s` must be a finite number", key);
    }
    return PC_OK;
}

/* *present false (no error) when key is absent or null. */
static pc_status opt_f64(yyjson_val *p, const char *key, const char *cmd, bool *present, double *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL || yyjson_is_null(v)) {
        *present = false;
        return PC_OK;
    }
    *present = true;
    return req_f64(p, key, cmd, out, err);
}

static pc_status req_index(yyjson_val *p, const char *key, const char *cmd, size_t *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return bad(err, cmd, "missing `%s`", key);
    }
    uint64_t n;
    if (!pc_json_as_u64(v, &n)) {
        return bad(err, cmd, "`%s` must be a character index", key);
    }
    *out = (size_t)n;
    return PC_OK;
}

static pc_status check_index(const char *text, size_t len, size_t index, const char *cmd, pc_error *err)
{
    size_t n = pc_engine_text_char_at(text, len, len);
    if (index > n) {
        return bad(err, cmd, "index %zu is past the end of the text (%zu characters)", index, n);
    }
    return PC_OK;
}

/* "layer" as a u64 id, else the active layer. */
static pc_status resolve_layer(const pc_engine_session *s, yyjson_val *p, const char *cmd, pc_layer_id *out,
                               pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "layer");
    if (v != NULL && !yyjson_is_null(v)) {
        uint64_t id;
        if (!pc_json_as_u64(v, &id)) {
            return bad(err, cmd, "`layer` must be a layer id");
        }
        *out = (pc_layer_id)id;
        return PC_OK;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL || !st->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    *out = st->active_layer;
    return PC_OK;
}

typedef struct {
    pc_layer_id id;
    const char *text;
    size_t tlen;
    pc_affine transform;
    bool has_ink;
    pc_rect ink;
    pc_text_layout layout;
} frame;

static void frame_fini(frame *f) { pc_text_layout_fini(&f->layout); }

static pc_status make_frame(const pc_document *doc, pc_layer_id id, const char *cmd, frame *out, pc_error *err)
{
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_TEXT) {
        return bad(err, cmd, "layer %llu is %s %s layer, not a type layer", (unsigned long long)id,
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
    }
    const pc_text_layer *t = &l->content.u.text;
    out->id = id;
    out->text = pc_str_c(&t->text);
    out->tlen = t->text.len;
    out->transform = t->transform;
    out->has_ink = false;
    if (t->has_cache) {
        pc_rect b = pc_surface_content_bounds(&t->cache);
        if (!pc_rect_is_empty(b)) {
            out->has_ink = true;
            out->ink = b;
        }
    }
    pc_text_layout_init(&out->layout);
    pc_text_engine *eng = NULL;
    if (pc_text_engine_shared_lock(&eng, NULL) == PC_OK) {
        (void)pc_text_engine_layout(eng, t, doc->resolution_dpi, &out->layout, NULL);
        pc_text_engine_shared_unlock();
    }
    return PC_OK;
}

static void to_text(pc_affine aff, double x, double y, float *tx, float *ty)
{
    pc_affine inv;
    if (!pc_affine_inverse(aff, &inv)) {
        inv = PC_AFFINE_IDENTITY;
    }
    pc_point p = pc_affine_apply(inv, pc_point_make(x, y));
    *tx = (float)p.x;
    *ty = (float)p.y;
}

static pc_status doc_point(pc_affine aff, float x, float y, const char *cmd, double out[2], pc_error *err)
{
    pc_point p = pc_affine_apply(aff, pc_point_make((double)x, (double)y));
    if (!(isfinite(p.x) && isfinite(p.y))) {
        return bad(err, cmd, "the caret is not a finite point");
    }
    out[0] = p.x;
    out[1] = p.y;
    return PC_OK;
}

static bool frame_contains(const frame *f, double x, double y)
{
    float tx, ty;
    to_text(f->transform, x, y, &tx, &ty);
    if (pc_text_point_inside(&f->layout, tx, ty, 0.0f)) {
        return true;
    }
    if (f->has_ink) {
        return x >= (double)f->ink.x0 && x <= (double)f->ink.x1 && y >= (double)f->ink.y0 && y <= (double)f->ink.y1;
    }
    return false;
}

/* Topmost visible type layer under the point. Returns true and fills *out (frame_fini it). */
static bool topmost(const pc_document *doc, double x, double y, frame *out)
{
    pc_walk w = {0};
    if (pc_document_walk(doc, &w, NULL) != PC_OK) {
        return false;
    }
    bool found = false;
    for (size_t ri = w.len; ri-- > 0;) {
        const pc_walk_item *it = &w.items[ri];
        if (it->layer->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        const size_t *path = &w.paths[it->path_start];
        size_t plen = it->depth + 1;
        bool shown = true;
        for (size_t k = 1; k <= plen; k++) {
            const pc_layer *a = pc_document_layer_at(doc, path, k);
            if (a == NULL || !a->visible) {
                shown = false;
                break;
            }
        }
        if (!shown) {
            continue;
        }
        frame f = {0};
        if (make_frame(doc, it->layer->id, "type.hitTest", &f, NULL) != PC_OK) {
            continue;
        }
        if (frame_contains(&f, x, y)) {
            *out = f;
            found = true;
            break;
        }
        frame_fini(&f);
    }
    pc_walk_fini(&w);
    return found;
}

/* Line-space column for a document x (keeps the caret's own column when x is absent). */
static float column(const pc_text_layout *l, pc_affine aff, const char *text, size_t tlen, size_t index,
                    bool have_x, double doc_x)
{
    float x, top, bottom;
    pc_text_layout_caret(l, pc_engine_text_byte_at(text, tlen, index), &x, &top, &bottom);
    if (!have_x) {
        return x;
    }
    double y = (double)(top + bottom) * 0.5;
    double a = aff.m[0], c = aff.m[2], e = aff.m[4];
    double coef, along;
    if (l->vertical) {
        coef = c;
        along = doc_x - e + a * y;
    } else {
        coef = a;
        along = doc_x - e - c * y;
    }
    if (fabs(coef) < 1e-8) {
        return x;
    }
    double solved = along / coef;
    return isfinite(solved) ? (float)solved : x;
}

static pc_status run_hit_test(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "type.hitTest";
    double x = 0, y = 0;
    if (req_f64(p, "x", CMD, &x, err) != PC_OK || req_f64(p, "y", CMD, &y, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    frame f = {0};
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv != NULL && !yyjson_is_null(lv)) {
        uint64_t id;
        if (!pc_json_as_u64(lv, &id)) {
            return bad(err, CMD, "`layer` must be a layer id");
        }
        if (make_frame(doc, (pc_layer_id)id, CMD, &f, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    } else if (!topmost(doc, x, y, &f)) {
        return bad(err, CMD, "no type layer under the point");
    }
    float tx, ty;
    to_text(f.transform, x, y, &tx, &ty);
    size_t index = 0, line = 0;
    pc_text_hit_char(&f.layout, f.text, f.tlen, tx, ty, &index, &line);
    bool inside = frame_contains(&f, x, y);
    pc_layer_id id = f.id;
    frame_fini(&f);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_obj_add_uint(rd, root, "index", index);
    yyjson_mut_obj_add_uint(rd, root, "line", line);
    yyjson_mut_obj_add_bool(rd, root, "inside", inside);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_caret(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "type.caret";
    size_t index = 0;
    if (req_index(p, "index", CMD, &index, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_layer_id id = 0;
    if (resolve_layer(s, p, CMD, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    frame f = {0};
    if (make_frame(doc, id, CMD, &f, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_status rc = check_index(f.text, f.tlen, index, CMD, err);
    if (rc != PC_OK) {
        frame_fini(&f);
        return rc;
    }
    size_t byte = pc_engine_text_byte_at(f.text, f.tlen, index);
    float seg[4];
    pc_text_layout_caret_segment(&f.layout, byte, seg);
    double p0[2] = {0}, p1[2] = {0};
    if (doc_point(f.transform, seg[0], seg[1], CMD, p0, err) != PC_OK ||
        doc_point(f.transform, seg[2], seg[3], CMD, p1, err) != PC_OK) {
        frame_fini(&f);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    size_t line = pc_text_line_index(&f.layout, byte);
    frame_fini(&f);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "index", index);
    yyjson_mut_obj_add_uint(rd, root, "line", line);
    yyjson_mut_val *seg_arr = yyjson_mut_arr(rd);
    yyjson_mut_val *a0 = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_real(rd, a0, p0[0]);
    yyjson_mut_arr_add_real(rd, a0, p0[1]);
    yyjson_mut_val *a1 = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_real(rd, a1, p1[0]);
    yyjson_mut_arr_add_real(rd, a1, p1[1]);
    yyjson_mut_arr_append(seg_arr, a0);
    yyjson_mut_arr_append(seg_arr, a1);
    yyjson_mut_obj_add_val(rd, root, "segment", seg_arr);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_navigate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "type.navigate";
    size_t index = 0;
    if (req_index(p, "index", CMD, &index, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    size_t ml = 0;
    const char *mov = pc_json_as_str(pc_json_get(p, "move"), &ml);
    if (mov == NULL) {
        return bad(err, CMD, "missing `move`");
    }
    bool have_x = false;
    double doc_x = 0.0;
    if (opt_f64(p, "x", CMD, &have_x, &doc_x, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_layer_id id = 0;
    if (resolve_layer(s, p, CMD, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    frame f = {0};
    if (make_frame(doc, id, CMD, &f, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_status rc = check_index(f.text, f.tlen, index, CMD, err);
    if (rc != PC_OK) {
        frame_fini(&f);
        return rc;
    }
    size_t next;
    if (ml == 8 && memcmp(mov, "wordPrev", 8) == 0) {
        next = pc_text_word_boundary(f.text, f.tlen, index, false);
    } else if (ml == 8 && memcmp(mov, "wordNext", 8) == 0) {
        next = pc_text_word_boundary(f.text, f.tlen, index, true);
    } else if ((ml == 8 && memcmp(mov, "linePrev", 8) == 0) || (ml == 8 && memcmp(mov, "lineNext", 8) == 0)) {
        int dir = mov[4] == 'P' ? -1 : 1;
        float x = column(&f.layout, f.transform, f.text, f.tlen, index, have_x, doc_x);
        next = pc_text_line_step(&f.layout, f.text, f.tlen, index, x, dir);
    } else if (ml == 9 && memcmp(mov, "lineStart", 9) == 0) {
        next = pc_text_line_edge(&f.layout, f.text, f.tlen, index, false);
    } else if (ml == 7 && memcmp(mov, "lineEnd", 7) == 0) {
        next = pc_text_line_edge(&f.layout, f.text, f.tlen, index, true);
    } else if (ml == 5 && memcmp(mov, "start", 5) == 0) {
        next = 0;
    } else if (ml == 3 && memcmp(mov, "end", 3) == 0) {
        next = pc_engine_text_char_at(f.text, f.tlen, f.tlen);
    } else {
        frame_fini(&f);
        return bad(err, CMD,
                   "move \"%.*s\" must be wordPrev, wordNext, linePrev, lineNext, lineStart, lineEnd, start or end",
                   (int)ml, mov);
    }
    frame_fini(&f);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "index", next);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"type.hitTest", "Hit-Test Type", NULL, 0, NULL,
     "{\"layer\":id?, \"x\":px, \"y\":px} \xe2\x86\x92 {\"layer\",\"index\":char,\"line\",\"inside\":bool}. No "
     "layer: the topmost visible type layer whose laid-out text or rendered pixels contain the point (same order as "
     "the Type tool); a miss is an error. With a layer, index is the nearest caret even when inside is false",
     pc_engine_has_doc, run_hit_test, false},
    {"type.caret", "Type Caret", NULL, 0, NULL,
     "{\"layer\":id?, \"index\":char (0..=length; empty text is 0)} \xe2\x86\x92 "
     "{\"index\",\"line\",\"segment\":[[x,y],[x,y]]} caret segment in document pixels, for rotated and vertical "
     "type too",
     pc_engine_has_doc, run_caret, false},
    {"type.navigate", "Navigate Type", NULL, 0, NULL,
     "{\"layer\":id?, \"index\":char, "
     "\"move\":\"wordPrev|wordNext|linePrev|lineNext|lineStart|lineEnd|start|end\", \"x\":px?} \xe2\x86\x92 "
     "{\"index\"}. Words are alphanumeric runs. x is the document x to keep across linePrev/lineNext (a caret "
     "segment's x); omitted uses the caret's own column. Empty text returns index 0",
     pc_engine_has_doc, run_navigate, false},
};

const pc_engine_command *pc_engine_type_caret_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
