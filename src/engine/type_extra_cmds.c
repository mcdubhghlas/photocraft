/* The Type menu (port of crates/engine/src/type_extra_cmds.rs, E11): anti-aliasing, orientation,
 * point <-> paragraph conversion, Create Work Path, Convert to Shape, Rasterize, Warp Text, the
 * OpenType toggles, Paste Lorem Ipsum, Update All Text Layers, missing-font replacement and the
 * default type styles.
 *
 * Everything edits the typed text model and re-renders through pc_engine_type_refresh, so the PSD
 * TySh data saves as editable text. Routes through src/text.
 *
 * type.attachToPath / type.detachFromPath are PERMANENTLY DEFERRED: they need a doc-model
 * TextShape::Path variant src/doc lacks (only Point/Box). */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/json.h"
#include "base/str.h"
#include "color/color.h"
#include "doc/fill.h"
#include "doc/json.h"
#include "doc/vector.h"
#include "text/engine.h"
#include "text/fonts.h"
#include "text/layout.h"
#include "text/layout_data.h"
#include "text/render.h"
#include "text/warp.h"

#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */
#include "type_cmds.h"
#include "type_extra_cmds.h"
#include "vector_cmds.h" /* pc_engine_refresh_shape */

#define LOREM_IPSUM                                                                                                    \
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore "  \
    "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo " \
    "consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "        \
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est "\
    "laborum."

/* ---- missing fonts ------------------------------------------------------------------------ */

static bool strlist_has(const pc_strlist *l, const char *s, size_t len)
{
    for (size_t i = 0; i < l->n; i++) {
        if (strlen(l->v[i]) == len && memcmp(l->v[i], s, len) == 0) {
            return true;
        }
    }
    return false;
}

/* Appends (deduped) the families `t` uses that `db` cannot supply to *fams. */
static pc_status collect_missing_layer(pc_fontdb *db, const pc_text_layer *t, pc_strlist *fams, pc_error *err)
{
    pc_text_run *cr = NULL;
    size_t ncr = 0;
    if (pc_text_layer_char_runs(t, &cr, &ncr, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;
    for (size_t i = 0; i < ncr; i++) {
        const char *f = pc_str_c(&cr[i].style.font_family);
        size_t fl = cr[i].style.font_family.len;
        if (fl > 0 && !pc_fontdb_has_family(db, f, fl) && !strlist_has(fams, f, fl)) {
            rc = pc_strlist_push(fams, f, fl, err);
            if (rc != PC_OK) {
                break;
            }
        }
    }
    pc_text_runs_free(cr, ncr);
    return rc;
}

static pc_status warnings_from(const pc_strlist *fams, pc_strlist *out, pc_error *err)
{
    for (size_t i = 0; i < fams->n; i++) {
        char buf[320];
        int w = snprintf(buf, sizeof buf, "font '%s' is not installed; text may render using a fallback face",
                         fams->v[i]);
        if (w < 0) {
            continue;
        }
        if (pc_strlist_push(out, buf, (size_t)w < sizeof buf ? (size_t)w : sizeof buf - 1, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

pc_status pc_engine_missing_font_warnings_for_layer(const pc_text_layer *t, pc_strlist *out, pc_error *err)
{
    pc_text_engine *eng = NULL;
    if (pc_text_engine_shared_lock(&eng, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_strlist fams = {0};
    pc_status rc = collect_missing_layer(pc_text_engine_fonts(eng), t, &fams, err);
    pc_text_engine_shared_unlock();
    if (rc == PC_OK) {
        rc = warnings_from(&fams, out, err);
    }
    pc_strlist_fini(&fams);
    return rc;
}

/* Missing families across the whole document (deduped), into *fams. */
static pc_status missing_fonts_doc(const pc_document *doc, pc_strlist *fams, pc_error *err)
{
    pc_text_engine *eng = NULL;
    if (pc_text_engine_shared_lock(&eng, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_fontdb *db = pc_text_engine_fonts(eng);
    pc_walk w = {0};
    pc_status rc = pc_document_walk(doc, &w, err);
    if (rc == PC_OK) {
        for (size_t i = 0; i < w.len && rc == PC_OK; i++) {
            const pc_layer *l = w.items[i].layer;
            if (l->content.kind == PC_LAYER_TEXT) {
                rc = collect_missing_layer(db, &l->content.u.text, fams, err);
            }
        }
        pc_walk_fini(&w);
    }
    pc_text_engine_shared_unlock();
    return rc;
}

/* ---- predicates --------------------------------------------------------------------------- */

static const pc_text_layer *active_text(const pc_engine_session *s)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL || !st->has_active_layer) {
        return NULL;
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer);
    if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
        return NULL;
    }
    return &l->content.u.text;
}

static pc_status en_has_doc(const pc_engine_session *s, pc_error *r)
{
    return pc_engine_session_active(s) != NULL ? PC_OK : pc_engine_err_other(r, "no document open");
}

static pc_status en_has_text(const pc_engine_session *s, pc_error *r)
{
    return active_text(s) != NULL ? PC_OK : pc_engine_err_other(r, "the active layer is not a type layer");
}

static pc_status en_has_point_text(const pc_engine_session *s, pc_error *r)
{
    const pc_text_layer *t = active_text(s);
    if (t == NULL) {
        return pc_engine_err_other(r, "the active layer is not a type layer");
    }
    return t->shape.is_box ? pc_engine_err_other(r, "the type layer is already paragraph text") : PC_OK;
}

static pc_status en_has_box_text(const pc_engine_session *s, pc_error *r)
{
    const pc_text_layer *t = active_text(s);
    if (t == NULL) {
        return pc_engine_err_other(r, "the active layer is not a type layer");
    }
    return t->shape.is_box ? PC_OK : pc_engine_err_other(r, "the type layer is already point text");
}

static pc_status en_any_text(const pc_engine_session *s, pc_error *r)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_other(r, "no document open");
    }
    pc_walk w = {0};
    if (pc_document_walk(pc_doc_arc_get(st->doc), &w, NULL) != PC_OK) {
        return pc_engine_err_other(r, "the document has no type layers");
    }
    bool any = false;
    for (size_t i = 0; i < w.len; i++) {
        if (w.items[i].layer->content.kind == PC_LAYER_TEXT) {
            any = true;
            break;
        }
    }
    pc_walk_fini(&w);
    return any ? PC_OK : pc_engine_err_other(r, "the document has no type layers");
}

static pc_status en_has_defaults(const pc_engine_session *s, pc_error *r)
{
    pc_status rc = pc_engine_has_format_target(s, r);
    if (rc != PC_OK) {
        return rc;
    }
    return s->has_type_defaults ? PC_OK : pc_engine_err_other(r, "no default type styles have been saved");
}

/* ---- single-layer edit + geometry helpers ------------------------------------------------- */

typedef pc_status (*tx_fn)(pc_text_layer *t, const pc_document *doc, void *ud, pc_error *err);

typedef struct {
    pc_layer_id id;
    tx_fn f;
    void *ud;
} tx_ctx;

static pc_status tx_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    tx_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    bool auto_named = pc_engine_is_auto_named(l);
    if (l->content.kind != PC_LAYER_TEXT) {
        return pc_engine_err_other(err, "layer %llu is %s %s layer, not a type layer", (unsigned long long)c->id,
                                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
    }
    pc_text_layer *t = &l->content.u.text;
    pc_status rc = c->f(t, doc, c->ud, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_type_refresh(doc, t);
    if (auto_named) {
        char nm[256];
        size_t w = pc_engine_type_layer_name(pc_str_c(&t->text), t->text.len, nm, sizeof nm);
        return pc_str_set(&l->name, nm, w, err);
    }
    return PC_OK;
}

static pc_status tx_with_text(pc_engine_session *s, yyjson_val *p, const char *label, tx_fn f, void *ud,
                              pc_error *err)
{
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (pc_engine_check_text_unlocked(pc_doc_arc_get(st->doc), id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    tx_ctx ctx = {id, f, ud};
    return pc_engine_session_edit(s, label, tx_edit, &ctx, err);
}

/* m then translate(tx, ty). */
static pc_affine then_translate(const pc_affine *m, double tx, double ty)
{
    const double *a = m->m;
    pc_affine r;
    r.m[0] = a[0];
    r.m[1] = a[1];
    r.m[2] = a[2];
    r.m[3] = a[3];
    r.m[4] = a[0] * tx + a[2] * ty + a[4];
    r.m[5] = a[1] * tx + a[3] * ty + a[5];
    return r;
}

/* Lays out t for doc into *out; returns false and leaves *out empty on failure. */
static bool layout_of(const pc_document *doc, const pc_text_layer *t, pc_text_layout *out)
{
    pc_text_engine *eng = NULL;
    if (pc_text_engine_shared_lock(&eng, NULL) != PC_OK) {
        pc_text_layout_init(out);
        return false;
    }
    bool ok = pc_text_engine_layout(eng, t, doc->resolution_dpi, out, NULL) == PC_OK;
    pc_text_engine_shared_unlock();
    if (!ok) {
        pc_text_layout_init(out);
    }
    return ok;
}

static pc_text_align first_align(const pc_text_layer *t)
{
    pc_paragraph_run *pr = NULL;
    size_t n = 0;
    pc_text_align a = PC_TEXT_ALIGN_LEFT;
    if (pc_text_layer_paragraph_runs(t, &pr, &n, NULL) == PC_OK) {
        if (n > 0) {
            a = pr[0].style.align;
        }
        free(pr);
    }
    return a;
}

/* ---- anti-aliasing / orientation ---------------------------------------------------------- */

static const char *aa_name(pc_antialias a)
{
    switch (a) {
    case PC_AA_NONE: return "none";
    case PC_AA_SHARP: return "sharp";
    case PC_AA_CRISP: return "crisp";
    case PC_AA_STRONG: return "strong";
    case PC_AA_SMOOTH: return "smooth";
    case PC_AA_WINDOWS: return "windows";
    case PC_AA_WINDOWS_LCD: return "windowsLcd";
    }
    return "smooth";
}

static pc_status set_aa_cb(pc_text_layer *t, size_t a, size_t b, void *ud, pc_error *err)
{
    (void)a;
    (void)b;
    (void)err;
    t->antialias = *(pc_antialias *)ud;
    return PC_OK;
}

static pc_status one_key_result(pc_json_value *out, const char *key, const char *val, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, key, val);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status set_aa(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_antialias aa, pc_error *err)
{
    if (pc_engine_with_text_targets(s, p, "Anti-Alias", set_aa_cb, &aa, NULL, NULL, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return one_key_result(out, "antialias", aa_name(aa), err);
}

static pc_status set_orient_cb(pc_text_layer *t, size_t a, size_t b, void *ud, pc_error *err)
{
    (void)a;
    (void)b;
    (void)err;
    t->orientation = *(pc_orientation *)ud;
    return PC_OK;
}

static pc_status set_orientation(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_orientation o,
                                 pc_error *err)
{
    if (pc_engine_with_text_targets(s, p, "Change Text Orientation", set_orient_cb, &o, NULL, NULL, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return one_key_result(out, "orientation", o == PC_ORIENT_VERTICAL ? "vertical" : "horizontal", err);
}

/* ---- OpenType toggles --------------------------------------------------------------------- */

static bool feature_on(const pc_char_style *st, const char *tag)
{
    if (strcmp(tag, "liga") == 0) {
        return st->ligatures;
    }
    if (strcmp(tag, "dlig") == 0) {
        return st->discretionary_ligatures;
    }
    for (size_t i = 0; i < st->nfeatures; i++) {
        if (strcmp(pc_str_c(&st->features[i].tag), tag) == 0 && st->features[i].value > 0) {
            return true;
        }
    }
    return false;
}

typedef struct {
    const char *tag;
    bool on;
} feat_ctx;

static void set_feature(pc_char_style *st, void *ud)
{
    feat_ctx *c = ud;
    if (strcmp(c->tag, "liga") == 0) {
        st->ligatures = c->on;
        return;
    }
    if (strcmp(c->tag, "dlig") == 0) {
        st->discretionary_ligatures = c->on;
        return;
    }
    size_t tl = strlen(c->tag);
    for (size_t i = 0; i < st->nfeatures;) {
        if (strcmp(pc_str_c(&st->features[i].tag), c->tag) == 0) {
            pc_str_fini(&st->features[i].tag);
            memmove(&st->features[i], &st->features[i + 1], (st->nfeatures - i - 1) * sizeof st->features[0]);
            st->nfeatures--;
        } else {
            i++;
        }
    }
    if (c->on) {
        pc_font_feature *nf = realloc(st->features, (st->nfeatures + 1) * sizeof *nf);
        if (nf == NULL) {
            return;
        }
        st->features = nf;
        st->features[st->nfeatures].tag = (pc_str){0};
        if (pc_str_set(&st->features[st->nfeatures].tag, c->tag, tl, NULL) == PC_OK) {
            st->features[st->nfeatures].value = 1;
            st->nfeatures++;
        }
    }
}

static pc_status toggle_feature_cb(pc_text_layer *t, size_t a, size_t b, void *ud, pc_error *err)
{
    (void)err;
    pc_engine_style_range(t, a < b ? a : b, a < b ? b : a, set_feature, ud);
    return PC_OK;
}

static pc_status toggle_opentype(pc_engine_session *s, yyjson_val *p, pc_json_value *out, const char *tag,
                                 pc_error *err)
{
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (pc_engine_format_targets(s, p, &ids, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_layer_id rep = ids[0];
    if (st->has_active_layer) {
        for (size_t i = 0; i < n; i++) {
            if (ids[i] == st->active_layer) {
                rep = st->active_layer;
                break;
            }
        }
    }
    free(ids);
    bool cur = false;
    const pc_layer *l = pc_document_layer(doc, rep);
    if (l != NULL && l->content.kind == PC_LAYER_TEXT) {
        pc_text_run *cr = NULL;
        size_t ncr = 0;
        if (pc_text_layer_char_runs(&l->content.u.text, &cr, &ncr, NULL) == PC_OK) {
            if (ncr > 0) {
                cur = feature_on(&cr[0].style, tag);
            }
            pc_text_runs_free(cr, ncr);
        }
    }
    bool on;
    if (!pc_json_as_bool(pc_json_get(p, "on"), &on)) {
        on = !cur;
    }
    feat_ctx fc = {tag, on};
    if (pc_engine_with_text_targets(s, p, "Change OpenType Feature", toggle_feature_cb, &fc, NULL, NULL, err) !=
        PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "feature", tag);
    yyjson_mut_obj_add_bool(rd, root, "on", on);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- point <-> paragraph ------------------------------------------------------------------ */

typedef struct {
    double box[4];
    bool have;
    size_t inserted;
} conv_out;

static float max_size_px(const pc_text_layer *t, float dpi)
{
    float m = t->size_pt;
    pc_text_run *cr = NULL;
    size_t n = 0;
    if (pc_text_layer_char_runs(t, &cr, &n, NULL) == PC_OK) {
        for (size_t i = 0; i < n; i++) {
            if (cr[i].style.size_pt > m) {
                m = cr[i].style.size_pt;
            }
        }
        pc_text_runs_free(cr, n);
    }
    return m * dpi / 72.0f;
}

static pc_status to_paragraph_cb(pc_text_layer *t, const pc_document *doc, void *ud, pc_error *err)
{
    (void)err;
    conv_out *o = ud;
    pc_text_layout l;
    layout_of(doc, t, &l);
    float size_px = max_size_px(t, doc->resolution_dpi);
    pc_paragraph_run *pr = NULL;
    size_t npr = 0;
    pc_paragraph_style ps = pc_paragraph_style_default();
    if (pc_text_layer_paragraph_runs(t, &pr, &npr, NULL) == PC_OK) {
        if (npr > 0) {
            ps = pr[0].style;
        }
        free(pr);
    }
    float indents = (ps.start_indent_pt + ps.end_indent_pt);
    if (indents < 0.0f) {
        indents = 0.0f;
    }
    indents = indents * doc->resolution_dpi / 72.0f;
    float slack = size_px * 0.5f;
    if (l.vertical) {
        float lb[4];
        if (!pc_text_layout_line_bounds(&l, lb)) {
            lb[0] = 0.0f;
            lb[1] = -size_px / 2.0f;
            lb[2] = size_px;
            lb[3] = size_px / 2.0f;
        }
        float u0 = lb[0], v0 = lb[1], u1 = lb[2], v1 = lb[3];
        float height = (u1 - u0 > 1.0f ? u1 - u0 : 1.0f) + indents + (slack > 4.0f ? slack : 4.0f);
        float width = (v1 - v0 > 1.0f ? v1 - v0 : 1.0f) + size_px * 0.5f;
        float y = (ps.align == PC_TEXT_ALIGN_CENTER || ps.align == PC_TEXT_ALIGN_JUSTIFY_CENTER) ? -height / 2.0f
                  : (ps.align == PC_TEXT_ALIGN_RIGHT || ps.align == PC_TEXT_ALIGN_JUSTIFY_RIGHT) ? -height
                                                                                                 : 0.0f;
        float x = -v0 - width;
        t->shape.is_box = true;
        t->shape.x = 0.0f;
        t->shape.y = 0.0f;
        t->shape.width = width;
        t->shape.height = height;
        t->transform = then_translate(&t->transform, x, y);
        o->box[0] = x;
        o->box[1] = y;
        o->box[2] = width;
        o->box[3] = height;
        o->have = true;
        pc_text_layout_fini(&l);
        return PC_OK;
    }
    float b[4];
    if (!pc_text_layout_bounds(&l, b)) {
        b[0] = 0.0f;
        b[1] = -t->size_pt;
        b[2] = t->size_pt;
        b[3] = 0.0f;
    }
    float x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
    float width = (x1 - x0 > 1.0f ? x1 - x0 : 1.0f) + indents + (slack > 4.0f ? slack : 4.0f);
    float height = (y1 - y0 > 1.0f ? y1 - y0 : 1.0f) + size_px * 0.5f;
    float x = (ps.align == PC_TEXT_ALIGN_CENTER || ps.align == PC_TEXT_ALIGN_JUSTIFY_CENTER) ? -width / 2.0f
              : (ps.align == PC_TEXT_ALIGN_RIGHT || ps.align == PC_TEXT_ALIGN_JUSTIFY_RIGHT) ? -width
                                                                                             : 0.0f;
    pc_text_layout_fini(&l);
    /* Probe the first baseline for a box placed at (x, y0). */
    pc_text_layer probe;
    float b0 = 0.0f;
    if (pc_text_layer_clone(t, &probe, NULL) == PC_OK) {
        probe.shape.is_box = true;
        probe.shape.x = x;
        probe.shape.y = y0;
        probe.shape.width = width;
        probe.shape.height = height;
        pc_text_layout pl;
        layout_of(doc, &probe, &pl);
        if (pl.nlines > 0) {
            b0 = pl.lines[0].baseline;
        }
        pc_text_layout_fini(&pl);
        pc_text_layer_fini(&probe);
    }
    float y = y0 - b0;
    t->shape.is_box = true;
    t->shape.x = 0.0f;
    t->shape.y = 0.0f;
    t->shape.width = width;
    t->shape.height = height;
    t->transform = then_translate(&t->transform, x, y);
    o->box[0] = x;
    o->box[1] = y;
    o->box[2] = width;
    o->box[3] = height;
    o->have = true;
    return PC_OK;
}

static pc_status run_to_paragraph(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    conv_out o = {{0, 0, 0, 0}, false, 0};
    if (tx_with_text(s, p, "Convert to Paragraph Text", to_paragraph_cb, &o, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *ba = yyjson_mut_arr(rd);
    for (size_t i = 0; i < 4; i++) {
        yyjson_mut_arr_add_real(rd, ba, o.box[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "box", ba);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static int cmp_size_t(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static pc_status to_point_cb(pc_text_layer *t, const pc_document *doc, void *ud, pc_error *err)
{
    (void)err;
    conv_out *o = ud;
    if (!t->shape.is_box) {
        return pc_engine_err_other(err, "already point text");
    }
    pc_text_layout l;
    layout_of(doc, t, &l);
    float x = t->shape.x, width = t->shape.width;
    if (l.vertical) {
        x = t->shape.y;
        width = t->shape.height;
    }
    float baseline = l.nlines > 0 ? l.lines[0].baseline : 0.0f;
    /* Soft breaks: adjacent lines in the same paragraph. */
    size_t *breaks = l.nlines > 1 ? malloc((l.nlines - 1) * sizeof *breaks) : NULL;
    size_t nb = 0;
    for (size_t i = 0; i + 1 < l.nlines; i++) {
        if (l.lines[i].paragraph == l.lines[i + 1].paragraph) {
            breaks[nb++] = l.lines[i + 1].start;
        }
    }
    if (nb > 1) {
        qsort(breaks, nb, sizeof *breaks, cmp_size_t);
        size_t w = 0;
        for (size_t i = 0; i < nb; i++) {
            if (i == 0 || breaks[i] != breaks[i - 1]) {
                breaks[w++] = breaks[i];
            }
        }
        nb = w;
    }
    for (size_t i = nb; i-- > 0;) {
        pc_engine_replace_text(t, breaks[i], breaks[i], "\n", 1);
    }
    free(breaks);
    pc_text_align al = first_align(t);
    float ax = (al == PC_TEXT_ALIGN_CENTER || al == PC_TEXT_ALIGN_JUSTIFY_CENTER) ? x + width / 2.0f
               : (al == PC_TEXT_ALIGN_RIGHT || al == PC_TEXT_ALIGN_JUSTIFY_RIGHT) ? x + width
                                                                                  : x;
    t->shape.is_box = false;
    float tx = 0.0f, ty = 0.0f;
    pc_text_layout_to_text(&l, ax, baseline, &tx, &ty);
    t->transform = then_translate(&t->transform, tx, ty);
    pc_text_layout_fini(&l);
    o->inserted = nb;
    return PC_OK;
}

static pc_status run_to_point(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    conv_out o = {{0, 0, 0, 0}, false, 0};
    if (tx_with_text(s, p, "Convert to Point Text", to_point_cb, &o, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "inserted", o.inserted);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- outlines -> path --------------------------------------------------------------------- */

typedef struct {
    pc_knot *k;
    size_t n, cap;
} kvec;

static void kvec_push(kvec *v, pc_knot k)
{
    if (v->n == v->cap) {
        size_t c = v->cap ? v->cap * 2 : 8;
        pc_knot *nk = realloc(v->k, c * sizeof *nk);
        if (nk == NULL) {
            return;
        }
        v->k = nk;
        v->cap = c;
    }
    v->k[v->n++] = k;
}

static double signed_area(const pc_knot *k, size_t n)
{
    double s = 0.0;
    for (size_t i = 0; i < n; i++) {
        pc_point a = k[i].anchor, b = k[(i + 1) % n].anchor;
        s += a.x * b.y - b.x * a.y;
    }
    return s / 2.0;
}

static void contour_finish(kvec *cur, kvec **out, size_t *nout, size_t *cap)
{
    if (cur->n >= 2) {
        pc_knot f = cur->k[0], l = cur->k[cur->n - 1];
        if (fabs(f.anchor.x - l.anchor.x) < 1e-9 && fabs(f.anchor.y - l.anchor.y) < 1e-9) {
            cur->k[0].in_ctrl = l.in_ctrl;
            cur->n--;
        }
    }
    if (cur->n >= 2) {
        if (*nout == *cap) {
            size_t c = *cap ? *cap * 2 : 8;
            kvec *n = realloc(*out, c * sizeof **out);
            if (n == NULL) {
                free(cur->k);
                cur->k = NULL;
                cur->n = cur->cap = 0;
                return;
            }
            *out = n;
            *cap = c;
        }
        (*out)[(*nout)++] = *cur;
    } else {
        free(cur->k);
    }
    cur->k = NULL;
    cur->n = cur->cap = 0;
}

/* The type layer's glyph outlines as a document-space path (Create Work Path / Convert to Shape).
 * *out is initialised; returns PC_OK (an empty path on failure). */
static pc_status text_path(const pc_document *doc, const pc_text_layer *t, pc_path *out, pc_error *err)
{
    (void)err;
    memset(out, 0, sizeof *out);
    out->fill_rule = PC_FILL_RULE_NONZERO;
    pc_text_layout l;
    layout_of(doc, t, &l);
    pc_style_warp sw;
    bool has_warp = t->has_warp && pc_text_layout_warp(&l, &t->warp, &sw);
    pc_glyph_outline *glyphs = NULL;
    size_t ng = 0;
    (void)pc_text_outlines(&l, &t->transform, has_warp ? &sw : NULL, &glyphs, &ng, NULL);
    pc_text_layout_fini(&l);

    pc_subpath *subs = NULL;
    size_t nsubs = 0, scap = 0;
    for (size_t g = 0; g < ng; g++) {
        kvec cur = {0};
        kvec *cs = NULL;
        size_t ncs = 0, ccap = 0;
        for (size_t e = 0; e < glyphs[g].n; e++) {
            const pc_path_el *el = &glyphs[g].els[e];
            switch (el->kind) {
            case PC_PATH_MOVE:
                contour_finish(&cur, &cs, &ncs, &ccap);
                kvec_push(&cur, pc_knot_corner(el->p[0][0], el->p[0][1]));
                break;
            case PC_PATH_LINE:
                kvec_push(&cur, pc_knot_corner(el->p[0][0], el->p[0][1]));
                break;
            case PC_PATH_QUAD: {
                if (cur.n == 0) {
                    break;
                }
                pc_knot *last = &cur.k[cur.n - 1];
                pc_point s = last->anchor;
                double cx = el->p[0][0], cy = el->p[0][1], ax = el->p[1][0], ay = el->p[1][1];
                last->out_ctrl = pc_point_make(s.x + 2.0 / 3.0 * (cx - s.x), s.y + 2.0 / 3.0 * (cy - s.y));
                pc_knot k = {pc_point_make(ax, ay), pc_point_make(ax + 2.0 / 3.0 * (cx - ax), ay + 2.0 / 3.0 * (cy - ay)),
                             pc_point_make(ax, ay), false};
                kvec_push(&cur, k);
                break;
            }
            case PC_PATH_CURVE: {
                if (cur.n == 0) {
                    break;
                }
                pc_knot *last = &cur.k[cur.n - 1];
                last->out_ctrl = pc_point_make(el->p[0][0], el->p[0][1]);
                pc_knot k = {pc_point_make(el->p[2][0], el->p[2][1]), pc_point_make(el->p[1][0], el->p[1][1]),
                             pc_point_make(el->p[2][0], el->p[2][1]), false};
                kvec_push(&cur, k);
                break;
            }
            case PC_PATH_CLOSE:
                contour_finish(&cur, &cs, &ncs, &ccap);
                break;
            }
        }
        contour_finish(&cur, &cs, &ncs, &ccap);
        if (ncs == 0) {
            free(cs);
            continue;
        }
        double outer = 0.0;
        for (size_t i = 0; i < ncs; i++) {
            double ar = signed_area(cs[i].k, cs[i].n);
            if (fabs(ar) > fabs(outer)) {
                outer = ar;
            }
        }
        double osign = outer < 0.0 ? -1.0 : outer > 0.0 ? 1.0 : 0.0;
        for (size_t i = 0; i < ncs; i++) {
            double ar = signed_area(cs[i].k, cs[i].n);
            double sg = ar < 0.0 ? -1.0 : ar > 0.0 ? 1.0 : 0.0;
            pc_subpath sp = {true, cs[i].k, cs[i].n, sg == osign ? PC_PATH_COMBINE : PC_PATH_SUBTRACT};
            if (nsubs == scap) {
                size_t c = scap ? scap * 2 : 8;
                pc_subpath *ns = realloc(subs, c * sizeof *ns);
                if (ns == NULL) {
                    free(cs[i].k);
                    continue;
                }
                subs = ns;
                scap = c;
            }
            subs[nsubs++] = sp;
        }
        free(cs);
    }
    pc_glyph_outlines_free(glyphs, ng);
    out->subpaths = subs;
    out->nsubpaths = nsubs;
    return PC_OK;
}

typedef struct {
    pc_path path; /* moved into doc->work_path */
    size_t n;
} wp_ctx;

static pc_status edit_work_path(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    wp_ctx *c = vctx;
    if (doc->has_work_path) {
        pc_path_fini(&doc->work_path);
    }
    doc->work_path = c->path;
    doc->has_work_path = true;
    memset(&c->path, 0, sizeof c->path);
    return PC_OK;
}

static pc_status run_create_work_path(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
        return pc_engine_err_other(err, "not a type layer");
    }
    wp_ctx ctx = {{0}, 0};
    if (text_path(doc, &l->content.u.text, &ctx.path, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    ctx.n = ctx.path.nsubpaths;
    pc_status rc = pc_engine_session_edit(s, "Create Work Path", edit_work_path, &ctx, err);
    pc_path_fini(&ctx.path);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "subpaths", ctx.n);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

typedef struct {
    pc_layer_id id;
    size_t n;
} cshape_ctx;

static pc_status edit_convert_shape(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    cshape_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (l->content.kind != PC_LAYER_TEXT) {
        return pc_engine_err_other(err, "not a type layer");
    }
    const pc_text_layer *t = &l->content.u.text;
    pc_color color = t->color;
    pc_text_run *cr = NULL;
    size_t ncr = 0;
    if (pc_text_layer_char_runs(t, &cr, &ncr, NULL) == PC_OK) {
        if (ncr > 0) {
            color = cr[0].style.color;
        }
        pc_text_runs_free(cr, ncr);
    }
    pc_shape_layer sh;
    memset(&sh, 0, sizeof sh);
    if (text_path(doc, t, &sh.path, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    sh.has_fill = true;
    sh.fill = pc_fill_solid(color);
    pc_engine_refresh_shape(doc, &sh);
    c->n = sh.path.nsubpaths;
    pc_layer_content_fini(&l->content);
    l->content.kind = PC_LAYER_SHAPE;
    l->content.u.shape = sh;
    for (size_t i = 0; i < l->npsd_blocks;) {
        if (memcmp(l->psd_blocks[i].key, "TySh", 4) == 0) {
            pc_blob_release(l->psd_blocks[i].data);
            memmove(&l->psd_blocks[i], &l->psd_blocks[i + 1], (l->npsd_blocks - i - 1) * sizeof l->psd_blocks[0]);
            l->npsd_blocks--;
        } else {
            i++;
        }
    }
    return PC_OK;
}

static pc_status run_convert_to_shape(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (pc_engine_layer_param(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    cshape_ctx ctx = {id, 0};
    if (pc_engine_session_edit(s, "Convert to Shape", edit_convert_shape, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_obj_add_uint(rd, root, "subpaths", ctx.n);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- warp text ---------------------------------------------------------------------------- */

typedef struct {
    bool has;
    pc_text_warp warp;
} warp_ctx;

static pc_status set_warp_cb(pc_text_layer *t, size_t a, size_t b, void *ud, pc_error *err)
{
    (void)a;
    (void)b;
    warp_ctx *c = ud;
    if (t->has_warp) {
        pc_text_warp_fini(&t->warp);
        t->has_warp = false;
    }
    if (c->has) {
        if (pc_text_warp_clone(&c->warp, &t->warp, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        t->has_warp = true;
    }
    return PC_OK;
}

static float clamp_pct(yyjson_val *p, const char *k, double def)
{
    double v;
    if (!pc_json_as_f64(pc_json_get(p, k), &v)) {
        v = def;
    }
    v = v < -100.0 ? -100.0 : v > 100.0 ? 100.0 : v;
    return (float)v;
}

static pc_status run_warp_text(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t sl = 0;
    const char *style = pc_json_as_str(pc_json_get(p, "style"), &sl);
    if (style == NULL) {
        style = "arc";
        sl = 3;
    }
    const char *psd = pc_text_warp_psd_style(style, sl);
    if (psd == NULL) {
        return pc_engine_err_bad_params(err, "type.warpText", "unknown warp style \"%.*s\"", (int)sl, style);
    }
    warp_ctx c;
    c.has = false;
    memset(&c.warp, 0, sizeof c.warp);
    if (strcmp(psd, "warpNone") != 0) {
        size_t ol = 0;
        const char *orient = pc_json_as_str(pc_json_get(p, "orientation"), &ol);
        c.has = true;
        memset(&c.warp, 0, sizeof c.warp);
        if (pc_str_set_c(&c.warp.style, psd, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        c.warp.value = clamp_pct(p, "bend", 50.0);
        c.warp.horizontal_distortion = clamp_pct(p, "horizontalDistortion", 0.0);
        c.warp.vertical_distortion = clamp_pct(p, "verticalDistortion", 0.0);
        c.warp.horizontal = !(orient != NULL && ol == 8 && memcmp(orient, "vertical", 8) == 0);
    }
    pc_status rc = pc_engine_with_text_targets(s, p, "Warp Text", set_warp_cb, &c, NULL, NULL, err);
    yyjson_mut_doc *rd = NULL;
    pc_status r2 = rc == PC_OK ? pc_engine_result_doc(out, &rd, err) : rc;
    if (r2 == PC_OK && rd != NULL) {
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        if (c.has) {
            yyjson_mut_obj_add_val(rd, root, "warp", pc_text_warp_to_json(rd, &c.warp, PC_DOC_JSON_VALUE));
        } else {
            yyjson_mut_obj_add_null(rd, root, "warp");
        }
        yyjson_mut_doc_set_root(rd, root);
    }
    if (c.has) {
        pc_text_warp_fini(&c.warp);
    }
    return r2;
}

/* ---- lorem ipsum / update ----------------------------------------------------------------- */

typedef struct {
    yyjson_val *p;
    size_t caret;
} lorem_ctx;

static pc_status lorem_cb(pc_text_layer *t, const pc_document *doc, void *ud, pc_error *err)
{
    (void)doc;
    (void)err;
    lorem_ctx *c = ud;
    const char *text = pc_str_c(&t->text);
    size_t tlen = t->text.len;
    uint64_t atc;
    size_t at = pc_json_as_u64(pc_json_get(c->p, "at"), &atc) ? pc_engine_text_byte_at(text, tlen, (size_t)atc) : tlen;
    pc_engine_replace_text(t, at, at, LOREM_IPSUM, strlen(LOREM_IPSUM));
    c->caret = pc_engine_text_char_at(pc_str_c(&t->text), t->text.len, at + strlen(LOREM_IPSUM));
    return PC_OK;
}

static pc_status run_paste_lorem(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool newl = false;
    (void)pc_json_as_bool(pc_json_get(p, "new"), &newl);
    if (active_text(s) != NULL && !newl) {
        lorem_ctx c = {p, 0};
        if (tx_with_text(s, p, "Paste Lorem Ipsum", lorem_cb, &c, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, root, "caret", c.caret);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    double w = doc->size.width, h = doc->size.height;
    double size;
    if (!pc_json_as_f64(pc_json_get(p, "size"), &size)) {
        size = h / 20.0 * 72.0 / (double)doc->resolution_dpi;
        size = size < 6.0 ? 6.0 : size > 72.0 ? 72.0 : size;
    }
    char params[700];
    snprintf(params, sizeof params, "{\"box\":[%g,%g,%g,%g],\"text\":\"%s\",\"size\":%g}", w * 0.1, h * 0.1, w * 0.8,
             h * 0.8, LOREM_IPSUM, size);
    pc_json_value cr = PC_JSON_VALUE_INIT;
    if (pc_engine_session_execute_json(s, "type.create", params, &cr, err) != PC_OK) {
        pc_json_value_fini(&cr);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    uint64_t layer = 0;
    yyjson_mut_val *crroot = pc_json_value_root(&cr);
    if (crroot != NULL) {
        yyjson_mut_val *lv = yyjson_mut_obj_get(crroot, "layer");
        if (lv != NULL) {
            layer = yyjson_mut_get_uint(lv);
        }
    }
    pc_json_value_fini(&cr);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", layer);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* Collect all text-layer ids of a document into an owned array. */
static pc_status text_layer_ids(const pc_document *doc, pc_layer_id **out, size_t *n, pc_error *err)
{
    pc_walk w = {0};
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_id *ids = w.len > 0 ? malloc(w.len * sizeof *ids) : NULL;
    size_t m = 0;
    for (size_t i = 0; i < w.len; i++) {
        if (w.items[i].layer->content.kind == PC_LAYER_TEXT) {
            ids[m++] = w.items[i].layer->id;
        }
    }
    pc_walk_fini(&w);
    *out = ids;
    *n = m;
    return PC_OK;
}

typedef struct {
    size_t n;
} update_ctx;

static pc_status edit_update_all(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    update_ctx *c = vctx;
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (text_layer_ids(doc, &ids, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, ids[i]);
        if (l != NULL && l->content.kind == PC_LAYER_TEXT) {
            pc_engine_type_refresh(doc, &l->content.u.text);
            c->n++;
        }
    }
    free(ids);
    return PC_OK;
}

static pc_status run_update_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    update_ctx c = {0};
    if (pc_engine_session_edit(s, "Update All Text Layers", edit_update_all, &c, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "updated", c.n);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- missing-font replacement ------------------------------------------------------------- */

typedef struct {
    const char *family;
} setfont;

static void set_font_cb(pc_char_style *st, void *ud)
{
    const char *f = ((setfont *)ud)->family;
    (void)pc_str_set_c(&st->font_family, f, NULL);
    st->has_postscript_name = false;
    pc_str_fini(&st->postscript_name);
}

typedef struct {
    yyjson_val *map;
    bool all;
    const pc_strlist *missing;
    size_t n;
} rf_ctx;

/* The replacement for family `f`, or NULL to leave it. */
static const char *pick_replacement(const rf_ctx *c, const char *f, size_t fl)
{
    if (!strlist_has(c->missing, f, fl)) {
        return NULL;
    }
    if (c->map != NULL) {
        const char *to = pc_json_as_str(pc_json_get(c->map, f), NULL);
        if (to != NULL) {
            return to;
        }
    }
    return c->all ? PC_TEXT_DEFAULT_FAMILY : NULL;
}

static pc_status edit_replace_fonts(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    rf_ctx *c = vctx;
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (text_layer_ids(doc, &ids, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, ids[i]);
        if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        pc_text_layer *t = &l->content.u.text;
        pc_text_run *cr = NULL;
        size_t ncr = 0;
        if (pc_text_layer_char_runs(t, &cr, &ncr, NULL) != PC_OK) {
            continue;
        }
        bool changed = false;
        size_t at = 0;
        for (size_t r = 0; r < ncr; r++) {
            const char *to = pick_replacement(c, pc_str_c(&cr[r].style.font_family), cr[r].style.font_family.len);
            if (to != NULL) {
                setfont sf = {to};
                pc_engine_style_range(t, at, at + cr[r].len, set_font_cb, &sf);
                changed = true;
            }
            at += cr[r].len;
        }
        pc_text_runs_free(cr, ncr);
        if (changed) {
            pc_engine_type_refresh(doc, t);
            c->n++;
        }
    }
    free(ids);
    return PC_OK;
}

static pc_status replace_fonts(pc_engine_session *s, yyjson_val *map, bool all, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_strlist missing = {0};
    if (missing_fonts_doc(pc_doc_arc_get(st->doc), &missing, err) != PC_OK) {
        pc_strlist_fini(&missing);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    rf_ctx c = {map, all, &missing, 0};
    if (missing.n > 0) {
        if (pc_engine_session_edit(s, "Replace Missing Fonts", edit_replace_fonts, &c, err) != PC_OK) {
            pc_strlist_fini(&missing);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_strlist_fini(&missing);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *ma = yyjson_mut_arr(rd);
    for (size_t i = 0; i < missing.n; i++) {
        yyjson_mut_arr_add_strcpy(rd, ma, missing.v[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "missing", ma);
    yyjson_mut_obj_add_uint(rd, root, "replaced", c.n);
    yyjson_mut_doc_set_root(rd, root);
    pc_strlist_fini(&missing);
    return PC_OK;
}

static pc_status run_resolve_missing(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_val *map = pc_json_get(p, "map");
    if (map != NULL && yyjson_is_obj(map)) {
        return replace_fonts(s, map, false, out, err);
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_strlist missing = {0};
    if (missing_fonts_doc(pc_doc_arc_get(st->doc), &missing, err) != PC_OK) {
        pc_strlist_fini(&missing);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_strlist_fini(&missing);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *ma = yyjson_mut_arr(rd);
    for (size_t i = 0; i < missing.n; i++) {
        yyjson_mut_arr_add_strcpy(rd, ma, missing.v[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "missing", ma);
    yyjson_mut_doc_set_root(rd, root);
    pc_strlist_fini(&missing);
    return PC_OK;
}

static pc_status run_replace_all_missing(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return replace_fonts(s, NULL, true, out, err);
}

static pc_status run_rasterize_type_layer(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return pc_engine_session_execute(s, "type.rasterize", p, out, err);
}

/* ---- default type styles ------------------------------------------------------------------ */

static pc_status defaults_result(pc_json_value *out, const pc_char_style *c, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "font", pc_str_c(&c->font_family));
    yyjson_mut_obj_add_real(rd, root, "size", c->size_pt);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_save_defaults(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_text_layer *t = active_text(s);
    if (t == NULL) {
        return pc_engine_err_other(err, "the active layer is not a type layer");
    }
    pc_char_style c;
    pc_char_style_default(&c);
    pc_paragraph_style pp = pc_paragraph_style_default();
    pc_text_run *cr = NULL;
    size_t ncr = 0;
    if (pc_text_layer_char_runs(t, &cr, &ncr, NULL) == PC_OK) {
        if (ncr > 0) {
            pc_char_style_fini(&c);
            if (pc_char_style_clone(&cr[0].style, &c, err) != PC_OK) {
                pc_text_runs_free(cr, ncr);
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
        pc_text_runs_free(cr, ncr);
    }
    pc_paragraph_run *pr = NULL;
    size_t npr = 0;
    if (pc_text_layer_paragraph_runs(t, &pr, &npr, NULL) == PC_OK) {
        if (npr > 0) {
            pp = pr[0].style;
        }
        free(pr);
    }
    pc_char_style saved;
    if (pc_char_style_clone(&c, &saved, err) != PC_OK) {
        pc_char_style_fini(&c);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_engine_type_defaults_fini(s);
    s->type_defaults_char = saved;
    s->type_defaults_para = pp;
    s->has_type_defaults = true;
    pc_status rc = defaults_result(out, &c, err);
    pc_char_style_fini(&c);
    return rc;
}

typedef struct {
    const pc_char_style *c;
    const pc_paragraph_style *pp;
} load_ctx;

static void load_char_cb(pc_char_style *st, void *ud)
{
    const pc_char_style *src = ((load_ctx *)ud)->c;
    pc_char_style_fini(st);
    if (pc_char_style_clone(src, st, NULL) != PC_OK) {
        pc_char_style_default(st);
    }
}

static void load_para_cb(pc_paragraph_style *st, void *ud)
{
    *st = *((load_ctx *)ud)->pp;
}

static pc_status load_cb(pc_text_layer *t, size_t a, size_t b, void *ud, pc_error *err)
{
    (void)a;
    (void)b;
    (void)err;
    pc_engine_style_range(t, 0, t->text.len, load_char_cb, ud);
    pc_engine_style_paragraphs(t, 0, t->text.len, load_para_cb, ud);
    return PC_OK;
}

static pc_status run_load_defaults(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (!s->has_type_defaults) {
        return pc_engine_err_other(err, "no default type styles have been saved");
    }
    load_ctx lc = {&s->type_defaults_char, &s->type_defaults_para};
    if (pc_engine_with_text_targets(s, p, "Load Default Type Styles", load_cb, &lc, NULL, NULL, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return defaults_result(out, &s->type_defaults_char, err);
}

/* ---- registry ----------------------------------------------------------------------------- */

static pc_status run_aa_none(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return set_aa(s, p, o, PC_AA_NONE, e); }
static pc_status run_aa_sharp(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return set_aa(s, p, o, PC_AA_SHARP, e); }
static pc_status run_aa_crisp(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return set_aa(s, p, o, PC_AA_CRISP, e); }
static pc_status run_aa_strong(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return set_aa(s, p, o, PC_AA_STRONG, e); }
static pc_status run_aa_smooth(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return set_aa(s, p, o, PC_AA_SMOOTH, e); }
static pc_status run_aa_winlcd(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return set_aa(s, p, o, PC_AA_WINDOWS_LCD, e); }
static pc_status run_aa_win(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return set_aa(s, p, o, PC_AA_WINDOWS, e); }
static pc_status run_orient_h(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return set_orientation(s, p, o, PC_ORIENT_HORIZONTAL, e); }
static pc_status run_orient_v(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return set_orientation(s, p, o, PC_ORIENT_VERTICAL, e); }
static pc_status run_ot_liga(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "liga", e); }
static pc_status run_ot_calt(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "calt", e); }
static pc_status run_ot_dlig(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "dlig", e); }
static pc_status run_ot_swsh(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "swsh", e); }
static pc_status run_ot_onum(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "onum", e); }
static pc_status run_ot_salt(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "salt", e); }
static pc_status run_ot_titl(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "titl", e); }
static pc_status run_ot_ornm(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "ornm", e); }
static pc_status run_ot_ordn(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "ordn", e); }
static pc_status run_ot_frac(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return toggle_opentype(s, p, o, "frac", e); }

static const char *const MENU_TYPE[] = {"Type"};
static const char *const MENU_TYPE_AA[] = {"Type", "Anti-Alias"};
static const char *const MENU_TYPE_ORIENT[] = {"Type", "Orientation"};
static const char *const MENU_TYPE_OT[] = {"Type", "OpenType"};

#define FP "{\"layer\":id? or \"layers\":[id,\xe2\x80\xa6]? (default selected Type layers)}"
#define LP "{\"layer\":id?}"
#define OTP                                                                                                            \
    "{\"layer\":id? or \"layers\":[id,\xe2\x80\xa6]? (default selected Type layers),\"on\":bool? (default: toggle "    \
    "representative),\"range\":[startChar,endChar]? (single layer; default all)}"

static const pc_engine_command SPECS[] = {
    {"type.antiAlias.none", "None", MENU_TYPE_AA, 2, NULL, FP, pc_engine_has_format_target, run_aa_none, true},
    {"type.antiAlias.sharp", "Sharp", MENU_TYPE_AA, 2, NULL, FP, pc_engine_has_format_target, run_aa_sharp, true},
    {"type.antiAlias.crisp", "Crisp", MENU_TYPE_AA, 2, NULL, FP, pc_engine_has_format_target, run_aa_crisp, true},
    {"type.antiAlias.strong", "Strong", MENU_TYPE_AA, 2, NULL, FP, pc_engine_has_format_target, run_aa_strong, true},
    {"type.antiAlias.smooth", "Smooth", MENU_TYPE_AA, 2, NULL, FP, pc_engine_has_format_target, run_aa_smooth, true},
    {"type.antiAlias.windowsLcd", "Windows LCD", MENU_TYPE_AA, 2, NULL, FP, pc_engine_has_format_target, run_aa_winlcd, true},
    {"type.antiAlias.windows", "Windows", MENU_TYPE_AA, 2, NULL, FP, pc_engine_has_format_target, run_aa_win, true},
    {"type.orientation.horizontal", "Horizontal", MENU_TYPE_ORIENT, 2, NULL, FP, pc_engine_has_format_target, run_orient_h, true},
    {"type.orientation.vertical", "Vertical", MENU_TYPE_ORIENT, 2, NULL, FP, pc_engine_has_format_target, run_orient_v, true},
    {"type.openType.standardLigatures", "Standard Ligatures", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_liga, true},
    {"type.openType.contextualAlternates", "Contextual Alternates", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_calt, true},
    {"type.openType.discretionaryLigatures", "Discretionary Ligatures", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_dlig, true},
    {"type.openType.swash", "Swash", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_swsh, true},
    {"type.openType.oldstyle", "Oldstyle", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_onum, true},
    {"type.openType.stylisticAlternates", "Stylistic Alternates", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_salt, true},
    {"type.openType.titlingAlternates", "Titling Alternates", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_titl, true},
    {"type.openType.ornaments", "Ornaments", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_ornm, true},
    {"type.openType.ordinals", "Ordinals", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_ordn, true},
    {"type.openType.fractions", "Fractions", MENU_TYPE_OT, 2, NULL, OTP, pc_engine_has_format_target, run_ot_frac, true},
    {"type.createWorkPath", "Create Work Path", MENU_TYPE, 1, NULL, LP, en_has_text, run_create_work_path, true},
    {"type.convertToShape", "Convert to Shape", MENU_TYPE, 1, NULL, LP, en_has_text, run_convert_to_shape, true},
    {"type.rasterizeTypeLayer", "Rasterize Type Layer", MENU_TYPE, 1, NULL, LP, en_has_text, run_rasterize_type_layer, true},
    {"type.convertToParagraphText", "Convert to Paragraph Text", MENU_TYPE, 1, NULL, LP, en_has_point_text, run_to_paragraph, true},
    {"type.convertToPointText", "Convert to Point Text", MENU_TYPE, 1, NULL, LP, en_has_box_text, run_to_point, true},
    {"type.warpText", "Warp Text\xe2\x80\xa6", MENU_TYPE, 1, NULL,
     "{\"layer\":id? or \"layers\":[id,\xe2\x80\xa6]? (default selected Type layers),\"style\":\"none|arc|arcLower|"
     "arcUpper|arch|bulge|shellLower|shellUpper|flag|wave|fish|rise|fisheye|inflate|squeeze|twist\"=\"arc\","
     "\"orientation\":\"horizontal|vertical\"=\"horizontal\",\"bend\":-100..100=50,\"horizontalDistortion\":-100.."
     "100=0,\"verticalDistortion\":-100..100=0}",
     pc_engine_has_format_target, run_warp_text, true},
    {"type.updateAllTextLayers", "Update All Text Layers", MENU_TYPE, 1, NULL, "{}", en_any_text, run_update_all, true},
    {"type.replaceAllMissingFonts", "Replace All Missing Fonts", MENU_TYPE, 1, NULL, "{} (with the default family)",
     en_any_text, run_replace_all_missing, true},
    {"type.resolveMissingFonts", "Resolve Missing Fonts\xe2\x80\xa6", MENU_TYPE, 1, NULL,
     "{\"map\":{\"Missing Family\":\"Installed Family\"}?} (no map: list the missing families)", en_any_text,
     run_resolve_missing, true},
    {"type.pasteLoremIpsum", "Paste Lorem Ipsum", MENU_TYPE, 1, NULL,
     "{\"layer\":id?,\"at\":char? (default end),\"new\":bool=false (new paragraph text layer)}", en_has_doc,
     run_paste_lorem, true},
    {"type.saveDefaultTypeStyles", "Save Default Type Styles", MENU_TYPE, 1, NULL,
     "{} (from the active type layer; new type layers start from them)", en_has_text, run_save_defaults, true},
    {"type.loadDefaultTypeStyles", "Load Default Type Styles", MENU_TYPE, 1, NULL, FP, en_has_defaults,
     run_load_defaults, true},
};

const pc_engine_command *pc_engine_type_extra_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
