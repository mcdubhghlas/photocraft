/* Character Styles and Paragraph Styles (port of crates/engine/src/type_styles_cmds.rs, E11):
 * type.characterStyle.* / type.paragraphStyle.* new/duplicate/delete/rename/set/apply/redefine/
 * clearOverride/list. The style model is src/doc text_styles; runs keep fully resolved attributes
 * plus a style reference, so whatever differs is a local override. Routes through src/text. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/str.h"
#include "doc/json.h"
#include "doc/text_styles.h"
#include "text/layout.h"
#include "text/fonts.h"

#include "engine_internal.h"
#include "type_cmds.h" /* apply_char_props / apply_para_props / refresh / byte helpers */

const pc_engine_command *pc_engine_type_styles_specs(size_t *n);

#define FAMILY PC_TEXT_DEFAULT_FAMILY

static pc_status bad(pc_error *err, const char *cmd, const char *msg)
{
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

static pc_status exhausted_id(pc_error *err, bool paragraph)
{
    return pc_engine_err_other(err, "%s style id space exhausted", paragraph ? "paragraph" : "character");
}

/* ---- attrs plumbing ----------------------------------------------------------------------- */

/* Insert an immutable JSON value under key into a StyleAttrs (copying/normalising it). */
static pc_status attrs_insert_imm(pc_style_attrs *a, const char *key, yyjson_val *imm, pc_error *err)
{
    yyjson_mut_doc *scratch = yyjson_mut_doc_new(NULL);
    if (scratch == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "attrs");
    }
    yyjson_mut_val *mv = pc_json_normalized_copy_val(scratch, imm);
    pc_status rc = mv != NULL ? pc_style_attrs_insert(a, key, mv, err) : pc_fail(err, PC_ERR_NOMEM, "attrs");
    yyjson_mut_doc_free(scratch);
    return rc;
}

/* Insert every key of src into dst. */
static pc_status attrs_merge(pc_style_attrs *dst, const pc_style_attrs *src, pc_error *err)
{
    yyjson_mut_val *root = pc_json_value_root(src);
    if (root == NULL || !yyjson_mut_is_obj(root)) {
        return PC_OK;
    }
    yyjson_mut_obj_iter it;
    yyjson_mut_val *k;
    (void)yyjson_mut_obj_iter_init(root, &it);
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        yyjson_mut_val *v = yyjson_mut_obj_iter_get_val(k);
        if (pc_style_attrs_insert(dst, yyjson_mut_get_str(k), v, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

/* A one-key StyleAttrs from an immutable value (for validation). */
static pc_status one_attr(pc_style_attrs *out, const char *key, yyjson_val *v, pc_error *err)
{
    *out = (pc_json_value)PC_JSON_VALUE_INIT;
    return attrs_insert_imm(out, key, v, err);
}

/* ---- perturbed styles for `touched` ------------------------------------------------------- */

static void perturbed_char(pc_char_style *s)
{
    pc_char_style_default(s);
    (void)pc_str_set_c(&s->font_family, "\x01", NULL);
    (void)pc_str_set_c(&s->font_style, "\x01", NULL);
    (void)pc_str_set_c(&s->postscript_name, "\x01", NULL);
    s->has_postscript_name = true;
    s->weight = 1;
    s->italic = true;
    s->size_pt = -1.0f;
    s->color = pc_color_rgba(0.123f, 0.456f, 0.789f, 0.5f);
    s->tracking = -12345.0f;
    s->has_leading = true;
    s->leading_pt = -1.0f;
    s->baseline_shift_pt = -1.0f;
    s->horizontal_scale = -1.0f;
    s->vertical_scale = -1.0f;
    s->underline = true;
    s->strikethrough = true;
    s->faux_bold = true;
    s->faux_italic = true;
    s->kerning = PC_KERNING_OFF;
    s->kern = -12345.0f;
    s->caps = PC_CAPS_ALL_CAPS;
    s->ligatures = false;
    s->discretionary_ligatures = true;
    s->features = malloc(sizeof *s->features);
    if (s->features != NULL) {
        s->features[0].tag = (pc_str){0};
        (void)pc_str_set_c(&s->features[0].tag, "\x01", NULL);
        s->features[0].value = 9;
        s->nfeatures = 1;
    }
    s->variations = malloc(sizeof *s->variations);
    if (s->variations != NULL) {
        s->variations[0].axis = (pc_str){0};
        (void)pc_str_set_c(&s->variations[0].axis, "\x01", NULL);
        s->variations[0].value = -1.0f;
        s->nvariations = 1;
    }
    (void)pc_str_set_c(&s->language, "\x01", NULL);
    s->has_language = true;
}

static pc_paragraph_style perturbed_para(void)
{
    pc_paragraph_style s = pc_paragraph_style_default();
    s.align = PC_TEXT_ALIGN_JUSTIFY_ALL;
    s.first_line_indent_pt = -1.0f;
    s.start_indent_pt = -1.0f;
    s.end_indent_pt = -1.0f;
    s.space_before_pt = -1.0f;
    s.space_after_pt = -1.0f;
    s.auto_leading = -1.0f;
    s.direction = PC_TEXT_DIR_RTL;
    s.hyphenate = true;
    return s;
}

/* out = {k:v in attrs(a) : attrs(b)[k] == v}. */
static pc_status touched_common(const pc_style_attrs *aa, const pc_style_attrs *ab, pc_style_attrs *out, pc_error *err)
{
    *out = (pc_json_value)PC_JSON_VALUE_INIT;
    yyjson_mut_val *ra = pc_json_value_root(aa);
    if (ra == NULL || !yyjson_mut_is_obj(ra)) {
        return PC_OK;
    }
    yyjson_mut_obj_iter it;
    yyjson_mut_val *k;
    (void)yyjson_mut_obj_iter_init(ra, &it);
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        const char *key = yyjson_mut_get_str(k);
        yyjson_mut_val *va = yyjson_mut_obj_iter_get_val(k);
        yyjson_mut_val *vb = pc_style_attrs_get(ab, key);
        if (vb != NULL && pc_json_mut_equal(va, vb)) {
            if (pc_style_attrs_insert(out, key, va, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
    }
    return PC_OK;
}

static pc_status touched_char(yyjson_val *friendly, pc_style_attrs *out, pc_error *err)
{
    pc_char_style a, b;
    pc_char_style_default(&a);
    perturbed_char(&b);
    (void)pc_engine_apply_char_props(&a, friendly);
    (void)pc_engine_apply_char_props(&b, friendly);
    pc_style_attrs aa = PC_JSON_VALUE_INIT, ab = PC_JSON_VALUE_INIT;
    pc_status rc = pc_char_style_all_attrs(&a, &aa, err);
    if (rc == PC_OK) {
        rc = pc_char_style_all_attrs(&b, &ab, err);
    }
    if (rc == PC_OK) {
        rc = touched_common(&aa, &ab, out, err);
    }
    pc_json_value_fini(&aa);
    pc_json_value_fini(&ab);
    pc_char_style_fini(&a);
    pc_char_style_fini(&b);
    return rc;
}

static pc_status touched_para(yyjson_val *friendly, pc_style_attrs *out, pc_error *err)
{
    pc_paragraph_style a = pc_paragraph_style_default();
    pc_paragraph_style b = perturbed_para();
    (void)pc_engine_apply_para_props(&a, friendly);
    (void)pc_engine_apply_para_props(&b, friendly);
    pc_style_attrs aa = PC_JSON_VALUE_INIT, ab = PC_JSON_VALUE_INIT;
    pc_status rc = pc_paragraph_style_all_attrs(&a, &aa, err);
    if (rc == PC_OK) {
        rc = pc_paragraph_style_all_attrs(&b, &ab, err);
    }
    if (rc == PC_OK) {
        rc = touched_common(&aa, &ab, out, err);
    }
    pc_json_value_fini(&aa);
    pc_json_value_fini(&ab);
    return rc;
}

/* Splits `v` into character and paragraph attribute sets. */
static pc_status parse_attrs(const char *cmd, yyjson_val *v, pc_style_attrs *cout, pc_style_attrs *pout, pc_error *err)
{
    *cout = (pc_json_value)PC_JSON_VALUE_INIT;
    *pout = (pc_json_value)PC_JSON_VALUE_INIT;
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    if (!yyjson_is_obj(v)) {
        return bad(err, cmd, "`attrs` must be an object");
    }
    pc_char_style cd;
    pc_char_style_default(&cd);
    pc_paragraph_style pd = pc_paragraph_style_default();
    pc_style_attrs ckeys = PC_JSON_VALUE_INIT, pkeys = PC_JSON_VALUE_INIT;
    (void)pc_char_style_all_attrs(&cd, &ckeys, NULL);
    (void)pc_paragraph_style_all_attrs(&pd, &pkeys, NULL);
    pc_char_style_fini(&cd);

    /* friendly keys collected into a mutable doc, reparsed to immutable for apply_*_props. */
    yyjson_mut_doc *fdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *froot = fdoc != NULL ? yyjson_mut_obj(fdoc) : NULL;
    if (froot == NULL) {
        pc_json_value_fini(&ckeys);
        pc_json_value_fini(&pkeys);
        return pc_fail(err, PC_ERR_NOMEM, cmd);
    }
    yyjson_mut_doc_set_root(fdoc, froot);
    bool have_friendly = false;
    pc_status rc = PC_OK;

    size_t idx, max;
    yyjson_val *key, *x;
    yyjson_obj_foreach(v, idx, max, key, x) {
        size_t kl = 0;
        const char *k = pc_json_as_str(key, &kl);
        if (k == NULL) {
            continue;
        }
        pc_style_attrs one = PC_JSON_VALUE_INIT;
        if (one_attr(&one, k, x, err) != PC_OK) {
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
            pc_json_value_fini(&one);
            break;
        }
        bool is_char = pc_style_attrs_get(&ckeys, k) != NULL && pc_char_style_validate_attrs(&one, NULL) == PC_OK;
        bool is_para = !is_char && pc_style_attrs_get(&pkeys, k) != NULL &&
                       pc_paragraph_style_validate_attrs(&one, NULL) == PC_OK;
        pc_json_value_fini(&one);
        if (is_char) {
            rc = attrs_insert_imm(cout, k, x, err);
        } else if (is_para) {
            rc = attrs_insert_imm(pout, k, x, err);
        } else {
            yyjson_mut_val *mv = pc_json_normalized_copy_val(fdoc, x);
            if (mv != NULL) {
                (void)yyjson_mut_obj_add_val(fdoc, froot, k, mv);
                have_friendly = true;
            }
        }
        if (rc != PC_OK) {
            break;
        }
    }
    pc_json_value_fini(&ckeys);
    pc_json_value_fini(&pkeys);

    if (rc == PC_OK && have_friendly) {
        pc_buf buf = {0};
        yyjson_doc *fimm = NULL;
        if (pc_json_write(froot, 0, &buf, err) == PC_OK &&
            pc_json_parse((const char *)buf.data, buf.len, &fimm, err) == PC_OK) {
            yyjson_val *fr = yyjson_doc_get_root(fimm);
            pc_style_attrs fc = PC_JSON_VALUE_INIT, fp = PC_JSON_VALUE_INIT;
            rc = touched_char(fr, &fc, err);
            if (rc == PC_OK) {
                rc = touched_para(fr, &fp, err);
            }
            if (rc == PC_OK && pc_style_attrs_is_empty(&fc) && pc_style_attrs_is_empty(&fp)) {
                rc = bad(err, cmd, "unknown style attributes");
            }
            if (rc == PC_OK) {
                rc = attrs_merge(cout, &fc, err);
            }
            if (rc == PC_OK) {
                rc = attrs_merge(pout, &fp, err);
            }
            pc_json_value_fini(&fc);
            pc_json_value_fini(&fp);
        } else {
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_buf_fini(&buf);
        yyjson_doc_free(fimm);
    }
    yyjson_mut_doc_free(fdoc);

    if (rc == PC_OK) {
        rc = pc_char_style_validate_attrs(cout, err);
    }
    if (rc == PC_OK) {
        rc = pc_paragraph_style_validate_attrs(pout, err);
    }
    if (rc != PC_OK) {
        pc_json_value_fini(cout);
        pc_json_value_fini(pout);
    }
    return rc;
}

/* ---- targeting ---------------------------------------------------------------------------- */

static const pc_text_layer *text_of(const pc_document *doc, pc_layer_id id)
{
    const pc_layer *l = pc_document_layer(doc, id);
    return (l != NULL && l->content.kind == PC_LAYER_TEXT) ? &l->content.u.text : NULL;
}

static pc_status targets(const pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    *out = NULL;
    *n = 0;
    if (st == NULL) {
        return PC_OK;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    uint64_t single;
    if (pc_json_as_u64(pc_json_get(p, "layer"), &single)) {
        ids = malloc(sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "styles");
        }
        ids[0] = (pc_layer_id)single;
        nids = 1;
    } else {
        yyjson_val *arr = pc_json_get(p, "layers");
        if (arr != NULL && yyjson_is_arr(arr)) {
            size_t m = yyjson_arr_size(arr);
            ids = malloc((m + 1) * sizeof *ids);
            if (ids == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "styles");
            }
            size_t idx, mx;
            yyjson_val *v;
            yyjson_arr_foreach(arr, idx, mx, v) {
                uint64_t id;
                if (pc_json_as_u64(v, &id)) {
                    ids[nids++] = (pc_layer_id)id;
                }
            }
        } else {
            if (pc_engine_doc_state_selected_layers(st, &ids, &nids, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
    }
    pc_layer_id *tg = nids > 0 ? malloc(nids * sizeof *tg) : NULL;
    size_t nt = 0;
    for (size_t i = 0; i < nids; i++) {
        if (text_of(doc, ids[i]) != NULL) {
            tg[nt++] = ids[i];
        }
    }
    free(ids);
    *out = tg;
    *n = nt;
    return PC_OK;
}

static pc_status has_target(const pc_engine_session *s, pc_error *r)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_engine_err_other(r, "no document open");
    }
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (targets(s, NULL, &ids, &n, NULL) != PC_OK) {
        return pc_engine_err_other(r, "select a type layer");
    }
    free(ids);
    return n > 0 ? PC_OK : pc_engine_err_other(r, "select a type layer");
}

static void range_of(const char *text, size_t len, yyjson_val *p, size_t *a, size_t *b)
{
    yyjson_val *r = pc_json_get(p, "range");
    if (r != NULL && yyjson_is_arr(r) && yyjson_arr_size(r) == 2) {
        uint64_t x = 0, y;
        (void)pc_json_as_u64(yyjson_arr_get(r, 0), &x);
        if (!pc_json_as_u64(yyjson_arr_get(r, 1), &y)) {
            y = x;
        }
        uint64_t lo = x < y ? x : y, hi = x < y ? y : x;
        *a = pc_engine_text_byte_at(text, len, (size_t)lo);
        *b = pc_engine_text_byte_at(text, len, (size_t)hi);
    } else {
        *a = 0;
        *b = len;
    }
}

/* Reads p["id"]; *present false when absent/null. */
static pc_status id_param(const char *cmd, yyjson_val *p, bool *present, uint32_t *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "id");
    if (v == NULL || yyjson_is_null(v)) {
        *present = false;
        return PC_OK;
    }
    *present = true;
    return pc_engine_param_u32_id(cmd, "id", v, out, err);
}

/* ---- Op + restyle ------------------------------------------------------------------------- */

typedef struct {
    bool set_char;      /* change the character ref */
    bool set_char_some; /* the new ref is Some */
    uint32_t set_char_id;
    bool set_para;
    bool set_para_some;
    uint32_t set_para_id;
    bool clear_char;
    bool clear_para;
} Op;

static const uint32_t *opt_ref(bool some, const uint32_t *slot) { return some ? slot : NULL; }

typedef struct {
    size_t start, end;
    pc_paragraph_style np;
    bool has_old;
    uint32_t old_ref;
} para_slot;

typedef struct {
    pc_text_run *v;
    size_t n, cap;
} crunvec;

static bool crun_push(crunvec *cv, size_t len, pc_char_style *st)
{
    if (cv->n > 0 && pc_char_style_eq(&cv->v[cv->n - 1].style, st)) {
        cv->v[cv->n - 1].len += len;
        pc_char_style_fini(st);
        return true;
    }
    if (cv->n == cv->cap) {
        size_t c = cv->cap ? cv->cap * 2 : 8;
        pc_text_run *nv = realloc(cv->v, c * sizeof *nv);
        if (nv == NULL) {
            pc_char_style_fini(st);
            return false;
        }
        cv->v = nv;
        cv->cap = c;
    }
    cv->v[cv->n].len = len;
    cv->v[cv->n].style = *st;
    cv->n++;
    return true;
}

typedef struct {
    pc_paragraph_run *v;
    size_t n, cap;
} prunvec;

static bool prun_push(prunvec *pv, size_t len, pc_paragraph_style st)
{
    if (pv->n > 0 && pc_paragraph_style_eq(&pv->v[pv->n - 1].style, &st)) {
        pv->v[pv->n - 1].len += len;
        return true;
    }
    if (pv->n == pv->cap) {
        size_t c = pv->cap ? pv->cap * 2 : 8;
        pc_paragraph_run *nv = realloc(pv->v, c * sizeof *nv);
        if (nv == NULL) {
            return false;
        }
        pv->v = nv;
        pv->cap = c;
    }
    pv->v[pv->n].len = len;
    pv->v[pv->n].style = st;
    pv->n++;
    return true;
}

static bool style_sheet_ptr(const pc_char_style *s, uint32_t *slot, const uint32_t **out)
{
    if (s->has_style_sheet) {
        *slot = s->style_sheet;
        *out = slot;
        return true;
    }
    *out = NULL;
    return false;
}

/* Re-resolves every run/paragraph of t against `new`, applying op to bytes [a,b). */
static bool restyle(pc_text_layer *t, const pc_text_styles *old, const pc_text_styles *new_, bool has_range,
                    size_t a, size_t b, Op op)
{
    /* snapshot for change detection */
    pc_text_run *before_r = NULL;
    size_t nbefore_r = 0;
    pc_paragraph_run *before_p = NULL;
    size_t nbefore_p = 0;
    (void)pc_text_layer_char_runs(t, &before_r, &nbefore_r, NULL);
    (void)pc_text_layer_paragraph_runs(t, &before_p, &nbefore_p, NULL);

    const char *text = pc_str_c(&t->text);
    size_t tlen = t->text.len;
    bool whole = !has_range;
    if (!has_range) {
        a = 0;
        b = tlen;
    }

    pc_paragraph_run *old_paras = NULL;
    size_t nop = 0;
    (void)pc_text_layer_paragraph_runs(t, &old_paras, &nop, NULL);
    size_t *starts = nop > 0 ? malloc(nop * sizeof *starts) : NULL;
    size_t acc = 0;
    for (size_t i = 0; i < nop; i++) {
        starts[i] = acc;
        acc += old_paras[i].len;
    }
    size_t *offs = NULL, np_count = 0;
    (void)pc_text_split_paragraphs(text, tlen, &offs, &np_count, NULL);

    para_slot *paras = np_count > 0 ? malloc(np_count * sizeof *paras) : NULL;
    size_t npar = 0;
    for (size_t k = 0; k < np_count; k++) {
        size_t ps = offs[2 * k], pe = offs[2 * k + 1];
        /* para_at(ps) */
        pc_paragraph_style old_p = nop > 0 ? old_paras[0].style : pc_paragraph_style_default();
        for (size_t i = 0; i < nop; i++) {
            if (starts[i] <= ps) {
                old_p = old_paras[i].style;
            }
        }
        bool touches = whole || (ps < b && pe > a) || (a == b && a >= ps && (a < pe || pe == tlen));
        uint32_t ref_slot;
        const uint32_t *new_ref;
        if (op.set_para && touches) {
            new_ref = opt_ref(op.set_para_some, (ref_slot = op.set_para_id, &ref_slot));
        } else if (old_p.has_style_sheet) {
            ref_slot = old_p.style_sheet;
            new_ref = &ref_slot;
        } else {
            new_ref = NULL;
        }
        pc_paragraph_style npv;
        if (op.clear_para && touches) {
            (void)pc_text_styles_resolve_para(new_, new_ref, &npv, NULL);
        } else {
            (void)pc_text_styles_restyle_para(new_, old, &old_p, new_ref, &npv, NULL);
        }
        paras[npar].start = ps;
        paras[npar].end = pe;
        paras[npar].np = npv;
        paras[npar].has_old = old_p.has_style_sheet;
        paras[npar].old_ref = old_p.style_sheet;
        npar++;
    }

    /* character runs */
    crunvec runs = {0};
    pc_text_run *cr = NULL;
    size_t ncr = 0;
    (void)pc_text_layer_char_runs(t, &cr, &ncr, NULL);
    size_t at = 0;
    for (size_t ri = 0; ri < ncr; ri++) {
        size_t rs = at, re = at + cr[ri].len;
        at = re;
        /* edges = cuts strictly inside (rs,re): paragraph starts + a + b + len */
        /* gather candidate cuts */
        size_t s0 = rs;
        /* Build a sorted list of edge positions in (rs,re). */
        size_t *cand = malloc((npar + 3) * sizeof *cand);
        size_t nc = 0;
        for (size_t i = 0; i < npar; i++) {
            if (paras[i].start > rs && paras[i].start < re) {
                cand[nc++] = paras[i].start;
            }
        }
        if (a > rs && a < re) {
            cand[nc++] = a;
        }
        if (b > rs && b < re) {
            cand[nc++] = b;
        }
        /* insertion sort + dedup */
        for (size_t i = 1; i < nc; i++) {
            size_t v = cand[i];
            size_t j = i;
            while (j > 0 && cand[j - 1] > v) {
                cand[j] = cand[j - 1];
                j--;
            }
            cand[j] = v;
        }
        size_t w = 0;
        for (size_t i = 0; i < nc; i++) {
            if (i == 0 || cand[i] != cand[i - 1]) {
                cand[w++] = cand[i];
            }
        }
        nc = w;
        for (size_t ei = 0; ei <= nc; ei++) {
            size_t e = ei < nc ? cand[ei] : re;
            if (e <= s0 && rs != re) {
                continue;
            }
            /* paragraph index containing s0 */
            size_t pi = 0;
            for (size_t i = 0; i < npar; i++) {
                if (paras[i].start <= s0) {
                    pi = i;
                }
            }
            uint32_t op_slot, np_slot, chr_slot;
            const uint32_t *old_pref = npar > 0 && paras[pi].has_old ? (op_slot = paras[pi].old_ref, &op_slot) : NULL;
            const uint32_t *new_pref =
                npar > 0 && paras[pi].np.has_style_sheet ? (np_slot = paras[pi].np.style_sheet, &np_slot) : NULL;
            bool inside = whole || (s0 >= a && e <= b && a < b);
            const uint32_t *chr;
            if (op.set_char && inside) {
                chr = opt_ref(op.set_char_some, (chr_slot = op.set_char_id, &chr_slot));
            } else {
                chr = style_sheet_ptr(&cr[ri].style, &chr_slot, &chr) ? &chr_slot : NULL;
            }
            pc_char_style st;
            if (op.clear_char && inside) {
                (void)pc_text_styles_resolve_char(new_, new_pref, chr, FAMILY, &st, NULL);
            } else {
                (void)pc_text_styles_restyle_char(new_, old, &cr[ri].style, old_pref, new_pref, chr, FAMILY, &st,
                                                  NULL);
            }
            if (!crun_push(&runs, e - s0, &st)) {
                /* OOM: leave as-is */
            }
            s0 = e;
        }
        free(cand);
    }

    prunvec pruns = {0};
    for (size_t i = 0; i < npar; i++) {
        (void)prun_push(&pruns, paras[i].end - paras[i].start, paras[i].np);
    }

    if (runs.n == 0) {
        pc_char_style first;
        if (ncr > 0) {
            if (pc_char_style_clone(&cr[0].style, &first, NULL) != PC_OK) {
                pc_char_style_default(&first);
            }
        } else {
            pc_char_style_default(&first);
        }
        uint32_t np_slot, chr_slot;
        const uint32_t *new_pref =
            pruns.n > 0 && pruns.v[0].style.has_style_sheet ? (np_slot = pruns.v[0].style.style_sheet, &np_slot)
                                                            : NULL;
        const uint32_t *chr;
        if (op.set_char) {
            chr = opt_ref(op.set_char_some, (chr_slot = op.set_char_id, &chr_slot));
        } else {
            chr = style_sheet_ptr(&first, &chr_slot, &chr) ? &chr_slot : NULL;
        }
        pc_char_style st;
        (void)pc_text_styles_restyle_char(new_, old, &first, NULL, new_pref, chr, FAMILY, &st, NULL);
        pc_char_style_fini(&first);
        (void)crun_push(&runs, 0, &st);
    }

    /* compare to before */
    bool changed = runs.n != nbefore_r || pruns.n != nbefore_p;
    if (!changed) {
        for (size_t i = 0; i < runs.n; i++) {
            if (runs.v[i].len != before_r[i].len || !pc_char_style_eq(&runs.v[i].style, &before_r[i].style)) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) {
        for (size_t i = 0; i < pruns.n; i++) {
            if (pruns.v[i].len != before_p[i].len || !pc_paragraph_style_eq(&pruns.v[i].style, &before_p[i].style)) {
                changed = true;
                break;
            }
        }
    }

    pc_text_runs_free(t->runs, t->nruns);
    t->runs = runs.v;
    t->nruns = runs.n;
    free(t->paragraphs);
    t->paragraphs = pruns.v;
    t->nparagraphs = pruns.n;
    (void)pc_text_layer_sync_summary(t, NULL);

    pc_text_runs_free(before_r, nbefore_r);
    free(before_p);
    pc_text_runs_free(cr, ncr);
    free(old_paras);
    free(starts);
    free(offs);
    free(paras);
    return changed;
}

/* ---- apply / commit ----------------------------------------------------------------------- */

typedef struct {
    pc_layer_id id;
    bool has_range;
    size_t a, b;
} Target;

static size_t apply_styles(pc_document *doc, const pc_text_styles *new_, const Target *tg, size_t ntg, Op op)
{
    pc_text_styles old;
    if (pc_text_styles_clone(&doc->text_styles, &old, NULL) != PC_OK) {
        return 0;
    }
    pc_text_styles installed;
    if (pc_text_styles_clone(new_, &installed, NULL) != PC_OK) {
        pc_text_styles_fini(&old);
        return 0;
    }
    pc_text_styles_fini(&doc->text_styles);
    doc->text_styles = installed;

    pc_walk w = {0};
    if (pc_document_walk(doc, &w, NULL) != PC_OK) {
        pc_text_styles_fini(&old);
        return 0;
    }
    pc_layer_id *ids = w.len > 0 ? malloc(w.len * sizeof *ids) : NULL;
    size_t nids = 0;
    for (size_t i = 0; i < w.len; i++) {
        if (w.items[i].layer->content.kind == PC_LAYER_TEXT) {
            ids[nids++] = w.items[i].layer->id;
        }
    }
    pc_walk_fini(&w);

    size_t changed = 0;
    for (size_t i = 0; i < nids; i++) {
        pc_layer *l = pc_document_layer_mut(doc, ids[i]);
        if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        pc_text_layer *t = &l->content.u.text;
        const Target *found = NULL;
        for (size_t k = 0; k < ntg; k++) {
            if (tg[k].id == ids[i]) {
                found = &tg[k];
                break;
            }
        }
        bool did;
        if (found != NULL) {
            did = restyle(t, &old, &doc->text_styles, found->has_range, found->a, found->b, op);
        } else {
            Op none = {0};
            did = restyle(t, &old, &doc->text_styles, true, 0, 0, none);
        }
        if (did) {
            pc_engine_type_refresh(doc, t);
            changed++;
        }
    }
    free(ids);
    pc_text_styles_fini(&old);
    return changed;
}

typedef struct {
    const pc_text_styles *new_;
    const Target *tg;
    size_t ntg;
    Op op;
    size_t count;
} commit_ctx;

static pc_status edit_commit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    commit_ctx *c = vctx;
    c->count = apply_styles(doc, c->new_, c->tg, c->ntg, c->op);
    return PC_OK;
}

static pc_status commit(pc_engine_session *s, const char *label, const pc_text_styles *new_, const Target *tg,
                        size_t ntg, Op op, size_t *out_count, pc_error *err)
{
    commit_ctx c = {new_, tg, ntg, op, 0};
    pc_status rc = pc_engine_session_edit(s, label, edit_commit, &c, err);
    if (out_count != NULL) {
        *out_count = c.count;
    }
    return rc;
}

static pc_status styles_of(const pc_engine_session *s, pc_text_styles *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    return pc_text_styles_clone(&pc_doc_arc_get(st->doc)->text_styles, out, err);
}

static pc_status target_ranges(const pc_engine_session *s, yyjson_val *p, Target **out, size_t *n, pc_error *err)
{
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (targets(s, p, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (nids == 0) {
        free(ids);
        return pc_engine_err_other(err, "no type layer to apply the style to");
    }
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    bool use_range = pc_json_get(p, "range") != NULL && pc_json_get(p, "layers") == NULL;
    Target *tg = malloc(nids * sizeof *tg);
    if (tg == NULL) {
        free(ids);
        return pc_fail(err, PC_ERR_NOMEM, "styles");
    }
    for (size_t i = 0; i < nids; i++) {
        tg[i].id = ids[i];
        tg[i].has_range = false;
        tg[i].a = 0;
        tg[i].b = 0;
        if (use_range) {
            const pc_text_layer *t = text_of(doc, ids[i]);
            if (t != NULL) {
                range_of(pc_str_c(&t->text), t->text.len, p, &tg[i].a, &tg[i].b);
                tg[i].has_range = true;
            }
        }
    }
    free(ids);
    *out = tg;
    *n = nids;
    return PC_OK;
}

/* ---- sample / current --------------------------------------------------------------------- */

typedef struct {
    pc_char_style run;   /* owned */
    pc_paragraph_style para;
    pc_layer_id id;
    size_t span_a, span_b;
    bool ok;
} sample_t;

static sample_t sample(const pc_engine_session *s, yyjson_val *p)
{
    sample_t out = {0};
    pc_char_style_default(&out.run);
    out.para = pc_paragraph_style_default();
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (targets(s, p, &ids, &n, NULL) != PC_OK || n == 0) {
        free(ids);
        return out;
    }
    pc_layer_id id = ids[0];
    free(ids);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    const pc_text_layer *t = text_of(doc, id);
    if (t == NULL) {
        return out;
    }
    size_t a, b;
    range_of(pc_str_c(&t->text), t->text.len, p, &a, &b);
    (void)b;
    pc_text_run *cr = NULL;
    size_t ncr = 0;
    (void)pc_text_layer_char_runs(t, &cr, &ncr, NULL);
    size_t at = 0;
    bool run_found = false;
    for (size_t i = 0; i < ncr; i++) {
        size_t s0 = at, e0 = at + cr[i].len;
        at = e0;
        if (a < e0 || e0 == t->text.len) {
            pc_char_style_fini(&out.run);
            if (pc_char_style_clone(&cr[i].style, &out.run, NULL) != PC_OK) {
                pc_char_style_default(&out.run);
            }
            out.span_a = s0;
            out.span_b = e0;
            run_found = true;
            break;
        }
    }
    pc_text_runs_free(cr, ncr);
    if (!run_found) {
        return out;
    }
    pc_paragraph_run *pr = NULL;
    size_t npr = 0;
    (void)pc_text_layer_paragraph_runs(t, &pr, &npr, NULL);
    at = 0;
    bool para_found = false;
    for (size_t i = 0; i < npr; i++) {
        bool hit = a < at + pr[i].len || at + pr[i].len == t->text.len;
        if (hit) {
            out.para = pr[i].style;
            para_found = true;
            break;
        }
        at += pr[i].len;
    }
    free(pr);
    out.id = id;
    out.ok = para_found;
    return out;
}

static bool all_equal_u32(const uint32_t *v, size_t n, uint32_t *out)
{
    if (n == 0) {
        return false;
    }
    for (size_t i = 1; i < n; i++) {
        if (v[i] != v[0]) {
            return false;
        }
    }
    *out = v[0];
    return true;
}

static yyjson_mut_val *current_val(const pc_engine_session *s, yyjson_val *p, yyjson_mut_doc *rd)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return NULL;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_text_styles *styles = &doc->text_styles;
    Target *tr = NULL;
    size_t ntr = 0;
    if (target_ranges(s, p, &tr, &ntr, NULL) != PC_OK) {
        return NULL;
    }
    uint32_t *chars = NULL, *paras = NULL;
    size_t nchars = 0, nparas = 0, cap_c = 0, cap_p = 0;
    bool c_over = false, p_over = false;
    for (size_t ti = 0; ti < ntr; ti++) {
        const pc_text_layer *t = text_of(doc, tr[ti].id);
        if (t == NULL) {
            continue;
        }
        size_t a = tr[ti].has_range ? tr[ti].a : 0, b = tr[ti].has_range ? tr[ti].b : t->text.len;
        size_t tlen = t->text.len;
        /* paragraph spans */
        pc_paragraph_run *pr = NULL;
        size_t npr = 0;
        (void)pc_text_layer_paragraph_runs(t, &pr, &npr, NULL);
        size_t *pstart = npr > 0 ? malloc(npr * sizeof *pstart) : NULL;
        uint32_t *pref = npr > 0 ? malloc(npr * sizeof *pref) : NULL;
        bool *phas = npr > 0 ? malloc(npr * sizeof *phas) : NULL;
        size_t at = 0;
        for (size_t i = 0; i < npr; i++) {
            size_t s0 = at, e0 = at + pr[i].len;
            at = e0;
            pstart[i] = s0;
            phas[i] = pr[i].style.has_style_sheet;
            pref[i] = pr[i].style.style_sheet;
            bool touch = (s0 < b && e0 > a) || (a == b && a >= s0 && (a < e0 || e0 == tlen)) || tlen == 0;
            if (touch) {
                if (nparas == cap_p) {
                    cap_p = cap_p ? cap_p * 2 : 8;
                    paras = realloc(paras, cap_p * sizeof *paras);
                }
                paras[nparas++] = pr[i].style.has_style_sheet ? pr[i].style.style_sheet : 0;
                pc_style_attrs ov = PC_JSON_VALUE_INIT;
                if (pc_text_styles_para_overrides(styles, &pr[i].style, &ov, NULL) == PC_OK &&
                    !pc_style_attrs_is_empty(&ov)) {
                    p_over = true;
                }
                pc_json_value_fini(&ov);
            }
        }
        free(pr);
        /* char runs */
        pc_text_run *cr = NULL;
        size_t ncr = 0;
        (void)pc_text_layer_char_runs(t, &cr, &ncr, NULL);
        at = 0;
        for (size_t i = 0; i < ncr; i++) {
            size_t s0 = at, e0 = at + cr[i].len;
            at = e0;
            bool touch = (s0 < b && e0 > a) || (a == b && a >= s0 && a <= e0) || tlen == 0;
            if (touch) {
                if (nchars == cap_c) {
                    cap_c = cap_c ? cap_c * 2 : 8;
                    chars = realloc(chars, cap_c * sizeof *chars);
                }
                chars[nchars++] = cr[i].style.has_style_sheet ? cr[i].style.style_sheet : 0;
                /* paragraph ref at s0 */
                uint32_t prslot;
                const uint32_t *parg = NULL;
                for (size_t k = npr; k-- > 0;) {
                    if (pstart[k] <= s0) {
                        if (phas[k]) {
                            prslot = pref[k];
                            parg = &prslot;
                        }
                        break;
                    }
                }
                pc_style_attrs ov = PC_JSON_VALUE_INIT;
                if (pc_text_styles_char_overrides(styles, &cr[i].style, parg, FAMILY, &ov, NULL) == PC_OK &&
                    !pc_style_attrs_is_empty(&ov)) {
                    c_over = true;
                }
                pc_json_value_fini(&ov);
            }
        }
        pc_text_runs_free(cr, ncr);
        free(pstart);
        free(pref);
        free(phas);
    }
    free(tr);
    yyjson_mut_val *o = yyjson_mut_obj(rd);
    uint32_t one;
    if (all_equal_u32(chars, nchars, &one)) {
        yyjson_mut_obj_add_uint(rd, o, "character", one);
    } else {
        yyjson_mut_obj_add_null(rd, o, "character");
    }
    yyjson_mut_obj_add_bool(rd, o, "characterOverride", c_over);
    if (all_equal_u32(paras, nparas, &one)) {
        yyjson_mut_obj_add_uint(rd, o, "paragraph", one);
    } else {
        yyjson_mut_obj_add_null(rd, o, "paragraph");
    }
    yyjson_mut_obj_add_bool(rd, o, "paragraphOverride", p_over);
    free(chars);
    free(paras);
    return o;
}

/* ---- command helpers ---------------------------------------------------------------------- */

static pc_status lookup_id(const char *cmd, const pc_text_styles *st, yyjson_val *p, bool paragraph,
                           bool allow_default, uint32_t *out, pc_error *err)
{
    bool present = false;
    uint32_t id = 0;
    if (id_param(cmd, p, &present, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (!present) {
        return bad(err, cmd, "missing `id`");
    }
    bool ok = paragraph ? pc_text_styles_para_style(st, id) != NULL : pc_text_styles_char_style(st, id) != NULL;
    if (!(ok || (allow_default && id == 0))) {
        return pc_engine_err_bad_params(err, cmd, "no style with id %u", id);
    }
    if (id == 0 && !allow_default) {
        return bad(err, cmd, paragraph ? "Basic Paragraph can't be changed this way" : "\"None\" is not a style");
    }
    *out = id;
    return PC_OK;
}

static yyjson_mut_val *attrs_to_rd(yyjson_mut_doc *rd, const pc_style_attrs *a)
{
    yyjson_mut_val *root = pc_json_value_root(a);
    if (root == NULL) {
        return yyjson_mut_obj(rd);
    }
    yyjson_mut_val *copy = pc_json_normalized_copy(rd, root);
    return copy != NULL ? copy : yyjson_mut_obj(rd);
}

static yyjson_mut_val *char_style_resolved(yyjson_mut_doc *rd, const pc_text_styles *st, const uint32_t *para,
                                           const uint32_t *chr)
{
    pc_char_style r;
    if (pc_text_styles_resolve_char(st, para, chr, FAMILY, &r, NULL) != PC_OK) {
        return yyjson_mut_null(rd);
    }
    yyjson_mut_val *v = pc_char_style_to_json(rd, &r, PC_DOC_JSON_VALUE);
    pc_char_style_fini(&r);
    return v;
}

static yyjson_mut_val *para_style_resolved(yyjson_mut_doc *rd, const pc_text_styles *st, const uint32_t *para)
{
    pc_paragraph_style r;
    if (pc_text_styles_resolve_para(st, para, &r, NULL) != PC_OK) {
        r = pc_paragraph_style_default();
    }
    return pc_paragraph_style_to_json(rd, &r, PC_DOC_JSON_VALUE);
}

static yyjson_mut_val *char_json(yyjson_mut_doc *rd, const pc_character_style_def *d, const pc_text_styles *st)
{
    yyjson_mut_val *o = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, o, "id", d->id);
    yyjson_mut_obj_add_strcpy(rd, o, "name", pc_str_c(&d->name));
    yyjson_mut_obj_add_val(rd, o, "attrs", attrs_to_rd(rd, &d->attrs));
    yyjson_mut_obj_add_val(rd, o, "resolved", char_style_resolved(rd, st, NULL, &d->id));
    return o;
}

static yyjson_mut_val *para_json(yyjson_mut_doc *rd, const pc_paragraph_style_def *d, const pc_text_styles *st)
{
    yyjson_mut_val *o = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, o, "id", d->id);
    yyjson_mut_obj_add_strcpy(rd, o, "name", pc_str_c(&d->name));
    yyjson_mut_obj_add_val(rd, o, "paragraphAttrs", attrs_to_rd(rd, &d->para_attrs));
    yyjson_mut_obj_add_val(rd, o, "characterAttrs", attrs_to_rd(rd, &d->char_attrs));
    yyjson_mut_val *res = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_val(rd, res, "paragraph", para_style_resolved(rd, st, &d->id));
    yyjson_mut_obj_add_val(rd, res, "character", char_style_resolved(rd, st, &d->id, NULL));
    yyjson_mut_obj_add_val(rd, o, "resolved", res);
    return o;
}

static pc_status list(pc_engine_session *s, yyjson_val *p, bool paragraph, pc_json_value *out, pc_error *err)
{
    pc_text_styles st = {0};
    if (styles_of(s, &st, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_text_styles_fini(&st);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    if (paragraph) {
        yyjson_mut_arr_append(arr, para_json(rd, &st.basic, &st));
        for (size_t i = 0; i < st.nparagraph; i++) {
            yyjson_mut_arr_append(arr, para_json(rd, &st.paragraph[i], &st));
        }
    } else {
        yyjson_mut_val *none = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, none, "id", 0);
        yyjson_mut_obj_add_strcpy(rd, none, "name", PC_NO_CHARACTER_STYLE);
        yyjson_mut_obj_add_val(rd, none, "attrs", yyjson_mut_obj(rd));
        yyjson_mut_arr_append(arr, none);
        for (size_t i = 0; i < st.ncharacter; i++) {
            yyjson_mut_arr_append(arr, char_json(rd, &st.character[i], &st));
        }
    }
    yyjson_mut_obj_add_val(rd, root, "styles", arr);
    yyjson_mut_val *cur = current_val(s, p, rd);
    yyjson_mut_obj_add_val(rd, root, "current", cur != NULL ? cur : yyjson_mut_null(rd));
    yyjson_mut_doc_set_root(rd, root);
    pc_text_styles_fini(&st);
    return PC_OK;
}

static pc_status id_name_result(pc_json_value *out, uint32_t id, const char *name, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "id", id);
    yyjson_mut_obj_add_strcpy(rd, root, "name", name);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status new_style(pc_engine_session *s, yyjson_val *p, bool paragraph, pc_json_value *out, pc_error *err)
{
    const char *cmd = paragraph ? "type.paragraphStyle.new" : "type.characterStyle.new";
    pc_text_styles st = {0};
    if (styles_of(s, &st, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_style_attrs ca = PC_JSON_VALUE_INIT, pa = PC_JSON_VALUE_INIT;
    if (parse_attrs(cmd, pc_json_get(p, "attrs"), &ca, &pa, err) != PC_OK) {
        pc_text_styles_fini(&st);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool from_text = true;
    bool bb;
    if (pc_json_as_bool(pc_json_get(p, "fromSelection"), &bb)) {
        from_text = bb;
    }
    sample_t sm = {0};
    bool have_sample = false;
    if (from_text) {
        sm = sample(s, p);
        have_sample = sm.ok;
    }
    if (have_sample) {
        if (paragraph) {
            pc_char_style base_c;
            (void)pc_text_styles_resolve_char(&st, NULL, NULL, FAMILY, &base_c, NULL);
            pc_char_style run_noref;
            if (pc_char_style_clone(&sm.run, &run_noref, NULL) == PC_OK) {
                run_noref.has_style_sheet = false;
                pc_style_attrs c = PC_JSON_VALUE_INIT;
                (void)pc_char_style_diff_attrs(&run_noref, &base_c, &c, NULL);
                (void)attrs_merge(&c, &ca, NULL);
                pc_json_value_fini(&ca);
                ca = c;
                pc_char_style_fini(&run_noref);
            }
            pc_char_style_fini(&base_c);
            pc_paragraph_style base_p;
            (void)pc_text_styles_resolve_para(&st, NULL, &base_p, NULL);
            pc_style_attrs pp = PC_JSON_VALUE_INIT;
            (void)pc_paragraph_style_diff_attrs(&sm.para, &base_p, &pp, NULL);
            (void)attrs_merge(&pp, &pa, NULL);
            pc_json_value_fini(&pa);
            pa = pp;
        } else {
            uint32_t pslot;
            const uint32_t *pr = sm.para.has_style_sheet ? (pslot = sm.para.style_sheet, &pslot) : NULL;
            pc_char_style base_c;
            (void)pc_text_styles_resolve_char(&st, pr, NULL, FAMILY, &base_c, NULL);
            pc_style_attrs c = PC_JSON_VALUE_INIT;
            (void)pc_char_style_diff_attrs(&sm.run, &base_c, &c, NULL);
            (void)attrs_merge(&c, &ca, NULL);
            pc_json_value_fini(&ca);
            ca = c;
            pc_char_style_fini(&base_c);
        }
    }
    pc_char_style_fini(&sm.run);

    pc_str name = {0};
    size_t nl = 0;
    const char *nm = pc_json_as_str(pc_json_get(p, "name"), &nl);
    if (nm != NULL) {
        (void)pc_str_set(&name, nm, nl, NULL);
    } else {
        (void)pc_text_styles_unique_name(&st, paragraph, paragraph ? "Paragraph Style" : "Character Style", &name,
                                         NULL);
    }
    uint32_t id = 0;
    pc_status rc = PC_OK;
    if (paragraph) {
        if (!pc_text_styles_next_para_id(&st, &id)) {
            rc = exhausted_id(err, true);
        } else {
            pc_paragraph_style_def *nv = realloc(st.paragraph, (st.nparagraph + 1) * sizeof *nv);
            if (nv == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            } else {
                st.paragraph = nv;
                pc_paragraph_style_def *d = &st.paragraph[st.nparagraph++];
                d->id = id;
                d->name = name;
                d->para_attrs = pa;
                d->char_attrs = ca;
                name = (pc_str){0};
                pa = (pc_json_value)PC_JSON_VALUE_INIT;
                ca = (pc_json_value)PC_JSON_VALUE_INIT;
            }
        }
    } else {
        if (!pc_text_styles_next_char_id(&st, &id)) {
            rc = exhausted_id(err, false);
        } else {
            pc_character_style_def *nv = realloc(st.character, (st.ncharacter + 1) * sizeof *nv);
            if (nv == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            } else {
                st.character = nv;
                pc_character_style_def *d = &st.character[st.ncharacter++];
                d->id = id;
                d->name = name;
                d->attrs = ca;
                name = (pc_str){0};
                ca = (pc_json_value)PC_JSON_VALUE_INIT;
            }
        }
    }
    pc_str_fini(&name);
    pc_json_value_fini(&ca);
    pc_json_value_fini(&pa);
    if (rc != PC_OK) {
        pc_text_styles_fini(&st);
        return rc;
    }
    bool apply_it = false;
    (void)pc_json_as_bool(pc_json_get(p, "apply"), &apply_it);
    Target *tg = NULL;
    size_t ntg = 0;
    Op op = {0};
    if (apply_it) {
        if (target_ranges(s, p, &tg, &ntg, err) != PC_OK) {
            pc_text_styles_fini(&st);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        if (paragraph) {
            op.set_para = true;
            op.set_para_some = true;
            op.set_para_id = id;
        } else {
            op.set_char = true;
            op.set_char_some = true;
            op.set_char_id = id;
        }
    }
    rc = commit(s, paragraph ? "New Paragraph Style" : "New Character Style", &st, tg, ntg, op, NULL, err);
    free(tg);
    /* read back the name for the result */
    const pc_text_styles *live = &pc_doc_arc_get(pc_engine_session_active(s)->doc)->text_styles;
    const char *rname = NULL;
    if (paragraph) {
        const pc_paragraph_style_def *d = pc_text_styles_para_style(live, id);
        rname = d ? pc_str_c(&d->name) : "";
    } else {
        const pc_character_style_def *d = pc_text_styles_char_style(live, id);
        rname = d ? pc_str_c(&d->name) : "";
    }
    pc_status r2 = rc == PC_OK ? id_name_result(out, id, rname, err) : rc;
    pc_text_styles_fini(&st);
    return r2;
}

static pc_status duplicate(pc_engine_session *s, yyjson_val *p, bool paragraph, pc_json_value *out, pc_error *err)
{
    const char *cmd = paragraph ? "type.paragraphStyle.duplicate" : "type.characterStyle.duplicate";
    pc_text_styles st = {0};
    if (styles_of(s, &st, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    uint32_t id;
    if (lookup_id(cmd, &st, p, paragraph, paragraph, &id, err) != PC_OK) {
        pc_text_styles_fini(&st);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    uint32_t nid = 0;
    pc_str name = {0};
    pc_status rc = PC_OK;
    if (paragraph) {
        const pc_paragraph_style_def *src = pc_text_styles_para_style(&st, id);
        pc_paragraph_style_def copy;
        if (src == NULL || pc_paragraph_style_def_clone(src, &copy, err) != PC_OK) {
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
        } else if (!pc_text_styles_next_para_id(&st, &nid)) {
            pc_paragraph_style_def_fini(&copy);
            rc = exhausted_id(err, true);
        } else {
            char nm[256];
            snprintf(nm, sizeof nm, "%s copy", pc_str_c(&copy.name));
            pc_str_fini(&copy.name);
            copy.name = (pc_str){0};
            (void)pc_str_set_c(&copy.name, nm, NULL);
            (void)pc_str_set_c(&name, nm, NULL);
            copy.id = nid;
            size_t pos = st.nparagraph;
            for (size_t i = 0; i < st.nparagraph; i++) {
                if (st.paragraph[i].id == id) {
                    pos = i + 1;
                    break;
                }
            }
            pc_paragraph_style_def *nv = realloc(st.paragraph, (st.nparagraph + 1) * sizeof *nv);
            if (nv == NULL) {
                pc_paragraph_style_def_fini(&copy);
                rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            } else {
                st.paragraph = nv;
                memmove(&st.paragraph[pos + 1], &st.paragraph[pos], (st.nparagraph - pos) * sizeof *nv);
                st.paragraph[pos] = copy;
                st.nparagraph++;
            }
        }
    } else {
        const pc_character_style_def *src = pc_text_styles_char_style(&st, id);
        pc_character_style_def copy;
        if (src == NULL || pc_character_style_def_clone(src, &copy, err) != PC_OK) {
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
        } else if (!pc_text_styles_next_char_id(&st, &nid)) {
            pc_character_style_def_fini(&copy);
            rc = exhausted_id(err, false);
        } else {
            char nm[256];
            snprintf(nm, sizeof nm, "%s copy", pc_str_c(&copy.name));
            pc_str_fini(&copy.name);
            copy.name = (pc_str){0};
            (void)pc_str_set_c(&copy.name, nm, NULL);
            (void)pc_str_set_c(&name, nm, NULL);
            copy.id = nid;
            size_t pos = st.ncharacter;
            for (size_t i = 0; i < st.ncharacter; i++) {
                if (st.character[i].id == id) {
                    pos = i + 1;
                    break;
                }
            }
            pc_character_style_def *nv = realloc(st.character, (st.ncharacter + 1) * sizeof *nv);
            if (nv == NULL) {
                pc_character_style_def_fini(&copy);
                rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            } else {
                st.character = nv;
                memmove(&st.character[pos + 1], &st.character[pos], (st.ncharacter - pos) * sizeof *nv);
                st.character[pos] = copy;
                st.ncharacter++;
            }
        }
    }
    if (rc == PC_OK) {
        rc = commit(s, "Duplicate Style", &st, NULL, 0, (Op){0}, NULL, err);
    }
    pc_status r2 = rc == PC_OK ? id_name_result(out, nid, pc_str_c(&name), err) : rc;
    pc_str_fini(&name);
    pc_text_styles_fini(&st);
    return r2;
}

typedef struct {
    uint32_t id;
    bool paragraph;
    size_t n;
} del_ctx;

static pc_status edit_delete(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    del_ctx *c = vctx;
    if (c->paragraph) {
        for (size_t i = 0; i < doc->text_styles.nparagraph;) {
            if (doc->text_styles.paragraph[i].id == c->id) {
                pc_paragraph_style_def_fini(&doc->text_styles.paragraph[i]);
                memmove(&doc->text_styles.paragraph[i], &doc->text_styles.paragraph[i + 1],
                        (doc->text_styles.nparagraph - i - 1) * sizeof doc->text_styles.paragraph[0]);
                doc->text_styles.nparagraph--;
            } else {
                i++;
            }
        }
    } else {
        for (size_t i = 0; i < doc->text_styles.ncharacter;) {
            if (doc->text_styles.character[i].id == c->id) {
                pc_character_style_def_fini(&doc->text_styles.character[i]);
                memmove(&doc->text_styles.character[i], &doc->text_styles.character[i + 1],
                        (doc->text_styles.ncharacter - i - 1) * sizeof doc->text_styles.character[0]);
                doc->text_styles.ncharacter--;
            } else {
                i++;
            }
        }
    }
    pc_walk w = {0};
    if (pc_document_walk(doc, &w, NULL) != PC_OK) {
        return PC_OK;
    }
    pc_layer_id *ids = w.len > 0 ? malloc(w.len * sizeof *ids) : NULL;
    size_t nids = 0;
    for (size_t i = 0; i < w.len; i++) {
        ids[nids++] = w.items[i].layer->id;
    }
    pc_walk_fini(&w);
    for (size_t li = 0; li < nids; li++) {
        pc_layer *l = pc_document_layer_mut(doc, ids[li]);
        if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        pc_text_layer *t = &l->content.u.text;
        bool hit = false;
        if (c->paragraph) {
            for (size_t i = 0; i < t->nparagraphs; i++) {
                if (t->paragraphs[i].style.has_style_sheet && t->paragraphs[i].style.style_sheet == c->id) {
                    t->paragraphs[i].style.has_style_sheet = false;
                    hit = true;
                }
            }
        } else {
            for (size_t i = 0; i < t->nruns; i++) {
                if (t->runs[i].style.has_style_sheet && t->runs[i].style.style_sheet == c->id) {
                    t->runs[i].style.has_style_sheet = false;
                    hit = true;
                }
            }
        }
        if (hit) {
            pc_engine_type_refresh(doc, t);
            c->n++;
        }
    }
    free(ids);
    return PC_OK;
}

static pc_status delete_style(pc_engine_session *s, yyjson_val *p, bool paragraph, pc_json_value *out, pc_error *err)
{
    const char *cmd = paragraph ? "type.paragraphStyle.delete" : "type.characterStyle.delete";
    pc_text_styles st = {0};
    if (styles_of(s, &st, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    uint32_t id;
    pc_status rc = lookup_id(cmd, &st, p, paragraph, false, &id, err);
    pc_text_styles_fini(&st);
    if (rc != PC_OK) {
        return rc;
    }
    del_ctx c = {id, paragraph, 0};
    if (pc_engine_session_edit(s, "Delete Style", edit_delete, &c, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "deleted", id);
    yyjson_mut_obj_add_uint(rd, root, "layers", c.n);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status rename_style(pc_engine_session *s, yyjson_val *p, bool paragraph, pc_json_value *out, pc_error *err)
{
    const char *cmd = paragraph ? "type.paragraphStyle.rename" : "type.characterStyle.rename";
    pc_text_styles st = {0};
    if (styles_of(s, &st, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    uint32_t id;
    if (lookup_id(cmd, &st, p, paragraph, false, &id, err) != PC_OK) {
        pc_text_styles_fini(&st);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    size_t nl = 0;
    const char *nm = pc_json_as_str(pc_json_get(p, "name"), &nl);
    size_t a = 0, b = nl;
    while (a < b && (unsigned char)nm[a] <= ' ') a++;
    while (b > a && (unsigned char)nm[b - 1] <= ' ') b--;
    if (nm == NULL || b <= a) {
        pc_text_styles_fini(&st);
        return bad(err, cmd, "missing `name`");
    }
    char name[256];
    size_t w = (b - a) < sizeof name - 1 ? (b - a) : sizeof name - 1;
    memcpy(name, nm + a, w);
    name[w] = '\0';
    if (paragraph) {
        pc_paragraph_style_def *d = pc_text_styles_para_style_mut(&st, id);
        if (d != NULL) {
            (void)pc_str_set(&d->name, name, w, NULL);
        }
    } else {
        pc_character_style_def *d = pc_text_styles_char_style_mut(&st, id);
        if (d != NULL) {
            (void)pc_str_set(&d->name, name, w, NULL);
        }
    }
    pc_status rc = commit(s, "Rename Style", &st, NULL, 0, (Op){0}, NULL, err);
    pc_status r2 = rc == PC_OK ? id_name_result(out, id, name, err) : rc;
    pc_text_styles_fini(&st);
    return r2;
}

/* ---- set options / apply / clear override / redefine -------------------------------------- */

static pc_status prepare_options(pc_text_styles *st, yyjson_val *p, bool paragraph, uint32_t *out_id, pc_error *err)
{
    const char *cmd = paragraph ? "type.paragraphStyle.set" : "type.characterStyle.set";
    uint32_t id = 0;
    if (lookup_id(cmd, st, p, paragraph, paragraph, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_style_attrs ca = PC_JSON_VALUE_INIT, pa = PC_JSON_VALUE_INIT;
    if (parse_attrs(cmd, pc_json_get(p, "attrs"), &ca, &pa, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool replace = false;
    (void)pc_json_as_bool(pc_json_get(p, "replace"), &replace);
    yyjson_val *clear = pc_json_get(p, "clear");
    size_t nl = 0;
    const char *nm = pc_json_as_str(pc_json_get(p, "name"), &nl);
    size_t na = 0, nb = nl;
    if (nm != NULL) {
        while (na < nb && (unsigned char)nm[na] <= ' ') na++;
        while (nb > na && (unsigned char)nm[nb - 1] <= ' ') nb--;
    }
    bool have_name = nm != NULL && nb > na;
    pc_status rc = PC_OK;
    if (paragraph) {
        pc_paragraph_style_def *d = pc_text_styles_para_style_mut(st, id);
        if (d == NULL) {
            rc = bad(err, cmd, "no such style");
        } else {
            if (replace) {
                pc_json_value_fini(&d->char_attrs);
            }
            rc = attrs_merge(&d->char_attrs, &ca, err);
            if (replace) {
                pc_json_value_fini(&d->para_attrs);
            }
            if (rc == PC_OK) {
                rc = attrs_merge(&d->para_attrs, &pa, err);
            }
            if (rc == PC_OK && clear != NULL && yyjson_is_arr(clear)) {
                size_t idx, mx;
                yyjson_val *cv;
                yyjson_arr_foreach(clear, idx, mx, cv) {
                    const char *k = pc_json_as_str(cv, NULL);
                    if (k != NULL) {
                        /* removing a key: rebuild without it via diff is overkill; use a helper */
                        pc_style_attrs tmp = PC_JSON_VALUE_INIT;
                        yyjson_mut_val *root = pc_json_value_root(&d->char_attrs);
                        if (root != NULL) {
                            yyjson_mut_obj_iter it;
                            yyjson_mut_val *kk;
                            (void)yyjson_mut_obj_iter_init(root, &it);
                            while ((kk = yyjson_mut_obj_iter_next(&it)) != NULL) {
                                if (strcmp(yyjson_mut_get_str(kk), k) != 0) {
                                    (void)pc_style_attrs_insert(&tmp, yyjson_mut_get_str(kk),
                                                                yyjson_mut_obj_iter_get_val(kk), NULL);
                                }
                            }
                            pc_json_value_fini(&d->char_attrs);
                            d->char_attrs = tmp;
                        }
                        pc_style_attrs tmp2 = PC_JSON_VALUE_INIT;
                        yyjson_mut_val *proot = pc_json_value_root(&d->para_attrs);
                        if (proot != NULL) {
                            yyjson_mut_obj_iter it;
                            yyjson_mut_val *kk;
                            (void)yyjson_mut_obj_iter_init(proot, &it);
                            while ((kk = yyjson_mut_obj_iter_next(&it)) != NULL) {
                                if (strcmp(yyjson_mut_get_str(kk), k) != 0) {
                                    (void)pc_style_attrs_insert(&tmp2, yyjson_mut_get_str(kk),
                                                                yyjson_mut_obj_iter_get_val(kk), NULL);
                                }
                            }
                            pc_json_value_fini(&d->para_attrs);
                            d->para_attrs = tmp2;
                        }
                    }
                }
            }
            if (rc == PC_OK && have_name && id != 0) {
                (void)pc_str_set(&d->name, nm + na, nb - na, NULL);
            }
        }
    } else {
        if (!pc_style_attrs_is_empty(&pa)) {
            rc = bad(err, cmd, "character styles have no paragraph attributes");
        } else {
            pc_character_style_def *d = pc_text_styles_char_style_mut(st, id);
            if (d == NULL) {
                rc = bad(err, cmd, "no such style");
            } else {
                if (replace) {
                    pc_json_value_fini(&d->attrs);
                }
                rc = attrs_merge(&d->attrs, &ca, err);
                if (rc == PC_OK && clear != NULL && yyjson_is_arr(clear)) {
                    size_t idx, mx;
                    yyjson_val *cv;
                    yyjson_arr_foreach(clear, idx, mx, cv) {
                        const char *k = pc_json_as_str(cv, NULL);
                        if (k == NULL) {
                            continue;
                        }
                        pc_style_attrs tmp = PC_JSON_VALUE_INIT;
                        yyjson_mut_val *root = pc_json_value_root(&d->attrs);
                        if (root != NULL) {
                            yyjson_mut_obj_iter it;
                            yyjson_mut_val *kk;
                            (void)yyjson_mut_obj_iter_init(root, &it);
                            while ((kk = yyjson_mut_obj_iter_next(&it)) != NULL) {
                                if (strcmp(yyjson_mut_get_str(kk), k) != 0) {
                                    (void)pc_style_attrs_insert(&tmp, yyjson_mut_get_str(kk),
                                                                yyjson_mut_obj_iter_get_val(kk), NULL);
                                }
                            }
                            pc_json_value_fini(&d->attrs);
                            d->attrs = tmp;
                        }
                    }
                }
                if (rc == PC_OK && have_name) {
                    (void)pc_str_set(&d->name, nm + na, nb - na, NULL);
                }
            }
        }
    }
    pc_json_value_fini(&ca);
    pc_json_value_fini(&pa);
    *out_id = id;
    return rc;
}

static pc_status set_options(pc_engine_session *s, yyjson_val *p, bool paragraph, pc_json_value *out, pc_error *err)
{
    pc_text_styles st = {0};
    if (styles_of(s, &st, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    uint32_t id = 0;
    pc_status rc = prepare_options(&st, p, paragraph, &id, err);
    size_t layers = 0;
    if (rc == PC_OK) {
        rc = commit(s, "Style Options", &st, NULL, 0, (Op){0}, &layers, err);
    }
    pc_text_styles_fini(&st);
    if (rc != PC_OK) {
        return rc;
    }
    rc = list(s, NULL, paragraph, out, err);
    if (rc == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(out);
        if (root != NULL) {
            yyjson_mut_doc *rd = out->doc;
            yyjson_mut_obj_add_uint(rd, root, "layers", layers);
            yyjson_mut_obj_add_uint(rd, root, "id", id);
        }
    }
    return rc;
}

static pc_status result_with_current(pc_engine_session *s, yyjson_val *p, pc_json_value *out, uint32_t id,
                                     bool have_id, size_t layers, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    if (have_id) {
        yyjson_mut_obj_add_uint(rd, root, "id", id);
    }
    yyjson_mut_obj_add_uint(rd, root, "layers", layers);
    yyjson_mut_val *cur = current_val(s, p, rd);
    yyjson_mut_obj_add_val(rd, root, "current", cur != NULL ? cur : yyjson_mut_null(rd));
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status apply_style(pc_engine_session *s, yyjson_val *p, bool paragraph, pc_json_value *out, pc_error *err)
{
    const char *cmd = paragraph ? "type.paragraphStyle.apply" : "type.characterStyle.apply";
    pc_text_styles st = {0};
    if (styles_of(s, &st, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    uint32_t id = 0;
    yyjson_val *idv = pc_json_get(p, "id");
    if (idv != NULL && !yyjson_is_null(idv)) {
        if (lookup_id(cmd, &st, p, paragraph, true, &id, err) != PC_OK) {
            pc_text_styles_fini(&st);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }
    Target *tg = NULL;
    size_t ntg = 0;
    if (target_ranges(s, p, &tg, &ntg, err) != PC_OK) {
        pc_text_styles_fini(&st);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool clear = false;
    (void)pc_json_as_bool(pc_json_get(p, "clearOverrides"), &clear);
    Op op = {0};
    if (paragraph) {
        op.set_para = true;
        op.set_para_some = id != 0;
        op.set_para_id = id;
        op.clear_para = clear;
        op.clear_char = clear;
    } else {
        op.set_char = true;
        op.set_char_some = id != 0;
        op.set_char_id = id;
        op.clear_char = clear;
    }
    size_t n = 0;
    pc_status rc = commit(s, paragraph ? "Apply Paragraph Style" : "Apply Character Style", &st, tg, ntg, op, &n, err);
    free(tg);
    pc_text_styles_fini(&st);
    if (rc != PC_OK) {
        return rc;
    }
    return result_with_current(s, p, out, id, true, n, err);
}

static pc_status clear_override(pc_engine_session *s, yyjson_val *p, bool paragraph, pc_json_value *out, pc_error *err)
{
    pc_text_styles st = {0};
    if (styles_of(s, &st, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    Target *tg = NULL;
    size_t ntg = 0;
    if (target_ranges(s, p, &tg, &ntg, err) != PC_OK) {
        pc_text_styles_fini(&st);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    Op op = {0};
    op.clear_char = true;
    if (paragraph) {
        op.clear_para = true;
    }
    size_t n = 0;
    pc_status rc = commit(s, "Clear Override", &st, tg, ntg, op, &n, err);
    free(tg);
    pc_text_styles_fini(&st);
    if (rc != PC_OK) {
        return rc;
    }
    return result_with_current(s, p, out, 0, false, n, err);
}

static pc_status redefine(pc_engine_session *s, yyjson_val *p, bool paragraph, pc_json_value *out, pc_error *err)
{
    const char *cmd = paragraph ? "type.paragraphStyle.redefine" : "type.characterStyle.redefine";
    pc_text_styles st = {0};
    if (styles_of(s, &st, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    sample_t sm = sample(s, p);
    if (!sm.ok) {
        pc_char_style_fini(&sm.run);
        pc_text_styles_fini(&st);
        return bad(err, cmd, "select type to redefine the style from");
    }
    uint32_t id = 0;
    bool present = false;
    uint32_t pid;
    if (id_param(cmd, p, &present, &pid, err) != PC_OK) {
        pc_char_style_fini(&sm.run);
        pc_text_styles_fini(&st);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_status rc = PC_OK;
    if (present) {
        if (lookup_id(cmd, &st, p, paragraph, paragraph, &id, err) != PC_OK) {
            rc = err != NULL ? err->code : PC_ERR_INVALID;
        }
    } else if (paragraph) {
        id = sm.para.has_style_sheet ? sm.para.style_sheet : 0;
    } else if (sm.run.has_style_sheet) {
        id = sm.run.style_sheet;
    } else {
        rc = bad(err, cmd, "the selected text has no character style");
    }
    if (rc == PC_OK) {
        if (paragraph) {
            uint32_t pslot;
            const uint32_t *idp = id != 0 ? (pslot = id, &pslot) : NULL;
            pc_style_attrs c_over = PC_JSON_VALUE_INIT;
            (void)pc_text_styles_char_overrides(&st, &sm.run, idp, FAMILY, &c_over, NULL);
            /* remove keys present in the character style the run references */
            uint32_t cref = sm.run.style_sheet;
            if (sm.run.has_style_sheet) {
                const pc_character_style_def *cd = pc_text_styles_char_style(&st, cref);
                if (cd != NULL) {
                    pc_style_attrs filtered = PC_JSON_VALUE_INIT;
                    yyjson_mut_val *root = pc_json_value_root(&c_over);
                    if (root != NULL) {
                        yyjson_mut_obj_iter it;
                        yyjson_mut_val *kk;
                        (void)yyjson_mut_obj_iter_init(root, &it);
                        while ((kk = yyjson_mut_obj_iter_next(&it)) != NULL) {
                            if (pc_style_attrs_get(&cd->attrs, yyjson_mut_get_str(kk)) == NULL) {
                                (void)pc_style_attrs_insert(&filtered, yyjson_mut_get_str(kk),
                                                            yyjson_mut_obj_iter_get_val(kk), NULL);
                            }
                        }
                    }
                    pc_json_value_fini(&c_over);
                    c_over = filtered;
                }
            }
            pc_paragraph_style base_para;
            uint32_t pslot2;
            const uint32_t *idp2 = id != 0 ? (pslot2 = id, &pslot2) : NULL;
            (void)pc_text_styles_resolve_para(&st, idp2, &base_para, NULL);
            pc_style_attrs p_over = PC_JSON_VALUE_INIT;
            (void)pc_paragraph_style_diff_attrs(&sm.para, &base_para, &p_over, NULL);
            pc_paragraph_style_def *d = pc_text_styles_para_style_mut(&st, id);
            if (d == NULL) {
                rc = bad(err, cmd, "no such style");
            } else {
                (void)attrs_merge(&d->char_attrs, &c_over, NULL);
                (void)attrs_merge(&d->para_attrs, &p_over, NULL);
            }
            pc_json_value_fini(&c_over);
            pc_json_value_fini(&p_over);
        } else {
            pc_char_style run2;
            if (pc_char_style_clone(&sm.run, &run2, NULL) == PC_OK) {
                run2.has_style_sheet = true;
                run2.style_sheet = id;
                uint32_t pslot;
                const uint32_t *pr = sm.para.has_style_sheet ? (pslot = sm.para.style_sheet, &pslot) : NULL;
                pc_style_attrs over = PC_JSON_VALUE_INIT;
                (void)pc_text_styles_char_overrides(&st, &run2, pr, FAMILY, &over, NULL);
                pc_character_style_def *d = pc_text_styles_char_style_mut(&st, id);
                if (d == NULL) {
                    rc = bad(err, cmd, "no such style");
                } else {
                    (void)attrs_merge(&d->attrs, &over, NULL);
                }
                pc_json_value_fini(&over);
                pc_char_style_fini(&run2);
            }
        }
    }
    size_t n = 0;
    if (rc == PC_OK) {
        Target tg = {sm.id, true, sm.span_a, sm.span_b};
        Op op = {0};
        if (paragraph) {
            op.set_para = true;
            op.set_para_some = id != 0;
            op.set_para_id = id;
        } else {
            op.set_char = true;
            op.set_char_some = true;
            op.set_char_id = id;
        }
        rc = commit(s, paragraph ? "Redefine Paragraph Style" : "Redefine Character Style", &st, &tg, 1, op, &n, err);
    }
    pc_char_style_fini(&sm.run);
    pc_text_styles_fini(&st);
    if (rc != PC_OK) {
        return rc;
    }
    return result_with_current(s, p, out, id, true, n, err);
}

/* ---- dispatch wrappers + registry --------------------------------------------------------- */
#define CHARW(fn) static pc_status c_##fn(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return fn(s, p, false, o, e); }
#define PARAW(fn) static pc_status p_##fn(pc_engine_session *s, yyjson_val *p, pc_json_value *o, pc_error *e) { return fn(s, p, true, o, e); }
CHARW(new_style) PARAW(new_style)
CHARW(duplicate) PARAW(duplicate)
CHARW(delete_style) PARAW(delete_style)
CHARW(rename_style) PARAW(rename_style)
CHARW(set_options) PARAW(set_options)
CHARW(apply_style) PARAW(apply_style)
CHARW(redefine) PARAW(redefine)
CHARW(clear_override) PARAW(clear_override)
CHARW(list) PARAW(list)

#define TARGET_DOC                                                                                                     \
    "\"layer\":id? | \"layers\":[id]? (default: the selected type layers),\"range\":[startChar,endChar]? (one "        \
    "layer; default all text)"
#define ATTRS_DOC                                                                                                      \
    "\"attrs\":{model fields as in type.info (\"size_pt\":36,\"font_family\":\"Inter\",\"align\":\"Center\","          \
    "\xe2\x80\xa6) or Character/Paragraph panel keys as in type.setStyle (\"size\":36,\"font\":\"Inter\",\"color\":"    \
    "\"#rrggbb\",\"align\":\"center\",\xe2\x80\xa6)}"

static const pc_engine_command SPECS[] = {
    {"type.characterStyle.new", "New Character Style", NULL, 0, NULL,
     "{\"name\":str?," ATTRS_DOC "?,\"fromSelection\":bool=true (start from the targeted text's "
     "formatting),\"apply\":bool=false," TARGET_DOC "} \xe2\x86\x92 {\"id\",\"name\"}",
     pc_engine_has_doc, c_new_style, true},
    {"type.characterStyle.duplicate", "Duplicate Style", NULL, 0, NULL, "{\"id\":u32}", pc_engine_has_doc, c_duplicate, true},
    {"type.characterStyle.delete", "Delete Style", NULL, 0, NULL, "{\"id\":u32} (text keeps its formatting as "
     "overrides)", pc_engine_has_doc, c_delete_style, true},
    {"type.characterStyle.rename", "Rename Style", NULL, 0, NULL, "{\"id\":u32,\"name\":str}", pc_engine_has_doc, c_rename_style,
     true},
    {"type.characterStyle.set", "Character Style Options", NULL, 0, NULL,
     "{\"id\":u32 (paragraph: 0 = Basic Paragraph),\"name\":str?," ATTRS_DOC ",\"replace\":bool=false (drop "
     "attributes not given),\"clear\":[field]?} (text using the style updates, overrides kept)",
     pc_engine_has_doc, c_set_options, true},
    {"type.characterStyle.apply", "Apply Character Style", NULL, 0, NULL,
     "{\"id\":u32|0|null (0/null: None / Basic Paragraph),\"clearOverrides\":bool=false (Alt-click)," TARGET_DOC "}",
     has_target, c_apply_style, true},
    {"type.characterStyle.redefine", "Redefine Character Style", NULL, 0, NULL,
     "{\"id\":u32? (default: the targeted text's style)," TARGET_DOC "} (the style takes the formatting of the start "
     "of the targeted text)",
     has_target, c_redefine, true},
    {"type.characterStyle.clearOverride", "Clear Override", NULL, 0, NULL, "{" TARGET_DOC "}", has_target,
     c_clear_override, true},
    {"type.characterStyle.list", "List Character Styles", NULL, 0, NULL,
     "{" TARGET_DOC "} \xe2\x86\x92 {\"styles\":[{\"id\",\"name\",attributes,\"resolved\"}],\"current\":{"
     "\"character\":id|null (mixed),\"characterOverride\":bool,\"paragraph\":id|null,\"paragraphOverride\":bool}}",
     pc_engine_has_doc, c_list, false},
    {"type.paragraphStyle.new", "New Paragraph Style", NULL, 0, NULL,
     "{\"name\":str?," ATTRS_DOC "?,\"fromSelection\":bool=true (start from the targeted text's "
     "formatting),\"apply\":bool=false," TARGET_DOC "} \xe2\x86\x92 {\"id\",\"name\"}",
     pc_engine_has_doc, p_new_style, true},
    {"type.paragraphStyle.duplicate", "Duplicate Style", NULL, 0, NULL, "{\"id\":u32}", pc_engine_has_doc, p_duplicate, true},
    {"type.paragraphStyle.delete", "Delete Style", NULL, 0, NULL, "{\"id\":u32} (text keeps its formatting as "
     "overrides)", pc_engine_has_doc, p_delete_style, true},
    {"type.paragraphStyle.rename", "Rename Style", NULL, 0, NULL, "{\"id\":u32,\"name\":str}", pc_engine_has_doc, p_rename_style,
     true},
    {"type.paragraphStyle.set", "Paragraph Style Options", NULL, 0, NULL,
     "{\"id\":u32 (paragraph: 0 = Basic Paragraph),\"name\":str?," ATTRS_DOC ",\"replace\":bool=false (drop "
     "attributes not given),\"clear\":[field]?} (text using the style updates, overrides kept)",
     pc_engine_has_doc, p_set_options, true},
    {"type.paragraphStyle.apply", "Apply Paragraph Style", NULL, 0, NULL,
     "{\"id\":u32|0|null (0/null: None / Basic Paragraph),\"clearOverrides\":bool=false (Alt-click)," TARGET_DOC "}",
     has_target, p_apply_style, true},
    {"type.paragraphStyle.redefine", "Redefine Paragraph Style", NULL, 0, NULL,
     "{\"id\":u32? (default: the targeted text's style)," TARGET_DOC "} (the style takes the formatting of the start "
     "of the targeted text)",
     has_target, p_redefine, true},
    {"type.paragraphStyle.clearOverride", "Clear Override", NULL, 0, NULL, "{" TARGET_DOC "}", has_target,
     p_clear_override, true},
    {"type.paragraphStyle.list", "List Paragraph Styles", NULL, 0, NULL,
     "{" TARGET_DOC "} \xe2\x86\x92 {\"styles\":[{\"id\",\"name\",attributes,\"resolved\"}],\"current\":{"
     "\"character\":id|null (mixed),\"characterOverride\":bool,\"paragraph\":id|null,\"paragraphOverride\":bool}}",
     pc_engine_has_doc, p_list, false},
};

const pc_engine_command *pc_engine_type_styles_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
