/* Synthetic CMYK profile generator (crates/cms/src/synth.rs), operation for operation. */
#include "synth.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "builtin.h"
#include "clut.h"
#include "math.h"

static const double TAU = 6.28318530717958647692;

pc_cmyk_params pc_cmyk_params_default(void)
{
    pc_cmyk_params p;
    p.description = PC_SYNTH_DESCRIPTION;
    p.tac = 3.0;
    p.gcr_start = 0.2;
    p.k_max = 0.95;
    p.gcr_gamma = 1.1;
    p.tvi[0] = 0.14;
    p.tvi[1] = 0.14;
    p.tvi[2] = 0.14;
    p.tvi[3] = 0.17;
    p.grid_a2b = 11;
    p.grid_b2a = 21;
    return p;
}

/* f64::rem_euclid */
static double rem_euclid(double x, double m)
{
    double r = fmod(x, m);
    return r < 0.0 ? r + fabs(m) : r;
}

static void lab3(double l, double a, double b, double out[3])
{
    double lab[3];
    lab[0] = l;
    lab[1] = a;
    lab[2] = b;
    pc_lab_to_xyz(lab, PC_D50, out);
}

void pc_cmyk_model_coated(const double tvi[4], pc_cmyk_model *out)
{
    /* ISO 12647-2 (coated, paper type 1/2) Lab aims, D50/2 deg; CMY overprint estimated. */
    double base[8][3];
    double w[3];
    double k[3];
    const double floor_ = 0.006;
    int m;
    int i;
    lab3(95.0, 0.0, -2.0, w);
    memcpy(base[0], w, sizeof w); /* paper */
    lab3(55.0, -37.0, -50.0, base[1]); /* C */
    lab3(48.0, 74.0, -3.0, base[2]);   /* M */
    lab3(24.0, 22.0, -46.0, base[3]);  /* C+M (blue) */
    lab3(89.0, -5.0, 93.0, base[4]);   /* Y */
    lab3(50.0, -65.0, 27.0, base[5]);  /* C+Y (green) */
    lab3(47.0, 68.0, 48.0, base[6]);   /* M+Y (red) */
    lab3(23.0, 0.0, 0.0, base[7]);     /* C+M+Y */
    lab3(16.0, 0.0, 0.0, k);
    for (m = 0; m < 16; m++) {
        const double *p = base[m & 7];
        for (i = 0; i < 3; i++) {
            double v;
            if ((m & 8) == 0) {
                v = p[i];
            } else {
                /* Overprints with black: multiplicative above a first-surface floor. */
                double s = floor_ * w[i];
                v = s + (p[i] - s) * (k[i] - s) / (w[i] - s);
            }
            /* Rust: v.max(0.0).powf(1.0 / YN). LLVM compiles pow(x, 0.5) to fabs(sqrt(x)), which
             * glibc's pow doesn't always match, so write what the reference executes. */
            out->prim_n[m][i] = fabs(sqrt(fmax(v, 0.0)));
        }
    }
    memcpy(out->paper, w, sizeof w);
    memcpy(out->tvi, tvi, 4 * sizeof *tvi);
}

void pc_cmyk_model_xyz(const pc_cmyk_model *md, const double cmyk[4], double out[3])
{
    double a[4];
    double s[3] = {0.0, 0.0, 0.0};
    int m;
    int i;
    for (i = 0; i < 4; i++) {
        double v = pc_clampd(cmyk[i], 0.0, 1.0);
        a[i] = pc_clampd(v + 4.0 * md->tvi[i] * v * (1.0 - v), 0.0, 1.0);
    }
    for (m = 0; m < 16; m++) {
        double w = 1.0;
        for (i = 0; i < 4; i++) {
            w *= (m & (1 << i)) != 0 ? a[i] : 1.0 - a[i];
        }
        if (w == 0.0) {
            continue;
        }
        for (i = 0; i < 3; i++) {
            s[i] += w * md->prim_n[m][i];
        }
    }
    for (i = 0; i < 3; i++) {
        out[i] = s[i] * s[i]; /* Rust: v.powf(YN) with YN = 2, which LLVM compiles to v * v */
    }
}

void pc_cmyk_model_lab(const pc_cmyk_model *md, const double cmyk[4], double out[3])
{
    double x[3];
    double rel[3];
    int i;
    pc_cmyk_model_xyz(md, cmyk, x);
    for (i = 0; i < 3; i++) {
        rel[i] = x[i] * PC_D50[i] / md->paper[i];
    }
    pc_xyz_to_lab(rel, PC_D50, out);
}

static double sum3(const double x[3])
{
    double s = -0.0; /* Rust's f64 Sum */
    s += x[0];
    s += x[1];
    s += x[2];
    return s;
}

static void project(double x[3], double limit)
{
    int it;
    int i;
    for (it = 0; it < 4; it++) {
        double s;
        double active = 0.0;
        double d;
        for (i = 0; i < 3; i++) {
            x[i] = pc_clampd(x[i], 0.0, 1.0);
        }
        s = sum3(x);
        if (s <= limit + 1e-12) {
            break;
        }
        for (i = 0; i < 3; i++) {
            active += x[i] > 0.0 ? 1.0 : 0.0;
        }
        if (active < 1.0) {
            active = 1.0;
        }
        d = (s - limit) / active;
        for (i = 0; i < 3; i++) {
            if (x[i] > 0.0) {
                x[i] -= d;
            }
        }
    }
}

typedef struct solve_ctx {
    const pc_cmyk_model *model;
    const double *target;
    double k;
} solve_ctx;

static void err_at(const solve_ctx *c, const double x[3], double e[3])
{
    double cmyk[4];
    double l[3];
    cmyk[0] = x[0];
    cmyk[1] = x[1];
    cmyk[2] = x[2];
    cmyk[3] = c->k;
    pc_cmyk_model_lab(c->model, cmyk, l);
    e[0] = l[0] - c->target[0];
    e[1] = l[1] - c->target[1];
    e[2] = l[2] - c->target[2];
}

static double norm2(const double e[3])
{
    return e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
}

/* Bounded least squares for C, M, Y given K (projected Levenberg-Marquardt, numerical
 * Jacobian). Returns the dE76 residual; *best gets the solution. */
static double solve_cmy(const pc_cmyk_model *model, const double target[3], double k, double limit,
                        const double (*seeds)[3], size_t nseeds, double best_x[3])
{
    solve_ctx c;
    double best_f = INFINITY;
    size_t si;
    c.model = model;
    c.target = target;
    c.k = k;
    best_x[0] = best_x[1] = best_x[2] = 0.0;
    for (si = 0; si < nseeds; si++) {
        double x[3];
        double e[3];
        double f;
        double lambda = 1e-3;
        int it;
        memcpy(x, seeds[si], sizeof x);
        project(x, limit);
        err_at(&c, x, e);
        f = norm2(e);
        for (it = 0; it < 60; it++) {
            const double h = 1e-5;
            double j[3][3];
            pc_mat3 a;
            double g[3];
            bool improved = false;
            int r;
            int cc;
            int kk;
            int tries;
            for (cc = 0; cc < 3; cc++) {
                double xp[3];
                double ep[3];
                memcpy(xp, x, sizeof xp);
                xp[cc] += h;
                err_at(&c, xp, ep);
                for (r = 0; r < 3; r++) {
                    j[r][cc] = (ep[r] - e[r]) / h;
                }
            }
            /* (J^T J + lambda diag) delta = -J^T e */
            for (r = 0; r < 3; r++) {
                double gs = -0.0;
                for (cc = 0; cc < 3; cc++) {
                    double s = -0.0;
                    for (kk = 0; kk < 3; kk++) {
                        s += j[kk][r] * j[kk][cc];
                    }
                    a.m[r][cc] = s;
                }
                for (kk = 0; kk < 3; kk++) {
                    gs += j[kk][r] * e[kk];
                }
                g[r] = -gs;
            }
            for (tries = 0; tries < 8; tries++) {
                pc_mat3 al = a;
                pc_mat3 inv;
                double step[3];
                double xn[3];
                double en[3];
                double fnew;
                int d;
                for (d = 0; d < 3; d++) {
                    al.m[d][d] += lambda * (1.0 + a.m[d][d]);
                }
                if (!pc_mat3_invert(&al, &inv)) {
                    lambda *= 10.0;
                    continue;
                }
                pc_mat3_apply(&inv, g, step);
                xn[0] = x[0] + step[0];
                xn[1] = x[1] + step[1];
                xn[2] = x[2] + step[2];
                project(xn, limit);
                err_at(&c, xn, en);
                fnew = norm2(en);
                if (fnew < f) {
                    double moved = 0.0;
                    for (d = 0; d < 3; d++) {
                        moved = fmax(moved, fabs(xn[d] - x[d]));
                    }
                    memcpy(x, xn, sizeof x);
                    memcpy(e, en, sizeof e);
                    f = fnew;
                    lambda = fmax(lambda * 0.3, 1e-9);
                    improved = moved > 1e-9;
                    break;
                }
                lambda *= 10.0;
            }
            if (!improved || f < 1e-10) {
                break;
            }
        }
        if (f < best_f) {
            memcpy(best_x, x, sizeof x);
            best_f = f;
        }
        if (best_f < 1e-6) {
            break;
        }
    }
    return sqrt(best_f);
}

static const double SEEDS[5][3] = {
    {0.3, 0.3, 0.3}, {0.05, 0.05, 0.05}, {0.8, 0.8, 0.8}, {0.9, 0.2, 0.2}, {0.2, 0.9, 0.9},
};

double pc_cmyk_separate(const pc_cmyk_model *m, const pc_cmyk_params *p, const double target[3], const double *warm,
                        double cmyk[4])
{
    double seeds[8][3];
    size_t n = 0;
    size_t i;
    double cmy0[3];
    double cmy[3];
    double g;
    double k;
    double de;
    double seeds2[9][3];
    size_t n2 = 0;
    if (warm != NULL) {
        memcpy(seeds[n++], warm, 3 * sizeof *warm);
    }
    for (i = 0; i < 5; i++) {
        memcpy(seeds[n++], SEEDS[i], sizeof SEEDS[i]);
    }
    (void)solve_cmy(m, target, 0.0, 3.0, (const double(*)[3])seeds, n, cmy0);
    g = fmin(fmin(cmy0[0], cmy0[1]), cmy0[2]);
    k = g <= p->gcr_start ? 0.0 : p->k_max * pow((g - p->gcr_start) / (1.0 - p->gcr_start), p->gcr_gamma);
    seeds2[n2][0] = fmax(cmy0[0] - k, 0.0);
    seeds2[n2][1] = fmax(cmy0[1] - k, 0.0);
    seeds2[n2][2] = fmax(cmy0[2] - k, 0.0);
    n2++;
    memcpy(seeds2[n2++], cmy0, sizeof cmy0);
    for (i = 0; i < n; i++) {
        memcpy(seeds2[n2++], seeds[i], sizeof seeds[i]);
    }
    de = solve_cmy(m, target, k, fmax(p->tac - k, 0.0), (const double(*)[3])seeds2, n2, cmy);
    cmyk[0] = cmy[0];
    cmyk[1] = cmy[1];
    cmyk[2] = cmy[2];
    cmyk[3] = k;
    return de;
}

/* ---- gamut boundary descriptor: maximum chroma per (L*, hue) bin ---- */

enum { L_BINS = 41, H_BINS = 72 };

typedef struct gbd {
    double cmax[L_BINS * H_BINS];
    double black_l;
} gbd;

static double gbd_get(const double *v, long l, long h)
{
    long lc = l < 0 ? 0 : (l > L_BINS - 1 ? L_BINS - 1 : l);
    long hc = ((h % H_BINS) + H_BINS) % H_BINS;
    return v[lc * H_BINS + hc];
}

static void gbd_new(gbd *g, const pc_cmyk_model *model, const pc_cmyk_params *p, double *scratch)
{
    const int n = 21;
    int ci;
    int mi;
    int yi;
    int ki;
    long l;
    long h;
    double *filled = scratch;
    memset(g->cmax, 0, sizeof g->cmax);
    g->black_l = 100.0;
    for (ci = 0; ci < n; ci++) {
        for (mi = 0; mi < n; mi++) {
            for (yi = 0; yi < n; yi++) {
                for (ki = 0; ki < 11; ki++) {
                    double v[4];
                    double lab[3];
                    double c;
                    double hue;
                    size_t li;
                    size_t hi;
                    double sum = -0.0;
                    int t;
                    v[0] = (double)ci / (double)(n - 1);
                    v[1] = (double)mi / (double)(n - 1);
                    v[2] = (double)yi / (double)(n - 1);
                    v[3] = (double)ki / 10.0;
                    for (t = 0; t < 4; t++) {
                        sum += v[t];
                    }
                    if (sum > p->tac + 1e-9) {
                        continue;
                    }
                    pc_cmyk_model_lab(model, v, lab);
                    g->black_l = fmin(g->black_l, lab[0]);
                    c = hypot(lab[1], lab[2]);
                    hue = rem_euclid(atan2(lab[2], lab[1]), TAU);
                    li = pc_f64_to_size(pc_clampd(round((lab[0] / 100.0) * (double)(L_BINS - 1)), 0.0, (double)(L_BINS - 1)));
                    hi = pc_f64_to_size(round((hue / TAU) * (double)H_BINS)) % H_BINS;
                    g->cmax[li * H_BINS + hi] = fmax(g->cmax[li * H_BINS + hi], c);
                }
            }
        }
    }
    /* Fill holes and smooth: max over a 3x3 neighbourhood, then a box blur. */
    for (l = 0; l < L_BINS; l++) {
        for (h = 0; h < H_BINS; h++) {
            double m = 0.0;
            long dl;
            long dh;
            for (dl = -1; dl <= 1; dl++) {
                for (dh = -1; dh <= 1; dh++) {
                    m = fmax(m, gbd_get(g->cmax, l + dl, h + dh));
                }
            }
            filled[l * H_BINS + h] = m;
        }
    }
    for (l = 0; l < L_BINS; l++) {
        for (h = 0; h < H_BINS; h++) {
            double s = 0.0;
            long dl;
            long dh;
            for (dl = -1; dl <= 1; dl++) {
                for (dh = -1; dh <= 1; dh++) {
                    s += gbd_get(filled, l + dl, h + dh);
                }
            }
            g->cmax[l * H_BINS + h] = s / 9.0;
        }
    }
}

static double max_chroma(const gbd *g, double l, double h)
{
    double lp = pc_clampd(l / 100.0, 0.0, 1.0) * (double)(L_BINS - 1);
    double hp = (rem_euclid(h, TAU) / TAU) * (double)H_BINS;
    size_t l0 = pc_f64_to_size(floor(lp));
    size_t h0 = pc_f64_to_size(floor(hp));
    double fl = lp - (double)l0;
    double fh = hp - (double)h0;
    size_t l1 = l0 + 1 < L_BINS - 1 ? l0 + 1 : L_BINS - 1;
    size_t h0m = h0 % H_BINS;
    size_t h1m = (h0 + 1) % H_BINS;
    const double *v = g->cmax;
    double a = v[l0 * H_BINS + h0m] + (v[l0 * H_BINS + h1m] - v[l0 * H_BINS + h0m]) * fh;
    double b = v[l1 * H_BINS + h0m] + (v[l1 * H_BINS + h1m] - v[l1 * H_BINS + h0m]) * fh;
    return a + (b - a) * fl;
}

/* Perceptual gamut compression of a relative Lab colour. */
static void compress(const gbd *g, const double lab[3], double out[3])
{
    double l = g->black_l + pc_clampd(lab[0], 0.0, 100.0) * (100.0 - g->black_l) / 100.0;
    double c = hypot(lab[1], lab[2]);
    double h;
    double cm;
    double knee;
    double c2;
    if (c < 1e-9) {
        out[0] = l;
        out[1] = 0.0;
        out[2] = 0.0;
        return;
    }
    h = atan2(lab[2], lab[1]);
    cm = fmax(max_chroma(g, l, h), 1e-3);
    knee = 0.8 * cm;
    c2 = c <= knee ? c : knee + (cm - knee) * tanh((c - knee) / (cm - knee));
    out[0] = l;
    out[1] = c2 * cos(h);
    out[2] = c2 * sin(h);
}

static void decode_v2(const double v[3], double out[3])
{
    out[0] = v[0] * 100.0 * 65535.0 / 65280.0;
    out[1] = v[1] * 65535.0 / 256.0 - 128.0;
    out[2] = v[2] * 65535.0 / 256.0 - 128.0;
}

static void encode_v2(const double lab[3], float out[3])
{
    out[0] = pc_f64_to_f32(pc_clampd(lab[0] / (100.0 * 65535.0 / 65280.0), 0.0, 1.0));
    out[1] = pc_f64_to_f32(pc_clampd((lab[1] + 128.0) * 256.0 / 65535.0, 0.0, 1.0));
    out[2] = pc_f64_to_f32(pc_clampd((lab[2] + 128.0) * 256.0 / 65535.0, 0.0, 1.0));
}

typedef struct b2a_job {
    const pc_cmyk_model *model;
    const pc_cmyk_params *p;
    const gbd *g; /* NULL: colorimetric */
    float *data;
    size_t n;
} b2a_job;

/* One (L, a) row along b, warm-starting each solve from its neighbour. */
static pc_status b2a_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const b2a_job *j = ctx;
    size_t row;
    (void)chunk;
    for (row = begin; row < end; row++) {
        size_t li = row / j->n;
        size_t ai = row % j->n;
        double warm[4];
        bool have_warm = false;
        size_t bi;
        for (bi = 0; bi < j->n; bi++) {
            double node[3];
            double lab[3];
            double target[3];
            double cmyk[4];
            int k;
            node[0] = (double)li / (double)(j->n - 1);
            node[1] = (double)ai / (double)(j->n - 1);
            node[2] = (double)bi / (double)(j->n - 1);
            decode_v2(node, lab);
            if (j->g != NULL) {
                compress(j->g, lab, target);
            } else {
                memcpy(target, lab, sizeof target);
            }
            (void)pc_cmyk_separate(j->model, j->p, target, have_warm ? warm : NULL, cmyk);
            memcpy(warm, cmyk, sizeof warm);
            have_warm = true;
            for (k = 0; k < 4; k++) {
                j->data[(row * j->n + bi) * 4 + (size_t)k] = pc_f64_to_f32(cmyk[k]);
            }
        }
    }
    return PC_OK;
}

static pc_status b2a_table(const pc_cmyk_model *model, const pc_cmyk_params *p, const gbd *g, pc_cms_clut *out,
                           pc_error *err)
{
    size_t n = p->grid_b2a;
    size_t grid[3];
    size_t len;
    b2a_job j;
    if (n < 2 || n > 255 || !pc_size_mul3(n, n, n, &len) || !pc_size_mul(len, 4, &len)) {
        return pc_fail(err, PC_ERR_INVALID, "bad BToA grid size");
    }
    j.model = model;
    j.p = p;
    j.g = g;
    j.n = n;
    j.data = pc_calloc_array(len, sizeof *j.data);
    if (j.data == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    (void)pc_pool_for(pc_pool_global(), n * n, 1, b2a_rows, &j);
    grid[0] = grid[1] = grid[2] = n;
    return pc_cms_clut_init(out, grid, 3, 4, j.data, len, err);
}

/* A lut16 tag around clut (two-point identity tables on both sides). Takes the CLUT. */
static pc_status lut16(pc_cms_clut *clut, pc_lut **out, pc_error *err)
{
    pc_lut *l = calloc(1, sizeof *l);
    pc_cms_curve *a = pc_calloc_array(clut->inputs, sizeof *a);
    pc_cms_curve *b = pc_calloc_array(clut->outputs, sizeof *b);
    pc_cms_stage sa;
    pc_cms_stage sc;
    pc_cms_stage sb;
    pc_status s = PC_OK;
    size_t k;
    if (l == NULL || a == NULL || b == NULL) {
        free(l);
        free(a);
        free(b);
        pc_cms_clut_fini(clut);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (k = 0; k < clut->inputs; k++) {
        a[k] = pc_cms_curve_identity();
        if (s == PC_OK) {
            s = pc_cms_curve_table01(&a[k], err);
        }
    }
    for (k = 0; k < clut->outputs; k++) {
        b[k] = pc_cms_curve_identity();
        if (s == PC_OK) {
            s = pc_cms_curve_table01(&b[k], err);
        }
    }
    l->kind = PC_LUT16;
    l->inputs = clut->inputs;
    l->outputs = clut->outputs;
    sa = pc_cms_stage_curves(PC_STAGE_CURVES, a, l->inputs);
    sc = pc_cms_stage_clut(clut);
    sb = pc_cms_stage_curves(PC_STAGE_CURVES, b, l->outputs);
    if (s == PC_OK) { s = pc_cms_stages_push(&l->stages, &sa, err); } else { pc_cms_stage_fini(&sa); }
    if (s == PC_OK) { s = pc_cms_stages_push(&l->stages, &sc, err); } else { pc_cms_stage_fini(&sc); }
    if (s == PC_OK) { s = pc_cms_stages_push(&l->stages, &sb, err); } else { pc_cms_stage_fini(&sb); }
    if (s != PC_OK) {
        pc_cms_stages_fini(&l->stages);
        free(l);
        return s;
    }
    *out = l;
    return PC_OK;
}

static pc_status clone_lut(const pc_lut *l, pc_lut **out, pc_error *err)
{
    pc_lut *c = calloc(1, sizeof *c);
    pc_status s;
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *c = *l;
    s = pc_cms_stages_clone(&l->stages, &c->stages, err);
    if (s != PC_OK) {
        free(c);
        return s;
    }
    *out = c;
    return PC_OK;
}

static void a2b_node(void *ctx, const float *in, float *out)
{
    const pc_cmyk_model *m = ctx;
    double cmyk[4];
    double lab[3];
    cmyk[0] = (double)in[0];
    cmyk[1] = (double)in[1];
    cmyk[2] = (double)in[2];
    cmyk[3] = (double)in[3];
    pc_cmyk_model_lab(m, cmyk, lab);
    encode_v2(lab, out);
}

pc_status pc_synth_cmyk_profile(const pc_cmyk_params *p, pc_profile *out, pc_error *err)
{
    pc_cmyk_model model;
    pc_cms_clut a2b;
    pc_cms_clut perc;
    pc_cms_clut colo;
    pc_lut *la = NULL;
    pc_lut *lp = NULL;
    pc_lut *lc = NULL;
    gbd *g = NULL;
    double *scratch = NULL;
    pc_profile prof;
    size_t grid[4];
    pc_status s;
    int i;
    if (p->grid_a2b < 2 || p->grid_a2b > 255) {
        return pc_fail(err, PC_ERR_INVALID, "bad AToB grid size");
    }
    memset(&a2b, 0, sizeof a2b);
    memset(&perc, 0, sizeof perc);
    memset(&colo, 0, sizeof colo);
    pc_cmyk_model_coated(p->tvi, &model);
    grid[0] = grid[1] = grid[2] = grid[3] = p->grid_a2b;
    s = pc_cms_clut_sample(grid, 4, 3, a2b_node, &model, &a2b, err);
    if (s == PC_OK) {
        g = malloc(sizeof *g);
        scratch = pc_malloc_array(L_BINS * H_BINS, sizeof *scratch);
        if (g == NULL || scratch == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        } else {
            gbd_new(g, &model, p, scratch);
        }
    }
    if (s == PC_OK) { s = b2a_table(&model, p, NULL, &colo, err); }
    if (s == PC_OK) { s = b2a_table(&model, p, g, &perc, err); }
    free(g);
    free(scratch);
    if (s == PC_OK) { s = lut16(&a2b, &la, err); } else { pc_cms_clut_fini(&a2b); }
    if (s == PC_OK) { s = lut16(&perc, &lp, err); } else { pc_cms_clut_fini(&perc); }
    if (s == PC_OK) { s = lut16(&colo, &lc, err); } else { pc_cms_clut_fini(&colo); }
    if (s == PC_OK) { s = pc_profile_init_empty(&prof, err); }
    if (s != PC_OK) {
        if (la != NULL) { pc_cms_stages_fini(&la->stages); free(la); }
        if (lp != NULL) { pc_cms_stages_fini(&lp->stages); free(lp); }
        if (lc != NULL) { pc_cms_stages_fini(&lc->stages); free(lc); }
        return s;
    }
    prof.version_major = 4;
    prof.version_minor = 0x30;
    prof.class_sig = PC_CLASS_OUTPUT;
    prof.space = PC_CS_CMYK;
    prof.pcs = PC_PCS_LAB;
    prof.rendering_intent = PC_INTENT_PERCEPTUAL;
    for (i = 0; i < 3; i++) {
        prof.white_point[i] = pc_s15f16_round(model.paper[i]);
    }
    {
        const char *desc = p->description != NULL ? p->description : PC_SYNTH_DESCRIPTION;
        char *d = pc_strndup(desc, strlen(desc));
        char *c = pc_strndup(PC_BUILTIN_COPYRIGHT, strlen(PC_BUILTIN_COPYRIGHT));
        if (d == NULL || c == NULL) {
            free(d);
            free(c);
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        } else {
            free(prof.description);
            free(prof.copyright);
            prof.description = d;
            prof.description_len = strlen(d);
            prof.copyright = c;
            prof.copyright_len = strlen(c);
        }
    }
    /* a2b = [A, A, A]; b2a = [perceptual, colorimetric, perceptual]. */
    prof.a2b[0] = la;
    prof.b2a[0] = lp;
    prof.b2a[1] = lc;
    if (s == PC_OK) { s = clone_lut(la, &prof.a2b[1], err); }
    if (s == PC_OK) { s = clone_lut(la, &prof.a2b[2], err); }
    if (s == PC_OK) { s = clone_lut(lp, &prof.b2a[2], err); }
    if (s == PC_OK) { s = pc_profile_encode_bytes(&prof, err); }
    if (s != PC_OK) {
        pc_profile_fini(&prof);
        return s;
    }
    *out = prof;
    return PC_OK;
}

pc_status pc_synth_coated_cmyk(pc_profile *out, pc_error *err)
{
    pc_cmyk_params p = pc_cmyk_params_default();
    return pc_synth_cmyk_profile(&p, out, err);
}
