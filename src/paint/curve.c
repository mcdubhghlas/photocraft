/* A response curve: a monotone cubic (Fritsch-Carlson) through 0..1 control points (brush.h, port
 * of crates/paint/src/curve.rs). Points are sanitised; the cubic never overshoots, so a stronger
 * input never gives less output. Float expressions keep the reference's types and operation order;
 * `a.min(b)` / `a.max(b)` use the pc_rs_* helpers (sign-of-zero parity). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "paint/brush.h"

#define MAXP PC_PAINT_CURVE_MAX_POINTS

pc_paint_response_curve pc_paint_response_curve_linear(void)
{
    pc_paint_response_curve c;
    memset(&c, 0, sizeof c);
    c.n = 2;
    c.xs[0] = 0.0f;
    c.xs[1] = 1.0f;
    c.ys[0] = 0.0f;
    c.ys[1] = 1.0f;
    c.ms[0] = 1.0f;
    c.ms[1] = 1.0f;
    return c;
}

/* Rust f32::total_cmp: a total ordering over the bit patterns (-0 < +0, NaNs ordered). */
static int total_cmp_f32(float a, float b)
{
    int32_t ia, ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    ia ^= (int32_t)(((uint32_t)(ia >> 31)) >> 1);
    ib ^= (int32_t)(((uint32_t)(ib >> 31)) >> 1);
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

/* Stable bottom-up merge sort of the (xs[i], ys[i]) pairs by total_cmp on xs. */
static void sort_pairs(float *xs, float *ys, float *tx, float *ty, size_t n)
{
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n;
            size_t hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (total_cmp_f32(xs[j], xs[i]) < 0) { /* strict: equal keeps left (stable) */
                    tx[k] = xs[j];
                    ty[k] = ys[j];
                    j++;
                } else {
                    tx[k] = xs[i];
                    ty[k] = ys[i];
                    i++;
                }
                k++;
            }
            while (i < mid) {
                tx[k] = xs[i];
                ty[k] = ys[i];
                i++;
                k++;
            }
            while (j < hi) {
                tx[k] = xs[j];
                ty[k] = ys[j];
                j++;
                k++;
            }
        }
        memcpy(xs + 0, tx, n * sizeof *xs);
        memcpy(ys + 0, ty, n * sizeof *ys);
    }
}

pc_paint_response_curve pc_paint_response_curve_new(const float (*points)[2], size_t n)
{
    float *wx = NULL, *wy = NULL, *tx = NULL, *ty = NULL;
    float dpx[MAXP], dpy[MAXP]; /* deduped, truncated set */
    float d[MAXP];
    size_t mf = 0, cnt = 0;
    pc_paint_response_curve c;
    float prev;

    if (n == 0) {
        return pc_paint_response_curve_linear();
    }
    wx = pc_malloc_array(n, sizeof *wx);
    wy = pc_malloc_array(n, sizeof *wy);
    tx = pc_malloc_array(n, sizeof *tx);
    ty = pc_malloc_array(n, sizeof *ty);
    if (wx == NULL || wy == NULL || tx == NULL || ty == NULL) {
        free(wx);
        free(wy);
        free(tx);
        free(ty);
        return pc_paint_response_curve_linear(); /* hostile/OOM: a safe identity (never crash) */
    }

    /* Collect finite points, clamped to 0..1. */
    for (size_t i = 0; i < n; i++) {
        float x = points[i][0], y = points[i][1];
        if (!isfinite(x) || !isfinite(y)) {
            continue;
        }
        wx[mf] = pc_clampf(x, 0.0f, 1.0f);
        wy[mf] = pc_clampf(y, 0.0f, 1.0f);
        mf++;
    }
    sort_pairs(wx, wy, tx, ty, mf);

    /* Dedup equal inputs (the later point's output wins), then truncate to MAXP groups. Points
     * sharing a group are contiguous (sorted), so a group is final once a new input appears. */
    for (size_t i = 0; i < mf; i++) {
        if (cnt > 0 && fabsf(dpx[cnt - 1] - wx[i]) < 1e-6f) {
            dpy[cnt - 1] = wy[i];
        } else {
            if (cnt == MAXP) {
                break;
            }
            dpx[cnt] = wx[i];
            dpy[cnt] = wy[i];
            cnt++;
        }
    }
    free(wx);
    free(wy);
    free(tx);
    free(ty);

    if (cnt < 2) {
        return pc_paint_response_curve_linear();
    }

    memset(&c, 0, sizeof c);
    c.n = (uint8_t)cnt;
    /* Outputs never fall: the curve is monotone. */
    prev = 0.0f;
    for (size_t k = 0; k < cnt; k++) {
        c.xs[k] = dpx[k];
        prev = (k == 0) ? dpy[k] : pc_rs_maxf(dpy[k], prev);
        c.ys[k] = prev;
    }

    /* Fritsch-Carlson tangents: secant averages, zero at flat steps, limited so the cubic never
     * overshoots (which keeps it monotone). */
    for (size_t k = 0; k < cnt - 1; k++) {
        d[k] = (c.ys[k + 1] - c.ys[k]) / (c.xs[k + 1] - c.xs[k]);
    }
    c.ms[0] = d[0];
    c.ms[cnt - 1] = d[cnt - 2];
    for (size_t k = 1; k + 1 < cnt; k++) {
        c.ms[k] = (d[k - 1] * d[k] <= 0.0f) ? 0.0f : (d[k - 1] + d[k]) / 2.0f;
    }
    for (size_t k = 0; k < cnt - 1; k++) {
        float a, b, s;
        if (d[k] == 0.0f) {
            c.ms[k] = 0.0f;
            c.ms[k + 1] = 0.0f;
            continue;
        }
        a = c.ms[k] / d[k];
        b = c.ms[k + 1] / d[k];
        s = a * a + b * b;
        if (s > 9.0f) {
            float t = 3.0f / sqrtf(s);
            c.ms[k] = t * a * d[k];
            c.ms[k + 1] = t * b * d[k];
        }
    }
    return c;
}

bool pc_paint_response_curve_is_linear(const pc_paint_response_curve *c)
{
    return c->n == 2 && c->xs[0] == 0.0f && c->xs[1] == 1.0f && c->ys[0] == 0.0f && c->ys[1] == 1.0f;
}

float pc_paint_response_curve_eval(const pc_paint_response_curve *c, float x)
{
    size_t n, k, cnt;
    float x0, xn, xa, xb, ya, yb, ma, mb, h, t, t2, t3, y;
    x = isnan(x) ? 0.0f : pc_clampf(x, 0.0f, 1.0f);
    if (pc_paint_response_curve_is_linear(c)) {
        return x;
    }
    n = c->n < MAXP ? c->n : MAXP;
    if (n == 0) {
        return x;
    }
    x0 = c->xs[0];
    xn = c->xs[n - 1];
    if (x <= x0) {
        return c->ys[0];
    }
    if (x >= xn) {
        return c->ys[n - 1];
    }
    /* partition_point(|v| v <= x): the count of inputs <= x. */
    cnt = 0;
    for (size_t i = 0; i < n; i++) {
        if (c->xs[i] <= x) {
            cnt++;
        }
    }
    k = cnt == 0 ? 0 : cnt - 1;
    if (k > n - 2) {
        k = n - 2;
    }
    xa = c->xs[k];
    xb = c->xs[k + 1];
    ya = c->ys[k];
    yb = c->ys[k + 1];
    ma = c->ms[k];
    mb = c->ms[k + 1];
    h = xb - xa;
    t = (x - xa) / h;
    t2 = t * t;
    t3 = t * t * t;
    y = (2.0f * t3 - 3.0f * t2 + 1.0f) * ya + (t3 - 2.0f * t2 + t) * h * ma +
        (-2.0f * t3 + 3.0f * t2) * yb + (t3 - t2) * h * mb;
    return pc_clampf(y, pc_rs_minf(ya, yb), pc_rs_maxf(ya, yb));
}

bool pc_paint_response_curve_eq(const pc_paint_response_curve *a, const pc_paint_response_curve *b)
{
    if (a->n != b->n) {
        return false;
    }
    for (size_t k = 0; k < MAXP; k++) {
        if (a->xs[k] != b->xs[k] || a->ys[k] != b->ys[k] || a->ms[k] != b->ms[k]) {
            return false;
        }
    }
    return true;
}
