/* Magnetic Lasso (port of crates/engine/src/magnetic_cmds.rs, E7): a selection border that snaps
 * to edges between fastening points, traced by photocraft_algo::magnetic. Given the fastening
 * points, select.magneticLasso traces the whole border itself (for scripts and agents); with
 * trace=false it takes the finished outline verbatim (what the interactive tool commits). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/magnetic.h"
#include "algo/selection.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/surface.h"

#include "selection_cmds.h"

#define CMD "select.magneticLasso"
#define MAX_POINTS 100000u
#define MAX_LENGTH 200000.0

/* The pixels the tool follows: the active pixel layer, or the composite. */
struct edge_src {
    const pc_document *doc;
    bool has_layer;
    pc_layer_id layer;
};

static bool edge_fetch(void *vctx, pc_rect r, uint8_t (*out)[4], size_t n)
{
    struct edge_src *c = vctx;
    const pc_layer *l = c->has_layer ? pc_document_layer(c->doc, c->layer) : NULL;
    const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
    if (surf != NULL) {
        uint8_t (*img)[4] = NULL;
        size_t len = 0;
        if (pc_algo_selection_rgba8_image(surf, r, &img, &len, NULL) != PC_OK) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            if (i < len) {
                out[i][0] = img[i][0];
                out[i][1] = img[i][1];
                out[i][2] = img[i][2];
                out[i][3] = img[i][3];
            } else {
                out[i][0] = out[i][1] = out[i][2] = out[i][3] = 0;
            }
        }
        free(img);
        return true;
    }
    pc_compose_buffer buf;
    if (pc_compose_render(c->doc, r, &buf, NULL) != PC_OK) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        for (int k = 0; k < 4; k++) {
            float v = (i < buf.len) ? buf.px[i][k] : 0.0f;
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            out[i][k] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
    pc_compose_buffer_fini(&buf);
    return true;
}

/* Parse `points` into finite [x,y] doubles. *out owned (free()). */
static pc_status parse_points(yyjson_val *p, double (**out)[2], size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    yyjson_val *arr = pc_json_get(p, "points");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return pc_engine_err_bad_params(err, CMD, "`points` must be [[x, y], …]");
    }
    size_t cap = yyjson_arr_size(arr);
    if (cap > MAX_POINTS) {
        return pc_engine_err_bad_params(err, CMD, "at most %u points", MAX_POINTS);
    }
    double (*pts)[2] = malloc(cap ? cap * sizeof(*pts) : sizeof(*pts));
    if (pts == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "points");
    }
    size_t i, max, k = 0;
    yyjson_val *v;
    yyjson_arr_foreach(arr, i, max, v) {
        double x, y;
        yyjson_val *vx = (v != NULL && yyjson_is_arr(v) && yyjson_arr_size(v) == 2) ? yyjson_arr_get(v, 0)
                                                                                    : NULL;
        yyjson_val *vy = vx != NULL ? yyjson_arr_get(v, 1) : NULL;
        if (vx == NULL || vy == NULL || !pc_json_as_f64(vx, &x) || !pc_json_as_f64(vy, &y) ||
            !isfinite(x) || !isfinite(y)) {
            free(pts);
            return pc_engine_err_bad_params(err, CMD, "bad point (want [x, y])");
        }
        pts[k][0] = x;
        pts[k][1] = y;
        k++;
    }
    *out = pts;
    *n = k;
    return PC_OK;
}

/* A number in [lo, hi], or def when absent. */
static pc_status number(yyjson_val *p, const char *key, double def, double lo, double hi, double *out,
                        pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        *out = def;
        return PC_OK;
    }
    double x;
    if (pc_json_as_f64(v, &x) && x >= lo && x <= hi) {
        *out = x;
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, CMD, "`%s` must be a number in %g..%g", key, lo, hi);
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The closed border through the fastening points. *out owned (free()); *nout its length. */
static pc_status trace_border(struct edge_src *src, pc_algo_magnetic_tracer *t,
                              const double (*pts)[2], size_t npts, pc_algo_magnetic_settings st,
                              bool straight, double (**out)[2], size_t *nout, pc_error *err)
{
    pc_rect b = pc_algo_magnetic_tracer_bounds(t);
    /* ring = clamped points, closed, for the length check. */
    double (*ring)[2] = malloc((npts + 1) * sizeof(*ring));
    if (ring == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "ring");
    }
    for (size_t i = 0; i < npts; i++) {
        ring[i][0] = clampd(pts[i][0], b.x0, b.x1);
        ring[i][1] = clampd(pts[i][1], b.y0, b.y1);
    }
    if (npts > 0) {
        ring[npts][0] = ring[0][0];
        ring[npts][1] = ring[0][1];
    }
    if (pc_algo_magnetic_length((const double (*)[2])ring, npts > 0 ? npts + 1 : 0) > MAX_LENGTH) {
        free(ring);
        return pc_engine_err_bad_params(err, CMD, "the border is longer than %g px", MAX_LENGTH);
    }
    /* anchors = snap each (filter out points that don't snap). */
    double (*anchors)[2] = malloc(npts ? npts * sizeof(*anchors) : sizeof(*anchors));
    if (anchors == NULL) {
        free(ring);
        return pc_fail(err, PC_ERR_NOMEM, "anchors");
    }
    size_t na = 0;
    for (size_t i = 0; i < npts; i++) {
        double snapped[2];
        bool some = false;
        if (pc_algo_magnetic_snap(t, edge_fetch, src, ring[i], st, snapped, &some, err) != PC_OK) {
            free(ring);
            free(anchors);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        if (some) {
            anchors[na][0] = snapped[0];
            anchors[na][1] = snapped[1];
            na++;
        }
    }
    free(ring);
    if (na == 0) {
        free(anchors);
        return pc_engine_err_other(err, "the document has no pixels");
    }
    /* out starts at the first anchor, extends with each segment (skipping the repeated endpoint). */
    double (*res)[2] = NULL;
    size_t nres = 0, cap = 0;
    bool oom = false;
#define PUSH(px, py)                                                                                 \
    do {                                                                                             \
        if (nres == cap) {                                                                           \
            size_t nc = cap ? cap * 2 : 64;                                                          \
            double(*g)[2] = realloc(res, nc * sizeof(*g));                                           \
            if (g == NULL) {                                                                         \
                oom = true;                                                                          \
                break;                                                                               \
            }                                                                                        \
            res = g;                                                                                 \
            cap = nc;                                                                                \
        }                                                                                            \
        res[nres][0] = (px);                                                                         \
        res[nres][1] = (py);                                                                         \
        nres++;                                                                                      \
    } while (0)

    PUSH(anchors[0][0], anchors[0][1]);
    for (size_t i = 0; i + 1 < na && !oom; i++) {
        double (*seg)[2] = NULL;
        size_t ns = 0;
        if (pc_algo_magnetic_trace(t, edge_fetch, src, anchors[i], anchors[i + 1], NULL, 0, st, &seg,
                                   &ns, err) != PC_OK) {
            free(res);
            free(anchors);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        for (size_t k = 1; k < ns && !oom; k++) {
            PUSH(seg[k][0], seg[k][1]);
        }
        free(seg);
    }
    if (!straight && na > 0 && !oom) {
        double (*seg)[2] = NULL;
        size_t ns = 0;
        if (pc_algo_magnetic_trace(t, edge_fetch, src, anchors[na - 1], anchors[0], NULL, 0, st, &seg,
                                   &ns, err) != PC_OK) {
            free(res);
            free(anchors);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        for (size_t k = 1; k < ns && !oom; k++) {
            PUSH(seg[k][0], seg[k][1]);
        }
        free(seg);
        if (nres > 0) {
            nres--; /* the ring ends where it started */
        }
    }
#undef PUSH
    free(anchors);
    if (oom) {
        free(res);
        return pc_fail(err, PC_ERR_NOMEM, "border");
    }
    *out = res;
    *nout = nres;
    return PC_OK;
}

static pc_status run_magnetic(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    double (*pts)[2] = NULL;
    size_t npts = 0;
    pc_status rc = parse_points(p, &pts, &npts, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (npts < 3) {
        free(pts);
        return pc_engine_err_bad_params(err, CMD, "needs at least 3 points");
    }
    bool trace = true;
    yyjson_val *tv = pc_json_get(p, "trace");
    if (tv != NULL && !pc_json_as_bool(tv, &trace)) {
        free(pts);
        return pc_engine_err_bad_params(err, CMD, "`trace` must be true or false");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        free(pts);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);

    double (*outline)[2] = pts;
    size_t noutline = npts;
    bool outline_owned = true;
    if (trace) {
        double width, contrast;
        rc = number(p, "width", 10.0, PC_ALGO_MAGNETIC_WIDTH_MIN, PC_ALGO_MAGNETIC_WIDTH_MAX, &width,
                    err);
        if (rc == PC_OK) {
            rc = number(p, "contrast", 10.0, 1.0, 100.0, &contrast, err);
        }
        bool straight = false;
        if (rc == PC_OK) {
            size_t clen = 0;
            yyjson_val *cv = pc_json_get(p, "close");
            const char *cs = cv != NULL ? pc_json_as_str(cv, &clen) : NULL;
            if (cv == NULL || (cs != NULL && clen == 8 && memcmp(cs, "magnetic", 8) == 0)) {
                straight = false;
            } else if (cs != NULL && clen == 8 && memcmp(cs, "straight", 8) == 0) {
                straight = true;
            } else {
                rc = pc_engine_err_bad_params(err, CMD, "`close` must be \"magnetic\" or \"straight\"");
            }
        }
        if (rc != PC_OK) {
            free(pts);
            return rc;
        }
        pc_algo_magnetic_settings st = pc_algo_magnetic_settings_new(width, (float)(contrast / 100.0));
        struct edge_src src = {doc, false, 0};
        if (d->has_active_layer) {
            const pc_layer *l = pc_document_layer(doc, d->active_layer);
            if (l != NULL && pc_layer_surface(l) != NULL) {
                src.has_layer = true;
                src.layer = d->active_layer;
            }
        }
        pc_algo_magnetic_tracer *t = NULL;
        rc = pc_algo_magnetic_tracer_new(pc_document_bounds(doc), &t, err);
        if (rc != PC_OK) {
            free(pts);
            return rc;
        }
        double (*traced)[2] = NULL;
        size_t nt = 0;
        rc = trace_border(&src, t, (const double (*)[2])pts, npts, st, straight, &traced, &nt, err);
        pc_algo_magnetic_tracer_free(t);
        free(pts);
        if (rc != PC_OK) {
            return rc;
        }
        outline = traced;
        noutline = nt;
        outline_owned = true;
    }
    /* Convert to f32 pairs and build the selection. */
    float (*poly)[2] = malloc(noutline ? noutline * sizeof(*poly) : sizeof(*poly));
    if (poly == NULL) {
        if (outline_owned) {
            free(outline);
        }
        return pc_fail(err, PC_ERR_NOMEM, "magnetic");
    }
    for (size_t i = 0; i < noutline; i++) {
        poly[i][0] = (float)outline[i][0];
        poly[i][1] = (float)outline[i][1];
    }
    if (outline_owned) {
        free(outline);
    }
    rc = pc_engine_polygon_selection(s, CMD, "Magnetic Lasso", (const float (*)[2])poly, noutline, p,
                                     out, err);
    free(poly);
    if (rc != PC_OK) {
        return rc;
    }
    /* Add the point count to the result. */
    yyjson_mut_doc *rd = out->doc;
    yyjson_mut_val *root = pc_json_value_root(out);
    if (rd != NULL && root != NULL && yyjson_mut_is_obj(root)) {
        yyjson_mut_obj_add_uint(rd, root, "points", noutline);
    }
    return PC_OK;
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK
                                               : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static const pc_engine_command SPECS[] = {
    {CMD, "Magnetic Lasso", NULL, 0, NULL,
     "{\"points\":[[x,y],…] (≥3 fastening points in order; with trace=false, the finished outline),"
     "\"width\":1..256=10,\"contrast\":1..100=10,\"close\":\"magnetic|straight\"=\"magnetic\","
     "\"trace\":bool=true,\"mode\":\"replace|add|subtract|intersect\"=\"replace\",\"antiAlias\":"
     "bool=true,\"feather\":0..1000=0}",
     has_doc, run_magnetic, true},
};

const pc_engine_command *pc_engine_magnetic_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
