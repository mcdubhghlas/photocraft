/* Adaptive Wide Angle (port of crates/algo/src/wideangle.rs). See wideangle.h. */
#include "algo/wideangle.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/params.h"
#include "algo/photo_util.h"
#include "algo/warp.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define PI_D 3.14159265358979323846
#define TAU_D 6.28318530717958647692

static const char *const MODEL_NAMES[] = {"auto", "fisheye", "perspective", "fullSpherical"};
static const char *const ORIENT_NAMES[] = {"free", "horizontal", "vertical"};

bool pc_algo_wide_model_parse(const char *s, size_t len, pc_algo_wide_model *out)
{
    size_t i;
    for (i = 0; i < 4; i++) {
        if (strlen(MODEL_NAMES[i]) == len && memcmp(MODEL_NAMES[i], s, len) == 0) {
            *out = (pc_algo_wide_model)i;
            return true;
        }
    }
    if (len == 9 && memcmp(s, "spherical", 9) == 0) {
        *out = PC_ALGO_WIDE_FULL_SPHERICAL;
        return true;
    }
    return false;
}

void pc_algo_wide_angle_default(pc_algo_wide_angle *p)
{
    memset(p, 0, sizeof *p);
    p->model = PC_ALGO_WIDE_AUTO;
    p->crop_factor = 1.0;
    p->scale = 100.0;
}

void pc_algo_wide_angle_fini(pc_algo_wide_angle *p)
{
    if (p != NULL) {
        free(p->constraints);
        pc_algo_wide_angle_default(p);
    }
}

/* ---- JSON (serde_json::from_value semantics) ---- */

static pc_status read_f64(yyjson_val *v, double *out, pc_error *err)
{
    if (yyjson_is_sint(v) && yyjson_get_sint(v) == 0) {
        *out = -0.0; /* yyjson keeps "-0" as a signed zero integer; serde_json reads it as -0.0 */
    } else if (yyjson_is_uint(v)) {
        *out = (double)yyjson_get_uint(v);
    } else if (yyjson_is_sint(v)) {
        *out = (double)yyjson_get_sint(v);
    } else if (yyjson_is_real(v)) {
        *out = yyjson_get_real(v);
    } else {
        return pc_algo_jfail_type(v, "f64", err);
    }
    return PC_OK;
}

static pc_status read_f64x2(yyjson_val *v, double out[2], pc_error *err)
{
    size_t i = 0, total;
    yyjson_arr_iter it;
    yyjson_val *e;
    double t[2] = {0.0, 0.0};
    pc_status st;
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "an array of length 2", err);
    }
    total = yyjson_arr_size(v);
    yyjson_arr_iter_init(v, &it);
    while (i < 2 && (e = yyjson_arr_iter_next(&it)) != NULL) {
        if ((st = read_f64(e, &t[i], err)) != PC_OK) {
            return st;
        }
        i++;
    }
    if (i < 2) {
        return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected an array of length 2", i);
    }
    if (total > 2) {
        return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected 2 elements in sequence", total);
    }
    out[0] = t[0];
    out[1] = t[1];
    return PC_OK;
}

static pc_status read_enum(yyjson_val *v, const char *const *names, size_t n, int *out, pc_error *err)
{
    yyjson_val *name = v, *payload = NULL;
    const char *s;
    size_t i, len;
    if (yyjson_is_obj(v)) {
        yyjson_obj_iter it;
        if (yyjson_obj_size(v) != 1) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: map, expected map with a single key");
        }
        yyjson_obj_iter_init(v, &it);
        name = yyjson_obj_iter_next(&it);
        payload = yyjson_obj_iter_get_val(name);
    } else if (!yyjson_is_str(v)) {
        return pc_algo_jfail_type(v, "string or map", err);
    }
    s = yyjson_get_str(name);
    len = yyjson_get_len(name);
    for (i = 0; i < n; i++) {
        if (strlen(names[i]) == len && memcmp(names[i], s, len) == 0) {
            break;
        }
    }
    if (i == n) {
        return pc_algo_jfail_variant(s, len, names, n, err);
    }
    if (payload != NULL && !yyjson_is_null(payload)) {
        return pc_algo_jfail_type(payload, "unit", err);
    }
    *out = (int)i;
    return PC_OK;
}

/* A struct's object entries with serde_json::Value semantics: last duplicate wins. */
static yyjson_val *obj_last(yyjson_val *obj, const char *key)
{
    yyjson_obj_iter it;
    yyjson_val *k, *found = NULL;
    size_t klen = strlen(key);
    yyjson_obj_iter_init(obj, &it);
    while ((k = yyjson_obj_iter_next(&it)) != NULL) {
        if (yyjson_get_len(k) == klen && memcmp(yyjson_get_str(k), key, klen) == 0) {
            found = yyjson_obj_iter_get_val(k);
        }
    }
    return found;
}

static pc_status read_constraint(yyjson_val *v, pc_algo_wide_constraint *c, pc_error *err)
{
    yyjson_val *a, *b, *o;
    pc_status st;
    int ori = PC_ALGO_WIDE_FREE;
    if (!yyjson_is_obj(v)) {
        return pc_algo_jfail_type(v, "struct Constraint", err);
    }
    a = obj_last(v, "a");
    b = obj_last(v, "b");
    o = obj_last(v, "orientation");
    /* serde visits keys in byte order ("a" < "b" < "orientation") and reports missing fields after. */
    if (a != NULL && (st = read_f64x2(a, c->a, err)) != PC_OK) {
        return st;
    }
    if (b != NULL && (st = read_f64x2(b, c->b, err)) != PC_OK) {
        return st;
    }
    if (o != NULL && (st = read_enum(o, ORIENT_NAMES, 3, &ori, err)) != PC_OK) {
        return st;
    }
    if (a == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing field `a`");
    }
    if (b == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing field `b`");
    }
    c->orientation = (pc_algo_wide_orientation)ori;
    return PC_OK;
}

pc_status pc_algo_wide_angle_from_json(yyjson_val *v, pc_algo_wide_angle *out, pc_error *err)
{
    pc_algo_wide_angle p;
    yyjson_val *f;
    pc_status st = PC_OK;
    pc_algo_wide_angle_default(&p);
    if (!yyjson_is_obj(v)) {
        return pc_algo_jfail_type(v, "struct WideAngle", err);
    }
    /* Keys in byte order: constraints, cropFactor, focalLength, model, scale. */
    if ((f = obj_last(v, "constraints")) != NULL) {
        size_t n, i;
        yyjson_arr_iter it;
        yyjson_val *e;
        if (!yyjson_is_arr(f)) {
            return pc_algo_jfail_type(f, "a sequence", err);
        }
        n = yyjson_arr_size(f);
        if (n > 0) {
            p.constraints = pc_calloc_array(n, sizeof *p.constraints);
            if (p.constraints == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "out of memory reading constraints");
            }
        }
        yyjson_arr_iter_init(f, &it);
        for (i = 0; i < n && (e = yyjson_arr_iter_next(&it)) != NULL; i++) {
            if ((st = read_constraint(e, &p.constraints[i], err)) != PC_OK) {
                pc_algo_wide_angle_fini(&p);
                return st;
            }
        }
        p.n_constraints = n;
    }
    if (((f = obj_last(v, "cropFactor")) != NULL && (st = read_f64(f, &p.crop_factor, err)) != PC_OK) ||
        ((f = obj_last(v, "focalLength")) != NULL && (st = read_f64(f, &p.focal_length, err)) != PC_OK)) {
        pc_algo_wide_angle_fini(&p);
        return st;
    }
    if ((f = obj_last(v, "model")) != NULL) {
        int m = 0;
        if ((st = read_enum(f, MODEL_NAMES, 4, &m, err)) != PC_OK) {
            pc_algo_wide_angle_fini(&p);
            return st;
        }
        p.model = (pc_algo_wide_model)m;
    }
    if ((f = obj_last(v, "scale")) != NULL && (st = read_f64(f, &p.scale, err)) != PC_OK) {
        pc_algo_wide_angle_fini(&p);
        return st;
    }
    *out = p;
    return PC_OK;
}

static yyjson_mut_val *w_f64(yyjson_mut_doc *d, double v)
{
    return isfinite(v) ? yyjson_mut_real(d, v) : yyjson_mut_null(d);
}

static yyjson_mut_val *w_pt(yyjson_mut_doc *d, const double p[2])
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    if (a == NULL || !yyjson_mut_arr_append(a, w_f64(d, p[0])) || !yyjson_mut_arr_append(a, w_f64(d, p[1]))) {
        return NULL;
    }
    return a;
}

yyjson_mut_val *pc_algo_wide_angle_to_json(yyjson_mut_doc *d, const pc_algo_wide_angle *p)
{
    yyjson_mut_val *o = yyjson_mut_obj(d), *arr = yyjson_mut_arr(d);
    size_t i;
    if (o == NULL || arr == NULL) {
        return NULL;
    }
    for (i = 0; i < p->n_constraints; i++) {
        const pc_algo_wide_constraint *c = &p->constraints[i];
        yyjson_mut_val *e = yyjson_mut_obj(d);
        if (e == NULL || !yyjson_mut_obj_add_val(d, e, "a", w_pt(d, c->a)) ||
            !yyjson_mut_obj_add_val(d, e, "b", w_pt(d, c->b)) ||
            !yyjson_mut_obj_add_str(d, e, "orientation", ORIENT_NAMES[c->orientation % 3]) ||
            !yyjson_mut_arr_append(arr, e)) {
            return NULL;
        }
    }
    if (!yyjson_mut_obj_add_str(d, o, "model", MODEL_NAMES[p->model % 4]) ||
        !yyjson_mut_obj_add_val(d, o, "focalLength", w_f64(d, p->focal_length)) ||
        !yyjson_mut_obj_add_val(d, o, "cropFactor", w_f64(d, p->crop_factor)) ||
        !yyjson_mut_obj_add_val(d, o, "scale", w_f64(d, p->scale)) ||
        !yyjson_mut_obj_add_val(d, o, "constraints", arr)) {
        return NULL;
    }
    return o;
}

/* ---- camera ---- */

static double dot(const double *a, const double *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void norm(const double *v, double *o)
{
    double l = pc_rs_max_k(sqrt(dot(v, v)), 1e-300);
    o[0] = v[0] / l;
    o[1] = v[1] / l;
    o[2] = v[2] / l;
}

void pc_algo_wide_camera_init(pc_algo_wide_camera *cam, const pc_algo_wide_angle *p, pc_rect frame)
{
    double w = (double)(pc_rect_width(frame) > 1 ? pc_rect_width(frame) : 1);
    double h = (double)(pc_rect_height(frame) > 1 ? pc_rect_height(frame) : 1);
    double f35 = p->focal_length * pc_rs_max_k(p->crop_factor, 0.1);
    pc_algo_wide_model model = p->model;
    if (model == PC_ALGO_WIDE_AUTO) {
        if (fabs(w / h - 2.0) < 0.01) {
            model = PC_ALGO_WIDE_FULL_SPHERICAL;
        } else if (f35 > 0.0 && f35 < 17.0) {
            model = PC_ALGO_WIDE_FISHEYE;
        } else {
            model = PC_ALGO_WIDE_PERSPECTIVE;
        }
    }
    if (!(f35 > 0.0)) {
        f35 = model == PC_ALGO_WIDE_FISHEYE ? 12.0 : 24.0;
    }
    cam->model = model;
    cam->f = f35 / 36.0 * (w > h ? w : h);
    cam->c[0] = (double)pc_i32_wrapping_add(frame.x0, frame.x1) / 2.0;
    cam->c[1] = (double)pc_i32_wrapping_add(frame.y0, frame.y1) / 2.0;
    cam->size[0] = w;
    cam->size[1] = h;
}

void pc_algo_wide_camera_ray(const pc_algo_wide_camera *cam, double x, double y, double out[3])
{
    double dx = x - cam->c[0], dy = y - cam->c[1], v[3];
    if (cam->model == PC_ALGO_WIDE_FULL_SPHERICAL) {
        double lon = dx / cam->size[0] * TAU_D, lat = dy / cam->size[1] * PI_D;
        v[0] = sin(lon) * cos(lat);
        v[1] = sin(lat);
        v[2] = cos(lon) * cos(lat);
    } else if (cam->model == PC_ALGO_WIDE_FISHEYE) {
        double r = hypot(dx, dy), th = r / cam->f;
        if (r < 1e-12) {
            v[0] = 0.0, v[1] = 0.0, v[2] = 1.0;
        } else {
            v[0] = sin(th) * dx / r;
            v[1] = sin(th) * dy / r;
            v[2] = cos(th);
        }
    } else {
        v[0] = dx, v[1] = dy, v[2] = cam->f;
    }
    norm(v, out);
}

bool pc_algo_wide_camera_pixel(const pc_algo_wide_camera *cam, const double d[3], double out[2])
{
    if (cam->model == PC_ALGO_WIDE_FULL_SPHERICAL) {
        double lon = atan2(d[0], d[2]), lat = asin(pc_clampd(d[1], -1.0, 1.0));
        out[0] = cam->c[0] + lon / TAU_D * cam->size[0];
        out[1] = cam->c[1] + lat / PI_D * cam->size[1];
        return true;
    }
    if (cam->model == PC_ALGO_WIDE_FISHEYE) {
        double th = acos(pc_clampd(d[2], -1.0, 1.0)), s = hypot(d[0], d[1]);
        if (s < 1e-12) {
            out[0] = cam->c[0];
            out[1] = cam->c[1];
            return true;
        }
        out[0] = cam->c[0] + cam->f * th * d[0] / s;
        out[1] = cam->c[1] + cam->f * th * d[1] / s;
        return true;
    }
    if (!(d[2] > 1e-6)) {
        return false;
    }
    out[0] = cam->c[0] + cam->f * d[0] / d[2];
    out[1] = cam->c[1] + cam->f * d[1] / d[2];
    return true;
}

pc_status pc_algo_wide_camera_arc(const pc_algo_wide_camera *cam, const double a[2], const double b[2],
                                  size_t n, double (**out)[2], size_t *count, pc_error *err)
{
    double da[3], db[3], omega;
    double (*pts)[2];
    size_t k, m = 0;
    if (n >= PC_MAX_ALLOC / sizeof *pts) {
        return pc_fail(err, PC_ERR_LIMIT, "too many arc samples");
    }
    pts = pc_malloc_array(n + 1, sizeof *pts);
    if (pts == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_algo_wide_camera_ray(cam, a[0], a[1], da);
    pc_algo_wide_camera_ray(cam, b[0], b[1], db);
    omega = acos(pc_clampd(dot(da, db), -1.0, 1.0));
    for (k = 0; k <= n; k++) {
        double t = (double)k / (double)n, d[3];
        if (omega < 1e-9) {
            memcpy(d, da, sizeof d);
        } else {
            double sa = sin((1.0 - t) * omega) / sin(omega), sb = sin(t * omega) / sin(omega);
            double v[3] = {sa * da[0] + sb * db[0], sa * da[1] + sb * db[1], sa * da[2] + sb * db[2]};
            norm(v, d);
        }
        if (pc_algo_wide_camera_pixel(cam, d, pts[m])) {
            m++;
        }
    }
    *out = pts;
    *count = m;
    return PC_OK;
}

/* ---- mesh ---- */

void pc_algo_wide_mesh_fini(pc_algo_wide_mesh *m)
{
    size_t i;
    if (m == NULL) {
        return;
    }
    for (i = 0; i < m->n_curves; i++) {
        free(m->curves[i]);
    }
    free(m->curves);
    free(m->curve_len);
    free(m->out);
    free(m->src);
    memset(m, 0, sizeof *m);
}

pc_status pc_algo_wide_mesh_triangles(const pc_algo_wide_mesh *m, size_t (**out)[3], size_t *count,
                                      pc_error *err)
{
    size_t w1 = m->nx + 1, i, j, k = 0;
    size_t (*t)[3] = pc_malloc_array(m->nx * m->ny * 2 + 1, sizeof *t);
    if (t == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (j = 0; j < m->ny; j++) {
        for (i = 0; i < m->nx; i++) {
            size_t a = j * w1 + i;
            t[k][0] = a, t[k][1] = a + 1, t[k][2] = a + w1 + 1;
            k++;
            t[k][0] = a, t[k][1] = a + w1 + 1, t[k][2] = a + w1;
            k++;
        }
    }
    *out = t;
    *count = k;
    return PC_OK;
}

static void bilerp(const double *a, const double *b, const double *c, const double *d, double tx, double ty,
                   double out[2])
{
    out[0] = (a[0] * (1.0 - tx) + b[0] * tx) * (1.0 - ty) + (c[0] * (1.0 - tx) + d[0] * tx) * ty;
    out[1] = (a[1] * (1.0 - tx) + b[1] * tx) * (1.0 - ty) + (c[1] * (1.0 - tx) + d[1] * tx) * ty;
}

void pc_algo_wide_mesh_map(const pc_algo_wide_mesh *m, pc_rect frame, const double p[2], double out[2])
{
    double cw = (double)pc_rect_width(frame) / (double)m->nx,
           ch = (double)pc_rect_height(frame) / (double)m->ny;
    double gx = pc_clampd((p[0] - (double)frame.x0) / cw, 0.0, (double)m->nx - 1e-9);
    double gy = pc_clampd((p[1] - (double)frame.y0) / ch, 0.0, (double)m->ny - 1e-9);
    size_t i = pc_f64_to_size(floor(gx)), j = pc_f64_to_size(floor(gy)), w1 = m->nx + 1;
    double tx = gx - (double)i, ty = gy - (double)j;
    bilerp(m->out[j * w1 + i], m->out[j * w1 + i + 1], m->out[(j + 1) * w1 + i], m->out[(j + 1) * w1 + i + 1],
           tx, ty, out);
}

typedef struct grid {
    pc_rect frame;
    size_t nx, ny, w1;
    double cw, ch;
} grid;

/* Bilinear weights of a source point in the grid. */
static void weights(const grid *g, const double q[2], size_t idx[4], double w[4])
{
    double gx = pc_clampd((q[0] - (double)g->frame.x0) / g->cw, 0.0, (double)g->nx - 1e-9);
    double gy = pc_clampd((q[1] - (double)g->frame.y0) / g->ch, 0.0, (double)g->ny - 1e-9);
    size_t i = pc_f64_to_size(floor(gx)), j = pc_f64_to_size(floor(gy)), a = j * g->w1 + i;
    double tx = gx - (double)i, ty = gy - (double)j;
    idx[0] = a, w[0] = (1.0 - tx) * (1.0 - ty);
    idx[1] = a + 1, w[1] = tx * (1.0 - ty);
    idx[2] = a + g->w1, w[2] = (1.0 - tx) * ty;
    idx[3] = a + g->w1 + 1, w[3] = tx * ty;
}

static void pos(const grid *g, const double *x, const double q[2], double out[2])
{
    size_t idx[4];
    double w[4];
    int k;
    weights(g, q, idx, w);
    out[0] = 0.0;
    out[1] = 0.0;
    for (k = 0; k < 4; k++) {
        out[0] = out[0] + w[k] * x[2 * idx[k]];
        out[1] = out[1] + w[k] * x[2 * idx[k] + 1];
    }
}

/* Builds the least-squares system of one round. */
static pc_status build_system(pc_algo_photo_sparse_ls *ls, const grid *g, const double (*src)[2], size_t nv,
                              const pc_algo_wide_angle *p, const pc_algo_wide_mesh *m, const size_t *active,
                              size_t n_active, const double (*normals)[2], pc_error *err)
{
    size_t j, i, rot, k, ci, q;
    pc_status st;
    /* Shape: v0 = v1 + u (v2 - v1) + v R90 (v2 - v1), with (u, v) from the source; both
     * diagonals of every cell so the term is symmetric. */
    for (j = 0; j < g->ny; j++) {
        for (i = 0; i < g->nx; i++) {
            size_t a = j * g->w1 + i;
            size_t tris[4][3] = {{a, a + 1, a + g->w1 + 1},
                                 {a, a + g->w1 + 1, a + g->w1},
                                 {a, a + 1, a + g->w1},
                                 {a + 1, a + g->w1 + 1, a + g->w1}};
            for (k = 0; k < 4; k++) {
                for (rot = 0; rot < 3; rot++) {
                    size_t i0 = tris[k][rot], i1 = tris[k][(rot + 1) % 3], i2 = tris[k][(rot + 2) % 3];
                    const double *p0 = src[i0], *p1 = src[i1], *p2 = src[i2];
                    double e[2] = {p2[0] - p1[0], p2[1] - p1[1]}, d[2] = {p0[0] - p1[0], p0[1] - p1[1]};
                    double l2 = e[0] * e[0] + e[1] * e[1];
                    double u = (d[0] * e[0] + d[1] * e[1]) / l2, v = (d[0] * -e[1] + d[1] * e[0]) / l2;
                    pc_algo_photo_term rx[5] = {
                        {2 * i0, 1.0}, {2 * i1, -1.0 + u}, {2 * i2, -u}, {2 * i2 + 1, v}, {2 * i1 + 1, -v}};
                    pc_algo_photo_term ry[5] = {{2 * i0 + 1, 1.0},
                                                {2 * i1 + 1, -1.0 + u},
                                                {2 * i2 + 1, -u},
                                                {2 * i2, -v},
                                                {2 * i1, v}};
                    if ((st = pc_algo_photo_sparse_ls_add(ls, rx, 5, 0.0, 1.0, err)) != PC_OK ||
                        (st = pc_algo_photo_sparse_ls_add(ls, ry, 5, 0.0, 1.0, err)) != PC_OK) {
                        return st;
                    }
                }
            }
        }
    }
    /* Weak anchoring to the source (fixes the global similarity). */
    for (k = 0; k < nv; k++) {
        pc_algo_photo_term tx[1] = {{2 * k, 1.0}}, ty[1] = {{2 * k + 1, 1.0}};
        if ((st = pc_algo_photo_sparse_ls_add(ls, tx, 1, src[k][0], 0.02, err)) != PC_OK ||
            (st = pc_algo_photo_sparse_ls_add(ls, ty, 1, src[k][1], 0.02, err)) != PC_OK) {
            return st;
        }
    }
    /* Lines. */
    for (ci = 0; ci < n_active; ci++) {
        size_t kk = active[ci], i0[4], iq[4];
        const double (*c)[2] = (const double (*)[2])m->curves[kk];
        double n[2], w0[4], wq[4];
        switch (p->constraints[kk].orientation) {
        case PC_ALGO_WIDE_HORIZONTAL:
            n[0] = 0.0, n[1] = 1.0;
            break;
        case PC_ALGO_WIDE_VERTICAL:
            n[0] = 1.0, n[1] = 0.0;
            break;
        default:
            n[0] = normals[ci][0], n[1] = normals[ci][1];
            break;
        }
        weights(g, c[0], i0, w0);
        for (q = 1; q < m->curve_len[kk]; q++) {
            pc_algo_photo_term row[16];
            int t;
            weights(g, c[q], iq, wq);
            for (t = 0; t < 4; t++) {
                row[2 * t] = (pc_algo_photo_term){2 * iq[t], n[0] * wq[t]};
                row[2 * t + 1] = (pc_algo_photo_term){2 * iq[t] + 1, n[1] * wq[t]};
            }
            for (t = 0; t < 4; t++) {
                row[8 + 2 * t] = (pc_algo_photo_term){2 * i0[t], -n[0] * w0[t]};
                row[8 + 2 * t + 1] = (pc_algo_photo_term){2 * i0[t] + 1, -n[1] * w0[t]};
            }
            if ((st = pc_algo_photo_sparse_ls_add(ls, row, 16, 0.0, 10.0, err)) != PC_OK) {
                return st;
            }
        }
    }
    return PC_OK;
}

static pc_status solve_lines(const grid *g, const double (*src)[2], size_t nv, const pc_algo_wide_angle *p,
                             const pc_algo_wide_mesh *m, double *x, pc_error *err)
{
    size_t *active = NULL, n_active = 0, k, ci, round;
    double (*normals)[2] = NULL;
    pc_status st = PC_OK;
    bool all_fixed = true;
    active = pc_malloc_array(m->n_curves + 1, sizeof *active);
    normals = pc_malloc_array(m->n_curves + 1, sizeof *normals);
    if (active == NULL || normals == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }
    for (k = 0; k < m->n_curves; k++) {
        if (m->curve_len[k] >= 3) {
            normals[n_active][0] = 0.0;
            normals[n_active][1] = 1.0;
            active[n_active++] = k;
            all_fixed = all_fixed && p->constraints[k].orientation != PC_ALGO_WIDE_FREE;
        }
    }
    for (round = 0; round < 4 && n_active > 0; round++) {
        pc_algo_photo_sparse_ls ls;
        bool changed = false;
        pc_algo_photo_sparse_ls_init(&ls, 2 * nv);
        st = build_system(&ls, g, src, nv, p, m, active, n_active, (const double (*)[2])normals, err);
        if (st == PC_OK) {
            st = pc_algo_photo_sparse_ls_solve(&ls, x, 400 + 200 * round, x, err);
        }
        pc_algo_photo_sparse_ls_fini(&ls);
        if (st != PC_OK) {
            goto done;
        }
        /* Re-estimate free normals from the current end points. */
        for (ci = 0; ci < n_active; ci++) {
            size_t kk = active[ci];
            double a[2], b[2], d[2], l, nn[2];
            pos(g, x, m->curves[kk][0], a);
            pos(g, x, m->curves[kk][m->curve_len[kk] - 1], b);
            d[0] = b[0] - a[0];
            d[1] = b[1] - a[1];
            l = pc_rs_max_k(hypot(d[0], d[1]), 1e-9);
            nn[0] = -d[1] / l;
            nn[1] = d[0] / l;
            if (fabs(nn[0] - normals[ci][0]) + fabs(nn[1] - normals[ci][1]) > 1e-3) {
                changed = true;
            }
            normals[ci][0] = nn[0];
            normals[ci][1] = nn[1];
        }
        if (round == 0 && all_fixed) {
            break;
        }
        if (round > 0 && !changed) {
            break;
        }
    }
done:
    free(active);
    free(normals);
    return st;
}

pc_status pc_algo_wide_solve(const pc_algo_wide_angle *p, pc_rect frame, pc_algo_wide_mesh *out,
                             pc_error *err)
{
    pc_algo_wide_camera cam;
    pc_algo_wide_mesh m;
    grid g;
    double fw = (double)(pc_rect_width(frame) > 1 ? pc_rect_width(frame) : 1);
    double fh = (double)(pc_rect_height(frame) > 1 ? pc_rect_height(frame) : 1);
    double cell, s, worst = 0.0;
    double *x = NULL;
    size_t nv, k, q;
    pc_status st = PC_OK;
    memset(&m, 0, sizeof m);
    pc_algo_wide_camera_init(&cam, p, frame);
    cell = (fw > fh ? fw : fh) / 48.0;
    g.nx = pc_f64_to_size(round(fw / cell));
    g.ny = pc_f64_to_size(round(fh / cell));
    g.nx = g.nx < 2 ? 2 : (g.nx > 96 ? 96 : g.nx);
    g.ny = g.ny < 2 ? 2 : (g.ny > 96 ? 96 : g.ny);
    g.cw = fw / (double)g.nx;
    g.ch = fh / (double)g.ny;
    g.w1 = g.nx + 1;
    g.frame = frame;
    nv = g.w1 * (g.ny + 1);
    m.nx = g.nx;
    m.ny = g.ny;
    m.src = pc_malloc_array(nv, sizeof *m.src);
    m.out = pc_malloc_array(nv, sizeof *m.out);
    x = pc_malloc_array(2 * nv, sizeof *x);
    if (m.src == NULL || m.out == NULL || x == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto fail;
    }
    for (k = 0; k < nv; k++) {
        m.src[k][0] = (double)frame.x0 + (double)(k % g.w1) * g.cw;
        m.src[k][1] = (double)frame.y0 + (double)(k / g.w1) * g.ch;
        x[2 * k] = m.src[k][0];
        x[2 * k + 1] = m.src[k][1];
    }
    if (p->n_constraints > 0) {
        m.curves = pc_calloc_array(p->n_constraints, sizeof *m.curves);
        m.curve_len = pc_calloc_array(p->n_constraints, sizeof *m.curve_len);
        if (m.curves == NULL || m.curve_len == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto fail;
        }
        m.n_curves = p->n_constraints;
        for (k = 0; k < p->n_constraints; k++) {
            if ((st = pc_algo_wide_camera_arc(&cam, p->constraints[k].a, p->constraints[k].b, 24,
                                              &m.curves[k], &m.curve_len[k], err)) != PC_OK) {
                goto fail;
            }
        }
    }
    if ((st = solve_lines(&g, (const double (*)[2])m.src, nv, p, &m, x, err)) != PC_OK) {
        goto fail;
    }
    /* Scale about the centre. */
    s = pc_clampd(p->scale / 100.0, 0.1, 10.0);
    for (k = 0; k < nv; k++) {
        m.out[k][0] = cam.c[0] + (x[2 * k] - cam.c[0]) * s;
        m.out[k][1] = cam.c[1] + (x[2 * k + 1] - cam.c[1]) * s;
    }
    /* Straightness residual: distance of curve samples from the chord of their mapped ends. */
    for (k = 0; k < m.n_curves; k++) {
        double a[2], b[2], d[2], l;
        size_t len = m.curve_len[k];
        if (len < 3) {
            continue;
        }
        pc_algo_wide_mesh_map(&m, frame, m.curves[k][0], a);
        pc_algo_wide_mesh_map(&m, frame, m.curves[k][len - 1], b);
        d[0] = b[0] - a[0];
        d[1] = b[1] - a[1];
        l = pc_rs_max_k(hypot(d[0], d[1]), 1e-9);
        for (q = 0; q < len; q++) {
            double pq[2];
            pc_algo_wide_mesh_map(&m, frame, m.curves[k][q], pq);
            worst = pc_rs_max(worst, fabs((pq[0] - a[0]) * d[1] - (pq[1] - a[1]) * d[0]) / l);
        }
    }
    m.residual = worst;
    free(x);
    *out = m;
    return PC_OK;
fail:
    free(x);
    pc_algo_wide_mesh_fini(&m);
    return st;
}

pc_status pc_algo_wide_apply(const pc_surface *src, pc_rect frame, const pc_algo_wide_angle *p,
                             pc_algo_interp interp, pc_surface *out, pc_error *err)
{
    pc_algo_wide_mesh m;
    pc_algo_warp_vertex *verts = NULL;
    size_t (*tris)[3] = NULL;
    size_t nv, nt = 0, k;
    pc_status st = pc_algo_wide_solve(p, frame, &m, err);
    if (st != PC_OK) {
        return st;
    }
    nv = (m.nx + 1) * (m.ny + 1);
    verts = pc_malloc_array(nv, sizeof *verts);
    if (verts == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }
    for (k = 0; k < nv; k++) {
        memcpy(verts[k].dst, m.out[k], sizeof verts[k].dst);
        memcpy(verts[k].src, m.src[k], sizeof verts[k].src);
    }
    if ((st = pc_algo_wide_mesh_triangles(&m, &tris, &nt, err)) != PC_OK) {
        goto done;
    }
    st = pc_algo_warp_triangles(src, frame, verts, nv, (const size_t (*)[3])tris, nt, interp, out, err);
done:
    free(verts);
    free(tris);
    pc_algo_wide_mesh_fini(&m);
    return st;
}
