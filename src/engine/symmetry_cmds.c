/* Per-document painting symmetry: straight presets or a sampled Bézier path (port of
 * crates/engine/src/symmetry_cmds.rs, E10). Path samples reflect across their nearest segment,
 * including curved paths. The symmetry lives on DocState.symmetry; it never touches pixels or the
 * document's paths. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "doc/doc.h"
#include "doc/vector.h"
#include "paint/symmetry.h"

#include "engine_internal.h"
#include "symmetry_cmds.h"

/* Keep nearest-segment searches bounded for long interactive brush strokes. */
#define MAX_SEGMENTS 1024u
#define SAMPLES_PER_CURVE 12u

/* One sampled axis segment [[x0,y0],[x1,y1]]. */
typedef struct seg {
    double a[2];
    double b[2];
} seg;

struct pc_engine_symmetry {
    bool is_path;
    pc_paint_symmetry preset; /* when !is_path */
    char *source;             /* when is_path (owned) */
    seg *segments;            /* when is_path (owned) */
    size_t nsegments;
};

void pc_engine_symmetry_free(struct pc_engine_symmetry *sym)
{
    if (sym == NULL) {
        return;
    }
    free(sym->source);
    free(sym->segments);
    free(sym);
}

struct pc_engine_symmetry *pc_engine_symmetry_clone(const struct pc_engine_symmetry *sym)
{
    if (sym == NULL) {
        return NULL;
    }
    struct pc_engine_symmetry *out = pc_calloc_array(1, sizeof *out);
    if (out == NULL) {
        return NULL;
    }
    out->is_path = sym->is_path;
    out->preset = sym->preset;
    if (sym->source != NULL) {
        size_t n = strlen(sym->source) + 1;
        out->source = pc_malloc_array(n, 1);
        if (out->source == NULL) {
            free(out);
            return NULL;
        }
        memcpy(out->source, sym->source, n);
    }
    if (sym->nsegments > 0) {
        out->segments = pc_malloc_array(sym->nsegments, sizeof *out->segments);
        if (out->segments == NULL) {
            free(out->source);
            free(out);
            return NULL;
        }
        memcpy(out->segments, sym->segments, sym->nsegments * sizeof *out->segments);
        out->nsegments = sym->nsegments;
    }
    return out;
}

/* ---- path axis ---------------------------------------------------------------------------- */

static bool finite_in_range(double v)
{
    return isfinite(v) && fabs(v) <= 1000000.0;
}

/* Build the sampled segments of a path (SymmetryAxis::from_path). PC_ERR_INVALID with the
 * reference's messages on an over-complex, out-of-range or empty path. */
static pc_status axis_from_path(const char *source, const pc_path *path, struct pc_engine_symmetry **out,
                                pc_error *err)
{
    seg *segs = NULL;
    size_t n = 0, cap = 0;
    for (size_t si = 0; si < path->nsubpaths; si++) {
        const pc_subpath *sp = &path->subpaths[si];
        size_t nseg = pc_subpath_segment_count(sp);
        for (size_t i = 0; i < nseg; i++) {
            pc_point c[4];
            pc_subpath_segment(sp, i, c);
            double ax = c[0].x, ay = c[0].y, bx = c[1].x, by = c[1].y, cx = c[2].x, cy = c[2].y,
                   dx = c[3].x, dy = c[3].y;
            unsigned samples = (ax == bx && ay == by && cx == dx && cy == dy) ? 1u : SAMPLES_PER_CURVE;
            double prev[2] = {ax, ay};
            for (unsigned step = 1; step <= samples; step++) {
                if (n >= MAX_SEGMENTS) {
                    free(segs);
                    return pc_fail(err, PC_ERR_INVALID, "symmetry path is too complex");
                }
                double t = (double)step / (double)samples;
                double u = 1.0 - t;
                double next[2] = {
                    u * u * u * ax + 3.0 * u * u * t * bx + 3.0 * u * t * t * cx + t * t * t * dx,
                    u * u * u * ay + 3.0 * u * u * t * by + 3.0 * u * t * t * cy + t * t * t * dy};
                if (!(finite_in_range(prev[0]) && finite_in_range(prev[1]) && finite_in_range(next[0]) &&
                      finite_in_range(next[1]))) {
                    free(segs);
                    return pc_fail(err, PC_ERR_INVALID,
                                   "symmetry path coordinates must be finite and within canvas limits");
                }
                if (prev[0] != next[0] || prev[1] != next[1]) {
                    seg *grown = pc_grow_array(segs, &cap, n + 1, sizeof *segs);
                    if (grown == NULL) {
                        free(segs);
                        return pc_fail(err, PC_ERR_NOMEM, "symmetry");
                    }
                    segs = grown;
                    segs[n].a[0] = prev[0];
                    segs[n].a[1] = prev[1];
                    segs[n].b[0] = next[0];
                    segs[n].b[1] = next[1];
                    n++;
                }
                prev[0] = next[0];
                prev[1] = next[1];
            }
        }
    }
    if (n == 0) {
        free(segs);
        return pc_fail(err, PC_ERR_INVALID, "symmetry path needs at least one nonzero segment");
    }
    struct pc_engine_symmetry *sym = pc_calloc_array(1, sizeof *sym);
    if (sym == NULL) {
        free(segs);
        return pc_fail(err, PC_ERR_NOMEM, "symmetry");
    }
    size_t sn = strlen(source) + 1;
    sym->source = pc_malloc_array(sn, 1);
    if (sym->source == NULL) {
        free(segs);
        free(sym);
        return pc_fail(err, PC_ERR_NOMEM, "symmetry");
    }
    memcpy(sym->source, source, sn);
    sym->is_path = true;
    sym->segments = segs;
    sym->nsegments = n;
    *out = sym;
    return PC_OK;
}

/* Reflect one point across its nearest segment (SymmetryAxis::reflect). */
static pc_paint_point reflect_point(const struct pc_engine_symmetry *sym, pc_paint_point p)
{
    bool have = false;
    double best = 0.0, bq[2] = {0, 0}, btan[2] = {0, 0};
    for (size_t i = 0; i < sym->nsegments; i++) {
        double ax = sym->segments[i].a[0], ay = sym->segments[i].a[1];
        double vx = sym->segments[i].b[0] - ax, vy = sym->segments[i].b[1] - ay;
        double len2 = vx * vx + vy * vy;
        if (len2 <= DBL_EPSILON) {
            continue;
        }
        double t = ((p.x - ax) * vx + (p.y - ay) * vy) / len2;
        if (t < 0.0) {
            t = 0.0;
        } else if (t > 1.0) {
            t = 1.0;
        }
        double qx = ax + vx * t, qy = ay + vy * t;
        double dist2 = (p.x - qx) * (p.x - qx) + (p.y - qy) * (p.y - qy);
        if (!have || dist2 < best) {
            have = true;
            best = dist2;
            bq[0] = qx;
            bq[1] = qy;
            btan[0] = vx;
            btan[1] = vy;
        }
    }
    if (!have) {
        return p;
    }
    double len2 = btan[0] * btan[0] + btan[1] * btan[1];
    double dx = p.x - bq[0], dy = p.y - bq[1];
    double along = (dx * btan[0] + dy * btan[1]) / len2;
    pc_paint_point m = p;
    m.x = bq[0] + 2.0 * along * btan[0] - dx;
    m.y = bq[1] + 2.0 * along * btan[1] - dy;
    return m;
}

bool pc_engine_symmetry_has_distinct_mirror(const pc_paint_point *a, const pc_paint_point *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (fabs(a[i].x - b[i].x) > 1.0e-6 || fabs(a[i].y - b[i].y) > 1.0e-6) {
            return true;
        }
    }
    return false;
}

size_t pc_engine_symmetry_mirror_count(const struct pc_engine_symmetry *sym)
{
    if (sym == NULL) {
        return 0;
    }
    return sym->is_path ? 1 : pc_paint_symmetry_mirror_count(&sym->preset);
}

pc_status pc_engine_symmetry_reflected_passes(const struct pc_engine_symmetry *sym,
                                              const pc_paint_point *pts, size_t n,
                                              pc_paint_point_vec out[3], size_t *count, pc_error *err)
{
    if (sym->is_path) {
        *count = 1;
        for (size_t i = 0; i < n; i++) {
            pc_paint_point m = reflect_point(sym, pts[i]);
            if (pc_paint_point_vec_push(&out[0], &m, err) != PC_OK) {
                return PC_ERR_NOMEM;
            }
        }
        return PC_OK;
    }
    return pc_paint_symmetry_reflected_passes(&sym->preset, pts, n, out, count, err);
}

/* ---- JSON ---- */

static pc_status symmetry_inspect(const struct pc_engine_symmetry *sym, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    if (sym->is_path) {
        yyjson_mut_obj_add_str(d, root, "mode", "path");
        yyjson_mut_obj_add_strcpy(d, root, "source", sym->source != NULL ? sym->source : "");
        yyjson_mut_obj_add_uint(d, root, "segments", sym->nsegments);
    } else {
        yyjson_mut_obj_add_str(d, root, "mode", pc_paint_symmetry_mode_id(sym->preset.mode));
        yyjson_mut_val *c = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, c, sym->preset.center[0]);
        yyjson_mut_arr_add_real(d, c, sym->preset.center[1]);
        yyjson_mut_obj_add_val(d, root, "center", c);
        yyjson_mut_obj_add_real(d, root, "rotation", sym->preset.rotation);
    }
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- commands ----------------------------------------------------------------------------- */

static pc_status en_has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK
                                               : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status en_has_symmetry(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || d->symmetry == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active painting symmetry");
    }
    return PC_OK;
}

static const char *const SYM_FAIL =
    "symmetry requires a valid mode, a finite [x,y] centre and a finite rotation";

static pc_status run_set_preset(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    size_t len = 0;
    yyjson_val *mv = pc_json_get(p, "mode");
    const char *ms = mv != NULL ? pc_json_as_str(mv, &len) : NULL;
    pc_paint_symmetry_mode mode;
    if (ms == NULL || !pc_paint_symmetry_mode_from_id(ms, len, &mode)) {
        return pc_engine_err_other(err, "%s", SYM_FAIL);
    }
    double center[2];
    yyjson_val *cv = pc_json_get(p, "center");
    if (cv == NULL) {
        pc_size sz = doc->size;
        center[0] = (double)sz.width / 2.0;
        center[1] = (double)sz.height / 2.0;
    } else {
        if (!yyjson_is_arr(cv) || yyjson_arr_size(cv) != 2) {
            return pc_engine_err_other(err, "%s", SYM_FAIL);
        }
        double x, y;
        if (!pc_json_as_f64(yyjson_arr_get(cv, 0), &x) || !pc_json_as_f64(yyjson_arr_get(cv, 1), &y)) {
            return pc_engine_err_other(err, "%s", SYM_FAIL);
        }
        center[0] = x;
        center[1] = y;
    }
    double rotation = 0.0;
    yyjson_val *rv = pc_json_get(p, "rotation");
    if (rv != NULL && !pc_json_as_f64(rv, &rotation)) {
        return pc_engine_err_other(err, "%s", SYM_FAIL);
    }
    pc_paint_symmetry preset;
    if (!pc_paint_symmetry_new(mode, center, rotation, &preset)) {
        return pc_engine_err_other(err, "%s", SYM_FAIL);
    }
    struct pc_engine_symmetry *sym = pc_calloc_array(1, sizeof *sym);
    if (sym == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "symmetry");
    }
    sym->is_path = false;
    sym->preset = preset;
    if (symmetry_inspect(sym, out, err) != PC_OK) {
        pc_engine_symmetry_free(sym);
        return PC_ERR_NOMEM;
    }
    pc_engine_symmetry_free(st->symmetry);
    st->symmetry = sym;
    return PC_OK;
}

/* The path named by `name`: "work", "layer" (active shape/vector-mask path) or a saved path. *out
 * is borrowed from the document (valid until it changes). */
static pc_status resolve_named_path(const pc_engine_doc_state *st, const char *name, const pc_path **out,
                                    pc_error *err)
{
    const pc_document *doc = pc_doc_arc_get(st->doc);
    if (strcmp(name, "work") == 0) {
        if (!doc->has_work_path) {
            return pc_engine_err_other(err, "no work path");
        }
        *out = &doc->work_path;
        return PC_OK;
    }
    if (strcmp(name, "layer") == 0) {
        if (!st->has_active_layer) {
            return pc_engine_err_other(err, "no active layer");
        }
        const pc_layer *l = pc_document_layer(doc, st->active_layer);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, st->active_layer);
        }
        if (l->content.kind == PC_LAYER_SHAPE) {
            *out = &l->content.u.shape.path;
            return PC_OK;
        }
        if (l->has_vector_mask) {
            *out = &l->vector_mask.path;
            return PC_OK;
        }
        return pc_engine_err_other(err, "active layer has no vector path");
    }
    for (size_t i = 0; i < doc->npaths; i++) {
        if (strcmp(pc_str_c(&doc->paths[i].name), name) == 0) {
            *out = &doc->paths[i].path;
            return PC_OK;
        }
    }
    return pc_engine_err_other(err, "no path named `%s`", name);
}

static pc_status run_enable(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    size_t len = 0;
    yyjson_val *nv = pc_json_get(p, "name");
    const char *name = nv != NULL ? pc_json_as_str(nv, &len) : NULL;
    char buf[256];
    if (name == NULL) {
        name = "work";
    } else {
        if (len >= sizeof buf) {
            return pc_engine_err_other(err, "no path named `%.*s`", (int)len, name);
        }
        memcpy(buf, name, len);
        buf[len] = '\0';
        name = buf;
    }
    const pc_path *path = NULL;
    pc_status rc = resolve_named_path(st, name, &path, err);
    if (rc != PC_OK || path == NULL) {
        return rc != PC_OK ? rc : pc_engine_err_other(err, "no path named `%s`", name);
    }
    struct pc_engine_symmetry *sym = NULL;
    rc = axis_from_path(name, path, &sym, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t count = sym->nsegments;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_engine_symmetry_free(sym);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "enabled", true);
    yyjson_mut_obj_add_strcpy(d, root, "source", name);
    yyjson_mut_obj_add_uint(d, root, "segments", count);
    yyjson_mut_doc_set_root(d, root);
    pc_engine_symmetry_free(st->symmetry);
    st->symmetry = sym;
    return PC_OK;
}

static pc_status run_disable(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_engine_symmetry_free(st->symmetry);
    st->symmetry = NULL;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "enabled", false);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"paint.setSymmetry", "Set Painting Symmetry", NULL, 0, NULL,
     "{\"mode\":\"vertical|horizontal|dual|diagonal\",\"center\":[x,y]?,\"rotation\":degrees=0}",
     en_has_doc, run_set_preset, true},
    {"paint.symmetryFromPath", "Make Symmetry Path", NULL, 0, NULL,
     "{\"name\":\"work\"|\"layer\"|savedPathName=\"work\"} — mirror Brush, Pencil and Eraser strokes "
     "across the path",
     en_has_doc, run_enable, true},
    {"paint.symmetryDisable", "Disable Painting Symmetry", NULL, 0, NULL, "{}", en_has_symmetry,
     run_disable, true},
};

const pc_engine_command *pc_engine_symmetry_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
