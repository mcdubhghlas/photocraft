/* Image > Analysis: measurement scale, data points, Record Measurements and the Measurement Log,
 * the Ruler tool (with protractor and Straighten Layer), the Count tool and Place Scale Marker
 * (port of crates/engine/src/analysis_cmds.rs).
 *
 * DEFERRED in this build (graceful, never a crash), with owners:
 *   - Place Scale Marker -> builds a group with a bar raster + a type layer (E11 type.create / E5
 *     group placement); reported unsupported.
 *   - Straighten Layer -> runs image.rotation.arbitrary / image.crop / edit.transform as a compound
 *     step (E3/E4 arbitrary rotation + transform); reported unsupported.
 *   - Export Measurements to a path -> needs the file writer (E3 file_cmds); the CSV text is still
 *     returned when no path is given. */
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "algo/selection.h"
#include "base/mem.h"
#include "compose/compose.h"
#include "doc/analysis.h"
#include "engine_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

/* A heap copy of a NUL-terminated string (pc_str has no plain dup). */
static char *str_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (r != NULL) {
        memcpy(r, s, n);
    }
    return r;
}

/* ---- columns + data points ---------------------------------------------------------------- */

struct column {
    const char *key;
    const char *name;
};
static const struct column COLUMNS[] = {
    {"label", "Label"},
    {"dateTime", "Date and Time"},
    {"document", "Document"},
    {"source", "Source"},
    {"scale", "Scale"},
    {"scaleUnits", "Scale Units"},
    {"scaleFactor", "Scale Factor"},
    {"count", "Count"},
    {"area", "Area"},
    {"perimeter", "Perimeter"},
    {"circularity", "Circularity"},
    {"height", "Height"},
    {"width", "Width"},
    {"grayMin", "Gray Value (Minimum)"},
    {"grayMax", "Gray Value (Maximum)"},
    {"grayMean", "Gray Value (Mean)"},
    {"grayMedian", "Gray Value (Median)"},
    {"integratedDensity", "Integrated Density"},
    {"histogram", "Histogram"},
    {"length", "Length"},
    {"angle", "Angle"},
};
#define NCOLUMNS (sizeof COLUMNS / sizeof COLUMNS[0])

static const char *const COMMON[] = {"label",      "dateTime",   "document",
                                     "source",     "scale",      "scaleUnits",
                                     "scaleFactor"};
static const char *const SELECTION_ONLY[] = {"count",     "area",       "perimeter",
                                             "circularity", "height",     "width",
                                             "grayMin",    "grayMax",    "grayMean",
                                             "grayMedian", "integratedDensity", "histogram"};
static const char *const RULER_ONLY[] = {"length", "angle"};
static const char *const COUNT_ONLY[] = {"count"};

/* Source index: 0 selection, 1 ruler, 2 count. */
static int source_index(const char *source)
{
    if (strcmp(source, "selection") == 0) {
        return 0;
    }
    if (strcmp(source, "ruler") == 0) {
        return 1;
    }
    return 2;
}

/* Data points a measurement source can record, into out (static strings); returns the count. */
static size_t available(const char *source, const char **out)
{
    size_t n = 0;
    for (size_t i = 0; i < sizeof COMMON / sizeof COMMON[0]; i++) {
        out[n++] = COMMON[i];
    }
    int si = source_index(source);
    const char *const *extra;
    size_t ne;
    if (si == 0) {
        extra = SELECTION_ONLY;
        ne = sizeof SELECTION_ONLY / sizeof SELECTION_ONLY[0];
    } else if (si == 1) {
        extra = RULER_ONLY;
        ne = sizeof RULER_ONLY / sizeof RULER_ONLY[0];
    } else {
        extra = COUNT_ONLY;
        ne = sizeof COUNT_ONLY / sizeof COUNT_ONLY[0];
    }
    for (size_t i = 0; i < ne; i++) {
        out[n++] = extra[i];
    }
    return n;
}

static bool available_contains(const char *source, const char *key)
{
    const char *buf[32];
    size_t n = available(source, buf);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(buf[i], key) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- session analysis state --------------------------------------------------------------- */

struct log_row {
    uint64_t id;
    yyjson_mut_doc *values; /* an object doc of the row's data-point values */
};

struct str_list {
    char **items;
    size_t n;
};

struct pc_engine_analysis {
    struct log_row *log;
    size_t nlog, cap_log;
    uint64_t next_row;
    uint64_t next_measurement;
    struct str_list dp[3]; /* selection, ruler, count */
};

static void str_list_fini(struct str_list *l)
{
    for (size_t i = 0; i < l->n; i++) {
        free(l->items[i]);
    }
    free(l->items);
    l->items = NULL;
    l->n = 0;
}

static bool str_list_set_default(struct str_list *l, const char *source)
{
    const char *buf[32];
    size_t n = available(source, buf);
    char **items = pc_malloc_array(n, sizeof *items);
    if (items == NULL) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        items[i] = str_dup(buf[i]);
        if (items[i] == NULL) {
            for (size_t j = 0; j < i; j++) {
                free(items[j]);
            }
            free(items);
            return false;
        }
    }
    str_list_fini(l);
    l->items = items;
    l->n = n;
    return true;
}

void pc_engine_analysis_fini(pc_engine_session *s)
{
    if (s == NULL || s->analysis == NULL) {
        return;
    }
    struct pc_engine_analysis *a = s->analysis;
    for (size_t i = 0; i < a->nlog; i++) {
        yyjson_mut_doc_free(a->log[i].values);
    }
    free(a->log);
    for (int i = 0; i < 3; i++) {
        str_list_fini(&a->dp[i]);
    }
    free(a);
    s->analysis = NULL;
}

/* The analysis state, created with defaults on first use (NULL on OOM). */
static struct pc_engine_analysis *analysis(pc_engine_session *s)
{
    if (s->analysis != NULL) {
        return s->analysis;
    }
    struct pc_engine_analysis *a = pc_calloc_array(1, sizeof *a);
    if (a == NULL) {
        return NULL;
    }
    static const char *const names[3] = {"selection", "ruler", "count"};
    for (int i = 0; i < 3; i++) {
        if (!str_list_set_default(&a->dp[i], names[i])) {
            for (int j = 0; j < 3; j++) {
                str_list_fini(&a->dp[j]);
            }
            free(a);
            return NULL;
        }
    }
    s->analysis = a;
    return a;
}

/* ---- helpers ------------------------------------------------------------------------------ */

static pc_status bad(pc_error *err, const char *cmd, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK
                                               : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status has_ruler(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!pc_doc_arc_get(d->doc)->measurement.has_ruler) {
        return pc_fail(reason, PC_ERR_INVALID, "no ruler line");
    }
    return PC_OK;
}

static pc_status has_log(const pc_engine_session *s, pc_error *reason)
{
    const struct pc_engine_analysis *a = ((const pc_engine_session *)s)->analysis;
    if (a == NULL || a->nlog == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "the Measurement Log is empty");
    }
    return PC_OK;
}

static double round4(double v)
{
    return round(v * 10000.0) / 10000.0;
}

/* Read "x"/"y" doubles, or a [x,y] array; returns false when absent/ill-formed. */
static bool point_of(yyjson_val *v, double out[2])
{
    if (v == NULL || !yyjson_is_arr(v) || yyjson_arr_size(v) != 2) {
        return false;
    }
    yyjson_val *a = yyjson_arr_get(v, 0), *b = yyjson_arr_get(v, 1);
    if (!yyjson_is_num(a) || !yyjson_is_num(b)) {
        return false;
    }
    out[0] = yyjson_get_num(a);
    out[1] = yyjson_get_num(b);
    return true;
}

static yyjson_val *obj_get(yyjson_val *p, const char *key)
{
    return (p != NULL && yyjson_is_obj(p)) ? yyjson_obj_get(p, key) : NULL;
}

static bool get_bool(yyjson_val *p, const char *key, bool def)
{
    yyjson_val *v = obj_get(p, key);
    return (v != NULL && yyjson_is_bool(v)) ? yyjson_get_bool(v) : def;
}

/* Modify the active document without a history step (ruler line: view-like but saved). */
static pc_status set_quiet_begin(pc_engine_session *s, pc_document **out, pc_engine_doc_state **st_out,
                                 pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_document *d = NULL;
    if (pc_doc_arc_make_mut(&st->doc, &d, err) != PC_OK || d == NULL) {
        return err != NULL && err->code != PC_OK ? err->code : PC_ERR_NOMEM;
    }
    *out = d;
    *st_out = st;
    return PC_OK;
}

static void set_quiet_end(pc_engine_doc_state *st)
{
    bool clean = st->saved_revision == st->revision;
    st->revision++;
    if (clean) {
        st->saved_revision = st->revision;
    }
    st->has_last_damage = true;
    st->last_damage = PC_RECT_EMPTY;
}

/* ---- scale -------------------------------------------------------------------------------- */

static pc_status scale_json(yyjson_mut_doc *d, yyjson_mut_val **out, const pc_measurement_scale *sc,
                            pc_error *err)
{
    pc_str desc;
    desc.ptr = NULL;
    desc.len = 0;
    if (pc_measurement_scale_describe(sc, &desc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_real(d, o, "pixelLength", sc->pixel_length);
    yyjson_mut_obj_add_real(d, o, "logicalLength", sc->logical_length);
    yyjson_mut_obj_add_strcpy(d, o, "units", pc_str_c(&sc->units));
    yyjson_mut_obj_add_real(d, o, "factor", pc_measurement_scale_factor(sc));
    yyjson_mut_obj_add_strcpy(d, o, "text", pc_str_c(&desc));
    yyjson_mut_obj_add_bool(d, o, "default", pc_measurement_scale_is_default(sc));
    pc_str_fini(&desc);
    *out = o;
    return PC_OK;
}

struct scale_ctx {
    pc_measurement_scale next;
};

static pc_status edit_set_scale(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct scale_ctx *c = vctx;
    pc_measurement_scale copy;
    if (pc_measurement_scale_clone(&c->next, &copy, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_measurement_scale_fini(&doc->measurement.scale);
    doc->measurement.scale = copy;
    return PC_OK;
}

static pc_status run_set_scale(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "image.analysis.setMeasurementScale";
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_measurement_scale *cur = &pc_doc_arc_get(st->doc)->measurement.scale;
    bool empty = (p == NULL) || !yyjson_is_obj(p) || yyjson_obj_size(p) == 0;
    const char *preset = "custom";
    yyjson_val *pv = obj_get(p, "preset");
    if (pv != NULL && yyjson_is_str(pv)) {
        preset = yyjson_get_str(pv);
    } else if (empty) {
        preset = "query";
    }
    pc_measurement_scale next;
    bool next_owned = false;
    if (strcmp(preset, "query") == 0) {
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *o = NULL;
        if (scale_json(d, &o, cur, err) != PC_OK) {
            pc_json_value_fini(out);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        yyjson_mut_doc_set_root(d, o);
        return PC_OK;
    } else if (strcmp(preset, "default") == 0) {
        if (pc_measurement_scale_default(&next, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        next_owned = true;
    } else if (strcmp(preset, "custom") == 0) {
        double pl = cur->pixel_length, ll = cur->logical_length;
        yyjson_val *v = obj_get(p, "pixelLength");
        if (v != NULL && yyjson_is_num(v)) {
            pl = yyjson_get_num(v);
        }
        v = obj_get(p, "logicalLength");
        if (v != NULL && yyjson_is_num(v)) {
            ll = yyjson_get_num(v);
        }
        const char *units = pc_str_c(&cur->units);
        v = obj_get(p, "units");
        if (v == NULL) {
            v = obj_get(p, "logicalUnits");
        }
        if (v != NULL && yyjson_is_str(v)) {
            units = yyjson_get_str(v);
        }
        if (!(pl > 0.0 && isfinite(pl) && ll > 0.0 && isfinite(ll))) {
            return bad(err, cmd, "pixelLength and logicalLength must be positive");
        }
        /* units must not be only whitespace */
        bool blank = true;
        for (const char *q = units; *q; q++) {
            if (*q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') {
                blank = false;
                break;
            }
        }
        if (blank) {
            return bad(err, cmd, "units must not be empty");
        }
        memset(&next, 0, sizeof next);
        next.pixel_length = pl;
        next.logical_length = ll;
        if (pc_str_set_c(&next.units, units, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        next_owned = true;
    } else {
        return bad(err, cmd, "unknown preset `%s` (default|custom)", preset);
    }
    if (!pc_measurement_scale_eq(&next, cur)) {
        struct scale_ctx ctx = {next};
        pc_status rc = pc_engine_session_edit(s, "Set Measurement Scale", edit_set_scale, &ctx, err);
        if (rc != PC_OK) {
            if (next_owned) {
                pc_measurement_scale_fini(&next);
            }
            return rc;
        }
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        if (next_owned) {
            pc_measurement_scale_fini(&next);
        }
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *o = NULL;
    pc_status rc = scale_json(d, &o, &next, err);
    if (next_owned) {
        pc_measurement_scale_fini(&next);
    }
    if (rc != PC_OK) {
        pc_json_value_fini(out);
        return rc;
    }
    yyjson_mut_doc_set_root(d, o);
    return PC_OK;
}

/* ---- data points -------------------------------------------------------------------------- */

static void list_remove(struct str_list *l, const char *k)
{
    size_t w = 0;
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->items[i], k) == 0) {
            free(l->items[i]);
        } else {
            l->items[w++] = l->items[i];
        }
    }
    l->n = w;
}

static bool list_push(struct str_list *l, const char *k)
{
    char **items = pc_malloc_array(l->n + 1, sizeof *items);
    if (items == NULL) {
        return false;
    }
    for (size_t i = 0; i < l->n; i++) {
        items[i] = l->items[i];
    }
    items[l->n] = str_dup(k);
    if (items[l->n] == NULL) {
        free(items);
        return false;
    }
    free(l->items);
    l->items = items;
    l->n++;
    return true;
}

static int column_pos(const char *k)
{
    for (size_t i = 0; i < NCOLUMNS; i++) {
        if (strcmp(COLUMNS[i].key, k) == 0) {
            return (int)i;
        }
    }
    return (int)NCOLUMNS;
}

static int cmp_by_column(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    return column_pos(x) - column_pos(y);
}

static yyjson_mut_val *available_json(yyjson_mut_doc *d, const char *source)
{
    const char *buf[32];
    size_t n = available(source, buf);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_arr_add_strcpy(d, arr, buf[i]);
    }
    return arr;
}

static yyjson_mut_val *str_list_json(yyjson_mut_doc *d, const struct str_list *l)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < l->n; i++) {
        yyjson_mut_arr_add_strcpy(d, arr, l->items[i]);
    }
    return arr;
}

static pc_status run_select_data_points(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                         pc_error *err)
{
    const char *cmd = "image.analysis.selectDataPoints";
    struct pc_engine_analysis *a = analysis(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "analysis");
    }
    /* Work on a copy so a bad key leaves the state unchanged. */
    struct str_list next[3];
    memset(next, 0, sizeof next);
    static const char *const names[3] = {"selection", "ruler", "count"};
    bool reset = get_bool(p, "reset", false);
    for (int i = 0; i < 3; i++) {
        if (reset) {
            if (!str_list_set_default(&next[i], names[i])) {
                goto oom;
            }
        } else {
            next[i].items = pc_malloc_array(a->dp[i].n > 0 ? a->dp[i].n : 1, sizeof(char *));
            if (next[i].items == NULL) {
                goto oom;
            }
            for (size_t j = 0; j < a->dp[i].n; j++) {
                next[i].items[j] = str_dup(a->dp[i].items[j]);
                if (next[i].items[j] == NULL) {
                    next[i].n = j;
                    goto oom;
                }
            }
            next[i].n = a->dp[i].n;
        }
    }
    pc_status rc = PC_OK;
    for (int i = 0; i < 3; i++) {
        yyjson_val *v = obj_get(p, names[i]);
        if (v == NULL) {
            continue;
        }
        if (yyjson_is_arr(v)) {
            struct str_list keys;
            memset(&keys, 0, sizeof keys);
            size_t m = yyjson_arr_size(v);
            keys.items = pc_malloc_array(m > 0 ? m : 1, sizeof(char *));
            if (keys.items == NULL) {
                goto oom;
            }
            for (size_t j = 0; j < m; j++) {
                yyjson_val *kv = yyjson_arr_get(v, j);
                if (!yyjson_is_str(kv)) {
                    str_list_fini(&keys);
                    rc = bad(err, cmd, "`%s` entries must be strings", names[i]);
                    goto fail;
                }
                const char *k = yyjson_get_str(kv);
                if (!available_contains(names[i], k)) {
                    str_list_fini(&keys);
                    rc = bad(err, cmd, "`%s` is not a %s data point", k, names[i]);
                    goto fail;
                }
                if (!list_push(&keys, k)) {
                    str_list_fini(&keys);
                    goto oom;
                }
            }
            str_list_fini(&next[i]);
            next[i] = keys;
        } else if (yyjson_is_obj(v)) {
            size_t idx, max;
            yyjson_val *key, *on;
            yyjson_obj_foreach(v, idx, max, key, on) {
                const char *k = yyjson_get_str(key);
                if (k == NULL || !available_contains(names[i], k)) {
                    rc = bad(err, cmd, "`%s` is not a %s data point", k != NULL ? k : "", names[i]);
                    goto fail;
                }
                if (!yyjson_is_bool(on)) {
                    rc = bad(err, cmd, "`%s.%s` must be a bool", names[i], k);
                    goto fail;
                }
                list_remove(&next[i], k);
                if (yyjson_get_bool(on)) {
                    if (!list_push(&next[i], k)) {
                        goto oom;
                    }
                }
            }
        } else {
            rc = bad(err, cmd, "`%s` must be a list of keys or an object of bools", names[i]);
            goto fail;
        }
        if (next[i].n > 1) {
            qsort(next[i].items, next[i].n, sizeof(char *), cmp_by_column);
        }
    }
    /* commit */
    for (int i = 0; i < 3; i++) {
        str_list_fini(&a->dp[i]);
        a->dp[i] = next[i];
        memset(&next[i], 0, sizeof next[i]);
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *dpo = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, dpo, "selection", str_list_json(d, &a->dp[0]));
    yyjson_mut_obj_add_val(d, dpo, "ruler", str_list_json(d, &a->dp[1]));
    yyjson_mut_obj_add_val(d, dpo, "count", str_list_json(d, &a->dp[2]));
    yyjson_mut_obj_add_val(d, root, "dataPoints", dpo);
    yyjson_mut_val *av = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, av, "selection", available_json(d, "selection"));
    yyjson_mut_obj_add_val(d, av, "ruler", available_json(d, "ruler"));
    yyjson_mut_obj_add_val(d, av, "count", available_json(d, "count"));
    yyjson_mut_obj_add_val(d, root, "available", av);
    yyjson_mut_val *cols = yyjson_mut_arr(d);
    for (size_t i = 0; i < NCOLUMNS; i++) {
        yyjson_mut_val *co = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, co, "key", COLUMNS[i].key);
        yyjson_mut_obj_add_strcpy(d, co, "name", COLUMNS[i].name);
        yyjson_mut_arr_add_val(cols, co);
    }
    yyjson_mut_obj_add_val(d, root, "columns", cols);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
oom:
    rc = pc_fail(err, PC_ERR_NOMEM, "data points");
fail:
    for (int i = 0; i < 3; i++) {
        str_list_fini(&next[i]);
    }
    return rc;
}

/* ---- ruler -------------------------------------------------------------------------------- */

static pc_status ruler_info(yyjson_mut_doc *d, yyjson_mut_val **out, const pc_ruler *r,
                            const pc_measurement_scale *sc)
{
    double dx = r->end[0] - r->start[0], dy = r->end[1] - r->start[1];
    double l1 = hypot(dx, dy);
    double f = pc_measurement_scale_factor(sc);
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_real(d, o, "x", r->start[0]);
    yyjson_mut_obj_add_real(d, o, "y", r->start[1]);
    yyjson_mut_obj_add_real(d, o, "w", dx);
    yyjson_mut_obj_add_real(d, o, "h", dy);
    if (!r->has_protractor) {
        double ang = atan2(-dy, dx) * 180.0 / M_PI;
        yyjson_mut_obj_add_real(d, o, "angle", round4(ang));
        yyjson_mut_obj_add_real(d, o, "l1", round4(l1));
        yyjson_mut_obj_add_val(d, o, "l2", yyjson_mut_null(d));
        yyjson_mut_obj_add_real(d, o, "length", round4(l1 * f));
        yyjson_mut_obj_add_strcpy(d, o, "units", pc_str_c(&sc->units));
        yyjson_mut_obj_add_bool(d, o, "protractor", false);
    } else {
        double ex = r->protractor[0] - r->start[0], ey = r->protractor[1] - r->start[1];
        double l2 = hypot(ex, ey);
        double a1 = atan2(-dy, dx), a2 = atan2(-ey, ex);
        double ang = fabs((a1 - a2) * 180.0 / M_PI);
        if (ang > 180.0) {
            ang = 360.0 - ang;
        }
        yyjson_mut_obj_add_real(d, o, "angle", round4(ang));
        yyjson_mut_obj_add_real(d, o, "l1", round4(l1));
        yyjson_mut_obj_add_real(d, o, "l2", round4(l2));
        yyjson_mut_obj_add_real(d, o, "length", round4(l1 * f));
        yyjson_mut_obj_add_strcpy(d, o, "units", pc_str_c(&sc->units));
        yyjson_mut_obj_add_bool(d, o, "protractor", true);
    }
    *out = o;
    return PC_OK;
}

static yyjson_mut_val *ruler_json(yyjson_mut_doc *d, const pc_ruler *r)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_val *sa = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, sa, r->start[0]);
    yyjson_mut_arr_add_real(d, sa, r->start[1]);
    yyjson_mut_obj_add_val(d, o, "start", sa);
    yyjson_mut_val *ea = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, ea, r->end[0]);
    yyjson_mut_arr_add_real(d, ea, r->end[1]);
    yyjson_mut_obj_add_val(d, o, "end", ea);
    if (r->has_protractor) {
        yyjson_mut_val *pa = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, pa, r->protractor[0]);
        yyjson_mut_arr_add_real(d, pa, r->protractor[1]);
        yyjson_mut_obj_add_val(d, o, "protractor", pa);
    } else {
        yyjson_mut_obj_add_val(d, o, "protractor", yyjson_mut_null(d));
    }
    return o;
}

static pc_status run_ruler_tool(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "image.analysis.rulerTool";
    const pc_engine_doc_state *cst = pc_engine_session_active(s);
    if (cst == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (get_bool(p, "clear", false)) {
        pc_document *doc = NULL;
        pc_engine_doc_state *st = NULL;
        if (set_quiet_begin(s, &doc, &st, err) != PC_OK || doc == NULL || st == NULL) {
            return err != NULL && err->code != PC_OK ? err->code : PC_ERR_NOMEM;
        }
        doc->measurement.has_ruler = false;
        set_quiet_end(st);
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_val(d, root, "ruler", yyjson_mut_null(d));
        yyjson_mut_doc_set_root(d, root);
        return PC_OK;
    }
    const pc_measurement *m = &pc_doc_arc_get(cst->doc)->measurement;
    bool has = m->has_ruler;
    pc_ruler r = m->ruler;
    double start[2], end[2], prot[2];
    bool has_start = point_of(obj_get(p, "start"), start);
    bool has_end = point_of(obj_get(p, "end"), end);
    if (has_start && has_end) {
        r.start[0] = start[0];
        r.start[1] = start[1];
        r.end[0] = end[0];
        r.end[1] = end[1];
        yyjson_val *pr = obj_get(p, "protractor");
        if (pr == NULL || yyjson_is_null(pr)) {
            r.has_protractor = false;
        } else if (point_of(pr, prot)) {
            r.has_protractor = true;
            r.protractor[0] = prot[0];
            r.protractor[1] = prot[1];
        } else {
            return bad(err, cmd, "`protractor` must be [x, y] or null");
        }
        has = true;
    } else if (!has_start && !has_end) {
        yyjson_val *pr = obj_get(p, "protractor");
        if (has && pr != NULL) {
            if (yyjson_is_null(pr)) {
                r.has_protractor = false;
            } else if (point_of(pr, prot)) {
                r.has_protractor = true;
                r.protractor[0] = prot[0];
                r.protractor[1] = prot[1];
            } else {
                return bad(err, cmd, "`protractor` must be [x, y] or null");
            }
        }
    } else {
        return bad(err, cmd, "pass both `start` and `end`");
    }
    if (has) {
        const double *pts[3] = {r.start, r.end, r.has_protractor ? r.protractor : NULL};
        for (int i = 0; i < 3; i++) {
            if (pts[i] != NULL && (!isfinite(pts[i][0]) || !isfinite(pts[i][1]))) {
                return bad(err, cmd, "coordinates must be finite");
            }
        }
    }
    bool changed = (has != m->has_ruler) || (has && !pc_ruler_eq(&r, &m->ruler));
    if (changed) {
        pc_document *doc = NULL;
        pc_engine_doc_state *st = NULL;
        if (set_quiet_begin(s, &doc, &st, err) != PC_OK || doc == NULL || st == NULL) {
            return err != NULL && err->code != PC_OK ? err->code : PC_ERR_NOMEM;
        }
        doc->measurement.has_ruler = has;
        if (has) {
            doc->measurement.ruler = r;
        }
        set_quiet_end(st);
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    if (has) {
        yyjson_mut_obj_add_val(d, root, "ruler", ruler_json(d, &r));
        yyjson_mut_val *info = NULL;
        if (ruler_info(d, &info, &r, &pc_doc_arc_get(cst->doc)->measurement.scale) == PC_OK) {
            yyjson_mut_obj_add_val(d, root, "info", info);
        }
    } else {
        yyjson_mut_obj_add_val(d, root, "ruler", yyjson_mut_null(d));
    }
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- count -------------------------------------------------------------------------------- */

static const float GROUP_COLORS[6][3] = {{1, 0, 0},   {0, 0.6f, 1}, {0, 0.8f, 0},
                                         {1, 0.6f, 0}, {0.8f, 0, 0.8f}, {1, 1, 0}};

static pc_status new_group(pc_document *doc, const char *name, bool has_color, pc_color color,
                           pc_count_group *out, pc_error *err)
{
    pc_count_group g;
    if (pc_count_group_default(&g, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    size_t n = doc->measurement.ncount_groups;
    char buf[32];
    const char *nm = name;
    if (nm == NULL) {
        snprintf(buf, sizeof buf, "Count Group %zu", n + 1);
        nm = buf;
    }
    if (pc_str_set_c(&g.name, nm, err) != PC_OK) {
        pc_count_group_fini(&g);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (has_color) {
        g.color = color;
    } else {
        const float *c = GROUP_COLORS[n % 6];
        g.color = pc_color_rgb(c[0], c[1], c[2]);
    }
    *out = g;
    return PC_OK;
}

static size_t group_index(const pc_document *doc, yyjson_val *p, bool *ok)
{
    const pc_measurement *m = &doc->measurement;
    size_t i = m->active_count_group;
    yyjson_val *gv = obj_get(p, "group");
    if (gv != NULL && yyjson_is_num(gv) && yyjson_get_num(gv) >= 0) {
        i = (size_t)yyjson_get_num(gv);
    }
    *ok = i < m->ncount_groups;
    return i;
}

static yyjson_mut_val *count_json(yyjson_mut_doc *d, const pc_document *doc)
{
    const pc_measurement *m = &doc->measurement;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "total", pc_measurement_count_total(m));
    yyjson_mut_obj_add_uint(d, o, "activeGroup", m->active_count_group);
    yyjson_mut_val *groups = yyjson_mut_arr(d);
    for (size_t i = 0; i < m->ncount_groups; i++) {
        const pc_count_group *g = &m->count_groups[i];
        yyjson_mut_val *go = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, go, "name", pc_str_c(&g->name));
        float rgb[3];
        pc_color_to_rgb(&g->color, rgb);
        yyjson_mut_val *ca = yyjson_mut_arr(d);
        for (int k = 0; k < 3; k++) {
            yyjson_mut_arr_add_real(d, ca, rgb[k]);
        }
        yyjson_mut_obj_add_val(d, go, "color", ca);
        yyjson_mut_obj_add_uint(d, go, "markerSize", g->marker_size);
        yyjson_mut_obj_add_uint(d, go, "labelSize", g->label_size);
        yyjson_mut_obj_add_bool(d, go, "visible", g->visible);
        yyjson_mut_obj_add_uint(d, go, "count", g->npoints);
        yyjson_mut_val *pts = yyjson_mut_arr(d);
        for (size_t k = 0; k < g->npoints; k++) {
            yyjson_mut_val *pa = yyjson_mut_arr(d);
            yyjson_mut_arr_add_real(d, pa, g->points[k][0]);
            yyjson_mut_arr_add_real(d, pa, g->points[k][1]);
            yyjson_mut_arr_add_val(pts, pa);
        }
        yyjson_mut_obj_add_val(d, go, "points", pts);
        yyjson_mut_arr_add_val(groups, go);
    }
    yyjson_mut_obj_add_val(d, o, "groups", groups);
    return o;
}

static pc_status count_result(pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(d, count_json(d, pc_doc_arc_get(st->doc)));
    return PC_OK;
}

static pc_status run_count_tool(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return count_result(s, out, err);
}

/* Points array grow helper for a count group. */
static bool group_push_point(pc_count_group *g, double x, double y)
{
    double(*pts)[2] = pc_malloc_array(g->npoints + 1, sizeof *pts);
    if (pts == NULL) {
        return false;
    }
    for (size_t i = 0; i < g->npoints; i++) {
        pts[i][0] = g->points[i][0];
        pts[i][1] = g->points[i][1];
    }
    pts[g->npoints][0] = x;
    pts[g->npoints][1] = y;
    free(g->points);
    g->points = pts;
    g->npoints++;
    return true;
}

struct count_add_ctx {
    double at[2];
    bool has_group;
    size_t group;
    size_t number;
};

static pc_status edit_count_add(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct count_add_ctx *c = vctx;
    pc_measurement *m = &doc->measurement;
    if (m->ncount_groups == 0) {
        pc_count_group g;
        if (new_group(doc, NULL, false, (pc_color){0}, &g, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_count_group *arr = pc_malloc_array(1, sizeof *arr);
        if (arr == NULL) {
            pc_count_group_fini(&g);
            return pc_fail(err, PC_ERR_NOMEM, "count group");
        }
        arr[0] = g;
        m->count_groups = arr;
        m->ncount_groups = 1;
        m->active_count_group = 0;
    }
    size_t i = c->has_group ? c->group : m->active_count_group;
    if (i >= m->ncount_groups) {
        i = m->ncount_groups - 1;
    }
    if (!group_push_point(&m->count_groups[i], c->at[0], c->at[1])) {
        return pc_fail(err, PC_ERR_NOMEM, "count point");
    }
    c->number = m->count_groups[i].npoints;
    return PC_OK;
}

static bool xy_of(yyjson_val *p, double out[2])
{
    yyjson_val *x = obj_get(p, "x"), *y = obj_get(p, "y");
    if (x != NULL && yyjson_is_num(x) && y != NULL && yyjson_is_num(y)) {
        out[0] = yyjson_get_num(x);
        out[1] = yyjson_get_num(y);
        return true;
    }
    return false;
}

static pc_status run_count_add(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct count_add_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    if (!xy_of(p, ctx.at) && !point_of(obj_get(p, "point"), ctx.at)) {
        return bad(err, "count.add", "pass `x` and `y`");
    }
    if (!isfinite(ctx.at[0]) || !isfinite(ctx.at[1])) {
        return bad(err, "count.add", "coordinates must be finite");
    }
    yyjson_val *gv = obj_get(p, "group");
    if (gv != NULL && yyjson_is_num(gv) && yyjson_get_num(gv) >= 0) {
        ctx.has_group = true;
        ctx.group = (size_t)yyjson_get_num(gv);
    }
    pc_status rc = pc_engine_session_edit(s, "Count", edit_count_add, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = count_json(d, pc_doc_arc_get(st->doc));
    yyjson_mut_obj_add_uint(d, root, "number", ctx.number);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* Locate a marker (by index+group, or nearest to x/y within radius). */
static bool locate(const pc_document *doc, yyjson_val *p, size_t *gi, size_t *pi)
{
    const pc_measurement *m = &doc->measurement;
    yyjson_val *iv = obj_get(p, "index");
    if (iv != NULL && yyjson_is_num(iv) && yyjson_get_num(iv) >= 0) {
        bool ok;
        size_t g = group_index(doc, p, &ok);
        if (!ok) {
            return false;
        }
        size_t idx = (size_t)yyjson_get_num(iv);
        if (idx < m->count_groups[g].npoints) {
            *gi = g;
            *pi = idx;
            return true;
        }
        return false;
    }
    double at[2];
    if (!xy_of(p, at)) {
        return false;
    }
    double radius = 6.0;
    yyjson_val *rv = obj_get(p, "radius");
    if (rv != NULL && yyjson_is_num(rv)) {
        radius = yyjson_get_num(rv);
    }
    bool has_g = false;
    size_t only_g = 0;
    yyjson_val *gv = obj_get(p, "group");
    if (gv != NULL && yyjson_is_num(gv) && yyjson_get_num(gv) >= 0) {
        has_g = true;
        only_g = (size_t)yyjson_get_num(gv);
    }
    bool found = false;
    double best = 0.0;
    for (size_t g = 0; g < m->ncount_groups; g++) {
        if ((has_g && g != only_g) || !m->count_groups[g].visible) {
            continue;
        }
        for (size_t k = 0; k < m->count_groups[g].npoints; k++) {
            double dd = hypot(m->count_groups[g].points[k][0] - at[0],
                              m->count_groups[g].points[k][1] - at[1]);
            if (dd <= radius && (!found || dd < best)) {
                best = dd;
                *gi = g;
                *pi = k;
                found = true;
            }
        }
    }
    return found;
}

struct count_edit_ctx {
    int op; /* 0 remove, 1 move, 2 clear, 3 newGroup, 4 deleteGroup, 5 setGroup */
    size_t gi, pi;
    double to[2];
    bool clear_all;
    /* setGroup / newGroup fields */
    const char *name;
    bool has_color;
    pc_color color;
    bool has_marker;
    uint32_t marker;
    bool has_label;
    uint32_t label;
    bool has_visible;
    bool visible;
    bool activate;
};

static pc_status edit_count(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct count_edit_ctx *c = vctx;
    pc_measurement *m = &doc->measurement;
    if (c->op == 0) { /* remove */
        pc_count_group *g = &m->count_groups[c->gi];
        for (size_t k = c->pi; k + 1 < g->npoints; k++) {
            g->points[k][0] = g->points[k + 1][0];
            g->points[k][1] = g->points[k + 1][1];
        }
        g->npoints--;
    } else if (c->op == 1) { /* move */
        m->count_groups[c->gi].points[c->pi][0] = c->to[0];
        m->count_groups[c->gi].points[c->pi][1] = c->to[1];
    } else if (c->op == 2) { /* clear */
        for (size_t i = 0; i < m->ncount_groups; i++) {
            if (c->clear_all || i == c->gi) {
                free(m->count_groups[i].points);
                m->count_groups[i].points = NULL;
                m->count_groups[i].npoints = 0;
            }
        }
    } else if (c->op == 3) { /* newGroup */
        pc_count_group g;
        if (new_group(doc, c->name, c->has_color, c->color, &g, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_count_group *arr = pc_malloc_array(m->ncount_groups + 1, sizeof *arr);
        if (arr == NULL) {
            pc_count_group_fini(&g);
            return pc_fail(err, PC_ERR_NOMEM, "count group");
        }
        for (size_t i = 0; i < m->ncount_groups; i++) {
            arr[i] = m->count_groups[i];
        }
        arr[m->ncount_groups] = g;
        free(m->count_groups);
        m->count_groups = arr;
        m->ncount_groups++;
        m->active_count_group = m->ncount_groups - 1;
    } else if (c->op == 4) { /* deleteGroup */
        pc_count_group_fini(&m->count_groups[c->gi]);
        for (size_t i = c->gi; i + 1 < m->ncount_groups; i++) {
            m->count_groups[i] = m->count_groups[i + 1];
        }
        m->ncount_groups--;
        if (m->ncount_groups == 0) {
            free(m->count_groups);
            m->count_groups = NULL;
            m->active_count_group = 0;
        } else if (m->active_count_group >= m->ncount_groups) {
            m->active_count_group = m->ncount_groups - 1;
        }
    } else if (c->op == 5) { /* setGroup */
        pc_count_group *g = &m->count_groups[c->gi];
        if (c->name != NULL && pc_str_set_c(&g->name, c->name, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (c->has_color) {
            g->color = c->color;
        }
        if (c->has_marker) {
            g->marker_size = c->marker;
        }
        if (c->has_label) {
            g->label_size = c->label;
        }
        if (c->has_visible) {
            g->visible = c->visible;
        }
        if (c->activate) {
            m->active_count_group = c->gi;
        }
    }
    return PC_OK;
}

static pc_status run_count_remove(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    struct count_edit_ctx c;
    memset(&c, 0, sizeof c);
    c.op = 0;
    if (!locate(pc_doc_arc_get(st->doc), p, &c.gi, &c.pi)) {
        return pc_engine_err_other(err, "no count marker there");
    }
    pc_status rc = pc_engine_session_edit(s, "Count", edit_count, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return count_result(s, out, err);
}

static pc_status run_count_move(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    struct count_edit_ctx c;
    memset(&c, 0, sizeof c);
    c.op = 1;
    if (!point_of(obj_get(p, "to"), c.to)) {
        return bad(err, "count.move", "pass `to`: [x, y]");
    }
    if (!locate(pc_doc_arc_get(st->doc), p, &c.gi, &c.pi)) {
        return pc_engine_err_other(err, "no count marker there");
    }
    pc_status rc = pc_engine_session_edit(s, "Count", edit_count, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return count_result(s, out, err);
}

static pc_status run_count_clear(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    struct count_edit_ctx c;
    memset(&c, 0, sizeof c);
    c.op = 2;
    yyjson_val *gv = obj_get(p, "group");
    if (gv != NULL && yyjson_is_str(gv) && strcmp(yyjson_get_str(gv), "all") == 0) {
        c.clear_all = true;
    } else {
        bool ok;
        c.gi = group_index(pc_doc_arc_get(st->doc), p, &ok);
        if (!ok) {
            return bad(err, "count.clear", "no such count group");
        }
    }
    pc_status rc = pc_engine_session_edit(s, "Clear Count", edit_count, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return count_result(s, out, err);
}

static bool color_of(yyjson_val *p, const char *key, pc_color *out)
{
    if (obj_get(p, key) == NULL) {
        return false;
    }
    float def[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    float rgba[4];
    pc_engine_param_color(p, key, def, rgba);
    *out = pc_color_rgb(rgba[0], rgba[1], rgba[2]);
    return true;
}

static pc_status run_count_new_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                     pc_error *err)
{
    struct count_edit_ctx c;
    memset(&c, 0, sizeof c);
    c.op = 3;
    yyjson_val *nv = obj_get(p, "name");
    if (nv != NULL && yyjson_is_str(nv)) {
        c.name = yyjson_get_str(nv);
    }
    c.has_color = color_of(p, "color", &c.color);
    pc_status rc = pc_engine_session_edit(s, "New Count Group", edit_count, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return count_result(s, out, err);
}

static pc_status run_count_delete_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                        pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    struct count_edit_ctx c;
    memset(&c, 0, sizeof c);
    c.op = 4;
    bool ok;
    c.gi = group_index(pc_doc_arc_get(st->doc), p, &ok);
    if (!ok) {
        return bad(err, "count.deleteGroup", "no such count group");
    }
    pc_status rc = pc_engine_session_edit(s, "Delete Count Group", edit_count, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return count_result(s, out, err);
}

/* A whole-number size param (markerSize, labelSize), clamped lo..hi; *present false when absent. */
static pc_status whole_size(pc_error *err, const char *cmd, yyjson_val *p, const char *key, uint32_t lo,
                            uint32_t hi, bool *present, uint32_t *out)
{
    *present = false;
    yyjson_val *v = obj_get(p, key);
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    double n;
    if (yyjson_is_num(v)) {
        double f = yyjson_get_num(v);
        if (!isfinite(f) || f < 0.0 || f != floor(f)) {
            return bad(err, cmd, "`%s` must be a whole number (%u..=%u)", key, lo, hi);
        }
        n = f;
    } else {
        return bad(err, cmd, "`%s` must be a whole number (%u..=%u)", key, lo, hi);
    }
    if (n < lo) {
        n = lo;
    }
    if (n > hi) {
        n = hi;
    }
    *present = true;
    *out = (uint32_t)n;
    return PC_OK;
}

static pc_status run_count_set_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                     pc_error *err)
{
    const char *cmd = "count.setGroup";
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    struct count_edit_ctx c;
    memset(&c, 0, sizeof c);
    c.op = 5;
    bool ok;
    c.gi = group_index(pc_doc_arc_get(st->doc), p, &ok);
    if (!ok) {
        return bad(err, cmd, "no such count group");
    }
    yyjson_val *nv = obj_get(p, "name");
    if (nv != NULL && yyjson_is_str(nv)) {
        c.name = yyjson_get_str(nv);
    }
    c.has_color = color_of(p, "color", &c.color);
    pc_status rc;
    if ((rc = whole_size(err, cmd, p, "markerSize", 1, 10, &c.has_marker, &c.marker)) != PC_OK) {
        return rc;
    }
    if ((rc = whole_size(err, cmd, p, "labelSize", 8, 72, &c.has_label, &c.label)) != PC_OK) {
        return rc;
    }
    yyjson_val *vv = obj_get(p, "visible");
    if (vv != NULL && yyjson_is_bool(vv)) {
        c.has_visible = true;
        c.visible = yyjson_get_bool(vv);
    }
    /* Activate when asked, or when only `group` was given. */
    bool only_group = p != NULL && yyjson_is_obj(p) && yyjson_obj_size(p) == 1 &&
                      yyjson_obj_get(p, "group") != NULL;
    c.activate = get_bool(p, "active", false) || only_group;
    rc = pc_engine_session_edit(s, "Count Group Options", edit_count, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return count_result(s, out, err);
}

/* ---- selection feature measurement (measure_features) ------------------------------------- */

#define MAX_MEASURE_PIXELS 400000000ULL

typedef struct {
    double area, perimeter;
    pc_rect bounds;
    double gray_min, gray_max, gray_mean, gray_median;
    uint64_t histogram[256];
} feature;

static uint32_t uf_root(uint32_t *parent, uint32_t i)
{
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

static double gray_range(pc_sample_type depth)
{
    if (depth == PC_SAMPLE_TYPE_U8) {
        return 255.0;
    }
    if (depth == PC_SAMPLE_TYPE_U16) {
        return 32768.0;
    }
    return 1.0;
}

static int cmp_u16(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return (x < y) ? -1 : (x > y ? 1 : 0);
}

static double median_u16(uint16_t *v, size_t n)
{
    if (n == 0) {
        return 0.0;
    }
    qsort(v, n, sizeof *v, cmp_u16);
    size_t mid = n / 2;
    double m = (double)v[mid];
    if (n % 2 == 0) {
        m = (m + (double)v[mid - 1]) / 2.0;
    }
    return m / 65535.0;
}

/* Split `mask` (w*h) into 8-connected features measured against `gray` (0..1); returns the feature
 * array (free()) and count, or NULL on OOM. */
static feature *measure_features(const bool *mask, const float *gray, size_t w, size_t h, int32_t ox,
                                 int32_t oy, double range, size_t *out_n)
{
    size_t wh = w * h;
    uint32_t *label = pc_malloc_array(wh, sizeof *label);
    uint32_t *parent = pc_malloc_array(wh, sizeof *parent); /* worst case one label per pixel */
    if (label == NULL || parent == NULL) {
        free(label);
        free(parent);
        return NULL;
    }
    for (size_t i = 0; i < wh; i++) {
        label[i] = UINT32_MAX;
    }
    size_t nparent = 0;
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            size_t i = y * w + x;
            if (!mask[i]) {
                continue;
            }
            uint32_t nbrs[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
            if (x > 0) {
                nbrs[0] = label[i - 1];
            }
            if (y > 0) {
                nbrs[1] = label[i - w];
                if (x > 0) {
                    nbrs[2] = label[i - w - 1];
                }
                if (x + 1 < w) {
                    nbrs[3] = label[i - w + 1];
                }
            }
            uint32_t best = UINT32_MAX;
            for (int k = 0; k < 4; k++) {
                if (nbrs[k] != UINT32_MAX) {
                    uint32_t r = uf_root(parent, nbrs[k]);
                    if (r < best) {
                        best = r;
                    }
                }
            }
            if (best == UINT32_MAX) {
                best = (uint32_t)nparent;
                parent[nparent] = best;
                nparent++;
            } else {
                for (int k = 0; k < 4; k++) {
                    if (nbrs[k] != UINT32_MAX) {
                        uint32_t r = uf_root(parent, nbrs[k]);
                        if (r != best) {
                            uint32_t lo = r < best ? r : best, hi = r < best ? best : r;
                            parent[hi] = lo;
                            best = lo;
                        }
                    }
                }
            }
            label[i] = best;
        }
    }
    uint32_t *index = pc_malloc_array(nparent > 0 ? nparent : 1, sizeof *index);
    if (index == NULL) {
        free(label);
        free(parent);
        return NULL;
    }
    for (size_t i = 0; i < nparent; i++) {
        index[i] = UINT32_MAX;
    }
    uint32_t n = 0;
    for (size_t i = 0; i < wh; i++) {
        if (label[i] == UINT32_MAX) {
            continue;
        }
        uint32_t r = uf_root(parent, label[i]);
        if (index[r] == UINT32_MAX) {
            index[r] = n++;
        }
        label[i] = index[r];
    }
    free(parent);
    free(index);
    size_t nf = n;
    feature *feats = pc_calloc_array(nf > 0 ? nf : 1, sizeof *feats);
    double *sums = pc_calloc_array(nf > 0 ? nf : 1, sizeof *sums);
    uint16_t **vals = pc_calloc_array(nf > 0 ? nf : 1, sizeof *vals);
    size_t *vcap = pc_calloc_array(nf > 0 ? nf : 1, sizeof *vcap);
    size_t *vlen = pc_calloc_array(nf > 0 ? nf : 1, sizeof *vlen);
    if (feats == NULL || sums == NULL || vals == NULL || vcap == NULL || vlen == NULL) {
        free(label);
        free(feats);
        free(sums);
        free(vals);
        free(vcap);
        free(vlen);
        return NULL;
    }
    for (size_t i = 0; i < nf; i++) {
        feats[i].area = 0.0;
        feats[i].perimeter = 0.0;
        feats[i].bounds = PC_RECT_EMPTY;
        feats[i].gray_min = 1e300;
        feats[i].gray_max = -1e300;
        feats[i].gray_mean = 0.0;
        feats[i].gray_median = 0.0;
        memset(feats[i].histogram, 0, sizeof feats[i].histogram);
    }
    /* perimeter: marching squares over the padded pixel-centre grid. */
    const double HALF_DIAG = M_SQRT1_2;
    for (int64_t y = -1; y < (int64_t)h; y++) {
        for (int64_t x = -1; x < (int64_t)w; x++) {
            uint32_t c[4];
            int64_t xs[4] = {x, x + 1, x + 1, x};
            int64_t ys[4] = {y, y, y + 1, y + 1};
            int inside = 0;
            for (int k = 0; k < 4; k++) {
                c[k] = (xs[k] < 0 || ys[k] < 0 || xs[k] >= (int64_t)w || ys[k] >= (int64_t)h)
                           ? UINT32_MAX
                           : label[ys[k] * (int64_t)w + xs[k]];
                if (c[k] != UINT32_MAX) {
                    inside++;
                }
            }
            double len = 0.0;
            if (inside == 1 || inside == 3) {
                len = HALF_DIAG;
            } else if (inside == 2) {
                len = ((c[0] != UINT32_MAX) == (c[2] != UINT32_MAX)) ? 2.0 * HALF_DIAG : 1.0;
            }
            if (len > 0.0) {
                for (int k = 0; k < 4; k++) {
                    if (c[k] != UINT32_MAX) {
                        feats[c[k]].perimeter += len;
                        break;
                    }
                }
            }
        }
    }
    /* area, bounds, gray. */
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            uint32_t l = label[y * w + x];
            if (l == UINT32_MAX) {
                continue;
            }
            feature *f = &feats[l];
            f->area += 1.0;
            int32_t px = (int32_t)x + ox, py = (int32_t)y + oy;
            pc_rect r = pc_rect_make(px, py, px + 1, py + 1);
            f->bounds = pc_rect_is_empty(f->bounds) ? r : pc_rect_union(f->bounds, r);
            double g = (double)gray[y * w + x];
            if (g < f->gray_min) {
                f->gray_min = g;
            }
            if (g > f->gray_max) {
                f->gray_max = g;
            }
            sums[l] += g;
            double gc = g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);
            f->histogram[(size_t)(gc * 255.0 + 0.5)]++;
            if (vlen[l] == vcap[l]) {
                size_t nc = vcap[l] == 0 ? 16 : vcap[l] * 2;
                uint16_t *nv = pc_malloc_array(nc, sizeof *nv);
                if (nv == NULL) {
                    continue;
                }
                if (vlen[l] > 0) {
                    memcpy(nv, vals[l], vlen[l] * sizeof *nv);
                }
                free(vals[l]);
                vals[l] = nv;
                vcap[l] = nc;
            }
            vals[l][vlen[l]++] = (uint16_t)(gc * 65535.0 + 0.5);
        }
    }
    for (size_t i = 0; i < nf; i++) {
        feats[i].gray_mean = sums[i] / (feats[i].area > 1.0 ? feats[i].area : 1.0) * range;
        feats[i].gray_min *= range;
        feats[i].gray_max *= range;
        feats[i].gray_median = median_u16(vals[i], vlen[i]) * range;
        free(vals[i]);
    }
    free(label);
    free(sums);
    free(vals);
    free(vcap);
    free(vlen);
    *out_n = nf;
    return feats;
}

static double circularity(const feature *f)
{
    if (f->perimeter > 0.0) {
        double c = 4.0 * M_PI * f->area / (f->perimeter * f->perimeter);
        return c < 1.0 ? c : 1.0;
    }
    return 0.0;
}

/* ---- record + log ------------------------------------------------------------------------- */

static void base_row(yyjson_mut_doc *d, yyjson_mut_val *m, const pc_document *doc, const char *source,
                     const char *label, const char *now)
{
    const pc_measurement_scale *sc = &doc->measurement.scale;
    pc_str desc;
    desc.ptr = NULL;
    desc.len = 0;
    (void)pc_measurement_scale_describe(sc, &desc, NULL);
    yyjson_mut_obj_add_strcpy(d, m, "label", label);
    yyjson_mut_obj_add_strcpy(d, m, "dateTime", now);
    yyjson_mut_obj_add_strcpy(d, m, "document", pc_str_c(&doc->name));
    const char *src = strcmp(source, "selection") == 0 ? "Selection"
                      : strcmp(source, "ruler") == 0    ? "Ruler Tool"
                                                        : "Count Tool";
    yyjson_mut_obj_add_strcpy(d, m, "source", src);
    yyjson_mut_obj_add_strcpy(d, m, "scale", pc_str_c(&desc));
    yyjson_mut_obj_add_strcpy(d, m, "scaleUnits", pc_str_c(&sc->units));
    yyjson_mut_obj_add_real(d, m, "scaleFactor", pc_measurement_scale_factor(sc));
    pc_str_fini(&desc);
}

static void feature_values(yyjson_mut_doc *d, yyjson_mut_val *m, const feature *f, size_t count,
                           double factor)
{
    yyjson_mut_obj_add_uint(d, m, "count", count);
    yyjson_mut_obj_add_real(d, m, "area", round4(f->area * factor * factor));
    yyjson_mut_obj_add_real(d, m, "perimeter", round4(f->perimeter * factor));
    yyjson_mut_obj_add_real(d, m, "circularity", round4(circularity(f)));
    yyjson_mut_obj_add_real(d, m, "height", round4((double)pc_rect_height(f->bounds) * factor));
    yyjson_mut_obj_add_real(d, m, "width", round4((double)pc_rect_width(f->bounds) * factor));
    yyjson_mut_obj_add_real(d, m, "grayMin", round4(f->gray_min));
    yyjson_mut_obj_add_real(d, m, "grayMax", round4(f->gray_max));
    yyjson_mut_obj_add_real(d, m, "grayMean", round4(f->gray_mean));
    yyjson_mut_obj_add_real(d, m, "grayMedian", round4(f->gray_median));
    yyjson_mut_obj_add_real(d, m, "integratedDensity", round4(f->area * f->gray_mean));
    yyjson_mut_val *hist = yyjson_mut_arr(d);
    for (int i = 0; i < 256; i++) {
        yyjson_mut_arr_add_uint(d, hist, f->histogram[i]);
    }
    yyjson_mut_obj_add_val(d, m, "histogram", hist);
}

/* Current time as ISO 8601 UTC. */
static void now_iso(char out[32])
{
    time_t secs = time(NULL);
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

/* Build the selection summary + per-feature rows; returns the owned feature array via out params. */
static pc_status selection_rows(const pc_document *doc, feature **feats_out, size_t *nfeats,
                                feature *summary_out, double *med_out, pc_error *err)
{
    pc_rect bounds = pc_document_bounds(doc);
    pc_rect r;
    if (doc->has_selection) {
        r = pc_rect_intersect(pc_surface_content_bounds(&doc->selection), bounds);
    } else {
        r = bounds;
    }
    if (pc_rect_is_empty(r)) {
        return pc_engine_err_other(err, "the selection is empty");
    }
    uint64_t npx = (uint64_t)pc_rect_width(r) * (uint64_t)pc_rect_height(r);
    if (npx > MAX_MEASURE_PIXELS) {
        return pc_engine_err_other(err, "the area is too large to measure: select a smaller region");
    }
    size_t w = pc_rect_width(r), h = pc_rect_height(r), n = (size_t)npx;
    bool *mask = pc_malloc_array(n > 0 ? n : 1, sizeof *mask);
    if (mask == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "measure");
    }
    if (doc->has_selection) {
        size_t c = pc_surface_channels(&doc->selection);
        if (c == 0) {
            c = 1;
        }
        float *raw = NULL;
        size_t len = 0;
        if (pc_surface_read_region(&doc->selection, r, &raw, &len, err) != PC_OK) {
            free(mask);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        for (size_t i = 0; i < n; i++) {
            mask[i] = (i * c < len) && raw[i * c] >= 0.5f;
        }
        free(raw);
    } else {
        for (size_t i = 0; i < n; i++) {
            mask[i] = true;
        }
    }
    pc_compose_buffer buf;
    memset(&buf, 0, sizeof buf);
    if (pc_compose_render(doc, r, &buf, err) != PC_OK) {
        free(mask);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    float *gray = pc_malloc_array(n > 0 ? n : 1, sizeof *gray);
    if (gray == NULL) {
        free(mask);
        pc_compose_buffer_fini(&buf);
        return pc_fail(err, PC_ERR_NOMEM, "measure");
    }
    for (size_t i = 0; i < n && i < buf.len; i++) {
        gray[i] = 0.30f * buf.px[i][0] + 0.59f * buf.px[i][1] + 0.11f * buf.px[i][2];
    }
    pc_compose_buffer_fini(&buf);
    double range = gray_range(doc->depth);
    size_t nf = 0;
    feature *feats = measure_features(mask, gray, w, h, r.x0, r.y0, range, &nf);
    if (feats == NULL) {
        free(mask);
        free(gray);
        return pc_fail(err, PC_ERR_NOMEM, "measure");
    }
    if (nf == 0) {
        free(mask);
        free(gray);
        free(feats);
        return pc_engine_err_other(err, "the selection is empty");
    }
    /* pooled median over all selected pixels. */
    uint16_t *all = pc_malloc_array(n > 0 ? n : 1, sizeof *all);
    size_t na = 0;
    if (all != NULL) {
        for (size_t i = 0; i < n; i++) {
            if (mask[i]) {
                float g = gray[i] < 0.0f ? 0.0f : (gray[i] > 1.0f ? 1.0f : gray[i]);
                all[na++] = (uint16_t)(g * 65535.0f + 0.5f);
            }
        }
    }
    double med = median_u16(all, na) * range;
    free(all);
    free(mask);
    free(gray);
    /* summary. */
    feature sum;
    memset(&sum, 0, sizeof sum);
    sum.bounds = PC_RECT_EMPTY;
    sum.gray_min = 1e300;
    sum.gray_max = -1e300;
    double area_total = 0.0, mean_acc = 0.0;
    for (size_t i = 0; i < nf; i++) {
        sum.area += feats[i].area;
        sum.perimeter += feats[i].perimeter;
        sum.bounds = pc_rect_is_empty(sum.bounds) ? feats[i].bounds : pc_rect_union(sum.bounds, feats[i].bounds);
        if (feats[i].gray_min < sum.gray_min) {
            sum.gray_min = feats[i].gray_min;
        }
        if (feats[i].gray_max > sum.gray_max) {
            sum.gray_max = feats[i].gray_max;
        }
        for (int k = 0; k < 256; k++) {
            sum.histogram[k] += feats[i].histogram[k];
        }
        area_total += feats[i].area;
        mean_acc += feats[i].gray_mean * feats[i].area;
    }
    sum.gray_mean = mean_acc / (area_total > 1.0 ? area_total : 1.0);
    sum.gray_median = med;
    *feats_out = feats;
    *nfeats = nf;
    *summary_out = sum;
    *med_out = med;
    return PC_OK;
}

static bool log_push(struct pc_engine_analysis *a, uint64_t id, yyjson_mut_doc *values)
{
    if (a->nlog == a->cap_log) {
        size_t nc = a->cap_log == 0 ? 8 : a->cap_log * 2;
        struct log_row *nr = pc_malloc_array(nc, sizeof *nr);
        if (nr == NULL) {
            return false;
        }
        if (a->nlog > 0) {
            memcpy(nr, a->log, a->nlog * sizeof *nr);
        }
        free(a->log);
        a->log = nr;
        a->cap_log = nc;
    }
    a->log[a->nlog].id = id;
    a->log[a->nlog].values = values;
    a->nlog++;
    return true;
}

/* Keep only the data points selected for `source` (filter the row object in place by rebuilding). */
static yyjson_mut_doc *filtered_row(yyjson_mut_doc *src, const struct str_list *keep)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_val *simm = NULL;
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(src, NULL);
    if (imm == NULL) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    simm = yyjson_doc_get_root(imm);
    for (size_t i = 0; i < keep->n; i++) {
        yyjson_val *v = yyjson_obj_get(simm, keep->items[i]);
        if (v != NULL) {
            yyjson_mut_obj_add_val(d, root, keep->items[i], yyjson_val_mut_copy(d, v));
        }
    }
    yyjson_doc_free(imm);
    yyjson_mut_doc_set_root(d, root);
    return d;
}

static pc_status run_record(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "image.analysis.recordMeasurements";
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    struct pc_engine_analysis *a = analysis(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "analysis");
    }
    const char *source = "auto";
    yyjson_val *sv = obj_get(p, "source");
    if (sv != NULL && yyjson_is_str(sv)) {
        source = yyjson_get_str(sv);
    }
    if (strcmp(source, "auto") == 0) {
        if (doc->has_selection) {
            source = "selection";
        } else if (doc->measurement.has_ruler) {
            source = "ruler";
        } else if (pc_measurement_count_total(&doc->measurement) > 0) {
            source = "count";
        } else {
            source = "selection";
        }
    } else if (strcmp(source, "selection") != 0 && strcmp(source, "ruler") != 0 &&
               strcmp(source, "count") != 0) {
        return bad(err, cmd, "unknown source `%s` (auto|selection|ruler|count)", source);
    }
    char now[32];
    const char *nowp;
    yyjson_val *dtv = obj_get(p, "dateTime");
    if (dtv != NULL && yyjson_is_str(dtv)) {
        nowp = yyjson_get_str(dtv);
    } else {
        now_iso(now);
        nowp = now;
    }
    char label[48];
    snprintf(label, sizeof label, "Measurement %llu", (unsigned long long)(a->next_measurement + 1));
    double factor = pc_measurement_scale_factor(&doc->measurement.scale);

    /* Build the raw rows (full data) into temporary docs, then filter + commit. */
    yyjson_mut_doc *rows[64];
    char row_labels[64][48];
    size_t nrows = 0;
    pc_status rc = PC_OK;

    if (strcmp(source, "selection") == 0) {
        feature *feats = NULL;
        size_t nf = 0;
        feature sum;
        memset(&sum, 0, sizeof sum);
        double med = 0.0;
        rc = selection_rows(doc, &feats, &nf, &sum, &med, err);
        if (rc != PC_OK) {
            return rc;
        }
        yyjson_mut_doc *rd = yyjson_mut_doc_new(NULL);
        if (rd == NULL) {
            free(feats);
            return pc_fail(err, PC_ERR_NOMEM, "record");
        }
        yyjson_mut_val *m = yyjson_mut_obj(rd);
        base_row(rd, m, doc, source, label, nowp);
        feature_values(rd, m, &sum, nf, factor);
        yyjson_mut_doc_set_root(rd, m);
        rows[nrows] = rd;
        snprintf(row_labels[nrows], 48, "%s", label);
        nrows++;
        if (nf > 1) {
            for (size_t i = 0; i < nf && nrows < 64; i++) {
                yyjson_mut_doc *fd = yyjson_mut_doc_new(NULL);
                if (fd == NULL) {
                    break;
                }
                char flabel[48];
                snprintf(flabel, sizeof flabel, "Feature %zu", i + 1);
                yyjson_mut_val *fm = yyjson_mut_obj(fd);
                base_row(fd, fm, doc, source, flabel, nowp);
                feature_values(fd, fm, &feats[i], 1, factor);
                yyjson_mut_doc_set_root(fd, fm);
                rows[nrows] = fd;
                snprintf(row_labels[nrows], 48, "%s", flabel);
                nrows++;
            }
        }
        free(feats);
    } else if (strcmp(source, "ruler") == 0) {
        if (!doc->measurement.has_ruler) {
            return pc_engine_err_other(err, "no ruler line to measure");
        }
        yyjson_mut_doc *rd = yyjson_mut_doc_new(NULL);
        if (rd == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "record");
        }
        yyjson_mut_val *info = NULL;
        (void)ruler_info(rd, &info, &doc->measurement.ruler, &doc->measurement.scale);
        yyjson_mut_val *m = yyjson_mut_obj(rd);
        base_row(rd, m, doc, source, label, nowp);
        yyjson_mut_val *lv = yyjson_mut_obj_get(info, "length");
        yyjson_mut_val *av = yyjson_mut_obj_get(info, "angle");
        yyjson_mut_obj_add_real(rd, m, "length", lv != NULL ? yyjson_mut_get_real(lv) : 0.0);
        yyjson_mut_obj_add_real(rd, m, "angle", av != NULL ? yyjson_mut_get_real(av) : 0.0);
        yyjson_mut_doc_set_root(rd, m);
        rows[nrows] = rd;
        snprintf(row_labels[nrows], 48, "%s", label);
        nrows++;
    } else {
        yyjson_mut_doc *rd = yyjson_mut_doc_new(NULL);
        if (rd == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "record");
        }
        yyjson_mut_val *m = yyjson_mut_obj(rd);
        base_row(rd, m, doc, source, label, nowp);
        yyjson_mut_obj_add_uint(rd, m, "count", pc_measurement_count_total(&doc->measurement));
        yyjson_mut_doc_set_root(rd, m);
        rows[nrows] = rd;
        snprintf(row_labels[nrows], 48, "%s", label);
        nrows++;
    }

    a->next_measurement++;
    const struct str_list *keep = &a->dp[source_index(source)];
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        for (size_t i = 0; i < nrows; i++) {
            yyjson_mut_doc_free(rows[i]);
        }
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "source", source);
    yyjson_mut_val *outrows = yyjson_mut_arr(d);
    for (size_t i = 0; i < nrows; i++) {
        yyjson_mut_doc *frow = filtered_row(rows[i], keep);
        yyjson_mut_doc_free(rows[i]);
        if (frow == NULL) {
            continue;
        }
        a->next_row++;
        uint64_t id = a->next_row;
        /* result row {id, values}. */
        yyjson_doc *imm = yyjson_mut_doc_imut_copy(frow, NULL);
        if (imm != NULL) {
            yyjson_mut_val *ro = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, ro, "id", id);
            yyjson_mut_obj_add_val(d, ro, "values",
                                   yyjson_val_mut_copy(d, yyjson_doc_get_root(imm)));
            yyjson_mut_arr_add_val(outrows, ro);
            yyjson_doc_free(imm);
        }
        if (!log_push(a, id, frow)) {
            yyjson_mut_doc_free(frow);
        }
    }
    yyjson_mut_obj_add_val(d, root, "rows", outrows);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- log list / delete / export ----------------------------------------------------------- */

/* Columns any row has, in Photoshop's order (into out, static strings); returns count. */
static size_t log_columns(const struct pc_engine_analysis *a, const char **out)
{
    size_t n = 0;
    for (size_t i = 0; i < NCOLUMNS; i++) {
        bool any = false;
        for (size_t j = 0; j < a->nlog && !any; j++) {
            yyjson_mut_val *root = yyjson_mut_doc_get_root(a->log[j].values);
            if (root != NULL && yyjson_mut_obj_get(root, COLUMNS[i].key) != NULL) {
                any = true;
            }
        }
        if (any) {
            out[n++] = COLUMNS[i].key;
        }
    }
    return n;
}

static yyjson_mut_val *row_values_copy(yyjson_mut_doc *d, yyjson_mut_doc *src)
{
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(src, NULL);
    if (imm == NULL) {
        return yyjson_mut_obj(d);
    }
    yyjson_mut_val *v = yyjson_val_mut_copy(d, yyjson_doc_get_root(imm));
    yyjson_doc_free(imm);
    return v;
}

static pc_status run_log_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    struct pc_engine_analysis *a = analysis(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "analysis");
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *rows = yyjson_mut_arr(d);
    for (size_t i = 0; i < a->nlog; i++) {
        yyjson_mut_val *ro = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, ro, "id", a->log[i].id);
        yyjson_mut_obj_add_val(d, ro, "values", row_values_copy(d, a->log[i].values));
        yyjson_mut_arr_add_val(rows, ro);
    }
    yyjson_mut_obj_add_val(d, root, "rows", rows);
    const char *cols[NCOLUMNS];
    size_t nc = log_columns(a, cols);
    yyjson_mut_val *colsv = yyjson_mut_arr(d);
    for (size_t i = 0; i < nc; i++) {
        yyjson_mut_arr_add_strcpy(d, colsv, cols[i]);
    }
    yyjson_mut_obj_add_val(d, root, "columns", colsv);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* Collect the ids from a "rows":[...] param (or SIZE_MAX count = none). */
static bool ids_param(yyjson_val *p, uint64_t **out, size_t *n)
{
    yyjson_val *rv = obj_get(p, "rows");
    if (rv == NULL || !yyjson_is_arr(rv)) {
        return false;
    }
    size_t m = yyjson_arr_size(rv);
    uint64_t *ids = pc_malloc_array(m > 0 ? m : 1, sizeof *ids);
    if (ids == NULL) {
        return false;
    }
    size_t k = 0;
    for (size_t i = 0; i < m; i++) {
        yyjson_val *v = yyjson_arr_get(rv, i);
        if (yyjson_is_num(v) && yyjson_get_num(v) >= 0) {
            ids[k++] = (uint64_t)yyjson_get_num(v);
        }
    }
    *out = ids;
    *n = k;
    return true;
}

static bool id_in(const uint64_t *ids, size_t n, uint64_t id)
{
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

static pc_status run_log_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct pc_engine_analysis *a = analysis(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "analysis");
    }
    size_t before = a->nlog;
    uint64_t *ids = NULL;
    size_t nids = 0;
    if (ids_param(p, &ids, &nids)) {
        size_t w = 0;
        for (size_t i = 0; i < a->nlog; i++) {
            if (id_in(ids, nids, a->log[i].id)) {
                yyjson_mut_doc_free(a->log[i].values);
            } else {
                a->log[w++] = a->log[i];
            }
        }
        a->nlog = w;
        free(ids);
    } else if (get_bool(p, "all", false)) {
        for (size_t i = 0; i < a->nlog; i++) {
            yyjson_mut_doc_free(a->log[i].values);
        }
        a->nlog = 0;
    } else {
        return bad(err, "measurementLog.delete", "pass `rows`: [ids] or `all`: true");
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "deleted", before - a->nlog);
    yyjson_mut_obj_add_uint(d, root, "remaining", a->nlog);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* One CSV field from a value (JSON scalar/array). Appends to *buf (realloc). */
static void csv_append(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t sl = strlen(s);
    if (*len + sl + 1 > *cap) {
        size_t nc = (*cap == 0 ? 256 : *cap * 2);
        while (nc < *len + sl + 1) {
            nc *= 2;
        }
        char *nb = realloc(*buf, nc);
        if (nb == NULL) {
            return;
        }
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, sl);
    *len += sl;
    (*buf)[*len] = '\0';
}

static void csv_field(char **buf, size_t *len, size_t *cap, yyjson_val *v)
{
    char tmp[64];
    if (v == NULL || yyjson_is_null(v)) {
        return;
    }
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        bool quote = strpbrk(s, ",\"\n\r") != NULL;
        if (quote) {
            csv_append(buf, len, cap, "\"");
            for (const char *q = s; *q; q++) {
                char c[3];
                if (*q == '"') {
                    c[0] = '"';
                    c[1] = '"';
                    c[2] = '\0';
                } else {
                    c[0] = *q;
                    c[1] = '\0';
                }
                csv_append(buf, len, cap, c);
            }
            csv_append(buf, len, cap, "\"");
        } else {
            csv_append(buf, len, cap, s);
        }
    } else if (yyjson_is_arr(v)) {
        size_t m = yyjson_arr_size(v);
        for (size_t i = 0; i < m; i++) {
            if (i > 0) {
                csv_append(buf, len, cap, " ");
            }
            yyjson_val *e = yyjson_arr_get(v, i);
            if (yyjson_is_num(e)) {
                double x = yyjson_get_num(e);
                if (x == floor(x) && fabs(x) < 1e15) {
                    snprintf(tmp, sizeof tmp, "%lld", (long long)x);
                } else {
                    snprintf(tmp, sizeof tmp, "%g", x);
                }
                csv_append(buf, len, cap, tmp);
            }
        }
    } else if (yyjson_is_num(v)) {
        double x = yyjson_get_num(v);
        if (x == floor(x) && fabs(x) < 1e15) {
            snprintf(tmp, sizeof tmp, "%lld", (long long)x);
        } else {
            snprintf(tmp, sizeof tmp, "%g", x);
        }
        csv_append(buf, len, cap, tmp);
    } else if (yyjson_is_bool(v)) {
        csv_append(buf, len, cap, yyjson_get_bool(v) ? "true" : "false");
    }
}

static pc_status run_log_export(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct pc_engine_analysis *a = analysis(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "analysis");
    }
    uint64_t *ids = NULL;
    size_t nids = 0;
    bool filter = ids_param(p, &ids, &nids);
    const char *cols[NCOLUMNS];
    size_t nc = log_columns(a, cols);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    /* header (Photoshop column names). */
    for (size_t i = 0; i < nc; i++) {
        if (i > 0) {
            csv_append(&buf, &len, &cap, ",");
        }
        const char *name = cols[i];
        for (size_t j = 0; j < NCOLUMNS; j++) {
            if (strcmp(COLUMNS[j].key, cols[i]) == 0) {
                name = COLUMNS[j].name;
                break;
            }
        }
        csv_append(&buf, &len, &cap, name);
    }
    csv_append(&buf, &len, &cap, "\n");
    size_t nout = 0;
    for (size_t r = 0; r < a->nlog; r++) {
        if (filter && !id_in(ids, nids, a->log[r].id)) {
            continue;
        }
        nout++;
        yyjson_doc *imm = yyjson_mut_doc_imut_copy(a->log[r].values, NULL);
        yyjson_val *root = imm != NULL ? yyjson_doc_get_root(imm) : NULL;
        for (size_t i = 0; i < nc; i++) {
            if (i > 0) {
                csv_append(&buf, &len, &cap, ",");
            }
            csv_field(&buf, &len, &cap, root != NULL ? yyjson_obj_get(root, cols[i]) : NULL);
        }
        csv_append(&buf, &len, &cap, "\n");
        yyjson_doc_free(imm);
    }
    free(ids);
    const char *csv = buf != NULL ? buf : "";
    yyjson_val *path = obj_get(p, "path");
    pc_status rc;
    if (path != NULL && yyjson_is_str(path)) {
        /* Writing to a file needs the file writer (E3 file_cmds); deferred. */
        free(buf);
        return pc_engine_err_other(
            err, "exporting the Measurement Log to a file is not yet supported in this build (E3)");
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(buf);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "csv", csv);
    yyjson_mut_obj_add_uint(d, root, "rows", nout);
    yyjson_mut_doc_set_root(d, root);
    rc = PC_OK;
    free(buf);
    return rc;
}

/* ---- info --------------------------------------------------------------------------------- */

static pc_status run_info(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_measurement *m = &doc->measurement;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *sj = NULL;
    if (scale_json(d, &sj, &m->scale, err) == PC_OK) {
        yyjson_mut_obj_add_val(d, root, "scale", sj);
    }
    if (m->has_ruler) {
        yyjson_mut_val *ro = ruler_json(d, &m->ruler);
        yyjson_mut_val *info = NULL;
        if (ruler_info(d, &info, &m->ruler, &m->scale) == PC_OK) {
            yyjson_mut_obj_add_val(d, ro, "info", info);
        }
        yyjson_mut_obj_add_val(d, root, "ruler", ro);
    } else {
        yyjson_mut_obj_add_val(d, root, "ruler", yyjson_mut_null(d));
    }
    yyjson_mut_obj_add_val(d, root, "count", count_json(d, doc));
    yyjson_mut_obj_add_uint(d, root, "notes", 0);
    struct pc_engine_analysis *a = s->analysis;
    yyjson_mut_obj_add_uint(d, root, "logRows", a != NULL ? a->nlog : 0);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- deferred: Place Scale Marker, Straighten Layer --------------------------------------- */

static pc_status run_place_scale_marker(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                        pc_error *err)
{
    (void)s;
    (void)p;
    (void)out;
    return pc_engine_err_other(
        err, "Place Scale Marker is not yet supported in this build (needs type.create + a group layer)");
}

static pc_status run_straighten(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    (void)out;
    return pc_engine_err_other(
        err, "Straighten Layer is not yet supported in this build (needs arbitrary rotation / transform)");
}

/* ---- spec table --------------------------------------------------------------------------- */

static const char *const MENU_ANALYSIS[2] = {"Image", "Analysis"};

static const pc_engine_command SPECS[] = {
    {"image.analysis.setMeasurementScale", "Set Measurement Scale…", MENU_ANALYSIS, 2, NULL,
     "{\"preset\":\"default|custom\"=\"custom\",\"pixelLength\":px,\"logicalLength\":number,"
     "\"units\":str (e.g. \"mm\")} (no params: read the scale)",
     has_doc, run_set_scale, true},
    {"image.analysis.selectDataPoints", "Select Data Points…", MENU_ANALYSIS, 2, NULL,
     "{\"selection\":[keys]|{key:bool}?,\"ruler\":[keys]|{key:bool}?,\"count\":[keys]|{key:bool}?,"
     "\"reset\":bool=false} (keys: label,dateTime,document,source,scale,scaleUnits,scaleFactor,count,"
     "area,perimeter,circularity,height,width,grayMin,grayMax,grayMean,grayMedian,integratedDensity,"
     "histogram,length,angle)",
     pc_engine_always, run_select_data_points, true},
    {"image.analysis.recordMeasurements", "Record Measurements", MENU_ANALYSIS, 2, NULL,
     "{\"source\":\"auto|selection|ruler|count\"=\"auto\"} → appended Measurement Log rows "
     "(selection: summary + one row per feature)",
     has_doc, run_record, true},
    {"image.analysis.rulerTool", "Ruler Tool", MENU_ANALYSIS, 2, NULL,
     "{\"start\":[x,y],\"end\":[x,y],\"protractor\":[x,y]|null?,\"clear\":bool=false} (no params: "
     "read) → X/Y/W/H/angle/L1/L2",
     has_doc, run_ruler_tool, true},
    {"image.analysis.countTool", "Count Tool", MENU_ANALYSIS, 2, NULL,
     "{} → count groups and markers (edit with count.*)", has_doc, run_count_tool, false},
    {"image.analysis.placeScaleMarker", "Place Scale Marker…", MENU_ANALYSIS, 2, NULL,
     "{\"length\":logical units=nice ≈ width/5,\"font\":str?,\"fontSize\":pt=12,\"displayText\":bool="
     "true,\"textPosition\":\"top|bottom\"=\"bottom\",\"color\":\"black|white\"=\"black\"}",
     has_doc, run_place_scale_marker, true},
    {"image.analysis.straightenLayer", "Straighten Layer", NULL, 0, NULL,
     "{\"crop\":bool=(active layer is the Background)} (rotates so the ruler line is level; crop = "
     "rotate the canvas and crop to the image)",
     has_ruler, run_straighten, true},
    {"image.analysis.info", "Analysis Info", NULL, 0, NULL,
     "{} → scale, ruler readout, count groups, note and log counts", has_doc, run_info, false},
    {"count.add", "Add Count", NULL, 0, NULL, "{\"x\":px,\"y\":px,\"group\":index?}", has_doc,
     run_count_add, true},
    {"count.remove", "Remove Count", NULL, 0, NULL,
     "{\"index\":n,\"group\":index?} | {\"x\":px,\"y\":px,\"radius\":px=6}", has_doc, run_count_remove,
     true},
    {"count.move", "Move Count", NULL, 0, NULL,
     "{\"index\":n,\"group\":index?,\"to\":[x,y]} | {\"x\":px,\"y\":px,\"to\":[x,y]}", has_doc,
     run_count_move, true},
    {"count.clear", "Clear Count", NULL, 0, NULL, "{\"group\":index|\"all\"=active}", has_doc,
     run_count_clear, true},
    {"count.newGroup", "New Count Group", NULL, 0, NULL, "{\"name\":str?,\"color\":\"#rrggbb\"|[r,g,b]?}",
     has_doc, run_count_new_group, true},
    {"count.deleteGroup", "Delete Count Group", NULL, 0, NULL, "{\"group\":index=active}", has_doc,
     run_count_delete_group, true},
    {"count.setGroup", "Count Group Options", NULL, 0, NULL,
     "{\"group\":index=active,\"name\":str?,\"color\":\"#rrggbb\"|[r,g,b]?,\"markerSize\":1..10?,"
     "\"labelSize\":8..72?,\"visible\":bool?,\"active\":bool?}",
     has_doc, run_count_set_group, true},
    {"measurementLog.list", "Measurement Log", NULL, 0, NULL, "{} → rows and columns", pc_engine_always,
     run_log_list, false},
    {"measurementLog.delete", "Delete Measurements", NULL, 0, NULL, "{\"rows\":[ids]}|{\"all\":true}",
     has_log, run_log_delete, true},
    {"measurementLog.export", "Export Measurements…", NULL, 0, NULL,
     "{\"path\":str? (CSV file; omitted → returns the CSV text),\"rows\":[ids]?}", pc_engine_always,
     run_log_export, true},
};

const pc_engine_command *pc_engine_analysis_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
