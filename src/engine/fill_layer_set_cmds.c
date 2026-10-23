/* Editing an existing fill layer's content, and querying a layer's content editor (port of
 * crates/engine/src/solid_fill_cmds.rs, pattern_fill_cmds.rs and the layer.layerContentOptions
 * command of layer_menu_cmds.rs):
 *
 *   - layer.fill.solidColor.set      change a Solid Color Fill layer's colour
 *   - layer.fill.pattern.set         change a Pattern Fill layer's pattern/scale/angle/link/phase
 *   - layer.layerContentOptions      which editor the active layer opens (UIs show it)
 *
 * Each edit is one undoable content step, independent of the layer's masks; an unchanged fill is
 * not a history step. Never crashes on hostile params (REWRITE_PLAN.md rule 9): bad colours,
 * patterns, numbers, layers or stale documents are graceful errors that leave the document and
 * history untouched. */
#include <math.h>
#include <string.h>

#include "color/color.h"
#include "doc/doc.h"
#include "doc/fill.h"
#include "doc/pattern.h"

#include "adjust_params.h" /* pc_engine_adjustment_kind */
#include "engine_internal.h"
#include "fill_layer_set_cmds.h"
#include "layer_cmds.h"    /* pc_engine_layer_param */
#include "pattern_cmds.h"  /* pc_engine_pattern_resolve / ensure_in_doc */

/* ---- shared helpers ---------------------------------------------------------------------- */

static pc_status fls_has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK
                                               : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

/* The command's document is still active, and `layer` (when present) is an unsigned id. */
static pc_status fls_check_object(const char *cmd, const pc_engine_session *s, yyjson_val *p, pc_error *err)
{
    if (p == NULL || !yyjson_is_obj(p)) {
        return pc_engine_err_bad_params(err, cmd, "params must be an object and layer an unsigned id");
    }
    yyjson_val *lv = pc_json_get(p, "layer");
    uint64_t u;
    if (lv != NULL && !yyjson_is_null(lv) && !pc_json_as_u64(lv, &u)) {
        return pc_engine_err_bad_params(err, cmd, "params must be an object and layer an unsigned id");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    yyjson_val *dv = pc_json_get(p, "document");
    if (dv != NULL && !yyjson_is_null(dv)) {
        uint64_t want;
        if (!pc_json_as_u64(dv, &want) || want != pc_doc_arc_get(d->doc)->id) {
            return pc_engine_err_bad_params(err, cmd, "the target document is no longer active");
        }
    }
    return PC_OK;
}

static pc_status fls_check_pixels_unlocked(const pc_document *doc, pc_layer_id id, const char *cmd,
                                           pc_error *err)
{
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    pc_locks locks = pc_document_effective_locks(doc, id);
    if (locks.pixels || locks.all) {
        return pc_engine_err_bad_params(err, cmd, "Could not complete your request because the layer "
                                                  "\"%s\" is locked",
                                        pc_str_c(&l->name));
    }
    return PC_OK;
}

/* ---- Solid Color Fill ------------------------------------------------------------------- */

static bool color_eq(const pc_color *a, const pc_color *b)
{
    return a->mode == b->mode && a->c[0] == b->c[0] && a->c[1] == b->c[1] && a->c[2] == b->c[2] &&
           a->c[3] == b->c[3] && a->alpha == b->alpha;
}

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Parse the "color" value in solid_fill_cmds color_param: hex #rrggbb/#rrggbbaa, [r,g,b,a?] (0..1),
 * or a stored Color object {mode, c:[f32;4], alpha}. RGB without alpha preserves `original`'s. */
static pc_status solid_color_param(yyjson_val *v, pc_color original, pc_color_mode mode, pc_color *out,
                                   pc_error *err)
{
    const char *cmd = "layer.fill.solidColor.set";
    pc_color color;
    if (v == NULL) {
        return pc_engine_err_bad_params(err, cmd, "color is required");
    }
    size_t slen = 0;
    const char *s = pc_json_as_str(v, &slen);
    if (s != NULL) {
        const char *h = (slen > 0 && s[0] == '#') ? s + 1 : s;
        size_t n = (slen > 0 && s[0] == '#') ? slen - 1 : slen;
        if (n != 6 && n != 8) {
            return pc_engine_err_bad_params(err, cmd, "color must be #rrggbb or #rrggbbaa");
        }
        int vals[4] = {0, 0, 0, 255};
        for (size_t i = 0; i < n; i += 2) {
            int hi = hexval(h[i]), lo = hexval(h[i + 1]);
            if (hi < 0 || lo < 0) {
                return pc_engine_err_bad_params(err, cmd, "invalid hexadecimal color");
            }
            vals[i / 2] = hi * 16 + lo;
        }
        float a = (n == 8) ? (float)vals[3] / 255.0f : original.alpha;
        pc_color rgb = pc_color_rgba((float)vals[0] / 255.0f, (float)vals[1] / 255.0f,
                                     (float)vals[2] / 255.0f, a);
        color = pc_color_in_mode(&rgb, mode);
    } else if (yyjson_is_arr(v)) {
        size_t len = yyjson_arr_size(v);
        if (len < 3 || len > 4) {
            return pc_engine_err_bad_params(err, cmd, "RGB components must be finite numbers within 0..1");
        }
        float ch[4] = {0, 0, 0, original.alpha};
        for (size_t i = 0; i < len; i++) {
            double d;
            yyjson_val *e = yyjson_arr_get(v, i);
            if (e == NULL || !pc_json_as_f64(e, &d) || !isfinite(d) || d < 0.0 || d > 1.0) {
                return pc_engine_err_bad_params(err, cmd, i == 3
                                                             ? "alpha must be a finite number within 0..1"
                                                             : "RGB components must be finite numbers within 0..1");
            }
            ch[i] = (float)d;
        }
        pc_color rgb = pc_color_rgba(ch[0], ch[1], ch[2], ch[3]);
        color = pc_color_in_mode(&rgb, mode);
    } else if (yyjson_is_obj(v)) {
        yyjson_val *mv = yyjson_obj_get(v, "mode");
        size_t ml = 0;
        const char *ms = mv != NULL ? pc_json_as_str(mv, &ml) : NULL;
        pc_color_mode cm;
        if (ms == NULL || !pc_color_mode_from_name(ms, ml, &cm)) {
            return pc_engine_err_bad_params(err, cmd, "invalid stored Color");
        }
        color.mode = cm;
        color.c[0] = color.c[1] = color.c[2] = color.c[3] = 0.0f;
        yyjson_val *cv = yyjson_obj_get(v, "c");
        if (cv == NULL || !yyjson_is_arr(cv)) {
            return pc_engine_err_bad_params(err, cmd, "invalid stored Color");
        }
        for (size_t i = 0; i < 4 && i < yyjson_arr_size(cv); i++) {
            double d;
            yyjson_val *e = yyjson_arr_get(cv, i);
            if (e != NULL && pc_json_as_f64(e, &d)) {
                color.c[i] = (float)d;
            }
        }
        yyjson_val *av = yyjson_obj_get(v, "alpha");
        double da;
        if (av == NULL || !pc_json_as_f64(av, &da)) {
            return pc_engine_err_bad_params(err, cmd, "invalid stored Color");
        }
        color.alpha = (float)da;
    } else {
        return pc_engine_err_bad_params(err, cmd,
                                        "color must be hexadecimal RGB, [r,g,b,a?], or a stored Color");
    }
    if (!isfinite(color.c[0]) || !isfinite(color.c[1]) || !isfinite(color.c[2]) ||
        !isfinite(color.c[3]) || !isfinite(color.alpha) || color.alpha < 0.0f || color.alpha > 1.0f) {
        return pc_engine_err_bad_params(err, cmd, "color must be finite and alpha within 0..1");
    }
    *out = color;
    return PC_OK;
}

static yyjson_mut_val *color_to_json(yyjson_mut_doc *d, const pc_color *c)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, o, "mode", pc_color_mode_name(c->mode));
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (int i = 0; i < 4; i++) {
        yyjson_mut_arr_add_real(d, arr, (double)c->c[i]);
    }
    yyjson_mut_obj_add_val(d, o, "c", arr);
    yyjson_mut_obj_add_real(d, o, "alpha", (double)c->alpha);
    return o;
}

struct solid_ctx {
    pc_layer_id id;
    pc_color color;
};

static pc_status solid_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct solid_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_fill_fini(&l->content.u.fill);
    l->content.u.fill = pc_fill_solid(c->color);
    return PC_OK;
}

static pc_status run_solid_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "layer.fill.solidColor.set";
    pc_status rc = fls_check_object(cmd, s, p, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id id;
    if ((rc = pc_engine_layer_param(s, p, &id, err)) != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_FILL || l->content.u.fill.kind != PC_FILL_SOLID) {
        return pc_engine_err_bad_params(err, cmd, "the target must be a Solid Color Fill layer");
    }
    pc_color original = l->content.u.fill.u.solid;
    pc_color color = original; /* default unspecified fields to the current colour */
    if ((rc = solid_color_param(pc_json_get(p, "color"), original, doc->mode, &color, err)) != PC_OK) {
        return rc;
    }
    if (!color_eq(&color, &original)) {
        if ((rc = fls_check_pixels_unlocked(doc, id, cmd, err)) != PC_OK) {
            return rc;
        }
        if (pc_document_effective_locks(doc, id).transparency && color.alpha != original.alpha) {
            return pc_engine_err_bad_params(err, cmd, "the layer's transparency is locked");
        }
        struct solid_ctx c = {id, color};
        if ((rc = pc_engine_session_edit(s, "Solid Color", solid_edit, &c, err)) != PC_OK) {
            return rc;
        }
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_obj_add_val(rd, root, "color", color_to_json(rd, &color));
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- Pattern Fill ------------------------------------------------------------------------ */

/* Optional number in [min, max]; *present false when absent/null. */
static pc_status pat_num(yyjson_val *p, const char *k, double min, double max, bool *present, float *out,
                         pc_error *err)
{
    *present = false;
    yyjson_val *v = pc_json_get(p, k);
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    double d;
    if (!pc_json_as_f64(v, &d) || !isfinite(d) || d < min || d > max) {
        return pc_engine_err_bad_params(err, "layer.fill.pattern.set", "`%s` must be a number within %g..%g",
                                        k, min, max);
    }
    *present = true;
    *out = (float)d;
    return PC_OK;
}

struct pattern_ctx {
    pc_layer_id id;
    pc_fill fill;    /* moved into the layer */
    bool have_pat;
    pc_pattern pat;  /* copied into the document when present */
};

static pc_status pattern_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct pattern_ctx *c = vctx;
    if (c->have_pat) {
        pc_status rc = pc_engine_pattern_ensure_in_doc(doc, &c->pat, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_fill_fini(&l->content.u.fill);
    l->content.u.fill = c->fill;
    memset(&c->fill, 0, sizeof c->fill); /* moved */
    return PC_OK;
}

static pc_status run_pattern_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "layer.fill.pattern.set";
    pc_status rc = fls_check_object(cmd, s, p, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id id;
    if ((rc = pc_engine_layer_param(s, p, &id, err)) != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_FILL || l->content.u.fill.kind != PC_FILL_PATTERN) {
        return pc_engine_err_bad_params(err, cmd, "the target must be a Pattern Fill layer");
    }
    pc_fill original;
    if ((rc = pc_fill_clone(&l->content.u.fill, &original, err)) != PC_OK) {
        return rc;
    }

    /* Start from the layer's fill and overlay the given params. */
    struct pattern_ctx c;
    memset(&c, 0, sizeof c);
    if ((rc = pc_fill_clone(&original, &c.fill, err)) != PC_OK) {
        pc_fill_fini(&original);
        return rc;
    }

    /* pattern: id | name (resolve, then copy into the document on apply). */
    yyjson_val *pv = pc_json_get(p, "pattern");
    if (pv != NULL && !yyjson_is_null(pv)) {
        size_t klen = 0;
        const char *key = pc_json_as_str(pv, &klen);
        if (key == NULL || klen == 0) {
            pc_fill_fini(&c.fill);
            pc_fill_fini(&original);
            return pc_engine_err_bad_params(err, cmd, "`pattern` is a pattern id or name");
        }
        bool found = false;
        if ((rc = pc_engine_pattern_resolve(s, key, klen, &c.pat, &found, err)) != PC_OK) {
            pc_fill_fini(&c.fill);
            pc_fill_fini(&original);
            return rc;
        }
        if (!found) {
            pc_fill_fini(&c.fill);
            pc_fill_fini(&original);
            return pc_engine_err_bad_params(err, cmd, "no pattern \"%.*s\" (see pattern.list)", (int)klen, key);
        }
        c.have_pat = true;
        /* The fill takes the new pattern's name and id. */
        pc_str_fini(&c.fill.u.pattern.name);
        pc_str_fini(&c.fill.u.pattern.id);
        if (pc_str_clone(&c.pat.name, &c.fill.u.pattern.name, err) != PC_OK ||
            pc_str_clone(&c.pat.id, &c.fill.u.pattern.id, err) != PC_OK) {
            pc_fill_fini(&c.fill);
            pc_fill_fini(&original);
            if (c.have_pat) {
                pc_pattern_fini(&c.pat);
            }
            return pc_fail(err, PC_ERR_NOMEM, "pattern name");
        }
    }

    bool present;
    float f;
    if ((rc = pat_num(p, "scale", 1.0, 1000.0, &present, &f, err)) != PC_OK) {
        goto fail;
    }
    if (present) {
        c.fill.u.pattern.scale = f / 100.0f;
    }
    if ((rc = pat_num(p, "angle", -180.0, 180.0, &present, &f, err)) != PC_OK) {
        goto fail;
    }
    if (present) {
        c.fill.u.pattern.angle = f;
    }
    yyjson_val *lv = pc_json_get(p, "link");
    if (lv != NULL && !yyjson_is_null(lv)) {
        bool b;
        if (!pc_json_as_bool(lv, &b)) {
            rc = pc_engine_err_bad_params(err, cmd, "`link` is true or false");
            goto fail;
        }
        c.fill.u.pattern.link = b;
    }
    yyjson_val *ph = pc_json_get(p, "phase");
    if (ph != NULL && !yyjson_is_null(ph)) {
        if (!yyjson_is_arr(ph) || yyjson_arr_size(ph) != 2) {
            rc = pc_engine_err_bad_params(err, cmd, "`phase` is [x, y]");
            goto fail;
        }
        double px, py;
        yyjson_val *vx = yyjson_arr_get(ph, 0), *vy = yyjson_arr_get(ph, 1);
        const double MAX_PHASE = 1.0e7;
        if (vx == NULL || vy == NULL || !pc_json_as_f64(vx, &px) || !pc_json_as_f64(vy, &py) ||
            !isfinite(px) || !isfinite(py) || fabs(px) > MAX_PHASE || fabs(py) > MAX_PHASE) {
            rc = pc_engine_err_bad_params(err, cmd, "`phase` needs finite coordinates within +-1e7");
            goto fail;
        }
        c.fill.u.pattern.phase[0] = (float)px;
        c.fill.u.pattern.phase[1] = (float)py;
    }

    bool changed = !pc_fill_eq(&c.fill, &original);
    if (changed) {
        if ((rc = fls_check_pixels_unlocked(doc, id, cmd, err)) != PC_OK) {
            goto fail;
        }
        c.id = id;
        if ((rc = pc_engine_session_edit(s, "Pattern Fill", pattern_edit, &c, err)) != PC_OK) {
            goto fail;
        }
    }

    /* Build the result from c.fill if not moved (unchanged), else from original (== new values). */
    const pc_fill *res = changed ? &original : &c.fill;
    /* When changed, c.fill was moved out; use the values we applied by re-reading the layer. */
    if (changed) {
        const pc_layer *nl = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), id);
        res = (nl != NULL && nl->content.kind == PC_LAYER_FILL) ? &nl->content.u.fill : &original;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto fail;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_obj_add_strcpy(rd, root, "pattern", pc_str_c(&res->u.pattern.id));
    yyjson_mut_obj_add_real(rd, root, "scale", (double)res->u.pattern.scale * 100.0);
    yyjson_mut_obj_add_real(rd, root, "angle", (double)res->u.pattern.angle);
    yyjson_mut_obj_add_bool(rd, root, "link", res->u.pattern.link);
    yyjson_mut_val *pa = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_real(rd, pa, (double)res->u.pattern.phase[0]);
    yyjson_mut_arr_add_real(rd, pa, (double)res->u.pattern.phase[1]);
    yyjson_mut_obj_add_val(rd, root, "phase", pa);
    yyjson_mut_doc_set_root(rd, root);
    rc = PC_OK;
fail:
    pc_fill_fini(&c.fill);
    pc_fill_fini(&original);
    if (c.have_pat) {
        pc_pattern_fini(&c.pat);
    }
    return rc;
}

/* ---- Layer Content Options --------------------------------------------------------------- */

static pc_status content_options_enabled(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    switch (l->content.kind) {
    case PC_LAYER_ADJUSTMENT:
    case PC_LAYER_FILL:
    case PC_LAYER_TEXT:
    case PC_LAYER_SMART:
    case PC_LAYER_SHAPE:
        return PC_OK;
    default:
        return pc_fail(reason, PC_ERR_INVALID, "%s layers have no content options",
                       pc_layer_content_kind_name(&l->content));
    }
}

static pc_status run_content_options(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_layer *l = pc_engine_active_layer_of(s, err);
    if (l == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    char editor[64];
    switch (l->content.kind) {
    case PC_LAYER_ADJUSTMENT:
        (void)snprintf(editor, sizeof editor, "adjustment:%s",
                       pc_engine_adjustment_kind(&l->content.u.adjustment));
        break;
    case PC_LAYER_FILL:
        (void)snprintf(editor, sizeof editor, "fill");
        break;
    case PC_LAYER_TEXT:
        (void)snprintf(editor, sizeof editor, "text");
        break;
    case PC_LAYER_SMART:
        (void)snprintf(editor, sizeof editor, "smartObject");
        break;
    case PC_LAYER_SHAPE:
        (void)snprintf(editor, sizeof editor, "shape");
        break;
    default:
        return pc_engine_err_other(err, "%s layers have no content options",
                                   pc_layer_content_kind_name(&l->content));
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", l->id);
    yyjson_mut_obj_add_strcpy(rd, root, "editor", editor);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- registry ---------------------------------------------------------------------------- */

static const char *const MENU_LAYER[] = {"Layer"};

static const pc_engine_command SPECS[] = {
    {"layer.fill.solidColor.set", "Solid Color…", NULL, 0, NULL,
     "{\"layer\":id? (default active),\"document\":id? (reject a stale dialog),\"color\":\"#rrggbb\"|"
     "\"#rrggbbaa\"|[r,g,b,a?]|{mode,c:[f32;4],alpha:f32}}; RGB without alpha preserves it; a stored "
     "Color keeps its model and precision",
     fls_has_doc, run_solid_set, true},
    {"layer.fill.pattern.set", "Pattern Fill…", NULL, 0, NULL,
     "{\"layer\":id? (default active; a Pattern Fill layer),\"document\":id? (reject a stale dialog),"
     "\"pattern\":id|name?,\"scale\":1..1000?,\"angle\":-180..180?,\"link\":bool?,\"phase\":[x,y]? "
     "(Snap to Origin: [0,0])}; omitted values keep the layer's",
     fls_has_doc, run_pattern_set, true},
    {"layer.layerContentOptions", "Layer Content Options…", MENU_LAYER, 1, NULL, "{}",
     content_options_enabled, run_content_options, false},
};

const pc_engine_command *pc_engine_fill_layer_set_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
