/* Expressions mirror crates/geom/src/warp.rs operation for operation (same order, same libm
 * calls), so results are bit-identical to the reference. */
#include "warp.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"

/* std::f64::consts (M_PI is not ISO C). */
static const double PI = 3.14159265358979323846;
static const double FRAC_PI_2 = 1.57079632679489661923;

/* ---- styles ---- */

const pc_warp_style PC_WARP_PRESETS[15] = {
    PC_WARP_ARC,         PC_WARP_ARC_LOWER, PC_WARP_ARC_UPPER, PC_WARP_ARCH,    PC_WARP_BULGE,
    PC_WARP_SHELL_LOWER, PC_WARP_SHELL_UPPER, PC_WARP_FLAG,    PC_WARP_WAVE,    PC_WARP_FISH,
    PC_WARP_RISE,        PC_WARP_FISHEYE,   PC_WARP_INFLATE,   PC_WARP_SQUEEZE, PC_WARP_TWIST,
};

typedef struct style_names {
    const char *psd;
    const char *id;
    const char *label;
} style_names;

static const style_names NAMES[PC_WARP_STYLE_COUNT] = {
    {"warpNone", "none", "None"},
    {"warpCustom", "custom", "Custom"},
    {"warpArc", "arc", "Arc"},
    {"warpArcLower", "arcLower", "Arc Lower"},
    {"warpArcUpper", "arcUpper", "Arc Upper"},
    {"warpArch", "arch", "Arch"},
    {"warpBulge", "bulge", "Bulge"},
    {"warpShellLower", "shellLower", "Shell Lower"},
    {"warpShellUpper", "shellUpper", "Shell Upper"},
    {"warpFlag", "flag", "Flag"},
    {"warpWave", "wave", "Wave"},
    {"warpFish", "fish", "Fish"},
    {"warpRise", "rise", "Rise"},
    {"warpFisheye", "fisheye", "Fisheye"},
    {"warpInflate", "inflate", "Inflate"},
    {"warpSqueeze", "squeeze", "Squeeze"},
    {"warpTwist", "twist", "Twist"},
};

static const style_names *names_of(pc_warp_style s)
{
    return (unsigned)s < PC_WARP_STYLE_COUNT ? &NAMES[s] : &NAMES[PC_WARP_NONE];
}

const char *pc_warp_style_psd_name(pc_warp_style s) { return names_of(s)->psd; }
const char *pc_warp_style_id(pc_warp_style s) { return names_of(s)->id; }
const char *pc_warp_style_label(pc_warp_style s) { return names_of(s)->label; }

bool pc_warp_style_is_preset(pc_warp_style s)
{
    return (unsigned)s < PC_WARP_STYLE_COUNT && s != PC_WARP_NONE && s != PC_WARP_CUSTOM;
}

static bool ascii_ieq(const char *a, size_t alen, const char *b)
{
    size_t i;
    for (i = 0; i < alen; i++) {
        unsigned char x = (unsigned char)a[i];
        unsigned char y = (unsigned char)b[i];
        if (y == '\0') {
            return false;
        }
        if (x >= 'A' && x <= 'Z') { x = (unsigned char)(x + 32); }
        if (y >= 'A' && y <= 'Z') { y = (unsigned char)(y + 32); }
        if (x != y) {
            return false;
        }
    }
    return b[alen] == '\0';
}

bool pc_warp_style_parse(const char *text, size_t len, pc_warp_style *out)
{
    /* Every name is short ASCII: strip white space; anything else non-ASCII cannot match. */
    char key[32];
    size_t klen = 0;
    size_t i = 0;
    int s;
    if (text == NULL) {
        return false;
    }
    while (i < len) {
        uint32_t cp;
        if (!pc_utf8_next((const uint8_t *)text, len, &i, &cp)) {
            return false;
        }
        if (pc_unicode_is_space(cp)) {
            continue;
        }
        if (cp >= 0x80 || klen + 1 >= sizeof key) {
            return false;
        }
        key[klen++] = (char)cp;
    }
    for (s = 0; s < PC_WARP_STYLE_COUNT; s++) {
        if (ascii_ieq(key, klen, NAMES[s].id) || ascii_ieq(key, klen, NAMES[s].psd)) {
            *out = (pc_warp_style)s;
            return true;
        }
    }
    return false;
}

/* ---- preset maps ---- */

static double pct(double v)
{
    return isfinite(v) ? pc_clampd(v / 100.0, -1.0, 1.0) : 0.0;
}

bool pc_style_warp_init(pc_style_warp *out, pc_warp_style style, double bend, double h_distort, double v_distort,
                        bool vertical, const double bounds[4])
{
    double hw = (bounds[2] - bounds[0]) / 2.0;
    double hh = (bounds[3] - bounds[1]) / 2.0;
    double b;
    double hd;
    double vd;
    if (!pc_warp_style_is_preset(style) || !(hw > 0.0 && hh > 0.0)) {
        return false;
    }
    b = pct(bend);
    hd = pct(h_distort);
    vd = pct(v_distort);
    if (b == 0.0 && hd == 0.0 && vd == 0.0) {
        return false;
    }
    out->style = style;
    out->b = b;
    out->hd = hd;
    out->vd = vd;
    out->vertical = vertical;
    out->cx = (bounds[0] + bounds[2]) / 2.0;
    out->cy = (bounds[1] + bounds[3]) / 2.0;
    out->hw = hw;
    out->hh = hh;
    return true;
}

double pc_style_warp_max_segment(const pc_style_warp *w)
{
    return fmax(fmin(w->hw, w->hh) / 16.0, 0.5);
}

/* Along a circular arc: the box centre line becomes an arc spanning b x 180 degrees. */
static pc_point arc(double u, double v, double b, double aspect)
{
    double half;
    double theta_max;
    double r;
    double theta;
    double rr;
    if (fabs(b) < 1e-6) {
        return pc_point_make(u, v);
    }
    half = aspect;
    theta_max = b * FRAC_PI_2;
    r = half / theta_max;
    theta = u * theta_max;
    rr = r - v;
    return pc_point_make(rr * sin(theta) / half, r - rr * cos(theta));
}

static pc_point lerp2(pc_point a, pc_point b, double t)
{
    t = pc_clampd(t, 0.0, 1.0);
    return pc_point_make(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

/* The style's mapping in normalised space; aspect = half-length along / across the axis. */
static pc_point bend(const pc_style_warp *w, double u, double v, double aspect)
{
    double b = w->b;
    double bell = fmax(1.0 - u * u, 0.0);
    switch (w->style) {
    case PC_WARP_ARC: return arc(u, v, b, aspect);
    case PC_WARP_ARC_LOWER: return lerp2(pc_point_make(u, v), arc(u, v, b, aspect), (v + 1.0) / 2.0);
    case PC_WARP_ARC_UPPER: return lerp2(pc_point_make(u, v), arc(u, v, b, aspect), (1.0 - v) / 2.0);
    case PC_WARP_ARCH: return pc_point_make(u, v - b * bell);
    case PC_WARP_BULGE: return pc_point_make(u, v * (1.0 + b * bell));
    case PC_WARP_SHELL_LOWER: return pc_point_make(u, v + b * bell * (v + 1.0) / 2.0);
    case PC_WARP_SHELL_UPPER: return pc_point_make(u, v - b * bell * (1.0 - v) / 2.0);
    case PC_WARP_FLAG: return pc_point_make(u, v - b * 0.5 * sin(PI * u));
    case PC_WARP_WAVE: return pc_point_make(u, v - b * 0.5 * sin(PI * u + v * FRAC_PI_2));
    case PC_WARP_FISH: return pc_point_make(u, v * (1.0 + b * 0.6 * (1.0 - u) * sqrt(bell)) - b * 0.35 * bell);
    case PC_WARP_RISE: return pc_point_make(u, v - b * 0.5 * sin(FRAC_PI_2 * u));
    case PC_WARP_FISHEYE: {
        double r2 = fmin(u * u + v * v, 1.0);
        double k = 1.0 + b * (1.0 - r2);
        return pc_point_make(u * k, v * k);
    }
    case PC_WARP_INFLATE: {
        double bu = fmax(1.0 - v * v, 0.0);
        return pc_point_make(u * (1.0 + b * 0.5 * bu), v * (1.0 + b * 0.5 * bell));
    }
    case PC_WARP_SQUEEZE:
        return pc_point_make(u * (1.0 + b * 0.5 * fmax(1.0 - v * v, 0.0)), v * (1.0 - b * 0.5 * bell));
    case PC_WARP_TWIST: {
        double r = sqrt(u * u + v * v);
        double a;
        double s;
        double c;
        double x;
        if (r >= 1.0) {
            return pc_point_make(u, v);
        }
        /* Rotate in an isotropic frame so the twist stays circular on wide boxes. */
        a = b * FRAC_PI_2 * (1.0 - r) * (1.0 - r);
        s = sin(a);
        c = cos(a);
        x = u * aspect;
        return pc_point_make((x * c - v * s) / aspect, x * s + v * c);
    }
    case PC_WARP_NONE:
    case PC_WARP_CUSTOM:
    case PC_WARP_STYLE_COUNT:
    default: break;
    }
    return pc_point_make(u, v);
}

pc_point pc_style_warp_apply(const pc_style_warp *w, double x, double y)
{
    double dx = x - w->cx;
    double dy = y - w->cy;
    double u;
    double v;
    double hu;
    double hv;
    pc_point p;
    if (w->vertical) {
        u = dy / w->hh;
        v = dx / w->hw;
        hu = w->hh;
        hv = w->hw;
    } else {
        u = dx / w->hw;
        v = dy / w->hh;
        hu = w->hw;
        hv = w->hh;
    }
    p = bend(w, u, v, hu / hv);
    /* Distortions: a perspective-like taper along each axis. */
    if (w->hd != 0.0) {
        p.y *= fmax(1.0 + w->hd * p.x, 0.05);
    }
    if (w->vd != 0.0) {
        p.x *= fmax(1.0 + w->vd * p.y, 0.05);
    }
    if (w->vertical) {
        return pc_point_make(w->cx + p.y * w->hw, w->cy + p.x * w->hh);
    }
    return pc_point_make(w->cx + p.x * w->hw, w->cy + p.y * w->hh);
}

/* ---- Bezier meshes ---- */

/* Inverse of the Bernstein interpolation matrix at t = 0, 1/3, 2/3, 1 (rows: control point,
 * columns: sample). */
static const double BERN_INV[4][4] = {
    {1.0, 0.0, 0.0, 0.0},
    {-5.0 / 6.0, 3.0, -1.5, 1.0 / 3.0},
    {1.0 / 3.0, -1.5, 3.0, -5.0 / 6.0},
    {0.0, 0.0, 0.0, 1.0},
};

static void bernstein(double t, double out[4])
{
    double s = 1.0 - t;
    out[0] = s * s * s;
    out[1] = 3.0 * s * s * t;
    out[2] = 3.0 * s * t * t;
    out[3] = t * t * t;
}

static pc_point lerp(pc_point a, pc_point b, double t)
{
    return pc_point_make(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

/* De Casteljau split of a cubic at t: 7 points (left 0..3, right 3..6). */
static void split_cubic(const pc_point p[4], double t, pc_point out[7])
{
    pc_point a = lerp(p[0], p[1], t);
    pc_point b = lerp(p[1], p[2], t);
    pc_point c = lerp(p[2], p[3], t);
    pc_point d = lerp(a, b, t);
    pc_point e = lerp(b, c, t);
    pc_point f = lerp(d, e, t);
    out[0] = p[0];
    out[1] = a;
    out[2] = d;
    out[3] = f;
    out[4] = e;
    out[5] = c;
    out[6] = p[3];
}

size_t pc_mesh_nx(const pc_mesh *m)
{
    return m->nus >= 1 ? 3 * (m->nus - 1) + 1 : 0;
}

size_t pc_mesh_ny(const pc_mesh *m)
{
    return m->nvs >= 1 ? 3 * (m->nvs - 1) + 1 : 0;
}

/* Knot counts and point count consistent: safe to index (values may still be odd). */
static bool structural(const pc_mesh *m)
{
    size_t n;
    return m->nus >= 2 && m->nvs >= 2 && m->us != NULL && m->vs != NULL && m->points != NULL &&
           pc_size_mul(pc_mesh_nx(m), pc_mesh_ny(m), &n) && n == m->npoints;
}

void pc_mesh_fini(pc_mesh *m)
{
    if (m != NULL) {
        free(m->us);
        free(m->vs);
        free(m->points);
        memset(m, 0, sizeof *m);
    }
}

static double *dup_doubles(const double *src, size_t n)
{
    double *d = pc_malloc_array(n, sizeof *d);
    if (d != NULL && n > 0) {
        memcpy(d, src, n * sizeof *d);
    }
    return d;
}

pc_status pc_mesh_clone(const pc_mesh *m, pc_mesh *out, pc_error *err)
{
    pc_mesh c;
    memset(&c, 0, sizeof c);
    c.us = dup_doubles(m->us, m->nus);
    c.vs = dup_doubles(m->vs, m->nvs);
    c.points = pc_malloc_array(m->npoints, sizeof *c.points);
    if (c.us == NULL || c.vs == NULL || c.points == NULL) {
        pc_mesh_fini(&c);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a warp mesh");
    }
    if (m->npoints > 0) {
        memcpy(c.points, m->points, m->npoints * sizeof *c.points);
    }
    c.nus = m->nus;
    c.nvs = m->nvs;
    c.npoints = m->npoints;
    *out = c;
    return PC_OK;
}

pc_status pc_mesh_fit(pc_mesh_map_fn f, void *ctx, const double *us, size_t nus, const double *vs, size_t nvs,
                      pc_mesh *out, pc_error *err)
{
    pc_mesh m;
    size_t pc_;
    size_t pr;
    size_t nx;
    size_t ny;
    size_t npts;
    size_t pi;
    size_t pj;

    if (nus < 2 || nvs < 2 || us == NULL || vs == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "a warp mesh needs at least two knots per axis");
    }
    pc_ = nus - 1;
    pr = nvs - 1;
    if (!pc_size_mul(3, pc_, &nx) || !pc_size_add(nx, 1, &nx) || !pc_size_mul(3, pr, &ny) ||
        !pc_size_add(ny, 1, &ny) || !pc_size_mul(nx, ny, &npts)) {
        return pc_fail(err, PC_ERR_LIMIT, "warp mesh too large");
    }
    memset(&m, 0, sizeof m);
    m.us = dup_doubles(us, nus);
    m.vs = dup_doubles(vs, nvs);
    m.points = pc_calloc_array(npts, sizeof *m.points);
    if (m.us == NULL || m.vs == NULL || m.points == NULL) {
        pc_mesh_fini(&m);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory building a warp mesh");
    }
    m.nus = nus;
    m.nvs = nvs;
    m.npoints = npts;
    for (pj = 0; pj < pr; pj++) {
        for (pi = 0; pi < pc_; pi++) {
            pc_point s[4][4]; /* s[row j][col i] */
            int i;
            int j;
            for (j = 0; j < 4; j++) {
                for (i = 0; i < 4; i++) {
                    double u = us[pi] + (us[pi + 1] - us[pi]) * (double)i / 3.0;
                    double t = vs[pj] + (vs[pj + 1] - vs[pj]) * (double)j / 3.0;
                    s[j][i] = f(ctx, u, t);
                }
            }
            for (j = 0; j < 4; j++) {
                for (i = 0; i < 4; i++) {
                    pc_point acc = {0.0, 0.0};
                    int a;
                    int b;
                    for (a = 0; a < 4; a++) {
                        for (b = 0; b < 4; b++) {
                            double k = BERN_INV[j][a] * BERN_INV[i][b];
                            acc.x += k * s[a][b].x;
                            acc.y += k * s[a][b].y;
                        }
                    }
                    m.points[(3 * pj + (size_t)j) * nx + 3 * pi + (size_t)i] = acc;
                }
            }
        }
    }
    *out = m;
    return PC_OK;
}

typedef struct box_ctx {
    double b[4];
} box_ctx;

static pc_point box_map(void *ctx, double s, double t)
{
    const double *b = ((const box_ctx *)ctx)->b;
    return pc_point_make(b[0] + s * (b[2] - b[0]), b[1] + t * (b[3] - b[1]));
}

static pc_status uniform_knots(size_t n, double **out, pc_error *err)
{
    double *k = pc_malloc_array(n + 1, sizeof *k);
    size_t i;
    if (k == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i <= n; i++) {
        k[i] = (double)i / (double)n;
    }
    *out = k;
    return PC_OK;
}

static size_t clamp_patches(size_t n)
{
    return n < 1 ? 1 : (n > 64 ? 64 : n);
}

/* Fit f over cols x rows uniform patches. */
static pc_status fit_uniform(pc_mesh_map_fn f, void *ctx, size_t cols, size_t rows, pc_mesh *out, pc_error *err)
{
    double *us = NULL;
    double *vs = NULL;
    pc_status s;
    cols = clamp_patches(cols);
    rows = clamp_patches(rows);
    s = uniform_knots(cols, &us, err);
    if (s == PC_OK) {
        s = uniform_knots(rows, &vs, err);
    }
    if (s == PC_OK) {
        s = pc_mesh_fit(f, ctx, us, cols + 1, vs, rows + 1, out, err);
    }
    free(us);
    free(vs);
    return s;
}

pc_status pc_mesh_identity(const double bounds[4], size_t cols, size_t rows, pc_mesh *out, pc_error *err)
{
    box_ctx c;
    memcpy(c.b, bounds, sizeof c.b);
    return fit_uniform(box_map, &c, cols, rows, out, err);
}

bool pc_mesh_point(const pc_mesh *m, size_t i, size_t j, pc_point *out)
{
    size_t nx = pc_mesh_nx(m);
    if (!structural(m) || i >= nx || j >= pc_mesh_ny(m)) {
        return false;
    }
    *out = m->points[j * nx + i];
    return true;
}

static bool knots_ok(const double *k, size_t n)
{
    size_t i;
    if (n < 2 || k[0] != 0.0 || k[n - 1] != 1.0) {
        return false;
    }
    for (i = 1; i < n; i++) {
        if (!(k[i] > k[i - 1])) {
            return false;
        }
    }
    return true;
}

bool pc_mesh_is_valid(const pc_mesh *m)
{
    size_t i;
    if (!structural(m) || !knots_ok(m->us, m->nus) || !knots_ok(m->vs, m->nvs)) {
        return false;
    }
    for (i = 0; i < m->npoints; i++) {
        if (!isfinite(m->points[i].x) || !isfinite(m->points[i].y)) {
            return false;
        }
    }
    return true;
}

/* Which patch holds s, and the local parameter within it. */
static size_t locate(const double *knots, size_t nknots, double s, double *local)
{
    size_t n = nknots - 1;
    size_t i = 0;
    double w;
    s = pc_clampd(s, 0.0, 1.0);
    while (i + 1 < n && s > knots[i + 1]) {
        i++;
    }
    w = knots[i + 1] - knots[i];
    *local = w > 0.0 ? (s - knots[i]) / w : 0.0;
    return i;
}

pc_point pc_mesh_eval(const pc_mesh *m, double s, double t)
{
    double a;
    double b;
    double ba[4];
    double bb[4];
    size_t pi;
    size_t pj;
    size_t nx;
    pc_point out = {0.0, 0.0};
    int i;
    int j;
    if (!structural(m)) {
        return pc_point_make(NAN, NAN);
    }
    pi = locate(m->us, m->nus, s, &a);
    pj = locate(m->vs, m->nvs, t, &b);
    bernstein(a, ba);
    bernstein(b, bb);
    nx = pc_mesh_nx(m);
    for (j = 0; j < 4; j++) {
        for (i = 0; i < 4; i++) {
            pc_point p = m->points[(3 * pj + (size_t)j) * nx + 3 * pi + (size_t)i];
            double k = ba[i] * bb[j];
            out.x += p.x * k;
            out.y += p.y * k;
        }
    }
    return out;
}

static pc_status insert_knot(double **knots, size_t *n, size_t at, double value, pc_error *err)
{
    double *k = pc_malloc_array(*n + 1, sizeof *k);
    if (k == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory splitting a warp mesh");
    }
    memcpy(k, *knots, at * sizeof *k);
    k[at] = value;
    memcpy(k + at + 1, *knots + at, (*n - at) * sizeof *k);
    free(*knots);
    *knots = k;
    (*n)++;
    return PC_OK;
}

pc_status pc_mesh_split_u(pc_mesh *m, double s, bool *did_split, pc_error *err)
{
    double a;
    size_t pi;
    size_t nx;
    size_t ny;
    size_t nnx;
    size_t j;
    pc_point *pts;
    pc_status st;

    if (!structural(m)) {
        return pc_fail(err, PC_ERR_INVALID, "malformed warp mesh");
    }
    pi = locate(m->us, m->nus, s, &a);
    if (!(a >= 0.01 && a <= 0.99)) {
        *did_split = false;
        return PC_OK;
    }
    nx = pc_mesh_nx(m);
    ny = pc_mesh_ny(m);
    nnx = nx + 3;
    pts = pc_size_mul(nnx, ny, &j) ? pc_malloc_array(j, sizeof *pts) : NULL;
    if (pts == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory splitting a warp mesh");
    }
    for (j = 0; j < ny; j++) {
        const pc_point *row = m->points + j * nx;
        pc_point *dst = pts + j * nnx;
        memcpy(dst, row, 3 * pi * sizeof *dst);
        split_cubic(row + 3 * pi, a, dst + 3 * pi);
        memcpy(dst + 3 * pi + 7, row + 3 * pi + 4, (nx - 3 * pi - 4) * sizeof *dst);
    }
    st = insert_knot(&m->us, &m->nus, pi + 1, pc_clampd(s, 0.0, 1.0), err);
    if (st != PC_OK) {
        free(pts);
        return st;
    }
    free(m->points);
    m->points = pts;
    m->npoints = nnx * ny;
    *did_split = true;
    return PC_OK;
}

pc_status pc_mesh_remove_split_u(pc_mesh *m, size_t k, bool *removed, pc_error *err)
{
    double a;
    size_t nx;
    size_t ny;
    size_t nnx;
    size_t c0;
    size_t j;
    pc_point *pts;
    double *us;

    if (!structural(m)) {
        return pc_fail(err, PC_ERR_INVALID, "malformed warp mesh");
    }
    if (k == 0 || k + 1 >= m->nus) {
        *removed = false;
        return PC_OK;
    }
    a = (m->us[k] - m->us[k - 1]) / (m->us[k + 1] - m->us[k - 1]);
    nx = pc_mesh_nx(m);
    ny = pc_mesh_ny(m);
    nnx = nx - 3;
    c0 = 3 * (k - 1);
    pts = pc_malloc_array(nnx * ny, sizeof *pts); /* smaller than the existing points array */
    us = pc_malloc_array(m->nus - 1, sizeof *us);
    if (pts == NULL || us == NULL) {
        free(pts);
        free(us);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory merging warp patches");
    }
    for (j = 0; j < ny; j++) {
        const pc_point *row = m->points + j * nx;
        pc_point *dst = pts + j * nnx;
        pc_point p0 = row[c0];
        pc_point q1 = row[c0 + 1];
        pc_point r2 = row[c0 + 5];
        pc_point p3 = row[c0 + 6];
        /* De Casteljau inverse: q1 = lerp(p0, p1, a), r2 = lerp(p2, p3, a). */
        pc_point p1 = pc_point_make((q1.x - (1.0 - a) * p0.x) / a, (q1.y - (1.0 - a) * p0.y) / a);
        pc_point p2 = pc_point_make((r2.x - a * p3.x) / (1.0 - a), (r2.y - a * p3.y) / (1.0 - a));
        memcpy(dst, row, c0 * sizeof *dst);
        dst[c0] = p0;
        dst[c0 + 1] = p1;
        dst[c0 + 2] = p2;
        dst[c0 + 3] = p3;
        memcpy(dst + c0 + 4, row + c0 + 7, (nx - c0 - 7) * sizeof *dst);
    }
    memcpy(us, m->us, k * sizeof *us);
    memcpy(us + k, m->us + k + 1, (m->nus - k - 1) * sizeof *us);
    free(m->points);
    free(m->us);
    m->points = pts;
    m->npoints = nnx * ny;
    m->us = us;
    m->nus--;
    *removed = true;
    return PC_OK;
}

/* A transposed copy (rows <-> columns, us <-> vs). */
static pc_status transposed(const pc_mesh *m, pc_mesh *out, pc_error *err)
{
    size_t nx = pc_mesh_nx(m);
    size_t ny = pc_mesh_ny(m);
    size_t i;
    size_t j;
    pc_mesh t;
    pc_status s = pc_mesh_clone(m, &t, err);
    if (s != PC_OK) {
        return s;
    }
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            t.points[i * ny + j] = m->points[j * nx + i];
        }
    }
    {
        double *tmp = t.us;
        size_t n = t.nus;
        t.us = t.vs;
        t.nus = t.nvs;
        t.vs = tmp;
        t.nvs = n;
    }
    *out = t;
    return PC_OK;
}

/* Run op on the transposed mesh and transpose back; m changes only if everything succeeds. */
static pc_status via_transpose(pc_mesh *m, pc_status (*op)(pc_mesh *, double, size_t, bool *, pc_error *),
                               double s, size_t k, bool *done, pc_error *err)
{
    pc_mesh t;
    pc_mesh back;
    pc_status st;
    if (!structural(m)) {
        return pc_fail(err, PC_ERR_INVALID, "malformed warp mesh");
    }
    st = transposed(m, &t, err);
    if (st != PC_OK) {
        return st;
    }
    st = op(&t, s, k, done, err);
    if (st == PC_OK && *done) {
        st = transposed(&t, &back, err);
        if (st == PC_OK) {
            pc_mesh_fini(m);
            *m = back;
        }
    }
    pc_mesh_fini(&t);
    return st;
}

static pc_status op_split(pc_mesh *m, double s, size_t k, bool *done, pc_error *err)
{
    (void)k;
    return pc_mesh_split_u(m, s, done, err);
}

static pc_status op_remove(pc_mesh *m, double s, size_t k, bool *done, pc_error *err)
{
    (void)s;
    return pc_mesh_remove_split_u(m, k, done, err);
}

pc_status pc_mesh_split_v(pc_mesh *m, double t, bool *did_split, pc_error *err)
{
    return via_transpose(m, op_split, t, 0, did_split, err);
}

pc_status pc_mesh_remove_split_v(pc_mesh *m, size_t k, bool *removed, pc_error *err)
{
    return via_transpose(m, op_remove, 0.0, k, removed, err);
}

pc_status pc_mesh_map_points(const pc_mesh *m, pc_point_map_fn f, void *ctx, pc_mesh *out, pc_error *err)
{
    pc_mesh c;
    size_t i;
    pc_status s = pc_mesh_clone(m, &c, err);
    if (s != PC_OK) {
        return s;
    }
    for (i = 0; i < c.npoints; i++) {
        c.points[i] = f(ctx, c.points[i]);
    }
    *out = c;
    return PC_OK;
}

void pc_mesh_param_at(const pc_mesh *m, pc_point p, double *s_out, double *t_out)
{
    const int n = 48;
    double best_s = 0.5;
    double best_t = 0.5;
    double best_d = DBL_MAX;
    double s;
    double t;
    int i;
    int j;
    int it;
    for (j = 0; j <= n; j++) {
        for (i = 0; i <= n; i++) {
            double ss = (double)i / (double)n;
            double tt = (double)j / (double)n;
            pc_point q = pc_mesh_eval(m, ss, tt);
            double dx = q.x - p.x;
            double dy = q.y - p.y;
            double d = dx * dx + dy * dy;
            if (d < best_d) {
                best_s = ss;
                best_t = tt;
                best_d = d;
            }
        }
    }
    s = best_s;
    t = best_t;
    for (it = 0; it < 12; it++) {
        const double h = 1e-5;
        pc_point q = pc_mesh_eval(m, s, t);
        pc_point qs = pc_mesh_eval(m, s + h, t);
        pc_point qt = pc_mesh_eval(m, s, t + h);
        double a = (qs.x - q.x) / h;
        double b = (qt.x - q.x) / h;
        double c = (qs.y - q.y) / h;
        double d = (qt.y - q.y) / h;
        double det = a * d - b * c;
        double ex;
        double ey;
        if (fabs(det) < 1e-12) {
            break;
        }
        ex = p.x - q.x;
        ey = p.y - q.y;
        s = pc_clampd(s + (d * ex - b * ey) / det, 0.0, 1.0);
        t = pc_clampd(t + (a * ey - c * ex) / det, 0.0, 1.0);
    }
    *s_out = s;
    *t_out = t;
}

void pc_mesh_control_bounds(const pc_mesh *m, double out[4])
{
    double b[4] = {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
    size_t i;
    for (i = 0; i < m->npoints && m->points != NULL; i++) {
        b[0] = fmin(b[0], m->points[i].x);
        b[1] = fmin(b[1], m->points[i].y);
        b[2] = fmax(b[2], m->points[i].x);
        b[3] = fmax(b[3], m->points[i].y);
    }
    memcpy(out, b, sizeof b);
}

/* ---- warps ---- */

void pc_warp_none(pc_warp *w, const double bounds[4])
{
    memset(w, 0, sizeof *w);
    w->style = PC_WARP_NONE;
    memcpy(w->bounds, bounds, sizeof w->bounds);
}

void pc_warp_preset(pc_warp *w, pc_warp_style style, double bend_pct, const double bounds[4])
{
    pc_warp_none(w, bounds);
    w->style = style;
    w->bend = bend_pct;
}

void pc_warp_custom(pc_warp *w, pc_mesh *mesh, const double bounds[4])
{
    pc_warp_none(w, bounds);
    w->style = PC_WARP_CUSTOM;
    w->has_mesh = true;
    w->mesh = *mesh;
    memset(mesh, 0, sizeof *mesh);
}

void pc_warp_fini(pc_warp *w)
{
    if (w != NULL) {
        pc_mesh_fini(&w->mesh);
        w->has_mesh = false;
    }
}

static bool style_warp(const pc_warp *w, pc_style_warp *out)
{
    return pc_style_warp_init(out, w->style, w->bend, w->h_distort, w->v_distort, w->vertical, w->bounds);
}

pc_point pc_warp_map(const pc_warp *w, double x, double y)
{
    pc_style_warp sw;
    if (w->style == PC_WARP_NONE) {
        return pc_point_make(x, y);
    }
    if (w->style == PC_WARP_CUSTOM) {
        const double *b = w->bounds;
        double bw = b[2] - b[0];
        double bh = b[3] - b[1];
        if (!w->has_mesh || bw <= 0.0 || bh <= 0.0) {
            return pc_point_make(x, y);
        }
        return pc_mesh_eval(&w->mesh, (x - b[0]) / bw, (y - b[1]) / bh);
    }
    return style_warp(w, &sw) ? pc_style_warp_apply(&sw, x, y) : pc_point_make(x, y);
}

bool pc_warp_is_identity(const pc_warp *w)
{
    pc_style_warp sw;
    if (w->style == PC_WARP_NONE) {
        return true;
    }
    if (w->style == PC_WARP_CUSTOM) {
        pc_mesh id;
        box_ctx c;
        size_t i;
        bool same = true;
        if (!w->has_mesh) {
            return true;
        }
        if (!structural(&w->mesh)) {
            return false;
        }
        memcpy(c.b, w->bounds, sizeof c.b);
        if (pc_mesh_fit(box_map, &c, w->mesh.us, w->mesh.nus, w->mesh.vs, w->mesh.nvs, &id, NULL) != PC_OK) {
            return false; /* out of memory: treat as a real warp, which is always safe */
        }
        for (i = 0; i < id.npoints && same; i++) {
            same = fabs(w->mesh.points[i].x - id.points[i].x) < 1e-9 && fabs(w->mesh.points[i].y - id.points[i].y) < 1e-9;
        }
        pc_mesh_fini(&id);
        return same;
    }
    return !style_warp(w, &sw);
}

static pc_point warp_box_map(void *ctx, double s, double t)
{
    const pc_warp *w = ctx;
    const double *b = w->bounds;
    return pc_warp_map(w, b[0] + s * (b[2] - b[0]), b[1] + t * (b[3] - b[1]));
}

pc_status pc_warp_to_mesh(const pc_warp *w, size_t cols, size_t rows, pc_mesh *out, pc_error *err)
{
    if (w->style == PC_WARP_CUSTOM && w->has_mesh) {
        return pc_mesh_clone(&w->mesh, out, err);
    }
    /* fit_uniform calls warp_box_map with a const pc_warp; the callback only reads it. */
    return fit_uniform(warp_box_map, (void *)(uintptr_t)w, cols, rows, out, err);
}

static pc_point affine_map(void *ctx, pc_point p)
{
    const double *m = ctx;
    return pc_point_make(m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]);
}

pc_status pc_warp_then_affine(const pc_warp *w, const double m[6], pc_warp *out, pc_error *err)
{
    pc_mesh base;
    pc_mesh mapped;
    double mm[6];
    double bounds[4];
    pc_status s = pc_warp_to_mesh(w, 4, 4, &base, err);
    if (s != PC_OK) {
        return s;
    }
    memcpy(mm, m, sizeof mm);
    s = pc_mesh_map_points(&base, affine_map, mm, &mapped, err);
    pc_mesh_fini(&base);
    if (s != PC_OK) {
        return s;
    }
    memcpy(bounds, w->bounds, sizeof bounds);
    if (out == w) {
        pc_warp_fini(out); /* writing over the input: release its mesh first */
    }
    pc_warp_custom(out, &mapped, bounds);
    return PC_OK;
}

void pc_warp_output_bounds(const pc_warp *w, double out[4])
{
    const int n = 32;
    double b[4] = {DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX};
    int i;
    int j;
    for (j = 0; j <= n; j++) {
        for (i = 0; i <= n; i++) {
            const double *bb = w->bounds;
            double s = (double)i / (double)n;
            double t = (double)j / (double)n;
            pc_point p = pc_warp_map(w, bb[0] + s * (bb[2] - bb[0]), bb[1] + t * (bb[3] - bb[1]));
            b[0] = fmin(b[0], p.x);
            b[1] = fmin(b[1], p.y);
            b[2] = fmax(b[2], p.x);
            b[3] = fmax(b[3], p.y);
        }
    }
    memcpy(out, b, sizeof b);
}
