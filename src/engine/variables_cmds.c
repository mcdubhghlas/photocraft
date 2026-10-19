/* Image > Variables and Data Sets (data-driven graphics). Port of
 * crates/engine/src/variables_cmds.rs (E12). Define variables binding a layer's visibility, text
 * or pixels to named slots, then apply a data set to render a variation. */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/resample.h"
#include "base/num.h"
#include "doc/doc.h"
#include "doc/text.h"
#include "doc/variables.h"
#include "io/core.h"
#include "io/io.h"
#include "engine_internal.h"

static pc_status var_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    return PC_OK;
}

/* ---- enum <-> string mapping -------------------------------------------------------------- */

static const char *var_type_id(pc_var_type t)
{
    switch (t) {
    case PC_VAR_VISIBILITY:
        return "visibility";
    case PC_VAR_TEXT_REPLACEMENT:
        return "textReplacement";
    default:
        return "pixelReplacement";
    }
}

static const char *method_id(pc_pixel_method m)
{
    switch (m) {
    case PC_PIXEL_FILL:
        return "fill";
    case PC_PIXEL_AS_IS:
        return "asIs";
    case PC_PIXEL_CONFORM:
        return "conform";
    default:
        return "fit";
    }
}

static const char *align_id(pc_pixel_align a)
{
    switch (a) {
    case PC_ALIGN_TOP_LEFT:
        return "topLeft";
    case PC_ALIGN_TOP_CENTER:
        return "topCenter";
    case PC_ALIGN_TOP_RIGHT:
        return "topRight";
    case PC_ALIGN_CENTER_LEFT:
        return "centerLeft";
    case PC_ALIGN_CENTER_RIGHT:
        return "centerRight";
    case PC_ALIGN_BOTTOM_LEFT:
        return "bottomLeft";
    case PC_ALIGN_BOTTOM_CENTER:
        return "bottomCenter";
    case PC_ALIGN_BOTTOM_RIGHT:
        return "bottomRight";
    default:
        return "center";
    }
}

static bool str_eq(const char *s, size_t len, const char *lit)
{
    return len == strlen(lit) && memcmp(s, lit, len) == 0;
}

/* parse_kind(obj): false if "type" absent/unknown. */
static bool parse_kind(yyjson_val *v, pc_var_kind *out)
{
    size_t tl = 0;
    const char *t = NULL;
    yyjson_val *tv = pc_json_get(v, "type");
    if (tv != NULL) {
        t = pc_json_as_str(tv, &tl);
    }
    if (t == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->align = PC_ALIGN_CENTER;
    if (str_eq(t, tl, "visibility")) {
        out->type = PC_VAR_VISIBILITY;
        return true;
    }
    if (str_eq(t, tl, "textReplacement") || str_eq(t, tl, "text")) {
        out->type = PC_VAR_TEXT_REPLACEMENT;
        return true;
    }
    if (str_eq(t, tl, "pixelReplacement") || str_eq(t, tl, "pixel")) {
        out->type = PC_VAR_PIXEL_REPLACEMENT;
        out->method = PC_PIXEL_FIT;
        size_t ml = 0;
        const char *ms = NULL;
        yyjson_val *mv = pc_json_get(v, "method");
        if (mv != NULL && (ms = pc_json_as_str(mv, &ml)) != NULL) {
            if (str_eq(ms, ml, "fill")) {
                out->method = PC_PIXEL_FILL;
            } else if (str_eq(ms, ml, "asIs")) {
                out->method = PC_PIXEL_AS_IS;
            } else if (str_eq(ms, ml, "conform")) {
                out->method = PC_PIXEL_CONFORM;
            }
        }
        size_t al = 0;
        const char *as = NULL;
        yyjson_val *av = pc_json_get(v, "align");
        if (av != NULL && (as = pc_json_as_str(av, &al)) != NULL) {
            static const struct {
                const char *id;
                pc_pixel_align a;
            } T[] = {{"topLeft", PC_ALIGN_TOP_LEFT},           {"topCenter", PC_ALIGN_TOP_CENTER},
                     {"topRight", PC_ALIGN_TOP_RIGHT},         {"centerLeft", PC_ALIGN_CENTER_LEFT},
                     {"centerRight", PC_ALIGN_CENTER_RIGHT},   {"bottomLeft", PC_ALIGN_BOTTOM_LEFT},
                     {"bottomCenter", PC_ALIGN_BOTTOM_CENTER}, {"bottomRight", PC_ALIGN_BOTTOM_RIGHT}};
            for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
                if (str_eq(as, al, T[i].id)) {
                    out->align = T[i].a;
                    break;
                }
            }
        }
        bool clip = false;
        yyjson_val *cv = pc_json_get(v, "clip");
        if (cv != NULL) {
            (void)pc_json_as_bool(cv, &clip);
        }
        out->clip = clip;
        return true;
    }
    return false;
}

static void add_kind_json(yyjson_mut_doc *d, yyjson_mut_val *o, const pc_var_kind *k)
{
    yyjson_mut_obj_add_str(d, o, "type", var_type_id(k->type));
    if (k->type == PC_VAR_PIXEL_REPLACEMENT) {
        yyjson_mut_obj_add_str(d, o, "method", method_id(k->method));
        yyjson_mut_obj_add_str(d, o, "align", align_id(k->align));
        yyjson_mut_obj_add_bool(d, o, "clip", k->clip);
    }
}

/* ---- no-history variables mutation -------------------------------------------------------- */

typedef void (*vars_mut_fn)(pc_variables *v, void *ctx);

static pc_status set_vars(pc_engine_session *s, vars_mut_fn mut, void *ctx, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_document *doc = NULL;
    if (pc_doc_arc_make_mut(&st->doc, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    mut(&doc->variables, ctx);
    st->revision += 1;
    return PC_OK;
}

/* ---- list --------------------------------------------------------------------------------- */

static pc_status build_list(pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_variables *v = &pc_doc_arc_get(d->doc)->variables;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *defs = yyjson_mut_arr(rd);
    for (size_t i = 0; i < v->ndefs; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, o, "name", pc_str_c(&v->defs[i].name));
        yyjson_mut_obj_add_uint(rd, o, "layer", v->defs[i].layer);
        add_kind_json(rd, o, &v->defs[i].kind);
        yyjson_mut_arr_append(defs, o);
    }
    yyjson_mut_obj_add_val(rd, root, "defs", defs);
    yyjson_mut_val *sets = yyjson_mut_arr(rd);
    for (size_t i = 0; i < v->ndata_sets; i++) {
        const pc_data_set *ds = &v->data_sets[i];
        yyjson_mut_val *so = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, so, "name", pc_str_c(&ds->name));
        yyjson_mut_val *vals = yyjson_mut_arr(rd);
        for (size_t j = 0; j < ds->nvalues; j++) {
            const pc_data_value *dv = &ds->values[j];
            yyjson_mut_val *vo = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_strcpy(rd, vo, "variable", pc_str_c(&dv->variable));
            if (dv->kind == PC_DATA_VISIBILITY) {
                yyjson_mut_obj_add_str(rd, vo, "kind", "visibility");
                yyjson_mut_obj_add_bool(rd, vo, "value", dv->visibility);
            } else if (dv->kind == PC_DATA_TEXT) {
                yyjson_mut_obj_add_str(rd, vo, "kind", "text");
                yyjson_mut_obj_add_strcpy(rd, vo, "value", pc_str_c(&dv->text));
            } else {
                yyjson_mut_obj_add_str(rd, vo, "kind", "pixels");
                yyjson_mut_obj_add_strcpy(rd, vo, "value", pc_str_c(&dv->text));
            }
            yyjson_mut_arr_append(vals, vo);
        }
        yyjson_mut_obj_add_val(rd, so, "values", vals);
        yyjson_mut_arr_append(sets, so);
    }
    yyjson_mut_obj_add_val(rd, root, "dataSets", sets);
    if (v->has_active) {
        yyjson_mut_obj_add_uint(rd, root, "active", v->active);
    } else {
        yyjson_mut_obj_add_null(rd, root, "active");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- define ------------------------------------------------------------------------------- */

typedef struct {
    pc_variable_def *defs;
    size_t n;
} defs_ctx;

static void mut_set_defs(pc_variables *v, void *ctx)
{
    defs_ctx *c = ctx;
    for (size_t i = 0; i < v->ndefs; i++) {
        pc_variable_def_fini(&v->defs[i]);
    }
    free(v->defs);
    v->defs = c->defs;
    v->ndefs = c->n;
    c->defs = NULL;
    c->n = 0;
}

static pc_status run_define(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "image.variables.define";
    yyjson_val *arr = pc_json_get(p, "defs");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return pc_engine_err_bad_params(err, CMD, "need `defs`");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t m = yyjson_arr_size(arr);
    pc_variable_def *defs = m > 0 ? pc_malloc_array(m, sizeof *defs) : NULL;
    if (m > 0 && defs == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    size_t n = 0;
    pc_status rc = PC_OK;
    for (size_t i = 0; i < m; i++) {
        yyjson_val *dd = yyjson_arr_get(arr, i);
        size_t nl = 0;
        const char *name = NULL;
        yyjson_val *nv = pc_json_get(dd, "name");
        if (nv != NULL) {
            name = pc_json_as_str(nv, &nl);
        }
        if (name == NULL) {
            rc = pc_engine_err_bad_params(err, CMD, "a def needs `name`");
            break;
        }
        uint64_t layer = 0;
        yyjson_val *lv = pc_json_get(dd, "layer");
        if (lv == NULL || !pc_json_as_u64(lv, &layer)) {
            rc = pc_engine_err_bad_params(err, CMD, "a def needs `layer`");
            break;
        }
        if (pc_document_layer(doc, layer) == NULL) {
            rc = pc_engine_err_no_layer(err, layer);
            break;
        }
        pc_var_kind kind;
        if (!parse_kind(dd, &kind)) {
            rc = pc_engine_err_bad_params(err, CMD, "a def needs a valid `type`");
            break;
        }
        bool dup = false;
        for (size_t k = 0; k < n; k++) {
            if (pc_str_eq_mem(&defs[k].name, name, nl)) {
                dup = true;
                break;
            }
        }
        if (dup) {
            rc = pc_engine_err_bad_params(err, CMD, "duplicate variable `%.*s`", (int)nl, name);
            break;
        }
        memset(&defs[n], 0, sizeof defs[n]);
        if (pc_str_set(&defs[n].name, name, nl, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            break;
        }
        defs[n].layer = layer;
        defs[n].kind = kind;
        n++;
    }
    if (rc != PC_OK) {
        for (size_t k = 0; k < n; k++) {
            pc_variable_def_fini(&defs[k]);
        }
        free(defs);
        return rc;
    }
    defs_ctx c = {.defs = defs, .n = n};
    rc = set_vars(s, mut_set_defs, &c, err);
    if (rc != PC_OK) {
        for (size_t k = 0; k < c.n; k++) {
            pc_variable_def_fini(&c.defs[k]);
        }
        free(c.defs);
        return rc;
    }
    return build_list(s, out, err);
}

/* ---- parse a data value ------------------------------------------------------------------- */

/* Parse {variable, kind, value} into *out (variable set). false if invalid. */
static bool parse_value(yyjson_val *v, pc_data_value *out)
{
    size_t vl = 0;
    const char *var = NULL;
    yyjson_val *vv = pc_json_get(v, "variable");
    if (vv != NULL) {
        var = pc_json_as_str(vv, &vl);
    }
    if (var == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (pc_str_set(&out->variable, var, vl, NULL) != PC_OK) {
        return false;
    }
    size_t kl = 0;
    const char *kind = NULL;
    yyjson_val *kv = pc_json_get(v, "kind");
    if (kv != NULL) {
        kind = pc_json_as_str(kv, &kl);
    }
    yyjson_val *valv = pc_json_get(v, "value");
    if (kind != NULL && str_eq(kind, kl, "visibility")) {
        bool b;
        if (valv == NULL || !pc_json_as_bool(valv, &b)) {
            pc_data_value_fini(out);
            return false;
        }
        out->kind = PC_DATA_VISIBILITY;
        out->visibility = b;
        return true;
    }
    if (kind != NULL && (str_eq(kind, kl, "text") || str_eq(kind, kl, "pixels"))) {
        size_t sl = 0;
        const char *sv = valv != NULL ? pc_json_as_str(valv, &sl) : NULL;
        if (sv == NULL) {
            pc_data_value_fini(out);
            return false;
        }
        out->kind = str_eq(kind, kl, "text") ? PC_DATA_TEXT : PC_DATA_PIXELS;
        if (pc_str_set(&out->text, sv, sl, NULL) != PC_OK) {
            pc_data_value_fini(out);
            return false;
        }
        return true;
    }
    pc_data_value_fini(out);
    return false;
}

/* ---- dataSets ----------------------------------------------------------------------------- */

typedef struct {
    pc_data_set *sets;
    size_t n;
    bool append;
} sets_ctx;

static void mut_set_data(pc_variables *v, void *ctx)
{
    sets_ctx *c = ctx;
    if (c->append) {
        size_t bytes;
        if (!pc_size_mul(v->ndata_sets + c->n, sizeof *v->data_sets, &bytes)) {
            return;
        }
        pc_data_set *grown = realloc(v->data_sets, bytes);
        if (grown == NULL && (v->ndata_sets + c->n) > 0) {
            return;
        }
        if (grown != NULL) {
            v->data_sets = grown;
        }
        for (size_t i = 0; i < c->n; i++) {
            v->data_sets[v->ndata_sets + i] = c->sets[i];
        }
        v->ndata_sets += c->n;
        free(c->sets);
    } else {
        for (size_t i = 0; i < v->ndata_sets; i++) {
            pc_data_set_fini(&v->data_sets[i]);
        }
        free(v->data_sets);
        v->data_sets = c->sets;
        v->ndata_sets = c->n;
    }
    c->sets = NULL;
    c->n = 0;
}

static pc_status run_data_sets(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "image.variables.dataSets";
    yyjson_val *rows = pc_json_get(p, "dataSets");
    if (rows == NULL || !yyjson_is_arr(rows)) {
        return pc_engine_err_bad_params(err, CMD, "need `dataSets`");
    }
    size_t m = yyjson_arr_size(rows);
    pc_data_set *sets = m > 0 ? pc_malloc_array(m, sizeof *sets) : NULL;
    if (m > 0 && sets == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    size_t n = 0;
    pc_status rc = PC_OK;
    for (size_t i = 0; i < m; i++) {
        yyjson_val *r = yyjson_arr_get(rows, i);
        size_t nl = 0;
        const char *name = NULL;
        yyjson_val *nv = pc_json_get(r, "name");
        if (nv != NULL) {
            name = pc_json_as_str(nv, &nl);
        }
        if (name == NULL) {
            rc = pc_engine_err_bad_params(err, CMD, "a data set needs `name`");
            break;
        }
        pc_data_set ds;
        memset(&ds, 0, sizeof ds);
        if (pc_str_set(&ds.name, name, nl, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            break;
        }
        yyjson_val *vals = pc_json_get(r, "values");
        size_t nvals = vals != NULL && yyjson_is_arr(vals) ? yyjson_arr_size(vals) : 0;
        if (nvals > 0) {
            ds.values = pc_malloc_array(nvals, sizeof *ds.values);
            if (ds.values == NULL) {
                pc_data_set_fini(&ds);
                rc = pc_fail(err, PC_ERR_NOMEM, CMD);
                break;
            }
        }
        bool fail = false;
        for (size_t j = 0; j < nvals; j++) {
            if (!parse_value(yyjson_arr_get(vals, j), &ds.values[ds.nvalues])) {
                rc = pc_engine_err_bad_params(err, CMD, "a value needs `variable`, `kind` and `value`");
                fail = true;
                break;
            }
            ds.nvalues++;
        }
        if (fail) {
            pc_data_set_fini(&ds);
            break;
        }
        sets[n++] = ds;
    }
    if (rc != PC_OK) {
        for (size_t k = 0; k < n; k++) {
            pc_data_set_fini(&sets[k]);
        }
        free(sets);
        return rc;
    }
    bool append = false;
    yyjson_val *av = pc_json_get(p, "append");
    if (av != NULL) {
        (void)pc_json_as_bool(av, &append);
    }
    sets_ctx c = {.sets = sets, .n = n, .append = append};
    rc = set_vars(s, mut_set_data, &c, err);
    if (rc != PC_OK) {
        for (size_t k = 0; k < c.n; k++) {
            pc_data_set_fini(&c.sets[k]);
        }
        free(c.sets);
        return rc;
    }
    return build_list(s, out, err);
}

/* ---- apply -------------------------------------------------------------------------------- */

static void replace_pixels(pc_document *doc, pc_layer_id layer, const char *path, pc_var_kind kind)
{
    const pc_layer *cl = pc_document_layer(doc, layer);
    pc_rect target = pc_document_bounds(doc);
    if (cl != NULL) {
        const pc_surface *sf = pc_layer_surface(cl);
        if (sf != NULL) {
            pc_rect cb = pc_surface_content_bounds(sf);
            if (!pc_rect_is_empty(cb)) {
                target = cb;
            }
        }
    }
    if (pc_rect_is_empty(target)) {
        return;
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_buf bytes = PC_BUF_INIT;
    if (pc_engine_read_file(path, &bytes, NULL) != PC_OK) {
        return;
    }
    pc_document src;
    pc_status rc = pc_engine_import(path, bytes.data, bytes.len, &src, NULL);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        return;
    }
    pc_surface img;
    if (pc_engine_flattened(&src, fmt, &img, NULL) != PC_OK) {
        pc_document_fini(&src);
        return;
    }
    double iw = src.size.width > 0 ? (double)src.size.width : 1.0;
    double ih = src.size.height > 0 ? (double)src.size.height : 1.0;
    double tw = pc_rect_width(target), th = pc_rect_height(target);
    double sx, sy;
    switch (kind.method) {
    case PC_PIXEL_FIT: {
        double k = tw / iw < th / ih ? tw / iw : th / ih;
        sx = sy = k;
        break;
    }
    case PC_PIXEL_FILL: {
        double k = tw / iw > th / ih ? tw / iw : th / ih;
        sx = sy = k;
        break;
    }
    case PC_PIXEL_CONFORM:
        sx = tw / iw;
        sy = th / ih;
        break;
    default:
        sx = sy = 1.0;
        break;
    }
    pc_surface scaled;
    bool scaled_owned = false;
    if (fabs(sx - 1.0) > 1e-9 || fabs(sy - 1.0) > 1e-9) {
        if (pc_algo_resize_surface(&img, sx, sy, PC_ALGO_RESAMPLE_BICUBIC, &scaled, NULL) != PC_OK) {
            pc_surface_fini(&img);
            pc_document_fini(&src);
            return;
        }
        scaled_owned = true;
    } else {
        scaled = img;
    }
    double sw = iw * sx, sh = ih * sy;
    double fx = 0.5, fy = 0.5;
    switch (kind.align) {
    case PC_ALIGN_TOP_LEFT:
    case PC_ALIGN_CENTER_LEFT:
    case PC_ALIGN_BOTTOM_LEFT:
        fx = 0.0;
        break;
    case PC_ALIGN_TOP_RIGHT:
    case PC_ALIGN_CENTER_RIGHT:
    case PC_ALIGN_BOTTOM_RIGHT:
        fx = 1.0;
        break;
    default:
        break;
    }
    switch (kind.align) {
    case PC_ALIGN_TOP_LEFT:
    case PC_ALIGN_TOP_CENTER:
    case PC_ALIGN_TOP_RIGHT:
        fy = 0.0;
        break;
    case PC_ALIGN_BOTTOM_LEFT:
    case PC_ALIGN_BOTTOM_CENTER:
    case PC_ALIGN_BOTTOM_RIGHT:
        fy = 1.0;
        break;
    default:
        break;
    }
    int32_t dx = (int32_t)lround((double)target.x0 + (tw - sw) * fx);
    int32_t dy = (int32_t)lround((double)target.y0 + (th - sh) * fy);
    pc_surface placed;
    if (pc_algo_translate_surface(&scaled, dx, dy, &placed, NULL) != PC_OK) {
        if (scaled_owned) {
            pc_surface_fini(&scaled);
        } else {
            pc_surface_fini(&img);
        }
        pc_document_fini(&src);
        return;
    }
    if (scaled_owned) {
        pc_surface_fini(&scaled);
    } else {
        pc_surface_fini(&img);
    }
    pc_surface final = placed;
    bool final_is_placed = true;
    if (kind.clip) {
        pc_surface cropped;
        if (pc_algo_crop_surface(&placed, target, &cropped, NULL) == PC_OK) {
            pc_surface_fini(&placed);
            final = cropped;
            final_is_placed = false;
        }
    }
    (void)final_is_placed;
    pc_layer *l = pc_document_layer_mut(doc, layer);
    if (l != NULL && l->content.kind == PC_LAYER_RASTER) {
        pc_surface_fini(&l->content.u.raster);
        l->content.u.raster = final;
    } else {
        pc_surface_fini(&final);
    }
    pc_document_fini(&src);
}

static void apply_text(pc_document *doc, pc_layer_id layer, const char *text, size_t len)
{
    pc_document snapshot;
    bool have_snap = pc_document_clone(doc, &snapshot, NULL) == PC_OK;
    pc_layer *l = pc_document_layer_mut(doc, layer);
    if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
        if (have_snap) {
            pc_document_fini(&snapshot);
        }
        return;
    }
    pc_text_layer *t = &l->content.u.text;
    pc_char_style cstyle;
    pc_char_style_default(&cstyle);
    pc_paragraph_style pstyle = pc_paragraph_style_default();
    pc_text_run *cruns = NULL;
    size_t ncr = 0;
    if (pc_text_layer_char_runs(t, &cruns, &ncr, NULL) == PC_OK && ncr > 0) {
        /* clone the first run's style (it owns strings; the source runs are freed next) */
        pc_char_style cloned;
        if (pc_char_style_clone(&cruns[0].style, &cloned, NULL) == PC_OK) {
            pc_char_style_fini(&cstyle);
            cstyle = cloned;
        }
    }
    pc_text_runs_free(cruns, ncr);
    pc_paragraph_run *pruns = NULL;
    size_t npr = 0;
    if (pc_text_layer_paragraph_runs(t, &pruns, &npr, NULL) == PC_OK && npr > 0) {
        pstyle = pruns[0].style;
    }
    free(pruns);
    if (pc_str_set(&t->text, text, len, NULL) != PC_OK) {
        if (have_snap) {
            pc_document_fini(&snapshot);
        }
        return;
    }
    size_t tlen = len;
    pc_text_run *nr = malloc(sizeof *nr);
    pc_paragraph_run *np = malloc(sizeof *np);
    if (nr != NULL && np != NULL) {
        nr[0].len = tlen;
        nr[0].style = cstyle;
        np[0].len = tlen;
        np[0].style = pstyle;
        pc_text_runs_free(t->runs, t->nruns);
        free(t->paragraphs);
        t->runs = nr;
        t->nruns = 1;
        t->paragraphs = np;
        t->nparagraphs = 1;
        (void)pc_text_layer_sync_summary(t, NULL);
        if (have_snap) {
            pc_engine_hook_refresh_text(&snapshot, l);
        }
    } else {
        free(nr);
        free(np);
    }
    if (have_snap) {
        pc_document_fini(&snapshot);
    }
}

/* apply one data set's values to the document. */
static pc_status apply_to_doc(pc_document *doc, const pc_variables *vars, const pc_data_set *set,
                              pc_error *err)
{
    (void)err;
    for (size_t i = 0; i < set->nvalues; i++) {
        const pc_data_value *dv = &set->values[i];
        const pc_variable_def *def = pc_variables_def(vars, pc_str_c(&dv->variable), dv->variable.len);
        if (def == NULL) {
            continue;
        }
        if (def->kind.type == PC_VAR_VISIBILITY && dv->kind == PC_DATA_VISIBILITY) {
            pc_layer *l = pc_document_layer_mut(doc, def->layer);
            if (l != NULL) {
                l->visible = dv->visibility;
            }
        } else if (def->kind.type == PC_VAR_TEXT_REPLACEMENT && dv->kind == PC_DATA_TEXT) {
            apply_text(doc, def->layer, pc_str_c(&dv->text), dv->text.len);
        } else if (def->kind.type == PC_VAR_PIXEL_REPLACEMENT && dv->kind == PC_DATA_PIXELS) {
            replace_pixels(doc, def->layer, pc_str_c(&dv->text), def->kind);
        }
    }
    return PC_OK;
}

typedef struct {
    pc_variables vars; /* cloned */
    pc_data_set set;   /* cloned */
} apply_ctx;

static pc_status edit_apply(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    apply_ctx *c = ctx;
    return apply_to_doc(doc, &c->vars, &c->set, err);
}

/* resolve a data set by {name} or {index}: clones into *set, index in *idx. */
static pc_status resolve_set(pc_engine_session *s, yyjson_val *p, const char *cmd, pc_data_set *set,
                             size_t *idx, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_variables *v = &pc_doc_arc_get(d->doc)->variables;
    yyjson_val *nv = pc_json_get(p, "name");
    size_t nl = 0;
    const char *name = nv != NULL ? pc_json_as_str(nv, &nl) : NULL;
    if (name != NULL) {
        size_t i;
        if (!pc_variables_data_set_index(v, name, nl, &i)) {
            return pc_engine_err_bad_params(err, cmd, "no data set `%.*s`", (int)nl, name);
        }
        *idx = i;
        return pc_data_set_clone(&v->data_sets[i], set, err);
    }
    uint64_t i;
    yyjson_val *iv = pc_json_get(p, "index");
    if (iv != NULL && pc_json_as_u64(iv, &i)) {
        if (i >= v->ndata_sets) {
            return pc_engine_err_bad_params(err, cmd, "no data set at index %llu", (unsigned long long)i);
        }
        *idx = (size_t)i;
        return pc_data_set_clone(&v->data_sets[(size_t)i], set, err);
    }
    return pc_engine_err_bad_params(err, cmd, "need `name` or `index`");
}

typedef struct {
    size_t idx;
} active_ctx;

static void mut_active(pc_variables *v, void *ctx)
{
    v->has_active = true;
    v->active = ((active_ctx *)ctx)->idx;
}

static pc_status run_apply_data_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "image.applyDataSet";
    pc_data_set set;
    memset(&set, 0, sizeof set);
    size_t idx = 0;
    pc_status rc = resolve_set(s, p, CMD, &set, &idx, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Every pixel path must be authorized, including when preparing the request fails. */
    if (s->authorize != NULL) {
        for (size_t i = 0; i < set.nvalues; i++) {
            if (set.values[i].kind != PC_DATA_PIXELS)
                continue;
            yyjson_mut_doc *request = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *object = request ? yyjson_mut_obj(request) : NULL;
            pc_buf text = PC_BUF_INIT;
            yyjson_doc *parsed = NULL;
            pc_status check =
                object && yyjson_mut_obj_add_strcpy(request, object, "path", pc_str_c(&set.values[i].text))
                    ? pc_json_write(object, 0, &text, err)
                    : pc_fail(err, PC_ERR_NOMEM, "data set authorization");
            if (check == PC_OK)
                check = pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, err);
            if (check == PC_OK)
                check = s->authorize(CMD, yyjson_doc_get_root(parsed), err);
            yyjson_doc_free(parsed);
            pc_buf_fini(&text);
            yyjson_mut_doc_free(request);
            if (check != PC_OK) {
                pc_data_set_fini(&set);
                return check;
            }
        }
    }
    apply_ctx c;
    memset(&c, 0, sizeof c);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    rc = pc_variables_clone(&pc_doc_arc_get(d->doc)->variables, &c.vars, err);
    if (rc == PC_OK) {
        c.set = set; /* move */
    } else {
        pc_data_set_fini(&set);
        return rc;
    }
    char label[128];
    snprintf(label, sizeof label, "Apply Data Set \"%s\"", pc_str_c(&set.name));
    rc = pc_engine_session_edit(s, label, edit_apply, &c, err);
    pc_variables_fini(&c.vars);
    pc_data_set_fini(&c.set);
    if (rc != PC_OK) {
        return rc;
    }
    active_ctx ac = {.idx = idx};
    (void)set_vars(s, mut_active, &ac, NULL);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    /* name came from the resolved set; re-read for the result */
    const pc_engine_doc_state *d2 = pc_engine_session_active(s);
    const pc_variables *v = &pc_doc_arc_get(d2->doc)->variables;
    if (idx < v->ndata_sets) {
        yyjson_mut_obj_add_strcpy(rd, root, "applied", pc_str_c(&v->data_sets[idx].name));
    } else {
        yyjson_mut_obj_add_str(rd, root, "applied", "");
    }
    yyjson_mut_obj_add_uint(rd, root, "index", idx);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- CSV import --------------------------------------------------------------------------- */

/* Split one CSV line into fields (double-quote support) appended to *out (array of pc_str). */
static size_t split_csv(const char *line, size_t len, char delim, pc_str **out)
{
    pc_str *fields = NULL;
    size_t nf = 0, cap = 0;
    pc_buf cur = PC_BUF_INIT;
    bool in_q = false;
    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (in_q) {
            if (c == '"') {
                if (i + 1 < len && line[i + 1] == '"') {
                    (void)pc_buf_append_byte(&cur, '"', NULL);
                    i++;
                } else {
                    in_q = false;
                }
            } else {
                (void)pc_buf_append_byte(&cur, (uint8_t)c, NULL);
            }
        } else if (c == '"') {
            in_q = true;
        } else if (c == delim) {
            if (nf == cap) {
                cap = cap ? cap * 2 : 8;
                pc_str *g = realloc(fields, cap * sizeof *g);
                if (g == NULL) {
                    break;
                }
                fields = g;
            }
            memset(&fields[nf], 0, sizeof fields[nf]);
            (void)pc_str_set(&fields[nf], cur.data ? (const char *)cur.data : "", cur.len, NULL);
            nf++;
            pc_buf_fini(&cur);
            pc_buf_init(&cur);
        } else {
            (void)pc_buf_append_byte(&cur, (uint8_t)c, NULL);
        }
    }
    if (nf == cap) {
        cap = cap ? cap * 2 : 8;
        pc_str *g = realloc(fields, cap * sizeof *g);
        if (g != NULL) {
            fields = g;
        }
    }
    if (fields != NULL) {
        memset(&fields[nf], 0, sizeof fields[nf]);
        (void)pc_str_set(&fields[nf], cur.data ? (const char *)cur.data : "", cur.len, NULL);
        nf++;
    }
    pc_buf_fini(&cur);
    *out = fields;
    return nf;
}

static void free_fields(pc_str *f, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_str_fini(&f[i]);
    }
    free(f);
}

static bool truthy(const pc_str *s)
{
    const char *c = pc_str_c(s);
    while (*c == ' ' || *c == '\t') {
        c++;
    }
    char buf[16];
    size_t k = 0;
    while (c[k] != '\0' && c[k] != ' ' && c[k] != '\t' && k < sizeof buf - 1) {
        buf[k] = (char)tolower((unsigned char)c[k]);
        k++;
    }
    buf[k] = '\0';
    return strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0 || strcmp(buf, "yes") == 0 ||
           strcmp(buf, "visible") == 0 || strcmp(buf, "on") == 0;
}

static bool blank(const pc_str *s)
{
    const char *c = pc_str_c(s);
    for (size_t i = 0; i < s->len; i++) {
        if (c[i] != ' ' && c[i] != '\t' && c[i] != '\r' && c[i] != '\n') {
            return false;
        }
    }
    return true;
}

static pc_status import_data_sets_text(pc_engine_session *s, const char *text, size_t len, yyjson_val *p,
                                       pc_json_value *out, pc_error *err)
{
    const char *CMD = "file.import.variableDataSets";
    char delim = ',';
    yyjson_val *dv = pc_json_get(p, "delimiter");
    size_t dl = 0;
    const char *ds = dv != NULL ? pc_json_as_str(dv, &dl) : NULL;
    if (ds != NULL && dl > 0) {
        delim = ds[0];
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_variables vars;
    if (pc_variables_clone(&pc_doc_arc_get(d->doc)->variables, &vars, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    /* split into logical records on unquoted newlines */
    pc_str *header = NULL;
    size_t nheader = 0;
    pc_data_set *sets = NULL;
    size_t nsets = 0, cap = 0;
    size_t i = 0;
    bool in_q = false;
    size_t rec_start = 0;
    size_t row_index = 0;
    bool named = false;
    pc_status rc = PC_OK;
    while (i <= len) {
        bool end = (i == len);
        if (!end && text[i] == '"') {
            in_q = !in_q;
        }
        if (end || (text[i] == '\n' && !in_q)) {
            size_t rlen = i - rec_start;
            const char *rec = text + rec_start;
            if (rlen > 0 && rec[rlen - 1] == '\r') {
                rlen--;
            }
            /* skip blank records */
            bool all_ws = true;
            for (size_t k = 0; k < rlen; k++) {
                if (rec[k] != ' ' && rec[k] != '\t' && rec[k] != '\r') {
                    all_ws = false;
                    break;
                }
            }
            if (!(rlen == 0 || all_ws)) {
                pc_str *fields = NULL;
                size_t nf = split_csv(rec, rlen, delim, &fields);
                if (nheader == 0 && header == NULL) {
                    header = fields;
                    nheader = nf;
                    named = nheader > 0 &&
                            (blank(&header[0]) || pc_str_eq_c(&header[0], "DataSet") ||
                             pc_str_eq_c(&header[0], "dataset") || pc_str_eq_c(&header[0], "Dataset"));
                    if (nheader == 0) {
                        free_fields(fields, nf);
                        rc = pc_engine_err_bad_params(err, CMD, "empty CSV");
                        break;
                    }
                } else {
                    pc_data_set set;
                    memset(&set, 0, sizeof set);
                    char nm[64];
                    if (named && nf > 0 && !blank(&fields[0])) {
                        (void)pc_str_set(&set.name, pc_str_c(&fields[0]), fields[0].len, NULL);
                    } else {
                        snprintf(nm, sizeof nm, "Data Set %zu", row_index + 1);
                        (void)pc_str_set_c(&set.name, nm, NULL);
                    }
                    size_t start = named ? 1 : 0;
                    set.values = pc_malloc_array(nheader > 0 ? nheader : 1, sizeof *set.values);
                    for (size_t ci = start; ci < nheader && set.values != NULL; ci++) {
                        if (ci >= nf) {
                            continue;
                        }
                        const pc_variable_def *def =
                            pc_variables_def(&vars, pc_str_c(&header[ci]), header[ci].len);
                        if (def == NULL) {
                            continue;
                        }
                        pc_data_value val;
                        memset(&val, 0, sizeof val);
                        (void)pc_str_clone(&header[ci], &val.variable, NULL);
                        if (def->kind.type == PC_VAR_VISIBILITY) {
                            val.kind = PC_DATA_VISIBILITY;
                            val.visibility = truthy(&fields[ci]);
                        } else if (def->kind.type == PC_VAR_PIXEL_REPLACEMENT) {
                            val.kind = PC_DATA_PIXELS;
                            (void)pc_str_clone(&fields[ci], &val.text, NULL);
                        } else {
                            val.kind = PC_DATA_TEXT;
                            (void)pc_str_clone(&fields[ci], &val.text, NULL);
                        }
                        set.values[set.nvalues++] = val;
                    }
                    if (nsets == cap) {
                        cap = cap ? cap * 2 : 8;
                        pc_data_set *g = realloc(sets, cap * sizeof *g);
                        if (g == NULL) {
                            pc_data_set_fini(&set);
                            free_fields(fields, nf);
                            rc = pc_fail(err, PC_ERR_NOMEM, CMD);
                            break;
                        }
                        sets = g;
                    }
                    sets[nsets++] = set;
                    row_index++;
                    free_fields(fields, nf);
                }
            }
            rec_start = i + 1;
        }
        i++;
    }
    free_fields(header, nheader);
    if (rc != PC_OK) {
        for (size_t k = 0; k < nsets; k++) {
            pc_data_set_fini(&sets[k]);
        }
        free(sets);
        pc_variables_fini(&vars);
        return rc;
    }
    pc_variables_fini(&vars);
    size_t n = nsets;
    sets_ctx c = {.sets = sets, .n = nsets, .append = false};
    rc = set_vars(s, mut_set_data, &c, err);
    if (rc != PC_OK) {
        for (size_t k = 0; k < c.n; k++) {
            pc_data_set_fini(&c.sets[k]);
        }
        free(c.sets);
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "imported", n);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_import(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "file.import.variableDataSets";
    size_t pl = 0;
    const char *path = NULL;
    yyjson_val *pv = pc_json_get(p, "path");
    if (pv != NULL) {
        path = pc_json_as_str(pv, &pl);
    }
    if (path == NULL) {
        return pc_engine_err_bad_params(err, CMD, "need `path`");
    }
    pc_buf text = PC_BUF_INIT;
    pc_status rc = pc_engine_read_file(path, &text, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = import_data_sets_text(s, (const char *)text.data, text.len, p, out, err);
    pc_buf_fini(&text);
    return rc;
}

/* ---- export data sets as files ------------------------------------------------------------ */

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 1;
        }
        a++;
        b++;
    }
    return (*a || *b) ? 1 : 0;
}

static char *sanitize_name(const char *name)
{
    size_t n = strlen(name);
    char *out = malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        out[i] = (isalnum(c) || c == '-' || c == '_') ? name[i] : '_';
    }
    out[n] = '\0';
    return out;
}

static pc_status run_export(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "file.export.dataSetsAsFiles";
    size_t dl = 0;
    const char *dir = NULL;
    yyjson_val *dv = pc_json_get(p, "dir");
    if (dv != NULL) {
        dir = pc_json_as_str(dv, &dl);
    }
    if (dir == NULL) {
        return pc_engine_err_bad_params(err, CMD, "need `dir`");
    }
    char dirbuf[4096];
    if (dl >= sizeof dirbuf) {
        return pc_engine_err_bad_params(err, CMD, "`dir` is too long");
    }
    memcpy(dirbuf, dir, dl);
    dirbuf[dl] = '\0';
    size_t fl = 0;
    const char *fmt = "png";
    char fmtbuf[32];
    yyjson_val *fv = pc_json_get(p, "format");
    const char *fs = fv != NULL ? pc_json_as_str(fv, &fl) : NULL;
    if (fs != NULL && fl < sizeof fmtbuf) {
        memcpy(fmtbuf, fs, fl);
        fmtbuf[fl] = '\0';
        fmt = fmtbuf;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_document original;
    if (pc_document_clone(pc_doc_arc_get(d->doc), &original, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_variables vars;
    if (pc_variables_clone(&original.variables, &vars, err) != PC_OK) {
        pc_document_fini(&original);
        return PC_ERR_NOMEM;
    }
    /* selected names (optional) */
    yyjson_val *wanted = pc_json_get(p, "dataSets");
    const char *naming = "{name}";
    char nambuf[128];
    size_t nl = 0;
    yyjson_val *nv = pc_json_get(p, "naming");
    const char *ns = nv != NULL ? pc_json_as_str(nv, &nl) : NULL;
    if (ns != NULL && nl < sizeof nambuf) {
        memcpy(nambuf, ns, nl);
        nambuf[nl] = '\0';
        naming = nambuf;
    }
    (void)naming; /* only {name}/{index} default templates handled below */

    /* collect the chosen sets (indices into vars.data_sets) */
    size_t *chosen = pc_malloc_array(vars.ndata_sets > 0 ? vars.ndata_sets : 1, sizeof *chosen);
    if (chosen == NULL) {
        pc_variables_fini(&vars);
        pc_document_fini(&original);
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    size_t nchosen = 0;
    for (size_t i = 0; i < vars.ndata_sets; i++) {
        bool take = true;
        if (wanted != NULL && yyjson_is_arr(wanted)) {
            take = false;
            for (size_t k = 0; k < yyjson_arr_size(wanted); k++) {
                size_t wl = 0;
                const char *w = pc_json_as_str(yyjson_arr_get(wanted, k), &wl);
                if (w != NULL && pc_str_eq_mem(&vars.data_sets[i].name, w, wl)) {
                    take = true;
                    break;
                }
            }
        }
        if (take) {
            chosen[nchosen++] = i;
        }
    }
    if (nchosen == 0) {
        free(chosen);
        pc_variables_fini(&vars);
        pc_document_fini(&original);
        return pc_engine_err_bad_params(err, CMD, "no data sets to export");
    }
    /* plan names, detect collisions */
    char **names = pc_malloc_array(nchosen, sizeof *names);
    if (names == NULL) {
        free(chosen);
        pc_variables_fini(&vars);
        pc_document_fini(&original);
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    bool have_index_template = strstr(naming, "{index}") != NULL;
    pc_status rc = PC_OK;
    size_t made = 0;
    for (; made < nchosen; made++) {
        char *safe = sanitize_name(pc_str_c(&vars.data_sets[chosen[made]].name));
        if (safe == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, CMD);
            break;
        }
        char idx[24];
        snprintf(idx, sizeof idx, "%03zu", made + 1);
        /* default template {name}: just safe.fmt; if {index} present, use index */
        char *nm = malloc(strlen(safe) + strlen(fmt) + 16);
        if (nm == NULL) {
            free(safe);
            rc = pc_fail(err, PC_ERR_NOMEM, CMD);
            break;
        }
        if (have_index_template) {
            sprintf(nm, "%s.%s", idx, fmt);
        } else {
            sprintf(nm, "%s.%s", safe, fmt);
        }
        free(safe);
        /* collision (case-insensitive) */
        for (size_t k = 0; k < made; k++) {
            if (ci_eq(nm, names[k]) == 0) {
                free(nm);
                rc = pc_engine_err_bad_params(err, CMD,
                                              "data sets resolve to the same output; include {index} "
                                              "in `naming` to use distinct filenames");
                goto done_plan;
            }
        }
        names[made] = nm;
    }
done_plan:
    if (rc != PC_OK) {
        for (size_t k = 0; k < made; k++) {
            free(names[k]);
        }
        free(names);
        free(chosen);
        pc_variables_fini(&vars);
        pc_document_fini(&original);
        return rc;
    }
    /* render + write */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = rc == PC_OK ? yyjson_mut_obj(rd) : NULL;
    yyjson_mut_val *files = rc == PC_OK ? yyjson_mut_arr(rd) : NULL;
    for (size_t k = 0; k < nchosen && rc == PC_OK; k++) {
        pc_document doc;
        if (pc_document_clone(&original, &doc, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            break;
        }
        apply_to_doc(&doc, &vars, &vars.data_sets[chosen[k]], NULL);
        pc_io_export_opts opts = pc_io_export_opts_default();
        pc_io_export_result res;
        memset(&res, 0, sizeof res);
        pc_status erc = pc_io_export(&doc, fmt, &opts, &res, err);
        pc_document_fini(&doc);
        if (erc != PC_OK) {
            rc = erc;
            break;
        }
        char full[4200];
        snprintf(full, sizeof full, "%s/%s", dirbuf, names[k]);
        rc = pc_engine_write_file(full, res.bytes, res.len, err);
        pc_io_export_result_fini(&res);
        if (rc == PC_OK) {
            yyjson_mut_arr_add_strcpy(rd, files, full);
        }
    }
    if (rc == PC_OK) {
        yyjson_mut_obj_add_val(rd, root, "files", files);
        yyjson_mut_obj_add_uint(rd, root, "count", nchosen);
        yyjson_mut_doc_set_root(rd, root);
    } else {
        pc_json_value_fini(out);
    }
    for (size_t k = 0; k < nchosen; k++) {
        free(names[k]);
    }
    free(names);
    free(chosen);
    pc_variables_fini(&vars);
    pc_document_fini(&original);
    return rc;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return build_list(s, out, err);
}

static const char *const MENU_IMG_VARS[] = {"Image", "Variables"};
static const char *const MENU_IMG[] = {"Image"};
static const char *const MENU_FILE_IMPORT[] = {"File", "Import"};
static const char *const MENU_FILE_EXPORT[] = {"File", "Export"};

static const pc_engine_command SPECS[] = {
    {"image.variables.define", "Define\xE2\x80\xA6", MENU_IMG_VARS, 2, NULL,
     "{defs:[{name, layer:id, type:visibility|textReplacement|pixelReplacement, method?, align?, "
     "clip?}]} \xE2\x86\x92 {defs,dataSets,active}",
     var_has_doc, run_define, true},
    {"image.variables.dataSets", "Data Sets\xE2\x80\xA6", MENU_IMG_VARS, 2, NULL,
     "{dataSets:[{name, values:[{variable, kind:visibility|text|pixels, value}]}], append?} \xE2\x86\x92 "
     "{defs,dataSets,active}",
     var_has_doc, run_data_sets, true},
    {"image.applyDataSet", "Apply Data Set\xE2\x80\xA6", MENU_IMG, 1, NULL,
     "{name|index} \xE2\x86\x92 {applied, index}: sets layer visibility/text/pixels (one history step)",
     var_has_doc, run_apply_data_set, true},
    {"file.import.variableDataSets", "Variable Data Sets\xE2\x80\xA6", MENU_FILE_IMPORT, 2, NULL,
     "{path, delimiter?} \xE2\x86\x92 {imported}: CSV header = variable names, each row a data set",
     var_has_doc, run_import, true},
    {"file.export.dataSetsAsFiles", "Data Sets as Files\xE2\x80\xA6", MENU_FILE_EXPORT, 2, NULL,
     "{dir, format?:png, dataSets?[names], naming?:\"{name}|{index}|{document}\"} \xE2\x86\x92 "
     "{files,count}",
     var_has_doc, run_export, true},
    {"variables.list", "List Variables", NULL, 0, NULL, "{} \xE2\x86\x92 {defs,dataSets,active}", var_has_doc,
     run_list, false},
};

const pc_engine_command *pc_engine_variables_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
