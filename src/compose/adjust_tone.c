/* Tone curves of adjustment layers (port of the Levels / Curves half of
 * crates/compose/src/adjust.rs): levels at every depth, natural-spline curves, the per-channel
 * LUTs and their application in CMYK and Lab. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "color/convert.h"
#include "compose/adjust_internal.h"
#include "compose/internal.h"
#include "compose/stable_sort.h"

#define LUT PC_COMPOSE_LUT_SIZE

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

static float lut_x(size_t k)
{
    return (float)k / (float)(LUT - 1);
}

float pc_cmp_lut(const float *table, size_t len, float v)
{
    float x = pc_clampf(v, 0.0f, 1.0f) * (float)(len - 1);
    size_t i = pc_f32_to_size(floorf(x));
    size_t j;
    float f;
    if (i > len - 1) {
        i = len - 1; /* unreachable for clamped x; keeps the index in bounds */
    }
    j = i + 1 < len - 1 ? i + 1 : len - 1;
    f = x - (float)i;
    return table[i] * (1.0f - f) + table[j] * f;
}

float pc_compose_levels_q(const pc_levels_channel *ch, float v, bool has_q, float q)
{
    /* Photoshop works on whole levels: the input is a level, and the input-range stretch is
     * rounded (half up) to a whole level before the midtone gamma (psd-tools levels_grayscale). */
    float range = pc_rs_maxf_k(ch->in_white - ch->in_black, 1e-6f);
    float u;
    float g;
    float t;
    if (has_q && isfinite(q) && q >= 1.0f) {
        float vq = roundf(v * q) / q;
        u = floorf(pc_clampf((vq - ch->in_black) / range, 0.0f, 1.0f) * q + 0.5f + 1e-3f) / q;
    } else {
        u = pc_clampf((v - ch->in_black) / range, 0.0f, 1.0f);
    }
    g = pc_rs_maxf_k(ch->gamma, 0.01f);
    if (g > 1.0f) {
        /* A midtone gamma > 1 lifts shadows; Photoshop bounds the infinite slope of u^(1/g) at
         * black with a soft shadow toe (initial slope about 2^gamma): a p-norm soft-min of the
         * power curve with that line (sharpness 10, fitted). */
        float power = powf(u, 1.0f / g);
        float line = 0.93f * exp2f(g) * u;
        if (power <= 1e-6f || line <= 1e-6f) {
            t = pc_rs_minf(power, line);
        } else {
            const float p = 10.0f;
            t = powf(powf(power, -p) + powf(line, -p), -1.0f / p);
        }
    } else {
        t = powf(u, 1.0f / g);
    }
    return ch->out_black + t * (ch->out_white - ch->out_black);
}

float pc_compose_levels(const pc_levels_channel *ch, float v)
{
    return pc_compose_levels_q(ch, v, false, 0.0f);
}

/* f32::signum: NaN stays NaN, the sign of zero counts. */
static float signum(float x)
{
    return isnan(x) ? x : copysignf(1.0f, x);
}

float pc_compose_levels_float(const pc_levels_channel *ch, float v)
{
    float t = (v - ch->in_black) / pc_rs_maxf_k(ch->in_white - ch->in_black, 1e-6f);
    if (ch->gamma != 1.0f) {
        t = signum(t) * powf(fabsf(t), 1.0f / pc_rs_maxf_k(ch->gamma, 0.01f));
    }
    return ch->out_black + t * (ch->out_white - ch->out_black);
}

typedef struct pt {
    float x;
    float y;
} pt;

/* partial_cmp(..).unwrap_or(Equal) == Less. */
static bool pt_less(const void *a, const void *b)
{
    return ((const pt *)a)->x < ((const pt *)b)->x;
}

pc_status pc_compose_curve_lut(const pc_curve_point *points, size_t npoints, float *out, pc_error *err)
{
    pt *pts = NULL;
    double *m2 = NULL;
    size_t n = 0;
    size_t i;
    size_t k;
    size_t seg = 0;
    bool monotone = true;
    pc_status st;
    if (npoints > 0) {
        pts = pc_malloc_array(npoints, sizeof *pts);
        if (pts == NULL) {
            return nomem(err);
        }
        for (i = 0; i < npoints; i++) {
            pts[i].x = points[i].input;
            pts[i].y = points[i].output;
        }
        st = pc_cmp_stable_sort(pts, npoints, sizeof *pts, pt_less, err);
        if (st != PC_OK) {
            free(pts);
            return st;
        }
        /* dedup_by: drop a point within 1e-6 of the last one kept. */
        n = 1;
        for (i = 1; i < npoints; i++) {
            if (!(fabsf(pts[i].x - pts[n - 1].x) < 1e-6f)) {
                pts[n++] = pts[i];
            }
        }
    }
    if (n < 2) {
        for (k = 0; k < LUT; k++) {
            out[k] = lut_x(k);
        }
        free(pts);
        return PC_OK;
    }
    /* Second derivatives of the natural spline (tridiagonal solve). */
    m2 = pc_calloc_array(n, sizeof *m2);
    if (m2 == NULL) {
        free(pts);
        return nomem(err);
    }
    if (n > 2) {
        double *c = pc_calloc_array(n, sizeof *c);
        double *d = pc_calloc_array(n, sizeof *d);
        if (c == NULL || d == NULL) {
            free(c);
            free(d);
            free(m2);
            free(pts);
            return nomem(err);
        }
        for (i = 1; i + 1 < n; i++) {
            double x0 = (double)pts[i - 1].x;
            double x1 = (double)pts[i].x;
            double x2 = (double)pts[i + 1].x;
            double y0 = (double)pts[i - 1].y;
            double y1 = (double)pts[i].y;
            double y2 = (double)pts[i + 1].y;
            double h0 = x1 - x0;
            double h1 = x2 - x1;
            double a = h0 / 6.0;
            double b = (h0 + h1) / 3.0;
            double cc = h1 / 6.0;
            double r = (y2 - y1) / h1 - (y1 - y0) / h0;
            double denom = b - a * c[i - 1];
            c[i] = cc / denom;
            d[i] = (r - a * d[i - 1]) / denom;
        }
        for (i = n - 2; i >= 1; i--) {
            m2[i] = d[i] - c[i] * m2[i + 1];
        }
        free(c);
        free(d);
    }
    /* The segment search below is the reference's linear scan; with sorted, NaN-free points it is
     * monotone in x, so a moving start gives the same answer in linear time. */
    for (i = 0; i < n; i++) {
        monotone = monotone && !isnan(pts[i].x) && (i == 0 || pts[i - 1].x <= pts[i].x);
    }
    for (k = 0; k < LUT; k++) {
        float xv = lut_x(k);
        double x0;
        double y0;
        double x1;
        double y1;
        double h;
        double t;
        double a;
        double b;
        double y;
        if (xv <= pts[0].x) {
            out[k] = pc_clampf(pts[0].y, 0.0f, 1.0f);
            continue;
        }
        if (xv >= pts[n - 1].x) {
            out[k] = pc_clampf(pts[n - 1].y, 0.0f, 1.0f);
            continue;
        }
        i = monotone ? seg : 0;
        while (i + 1 < n && !(xv <= pts[i + 1].x)) {
            i++;
        }
        if (i + 1 >= n) {
            i = n - 2; /* unwrap_or(n - 2) */
        }
        seg = i;
        x0 = (double)pts[i].x;
        y0 = (double)pts[i].y;
        x1 = (double)pts[i + 1].x;
        y1 = (double)pts[i + 1].y;
        h = x1 - x0;
        t = (double)xv;
        a = (x1 - t) / h;
        b = (t - x0) / h;
        y = a * y0 + b * y1 + ((a * a * a - a) * m2[i] + (b * b * b - b) * m2[i + 1]) * h * h / 6.0;
        out[k] = pc_clampf((float)y, 0.0f, 1.0f);
    }
    free(m2);
    free(pts);
    return PC_OK;
}

/* f32::total_cmp(a, b) == Less for NaN-free values (-0.0 sorts before +0.0). */
static bool total_less(float a, float b)
{
    if (a < b) {
        return true;
    }
    return a == 0.0f && b == 0.0f && signbit(a) && !signbit(b);
}

static bool pt_total_less(const void *a, const void *b)
{
    return total_less(((const pt *)a)->x, ((const pt *)b)->x);
}

pc_status pc_compose_curve_map_lut(const pc_curve_point *points, size_t npoints, float *out, pc_error *err)
{
    pt *pts = NULL;
    size_t n = 0;
    size_t i;
    size_t k;
    size_t seg = 0;
    pc_status st;
    if (npoints > 0) {
        pts = pc_malloc_array(npoints, sizeof *pts);
        if (pts == NULL) {
            return nomem(err);
        }
        for (i = 0; i < npoints; i++) {
            if (isfinite(points[i].input) && isfinite(points[i].output)) {
                pts[n].x = points[i].input;
                pts[n].y = points[i].output;
                n++;
            }
        }
    }
    if (n > 0) {
        size_t kept = 1;
        st = pc_cmp_stable_sort(pts, n, sizeof *pts, pt_total_less, err);
        if (st != PC_OK) {
            free(pts);
            return st;
        }
        for (i = 1; i < n; i++) {
            if (!(fabsf(pts[i].x - pts[kept - 1].x) < 1e-6f)) {
                pts[kept++] = pts[i];
            }
        }
        n = kept;
    }
    if (n == 0) {
        for (k = 0; k < LUT; k++) {
            out[k] = lut_x(k);
        }
        free(pts);
        return PC_OK;
    }
    for (k = 0; k < LUT; k++) {
        float xv = lut_x(k);
        float t;
        if (xv <= pts[0].x) {
            out[k] = pc_clampf(pts[0].y, 0.0f, 1.0f);
            continue;
        }
        if (xv >= pts[n - 1].x) {
            out[k] = pc_clampf(pts[n - 1].y, 0.0f, 1.0f);
            continue;
        }
        while (seg + 1 < n && pts[seg + 1].x < xv) {
            seg++;
        }
        if (seg + 1 >= n) {
            out[k] = pc_clampf(pts[n - 1].y, 0.0f, 1.0f);
            continue;
        }
        t = pts[seg + 1].x > pts[seg].x ? (xv - pts[seg].x) / (pts[seg + 1].x - pts[seg].x) : 0.0f;
        out[k] = pc_clampf(pts[seg].y + (pts[seg + 1].y - pts[seg].y) * t, 0.0f, 1.0f);
    }
    free(pts);
    return PC_OK;
}

pc_status pc_compose_curve_lut_mode(const pc_curve_point *points, size_t npoints, bool pencil, float *out,
                                    pc_error *err)
{
    return pencil ? pc_compose_curve_map_lut(points, npoints, out, err)
                  : pc_compose_curve_lut(points, npoints, out, err);
}

typedef struct cpt {
    pc_curve_point p;
    bool corner;
} cpt;

/* partial_cmp(..).unwrap_or(Equal) == Less on the inputs. */
static bool cpt_less(const void *a, const void *b)
{
    return ((const cpt *)a)->p.input < ((const cpt *)b)->p.input;
}

pc_status pc_compose_contour_curve_lut(const pc_curve_point *points, size_t npoints, const bool *corners,
                                       size_t ncorners, float *out, pc_error *err)
{
    cpt *pts;
    pc_curve_point *seg = NULL;
    float *lut = NULL;
    bool any = false;
    bool have = false;
    size_t n;
    size_t i;
    size_t k;
    size_t start = 0;
    size_t last;
    pc_status st = PC_OK;
    for (i = 0; i < npoints && i < ncorners; i++) {
        any = any || corners[i];
    }
    if (!any) {
        return pc_compose_curve_lut(points, npoints, out, err);
    }
    pts = pc_malloc_array(npoints, sizeof *pts);
    seg = pc_malloc_array(npoints, sizeof *seg);
    lut = pc_malloc_array(LUT, sizeof *lut);
    if (pts == NULL || seg == NULL || lut == NULL) {
        free(pts);
        free(seg);
        free(lut);
        return nomem(err);
    }
    for (i = 0; i < npoints; i++) {
        pts[i].p = points[i];
        pts[i].corner = i < ncorners && corners[i];
    }
    st = pc_cmp_stable_sort(pts, npoints, sizeof *pts, cpt_less, err);
    n = npoints > 0 ? 1 : 0;
    for (i = 1; st == PC_OK && i < npoints; i++) {
        if (!(fabsf(pts[i].p.input - pts[n - 1].p.input) < 1e-6f)) {
            pts[n++] = pts[i];
        }
    }
    last = n > 0 ? n - 1 : 0;
    for (i = 1; st == PC_OK && i <= last; i++) {
        float from;
        size_t j;
        if (!(pts[i].corner || i == last)) {
            continue;
        }
        for (j = start; j <= i; j++) {
            seg[j - start] = pts[j].p;
        }
        from = pts[start].p.input;
        st = pc_compose_curve_lut(seg, i - start + 1, lut, err);
        if (st == PC_OK) {
            if (!have) {
                memcpy(out, lut, LUT * sizeof *out);
                have = true;
            } else {
                for (k = 0; k < LUT; k++) {
                    if ((float)k / (float)(LUT - 1) >= from) {
                        out[k] = lut[k];
                    }
                }
            }
        }
        start = i;
    }
    free(pts);
    free(seg);
    free(lut);
    if (st == PC_OK && !have) {
        st = pc_compose_curve_lut(points, npoints, out, err);
    }
    return st;
}

void pc_compose_float_tone_identity(const pc_adjustment *adj, bool has_depth, pc_sample_type depth,
                                    bool out[3])
{
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = false;
    }
    if (adj->kind == PC_ADJ_CURVES && adj->u.curves.space == PC_TONE_RGB && has_depth &&
        depth == PC_SAMPLE_TYPE_F32) {
        bool master = pc_curve_is_identity(adj->u.curves.master.pts, adj->u.curves.master.len);
        for (i = 0; i < 3; i++) {
            out[i] = master &&
                     pc_curve_is_identity(adj->u.curves.per_channel[i].pts, adj->u.curves.per_channel[i].len);
        }
    }
}

static void identity_row(float *row)
{
    size_t k;
    for (k = 0; k < LUT; k++) {
        row[k] = lut_x(k);
    }
}

pc_status pc_compose_tone_luts_q(const pc_adjustment *adj, bool has_q, float q, float *out, pc_error *err)
{
    size_t r;
    size_t k;
    if (adj->kind == PC_ADJ_LEVELS) {
        pc_levels_channel ident = pc_levels_channel_default();
        const pc_levels_channel *m = adj->u.levels.space == PC_TONE_LAB ? &ident : &adj->u.levels.master;
        for (r = 0; r < 4; r++) {
            const pc_levels_channel *c =
                r < 3 ? &adj->u.levels.per_channel[r]
                      : (adj->u.levels.space == PC_TONE_CMYK ? &adj->u.levels.black : &ident);
            for (k = 0; k < LUT; k++) {
                /* Each channel's record first, then the master (psd-tools levels_rgb.psd, #975). */
                out[r * LUT + k] =
                    pc_compose_levels_q(m, pc_compose_levels_q(c, lut_x(k), has_q, q), has_q, q);
            }
        }
        return PC_OK;
    }
    if (adj->kind == PC_ADJ_CURVES) {
        float *m = pc_malloc_array(2 * LUT, sizeof *m);
        float *row = m + LUT;
        pc_status st;
        if (m == NULL) {
            return nomem(err);
        }
        bool pencil = adj->u.curves.pencil;
        if (adj->u.curves.space == PC_TONE_LAB) {
            identity_row(m);
            st = PC_OK;
        } else {
            st =
                pc_compose_curve_lut_mode(adj->u.curves.master.pts, adj->u.curves.master.len, pencil, m, err);
        }
        for (r = 0; st == PC_OK && r < 4; r++) {
            if (r < 3) {
                st = pc_compose_curve_lut_mode(adj->u.curves.per_channel[r].pts,
                                               adj->u.curves.per_channel[r].len, pencil, row, err);
            } else if (adj->u.curves.space == PC_TONE_CMYK) {
                st = pc_compose_curve_lut_mode(adj->u.curves.black.pts, adj->u.curves.black.len, pencil, row,
                                               err);
            } else {
                identity_row(row);
            }
            for (k = 0; st == PC_OK && k < LUT; k++) {
                out[r * LUT + k] = pc_cmp_lut(m, LUT, row[k]);
            }
        }
        free(m);
        return st;
    }
    for (r = 0; r < 4; r++) {
        identity_row(out + r * LUT);
    }
    return PC_OK;
}

pc_status pc_compose_tone_luts(const pc_adjustment *adj, float *out, pc_error *err)
{
    return pc_compose_tone_luts_q(adj, false, 0.0f, out, err);
}

pc_status pc_compose_tone_luts_depth(const pc_adjustment *adj, bool has_depth, pc_sample_type depth,
                                     float *out, pc_error *err)
{
    float q = 0.0f;
    bool has_q;
    if (adj->kind == PC_ADJ_LEVELS && adj->u.levels.space == PC_TONE_RGB && has_depth &&
        depth == PC_SAMPLE_TYPE_F32) {
        /* 32-bit Levels: levels_float, sampled on 0..1 and kept in 0..1 for LUT consumers (float
         * RGB rendering evaluates the formula directly instead). */
        size_t r;
        size_t k;
        for (r = 0; r < 3; r++) {
            for (k = 0; k < LUT; k++) {
                out[r * LUT + k] = pc_clampf(
                    pc_compose_levels_float(&adj->u.levels.master,
                                            pc_compose_levels_float(&adj->u.levels.per_channel[r], lut_x(k))),
                    0.0f, 1.0f);
            }
        }
        identity_row(out + 3 * LUT);
        return PC_OK;
    }
    has_q = has_depth && pc_compose_adjustment_quantum(depth, &q);
    return pc_compose_tone_luts_q(adj, has_q, q, out, err);
}

static void lab_back(const float v[3], float out[3])
{
    const float lab[3] = {v[0] * 100.0f, v[1] * 255.0f - 128.0f, v[2] * 255.0f - 128.0f};
    pc_lab_to_srgb(lab, out);
}

void pc_cmp_tone_in_space(pc_tone_space space, const float *luts, const float c[3], float out[3])
{
    /* Through the conversions the document's surfaces use; the change is added as a difference
     * of two round trips, so channels a curve leaves alone (and out-of-gamut colours) stay
     * exact. */
    float before[3];
    float after[3];
    int i;
    if (space == PC_TONE_CMYK) {
        float ink[4];
        float o[4];
        pc_rgb_to_cmyk(c, ink);
        for (i = 0; i < 4; i++) {
            o[i] = 1.0f - pc_cmp_lut(luts + (size_t)i * LUT, LUT, 1.0f - ink[i]);
        }
        if (o[0] == ink[0] && o[1] == ink[1] && o[2] == ink[2] && o[3] == ink[3]) {
            memcpy(out, c, 3 * sizeof(float));
            return;
        }
        pc_cmyk_to_rgb(ink, before);
        pc_cmyk_to_rgb(o, after);
    } else if (space == PC_TONE_LAB) {
        float l[3];
        float nn[3];
        float o[3];
        pc_srgb_to_lab(c, l);
        nn[0] = l[0] / 100.0f;
        nn[1] = (l[1] + 128.0f) / 255.0f;
        nn[2] = (l[2] + 128.0f) / 255.0f;
        for (i = 0; i < 3; i++) {
            o[i] = pc_cmp_lut(luts + (size_t)i * LUT, LUT, nn[i]);
        }
        if (o[0] == nn[0] && o[1] == nn[1] && o[2] == nn[2]) {
            memcpy(out, c, 3 * sizeof(float));
            return;
        }
        lab_back(nn, before);
        lab_back(o, after);
    } else {
        for (i = 0; i < 3; i++) {
            out[i] = pc_cmp_lut(luts + (size_t)i * LUT, LUT, c[i]);
        }
        return;
    }
    for (i = 0; i < 3; i++) {
        out[i] = pc_clampf(c[i] + after[i] - before[i], 0.0f, 1.0f);
    }
}
