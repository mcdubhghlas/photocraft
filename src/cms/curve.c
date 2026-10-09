#include "curve.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"

pc_cms_curve pc_cms_curve_identity(void)
{
    pc_cms_curve c;
    memset(&c, 0, sizeof c);
    c.kind = PC_CURVE_IDENTITY;
    return c;
}

pc_cms_curve pc_cms_curve_gamma(double g)
{
    pc_cms_curve c = pc_cms_curve_identity();
    c.kind = PC_CURVE_GAMMA;
    c.gamma = g;
    return c;
}

pc_cms_curve pc_cms_curve_parametric(uint16_t ptype, const double p[7])
{
    pc_cms_curve c = pc_cms_curve_identity();
    c.kind = PC_CURVE_PARAMETRIC;
    c.ptype = ptype;
    memcpy(c.p, p, sizeof c.p);
    return c;
}

pc_cms_curve pc_cms_curve_table(float *table, size_t n)
{
    pc_cms_curve c = pc_cms_curve_identity();
    c.kind = PC_CURVE_TABLE;
    c.table = table;
    c.n = n;
    return c;
}

pc_status pc_cms_curve_table01(pc_cms_curve *out, pc_error *err)
{
    float *t = pc_malloc_array(2, sizeof *t);
    if (t == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    t[0] = 0.0f;
    t[1] = 1.0f;
    *out = pc_cms_curve_table(t, 2);
    return PC_OK;
}

pc_cms_curve pc_cms_srgb_trc(void)
{
    const double p[7] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045, 0.0, 0.0};
    return pc_cms_curve_parametric(3, p);
}

void pc_cms_curve_fini(pc_cms_curve *c)
{
    if (c != NULL) {
        free(c->table);
        c->table = NULL;
        c->n = 0;
    }
}

pc_status pc_cms_curve_clone(const pc_cms_curve *c, pc_cms_curve *out, pc_error *err)
{
    pc_cms_curve o = *c;
    if (c->kind == PC_CURVE_TABLE) {
        o.table = pc_malloc_array(c->n, sizeof *o.table);
        if (o.table == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a curve");
        }
        if (c->n > 0) {
            memcpy(o.table, c->table, c->n * sizeof *o.table);
        }
    } else {
        o.table = NULL;
        o.n = 0;
    }
    *out = o;
    return PC_OK;
}

bool pc_cms_curve_eq(const pc_cms_curve *a, const pc_cms_curve *b)
{
    size_t i;
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_CURVE_IDENTITY: return true;
    case PC_CURVE_GAMMA: return a->gamma == b->gamma;
    case PC_CURVE_PARAMETRIC:
        if (a->ptype != b->ptype) {
            return false;
        }
        for (i = 0; i < 7; i++) {
            if (a->p[i] != b->p[i]) {
                return false;
            }
        }
        return true;
    case PC_CURVE_TABLE:
        if (a->n != b->n) {
            return false;
        }
        for (i = 0; i < a->n; i++) {
            if (a->table[i] != b->table[i]) {
                return false;
            }
        }
        return true;
    }
    return false;
}

size_t pc_cms_curve_param_count(uint16_t ptype)
{
    switch (ptype) {
    case 0: return 1;
    case 1: return 3;
    case 2: return 4;
    case 3: return 5;
    default: return 7;
    }
}

bool pc_cms_curve_is_identity(const pc_cms_curve *c)
{
    size_t i;
    switch (c->kind) {
    case PC_CURVE_IDENTITY: return true;
    case PC_CURVE_GAMMA: return fabs(c->gamma - 1.0) < 1e-9;
    case PC_CURVE_PARAMETRIC: return c->ptype == 0 && fabs(c->p[0] - 1.0) < 1e-9;
    case PC_CURVE_TABLE:
        if (c->n < 2) {
            return true;
        }
        for (i = 0; i < c->n; i++) {
            if (!(fabs((double)c->table[i] - (double)i / (double)(c->n - 1)) < 1.0 / 65535.0)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static double powg(double v, double g) { return pow(v, g); }

/* mirror(x, |v| v^g) */
static double mirror_pow(double x, double g)
{
    return x < 0.0 ? -powg(-x, g) : powg(x, g);
}

static double pw(double base, double g)
{
    return base <= 0.0 ? 0.0 : pow(base, g);
}

static double para(uint16_t kind, const double *p, double x)
{
    double g = p[0];
    double a = p[1];
    double b = p[2];
    double c = p[3];
    double d = p[4];
    double e = p[5];
    double f = p[6];
    switch (kind) {
    case 0: return mirror_pow(x, g);
    case 1: return (a != 0.0 && x >= -b / a) ? pw(a * x + b, g) : 0.0;
    case 2: return (a != 0.0 && x >= -b / a) ? pw(a * x + b, g) + c : c;
    case 3:
        if (x < 0.0 && b == 0.0 && d == 0.0) {
            return -para(kind, p, -x); /* pure gamma written as type 3: mirror negatives */
        }
        return x >= d ? pw(a * x + b, g) : c * x;
    default: return x >= d ? pw(a * x + b, g) + e : c * x + f;
    }
}

static double root(double v, double g)
{
    return v <= 0.0 ? 0.0 : pow(v, 1.0 / g);
}

/* Closed-form inverse; false when there is none (the caller then bisects). */
static bool para_inverse(uint16_t kind, const double *p, double y, double *out)
{
    double g = p[0];
    double a = p[1];
    double b = p[2];
    double c = p[3];
    double d = p[4];
    double e = p[5];
    double f = p[6];
    double yd;
    if (fabs(g) < 1e-12) {
        return false;
    }
    switch (kind) {
    case 0: *out = mirror_pow(y, 1.0 / g); return true;
    case 1:
        if (a == 0.0) { return false; }
        *out = y <= 0.0 ? -b / a : (root(y, g) - b) / a;
        return true;
    case 2:
        if (a == 0.0) { return false; }
        *out = y <= c ? -b / a : (root(y - c, g) - b) / a;
        return true;
    case 3:
        if (a == 0.0) { return false; }
        if (y < 0.0 && b == 0.0 && d == 0.0) {
            double v;
            if (!para_inverse(kind, p, -y, &v)) {
                return false;
            }
            *out = -v;
            return true;
        }
        yd = pw(a * d + b, g);
        *out = y >= yd ? (root(y, g) - b) / a : (c != 0.0 ? y / c : 0.0);
        return true;
    default:
        if (a == 0.0) { return false; }
        yd = pw(a * d + b, g) + e;
        *out = y >= yd ? (root(y - e, g) - b) / a : (c != 0.0 ? (y - f) / c : 0.0);
        return true;
    }
}

/* Bisection inverse of a monotonic function on [0, 1]. */
static double numeric_inverse(uint16_t kind, const double *p, double y)
{
    double f0 = para(kind, p, 0.0);
    double f1 = para(kind, p, 1.0);
    bool increasing = f1 >= f0;
    double lo = 0.0;
    double hi = 1.0;
    int i;
    for (i = 0; i < 60; i++) {
        double mid = 0.5 * (lo + hi);
        double v = para(kind, p, mid);
        if ((v < y) == increasing) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5 * (lo + hi);
}

static float table_eval(const float *t, size_t n, float x)
{
    float pos;
    size_t i;
    if (n == 0) {
        return x;
    }
    if (n == 1) {
        return t[0];
    }
    pos = pc_clampf(x, 0.0f, 1.0f) * (float)(n - 1);
    i = pc_f32_to_size(pos);
    if (i > n - 2) {
        i = n - 2;
    }
    return t[i] + (t[i + 1] - t[i]) * (pos - (float)i);
}

/* For a flat run at the table start (start = true) the last index of the run, or for a flat
 * run at the end the first index of it, as a normalised input. */
static float edge_index(const float *t, size_t n, bool start)
{
    size_t i;
    if (start) {
        float v = t[0];
        size_t idx = n - 1;
        for (i = 0; i < n; i++) {
            if (fabsf(t[i] - v) > 1e-7f) {
                idx = i - 1; /* i >= 1: t[0] differs from itself by 0 */
                break;
            }
        }
        return (float)idx / (float)(n - 1);
    } else {
        float v = t[n - 1];
        size_t idx = 0;
        for (i = n; i-- > 0;) {
            if (fabsf(t[i] - v) > 1e-7f) {
                idx = i + 1;
                break;
            }
        }
        return (float)idx / (float)(n - 1);
    }
}

/* Exact inverse of a piecewise-linear table (binary search), increasing or decreasing. */
static float table_inverse(const float *t, size_t n, float y)
{
    bool increasing;
    float lo_v;
    float hi_v;
    size_t lo = 0;
    size_t hi;
    float a;
    float b;
    float f;
    if (n < 2) {
        return y;
    }
    increasing = t[n - 1] >= t[0];
    lo_v = increasing ? t[0] : t[n - 1];
    hi_v = increasing ? t[n - 1] : t[0];
    if (y <= lo_v) {
        return edge_index(t, n, increasing);
    }
    if (y >= hi_v) {
        return edge_index(t, n, !increasing);
    }
    hi = n - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if ((t[mid] < y) == increasing) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    a = t[lo];
    b = t[hi];
    f = fabsf(b - a) > 1e-12f ? (y - a) / (b - a) : 0.5f;
    return ((float)lo + f) / (float)(n - 1);
}

double pc_cms_curve_eval64(const pc_cms_curve *c, double x)
{
    switch (c->kind) {
    case PC_CURVE_IDENTITY: return x;
    case PC_CURVE_GAMMA: return mirror_pow(x, c->gamma);
    case PC_CURVE_PARAMETRIC: return para(c->ptype, c->p, x);
    case PC_CURVE_TABLE: return (double)table_eval(c->table, c->n, pc_f64_to_f32(x));
    }
    return x;
}

float pc_cms_curve_eval(const pc_cms_curve *c, float x)
{
    switch (c->kind) {
    case PC_CURVE_IDENTITY: return x;
    case PC_CURVE_TABLE: return table_eval(c->table, c->n, x);
    case PC_CURVE_GAMMA:
    case PC_CURVE_PARAMETRIC: break;
    }
    return pc_f64_to_f32(pc_cms_curve_eval64(c, (double)x));
}

double pc_cms_curve_eval_inverse64(const pc_cms_curve *c, double y)
{
    double v;
    switch (c->kind) {
    case PC_CURVE_IDENTITY: return y;
    case PC_CURVE_GAMMA: return fabs(c->gamma) < 1e-12 ? y : mirror_pow(y, 1.0 / c->gamma);
    case PC_CURVE_PARAMETRIC:
        return para_inverse(c->ptype, c->p, y, &v) ? v : numeric_inverse(c->ptype, c->p, y);
    case PC_CURVE_TABLE: return (double)table_inverse(c->table, c->n, pc_f64_to_f32(y));
    }
    return y;
}

float pc_cms_curve_eval_inverse(const pc_cms_curve *c, float y)
{
    switch (c->kind) {
    case PC_CURVE_IDENTITY: return y;
    case PC_CURVE_TABLE: return table_inverse(c->table, c->n, y);
    case PC_CURVE_GAMMA:
    case PC_CURVE_PARAMETRIC: break;
    }
    return pc_f64_to_f32(pc_cms_curve_eval_inverse64(c, (double)y));
}

void pc_cms_curve_sample(const pc_cms_curve *c, size_t n, float *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        double x = n > 1 ? (double)i / (double)(n - 1) : 0.0;
        out[i] = pc_f64_to_f32(pc_cms_curve_eval64(c, x));
    }
}

bool pc_cms_curve_is_monotonic(const pc_cms_curve *c)
{
    float s[1024];
    size_t i;
    bool up = true;
    bool down = true;
    pc_cms_curve_sample(c, 1024, s);
    for (i = 1; i < 1024; i++) {
        up = up && s[i] >= s[i - 1] - 1e-6f;
        down = down && s[i] <= s[i - 1] + 1e-6f;
    }
    return up || down;
}
