/* Filter > Liquify dialog model. See liquify_dialog.h. Clean-room port of liquify_ui.rs. */
#include "ui/liquify_dialog.h"

#include <math.h>
#include <string.h>

#include "algo/params.h"
#include "base/json.h"
#include "base/mem.h"

#define LIQ_SIZE_MIN 1.0
#define LIQ_SIZE_MAX 15000.0
#define LIQ_HARDNESS_TRAVEL 200.0

static double clampd(double v, double lo, double hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/* ---- options ---- */

void pc_ui_liquify_opts_default(pc_ui_liquify_opts *o)
{
    o->tool = PC_ALGO_LIQUIFY_FORWARD_WARP;
    o->size = 100.0;
    o->density = 50.0;
    o->pressure = 100.0;
    o->rate = 80.0;
    o->show_mesh = false;
    o->mesh_size = 1;
    o->show_mask = true;
    o->show_backdrop = false;
    o->backdrop_opacity = 50.0;
    o->reconstruct_amount = 100.0;
}

static bool read_finite(yyjson_val *obj, const char *key, double *out)
{
    yyjson_val *v = pc_json_get(obj, key);
    double d;
    if (v == NULL || !pc_json_as_f64(v, &d) || !isfinite(d))
        return false;
    *out = d;
    return true;
}

pc_status pc_ui_liquify_opts_apply(pc_ui_liquify_opts *o, yyjson_val *v, pc_error *err)
{
    if (o == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null opts");
    if (v == NULL || !yyjson_is_obj(v))
        return PC_OK; /* a non-object pref leaves the defaults */
    double d;
    if (read_finite(v, "size", &d))
        o->size = clampd(d, LIQ_SIZE_MIN, LIQ_SIZE_MAX);
    if (read_finite(v, "density", &d))
        o->density = clampd(d, 0.0, 100.0);
    if (read_finite(v, "pressure", &d))
        o->pressure = clampd(d, 0.0, 100.0);
    if (read_finite(v, "rate", &d))
        o->rate = clampd(d, 0.0, 100.0);
    if (read_finite(v, "backdropOpacity", &d))
        o->backdrop_opacity = clampd(d, 0.0, 100.0);
    bool b;
    yyjson_val *t;
    if ((t = pc_json_get(v, "showMesh")) != NULL && pc_json_as_bool(t, &b))
        o->show_mesh = b;
    if ((t = pc_json_get(v, "showMask")) != NULL && pc_json_as_bool(t, &b))
        o->show_mask = b;
    if ((t = pc_json_get(v, "showBackdrop")) != NULL && pc_json_as_bool(t, &b))
        o->show_backdrop = b;
    if ((t = pc_json_get(v, "meshSize")) != NULL) {
        size_t len = 0;
        const char *s = pc_json_as_str(t, &len);
        if (s != NULL) {
            if (len == 5 && memcmp(s, "small", 5) == 0)
                o->mesh_size = 0;
            else if (len == 5 && memcmp(s, "large", 5) == 0)
                o->mesh_size = 2;
            else
                o->mesh_size = 1;
        }
    }
    /* A bad tool is applied last and reported (other fields are already set). */
    if ((t = pc_json_get(v, "tool")) != NULL) {
        size_t len = 0;
        const char *s = pc_json_as_str(t, &len);
        int tv;
        if (s == NULL || !pc_algo_enum_from_name(&pc_algo_liquify_tool_desc, s, len, &tv))
            return pc_fail(err, PC_ERR_INVALID, "bad tool");
        o->tool = (pc_algo_liquify_tool)tv;
    }
    return PC_OK;
}

yyjson_mut_val *pc_ui_liquify_opts_to_json(yyjson_mut_doc *d, const pc_ui_liquify_opts *o)
{
    yyjson_mut_val *obj = yyjson_mut_obj(d);
    if (obj == NULL)
        return NULL;
    static const char *const MESH[3] = {"small", "medium", "large"};
    int ms = o->mesh_size < 0 ? 0 : (o->mesh_size > 2 ? 2 : o->mesh_size);
    yyjson_mut_obj_add_str(d, obj, "tool", pc_algo_enum_name(&pc_algo_liquify_tool_desc, (int)o->tool));
    yyjson_mut_obj_add_real(d, obj, "size", o->size);
    yyjson_mut_obj_add_real(d, obj, "density", o->density);
    yyjson_mut_obj_add_real(d, obj, "pressure", o->pressure);
    yyjson_mut_obj_add_real(d, obj, "rate", o->rate);
    yyjson_mut_obj_add_bool(d, obj, "showMesh", o->show_mesh);
    yyjson_mut_obj_add_str(d, obj, "meshSize", MESH[ms]);
    yyjson_mut_obj_add_bool(d, obj, "showMask", o->show_mask);
    yyjson_mut_obj_add_bool(d, obj, "showBackdrop", o->show_backdrop);
    yyjson_mut_obj_add_real(d, obj, "backdropOpacity", o->backdrop_opacity);
    return obj;
}

/* ---- stroke vector helpers (move ownership) ---- */

static pc_status push_stroke(pc_algo_liquify_stroke **vec, size_t *n, size_t *cap,
                             pc_algo_liquify_stroke s, pc_error *err)
{
    void *grown = pc_grow_array(*vec, cap, *n + 1, sizeof **vec);
    if (grown == NULL) {
        pc_algo_liquify_stroke_fini(&s);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *vec = grown;
    (*vec)[(*n)++] = s;
    return PC_OK;
}

static void push_redo(pc_ui_liquify_dialog *s, pc_algo_liquify_stroke st)
{
    if (s->nredo >= PC_UI_LIQUIFY_REDO_LIMIT) {
        pc_algo_liquify_stroke_fini(&s->redo[0]);
        memmove(&s->redo[0], &s->redo[1], (s->nredo - 1) * sizeof *s->redo);
        s->nredo--;
    }
    if (push_stroke(&s->redo, &s->nredo, &s->cap_redo, st, NULL) != PC_OK)
        return; /* on OOM the stroke was freed */
}

static void redo_clear(pc_ui_liquify_dialog *s)
{
    for (size_t i = 0; i < s->nredo; i++)
        pc_algo_liquify_stroke_fini(&s->redo[i]);
    s->nredo = 0;
}

static pc_algo_liquify_stroke stroke_from_opts(const pc_ui_liquify_opts *o, pc_algo_liquify_tool tool)
{
    pc_algo_liquify_stroke s = pc_algo_liquify_stroke_make(tool, o->size);
    s.density = o->density;
    s.pressure = o->pressure;
    s.rate = o->rate;
    return s;
}

/* ---- lifecycle ---- */

pc_status pc_ui_liquify_begin(pc_ui_liquify_dialog *s, uint64_t layer, pc_rect canvas, yyjson_val *prefs,
                              pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    if (canvas.x1 <= canvas.x0 || canvas.y1 <= canvas.y0)
        return pc_fail(err, PC_ERR_INVALID, "liquify needs a non-empty canvas");
    pc_ui_liquify_fini(s);
    memset(s, 0, sizeof *s);
    s->active = true;
    s->layer = layer;
    s->canvas = canvas;
    s->cell = pc_algo_liquify_auto_cell(canvas);
    pc_status rc = pc_algo_liquify_field_init(&s->field, canvas, s->cell, err);
    if (rc != PC_OK) {
        s->active = false;
        return rc;
    }
    pc_ui_liquify_opts_default(&s->opts);
    double longer = (double)(canvas.x1 - canvas.x0);
    double h = (double)(canvas.y1 - canvas.y0);
    if (h > longer)
        longer = h;
    s->opts.size = clampd(round(longer / 10.0), 10.0, 1500.0);
    (void)pc_ui_liquify_opts_apply(&s->opts, prefs, NULL); /* bad fields skipped */
    return PC_OK;
}

void pc_ui_liquify_fini(pc_ui_liquify_dialog *s)
{
    if (s == NULL)
        return;
    pc_algo_liquify_field_fini(&s->field);
    for (size_t i = 0; i < s->nstrokes; i++)
        pc_algo_liquify_stroke_fini(&s->strokes[i]);
    for (size_t i = 0; i < s->nredo; i++)
        pc_algo_liquify_stroke_fini(&s->redo[i]);
    free(s->strokes);
    free(s->redo);
    if (s->has_cur)
        pc_algo_liquify_stroke_fini(&s->cur);
    free(s->lasso);
    memset(s, 0, sizeof *s);
}

static void liq_end(pc_ui_liquify_dialog *s, pc_error *err)
{
    if (!s->has_cur)
        return;
    s->has_cur = false;
    if (push_stroke(&s->strokes, &s->nstrokes, &s->cap_strokes, s->cur, err) != PC_OK)
        return; /* on OOM the cur stroke was freed by push_stroke */
    memset(&s->cur, 0, sizeof s->cur);
}

static pc_status rebuild_field(pc_ui_liquify_dialog *s, pc_error *err)
{
    pc_algo_liquify_field nf;
    memset(&nf, 0, sizeof nf);
    pc_status rc = pc_algo_liquify_field_from_strokes(s->canvas, s->cell, s->strokes, s->nstrokes, &nf, err);
    if (rc != PC_OK)
        return rc;
    pc_algo_liquify_field_fini(&s->field);
    s->field = nf;
    return PC_OK;
}

/* ---- pointer ---- */

pc_algo_liquify_tool pc_ui_liquify_alt_tool(pc_algo_liquify_tool t)
{
    switch (t) {
    case PC_ALGO_LIQUIFY_TWIRL_CW:
        return PC_ALGO_LIQUIFY_TWIRL_CCW;
    case PC_ALGO_LIQUIFY_TWIRL_CCW:
        return PC_ALGO_LIQUIFY_TWIRL_CW;
    case PC_ALGO_LIQUIFY_PUCKER:
        return PC_ALGO_LIQUIFY_BLOAT;
    case PC_ALGO_LIQUIFY_BLOAT:
        return PC_ALGO_LIQUIFY_PUCKER;
    default:
        return t;
    }
}

static pc_status lasso_push(pc_ui_liquify_dialog *s, double x, double y, pc_error *err)
{
    void *grown = pc_grow_array(s->lasso, &s->cap_lasso, s->nlasso + 1, sizeof *s->lasso);
    if (grown == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    s->lasso = grown;
    s->lasso[s->nlasso][0] = x;
    s->lasso[s->nlasso][1] = y;
    s->nlasso++;
    return PC_OK;
}

static pc_status liq_close_lasso(pc_ui_liquify_dialog *s, pc_error *err)
{
    bool subtract = s->lasso_subtract;
    size_t n = s->nlasso;
    if (n < 3) {
        s->nlasso = 0;
        s->lasso_active = false;
        return PC_OK;
    }
    redo_clear(s);
    pc_algo_liquify_stroke st = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_LASSO_MASK, s->opts.size);
    st.has_amount = true;
    st.amount = subtract ? 0.0 : 1.0;
    pc_status rc = PC_OK;
    for (size_t i = 0; i < n && rc == PC_OK; i++) {
        double p[2] = {s->lasso[i][0], s->lasso[i][1]};
        rc = pc_algo_liquify_stroke_push(&st, p, 2, err);
    }
    /* auto-close the polygon */
    if (rc == PC_OK && (fabs(s->lasso[0][0] - s->lasso[n - 1][0]) > 0.5 ||
                        fabs(s->lasso[0][1] - s->lasso[n - 1][1]) > 0.5)) {
        double p[2] = {s->lasso[0][0], s->lasso[0][1]};
        rc = pc_algo_liquify_stroke_push(&st, p, 2, err);
    }
    if (rc == PC_OK)
        rc = pc_algo_liquify_apply_stroke(&s->field, &st, NULL, err);
    if (rc == PC_OK)
        rc = push_stroke(&s->strokes, &s->nstrokes, &s->cap_strokes, st, err);
    else
        pc_algo_liquify_stroke_fini(&st);
    s->nlasso = 0;
    s->lasso_active = false;
    return rc;
}

pc_status pc_ui_liquify_pointer(pc_ui_liquify_dialog *s, pc_ui_pointer_kind kind, double x, double y,
                                double pressure, bool alt, pc_error *err)
{
    if (s == NULL || !s->active)
        return pc_fail(err, PC_ERR_INVALID, "no liquify session");
    if (kind == PC_UI_POINTER_CANCEL) {
        s->lasso_active = false;
        s->nlasso = 0;
        return PC_OK;
    }
    if (!isfinite(x) || !isfinite(y))
        return pc_fail(err, PC_ERR_INVALID, "bad liquify pointer");
    if (!isfinite(pressure))
        pressure = 1.0;
    pressure = clampd(pressure, 0.0, 1.0);
    double p[3] = {x, y, pressure};

    if (kind == PC_UI_POINTER_DOWN) {
        if (s->opts.tool == PC_ALGO_LIQUIFY_LASSO_MASK) {
            s->lasso_active = true;
            s->lasso_subtract = alt;
            s->nlasso = 0;
            return lasso_push(s, x, y, err);
        }
        redo_clear(s);
        pc_algo_liquify_tool tool = alt ? pc_ui_liquify_alt_tool(s->opts.tool) : s->opts.tool;
        s->cur = stroke_from_opts(&s->opts, tool);
        pc_status rc = pc_algo_liquify_stroke_push(&s->cur, p, 3, err);
        if (rc == PC_OK)
            rc = pc_algo_liquify_stroke_begin(&s->field, &s->cur, p, NULL, err);
        if (rc != PC_OK) {
            pc_algo_liquify_stroke_fini(&s->cur);
            memset(&s->cur, 0, sizeof s->cur);
            return rc;
        }
        s->has_cur = true;
        memcpy(s->last_point, p, sizeof p);
        return PC_OK;
    }
    if (kind == PC_UI_POINTER_MOVE) {
        if (s->lasso_active) {
            if (s->nlasso == 0 || s->lasso[s->nlasso - 1][0] != x || s->lasso[s->nlasso - 1][1] != y)
                return lasso_push(s, x, y, err);
            return PC_OK;
        }
        if (s->has_cur) {
            pc_status rc = pc_algo_liquify_stroke_segment(&s->field, &s->cur, s->last_point, p, NULL, err);
            if (rc != PC_OK)
                return rc;
            rc = pc_algo_liquify_stroke_push(&s->cur, p, 3, err);
            if (rc != PC_OK)
                return rc;
            memcpy(s->last_point, p, sizeof p);
        }
        return PC_OK;
    }
    /* UP */
    if (s->lasso_active)
        return liq_close_lasso(s, err);
    if (s->has_cur) {
        if (s->last_point[0] != x || s->last_point[1] != y) {
            double up[3] = {x, y, s->last_point[2]};
            pc_status rc = pc_algo_liquify_stroke_segment(&s->field, &s->cur, s->last_point, up, NULL, err);
            if (rc != PC_OK)
                return rc;
            rc = pc_algo_liquify_stroke_push(&s->cur, up, 3, err);
            if (rc != PC_OK)
                return rc;
        }
        liq_end(s, err);
    }
    return PC_OK;
}

pc_status pc_ui_liquify_global(pc_ui_liquify_dialog *s, pc_algo_liquify_tool tool, bool has_amount,
                              double amount, pc_error *err)
{
    if (s == NULL || !s->active)
        return pc_fail(err, PC_ERR_INVALID, "no liquify session");
    liq_end(s, err);
    redo_clear(s);
    pc_algo_liquify_stroke st = pc_algo_liquify_stroke_make(tool, 1.0);
    st.has_amount = has_amount;
    st.amount = amount;
    pc_status rc = pc_algo_liquify_apply_stroke(&s->field, &st, NULL, err);
    if (rc != PC_OK) {
        pc_algo_liquify_stroke_fini(&st);
        return rc;
    }
    return push_stroke(&s->strokes, &s->nstrokes, &s->cap_strokes, st, err);
}

pc_status pc_ui_liquify_restore_all(pc_ui_liquify_dialog *s, pc_error *err)
{
    if (s == NULL || !s->active)
        return pc_fail(err, PC_ERR_INVALID, "no liquify session");
    if (s->has_cur) {
        pc_algo_liquify_stroke_fini(&s->cur);
        memset(&s->cur, 0, sizeof s->cur);
        s->has_cur = false;
    }
    s->lasso_active = false;
    s->nlasso = 0;
    for (size_t i = 0; i < s->nstrokes; i++)
        pc_algo_liquify_stroke_fini(&s->strokes[i]);
    s->nstrokes = 0;
    redo_clear(s);
    return rebuild_field(s, err);
}

pc_status pc_ui_liquify_undo(pc_ui_liquify_dialog *s, pc_error *err)
{
    if (s == NULL || !s->active)
        return pc_fail(err, PC_ERR_INVALID, "no liquify session");
    liq_end(s, err);
    if (s->nstrokes == 0)
        return PC_OK;
    pc_algo_liquify_stroke popped = s->strokes[--s->nstrokes];
    push_redo(s, popped);
    return rebuild_field(s, err);
}

pc_status pc_ui_liquify_redo(pc_ui_liquify_dialog *s, pc_error *err)
{
    if (s == NULL || !s->active)
        return pc_fail(err, PC_ERR_INVALID, "no liquify session");
    liq_end(s, err);
    if (s->nredo == 0)
        return PC_OK;
    pc_algo_liquify_stroke st = s->redo[--s->nredo];
    pc_status rc = pc_algo_liquify_apply_stroke(&s->field, &st, NULL, err);
    if (rc != PC_OK) {
        pc_algo_liquify_stroke_fini(&st);
        return rc;
    }
    return push_stroke(&s->strokes, &s->nstrokes, &s->cap_strokes, st, err);
}

void pc_ui_liquify_size_step(pc_ui_liquify_dialog *s, int dir)
{
    if (s == NULL)
        return;
    double sz = s->opts.size;
    if (dir < 0)
        sz = clampd(fmin(round(sz * 0.9), sz - 1.0), LIQ_SIZE_MIN, LIQ_SIZE_MAX);
    else if (dir > 0)
        sz = fmin(fmax(round(sz * 1.1), sz + 1.0), LIQ_SIZE_MAX);
    s->opts.size = sz;
}

bool pc_ui_liquify_resize_gesture(bool alt, bool ctrl, bool secondary)
{
    return (secondary && alt) || (ctrl && alt && !secondary);
}

void pc_ui_liquify_resized_brush(double start_size, double start_density, double dx, double dy,
                                 double *size, double *density)
{
    if (size != NULL)
        *size = clampd(round(start_size + dx), LIQ_SIZE_MIN, LIQ_SIZE_MAX);
    if (density != NULL) {
        double d = clampd(start_density + dy * 100.0 / LIQ_HARDNESS_TRAVEL, 0.0, 100.0);
        *density = round(d * 10.0) / 10.0;
    }
}

/* ---- describe / commit ---- */

pc_status pc_ui_liquify_commit_params(pc_ui_liquify_dialog *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                      bool *run, pc_error *err)
{
    if (s == NULL || doc == NULL || out == NULL || run == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    *out = NULL;
    *run = false;
    liq_end(s, err);
    if (s->nstrokes == 0)
        return PC_OK;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    if (obj == NULL || arr == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    for (size_t i = 0; i < s->nstrokes; i++) {
        yyjson_mut_val *sj = pc_algo_liquify_stroke_to_json(doc, &s->strokes[i]);
        if (sj == NULL)
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        yyjson_mut_arr_add_val(arr, sj);
    }
    yyjson_mut_obj_add_val(doc, obj, "strokes", arr);
    yyjson_mut_obj_add_real(doc, obj, "meshSize", s->cell);
    yyjson_mut_obj_add_uint(doc, obj, "layer", s->layer);
    *out = obj;
    *run = true;
    return PC_OK;
}

pc_status pc_ui_liquify_describe(pc_ui_liquify_dialog *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                pc_error *err)
{
    if (s == NULL || doc == NULL || out == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    if (obj == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    yyjson_mut_obj_add_uint(doc, obj, "layer", s->layer);
    yyjson_mut_obj_add_str(doc, obj, "tool", pc_algo_enum_name(&pc_algo_liquify_tool_desc, (int)s->opts.tool));
    yyjson_mut_obj_add_real(doc, obj, "size", s->opts.size);
    yyjson_mut_obj_add_uint(doc, obj, "strokes", s->nstrokes + (s->has_cur ? 1 : 0));
    yyjson_mut_obj_add_uint(doc, obj, "redo", s->nredo);
    yyjson_mut_obj_add_real(doc, obj, "meshSize", s->cell);
    yyjson_mut_obj_add_real(doc, obj, "maxDisplacement", (double)pc_algo_liquify_max_displacement(&s->field));
    *out = obj;
    return PC_OK;
}
