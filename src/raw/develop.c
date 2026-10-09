/* Raw development (port of crates/raw/src/develop.rs): linearize, subtract black, scale to white,
 * white balance, clip highlights, gain maps, demosaic, camera -> XYZ (D50) -> linear ProPhoto,
 * exposure, the profile's tone curve, gamma 1.8 to 16 bits, orientation. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"
#include "raw/internal.h"

static size_t sat_sub(size_t a, size_t b) { return a > b ? a - b : 0; }

/* Linearization + black/white scaling for one sample (pre white balance), 0..1. */
static float scale_value(const pc_raw_sensor *s, size_t x, size_t y, size_t sample)
{
    uint16_t raw = s->data[(y * s->width + x) * s->samples + sample];
    float v;
    float black;
    float white;
    float range;
    float n;
    if (s->linearization != NULL) {
        uint16_t l = raw < s->nlinearization ? s->linearization[raw] : s->linearization[s->nlinearization - 1];
        v = (float)l;
    } else {
        v = (float)raw;
    }
    black = pc_raw_black_at(&s->black, sat_sub(x, s->active.x), sat_sub(y, s->active.y), sample, s->samples);
    white = s->white[sample < 2 ? sample : 2];
    range = white - black;
    if (isnan(range) || range < 1.0f) {
        return 0.0f;
    }
    n = (v - black) / range;
    return isfinite(n) ? n : 0.0f;
}

typedef struct grey_ctx {
    const pc_raw_sensor *s;
    size_t step;
    double (*sum)[3];
    uint64_t (*n)[3];
} grey_ctx;

static pc_status grey_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    grey_ctx *g = ctx;
    const pc_raw_sensor *s = g->s;
    pc_raw_rect c = s->crop;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        size_t y = c.y + i * g->step;
        double sum[3] = {0.0, 0.0, 0.0};
        uint64_t n[3] = {0, 0, 0};
        size_t x;
        size_t ks = s->samples < 3 ? s->samples : 3;
        for (x = c.x; x < c.x + c.width; x++) {
            size_t k;
            for (k = 0; k < ks; k++) {
                size_t ch = s->has_cfa ? pc_raw_cfa_color(&s->cfa, x, y) : k;
                float v = scale_value(s, x, y, k);
                if (v > 0.002f && v < 0.9f && ch < 3) {
                    sum[ch] += (double)v;
                    n[ch]++;
                }
            }
        }
        memcpy(g->sum[i], sum, sizeof sum);
        memcpy(g->n[i], n, sizeof n);
    }
    return PC_OK;
}

/* Grey-world neutral (camera-space R, G, B) from unclipped mid-range samples. */
static pc_status grey_world(const pc_raw_sensor *s, double out[3], pc_error *err)
{
    pc_raw_rect c = s->crop;
    size_t step = c.height / 512;
    size_t nrows;
    grey_ctx g;
    double sum[3] = {0.0, 0.0, 0.0};
    uint64_t n[3] = {0, 0, 0};
    double mean[3];
    double m = -1.7976931348623157e308;
    size_t i;
    int k;
    if (step < 1) {
        step = 1;
    }
    nrows = c.height / step + (c.height % step != 0);
    g.s = s;
    g.step = step;
    g.sum = pc_calloc_array(nrows > 0 ? nrows : 1, sizeof *g.sum);
    g.n = pc_calloc_array(nrows > 0 ? nrows : 1, sizeof *g.n);
    if (g.sum == NULL || g.n == NULL) {
        free(g.sum);
        free(g.n);
        return rx_nomem(err);
    }
    (void)pc_pool_for(pc_pool_global(), nrows, 1, grey_rows, &g);
    for (i = 0; i < nrows; i++) {
        for (k = 0; k < 3; k++) {
            sum[k] += g.sum[i][k];
            n[k] += g.n[i][k];
        }
    }
    free(g.sum);
    free(g.n);
    for (k = 0; k < 3; k++) {
        mean[k] = n[k] > 0 ? sum[k] / (double)n[k] : 0.0;
    }
    for (k = 0; k < 3; k++) {
        if (isnan(mean[k]) || mean[k] <= 1e-6) {
            out[0] = out[1] = out[2] = 1.0;
            return PC_OK;
        }
    }
    for (k = 0; k < 3; k++) {
        m = pc_rs_max(m, mean[k]);
    }
    for (k = 0; k < 3; k++) {
        out[k] = mean[k] / m;
    }
    return PC_OK;
}

static bool normalize(const double n[3], double out[3])
{
    double m = -1.7976931348623157e308;
    int k;
    for (k = 0; k < 3; k++) {
        m = pc_rs_max(m, n[k]);
    }
    if (!(m > 0.0)) {
        return false;
    }
    for (k = 0; k < 3; k++) {
        if (!(isfinite(n[k]) && n[k] > 1e-4)) {
            return false;
        }
    }
    for (k = 0; k < 3; k++) {
        out[k] = n[k] / m;
    }
    return true;
}

/* Gain-map factors for data row y, columns x0..x0+w, sample k; false when no map touches the
 * row. row_buf has room for the widest map's points. */
static bool gain_row(const pc_raw_sensor *s, size_t y, size_t x0, size_t w, size_t k, float *out, float *row_buf)
{
    pc_raw_rect a = s->active;
    size_t ay;
    double rv;
    bool any = false;
    size_t mi;
    if (s->ngain_maps == 0 || y < a.y || a.width == 0 || a.height == 0) {
        return false;
    }
    ay = y - a.y;
    rv = ((double)ay + 0.5) / (double)a.height;
    for (mi = 0; mi < s->ngain_maps; mi++) {
        const pc_raw_gain_map *m = &s->gain_maps[mi];
        size_t pe = m->plane > SIZE_MAX - m->planes ? SIZE_MAX : m->plane + m->planes;
        size_t i;
        if (k < m->plane || k >= pe || ay < m->top || ay >= m->bottom || (ay - m->top) % m->row_pitch != 0) {
            continue;
        }
        rx_gain_map_row(m, rv, k - m->plane, row_buf);
        if (!any) {
            for (i = 0; i < w; i++) {
                out[i] = 1.0f;
            }
            any = true;
        }
        for (i = 0; i < w; i++) {
            size_t x = x0 + i;
            if (x >= a.x && rx_gain_map_covers(m, x - a.x, ay)) {
                out[i] *= rx_gain_map_at(m, row_buf, ((double)(x - a.x) + 0.5) / (double)a.width);
            }
        }
    }
    return any;
}

static size_t max_points(const pc_raw_sensor *s)
{
    size_t i;
    size_t m = 1;
    for (i = 0; i < s->ngain_maps; i++) {
        m = s->gain_maps[i].points_h > m ? s->gain_maps[i].points_h : m;
    }
    return m;
}

/* `curve` at linear v: interpolation in log2 of the input, a line through 0 below the first point,
 * the last output beyond the last. */
static float tone(const float (*curve)[2], size_t n, float v)
{
    size_t i;
    size_t lo;
    size_t hi;
    float x0;
    float y0;
    float x1;
    float y1;
    float f;
    if (n == 0) {
        return v;
    }
    if (v <= curve[0][0]) {
        return curve[0][0] > 0.0f ? v * curve[0][1] / curve[0][0] : curve[0][1];
    }
    if (v >= curve[n - 1][0]) {
        return curve[n - 1][1];
    }
    /* partition_point(p[0] <= v) */
    lo = 0;
    hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (curve[mid][0] <= v) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    i = lo < 1 ? 1 : lo > n - 1 ? n - 1 : lo;
    x0 = curve[i - 1][0];
    y0 = curve[i - 1][1];
    x1 = curve[i][0];
    y1 = curve[i][1];
    f = (log2f(v) - log2f(x0)) / (log2f(x1) - log2f(x0));
    return y0 + (y1 - y0) * f;
}

void rx_gamma_table(const float (*curve)[2], size_t n, float table[RX_GAMMA_STEPS + 2])
{
    size_t i;
    for (i = 0; i <= RX_GAMMA_STEPS + 1; i++) {
        float s = pc_rs_minf_k((float)i / (float)RX_GAMMA_STEPS, 1.0f);
        table[i] = powf(pc_clampf(tone(curve, n, s * s), 0.0f, 1.0f), 1.0f / 1.8f) * 65535.0f;
    }
}

uint16_t rx_gamma_encode(const float table[RX_GAMMA_STEPS + 2], float v)
{
    float x;
    size_t i;
    float f;
    float a;
    float b;
    v = isnan(v) ? 0.0f : pc_clampf(v, 0.0f, 1.0f);
    x = sqrtf(v) * (float)RX_GAMMA_STEPS;
    i = pc_f32_to_size(x);
    if (i > RX_GAMMA_STEPS) {
        i = RX_GAMMA_STEPS;
    }
    f = x - (float)i;
    a = table[i];
    b = table[i + 1];
    return pc_f32_to_u16(a + (b - a) * f + 0.5f);
}

typedef struct orient_ctx {
    const uint16_t *src;
    uint16_t *out;
    size_t w, h, ow;
    uint16_t o;
} orient_ctx;

static pc_status orient_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    orient_ctx *c = ctx;
    size_t y2;
    (void)chunk;
    for (y2 = begin; y2 < end; y2++) {
        size_t x2;
        for (x2 = 0; x2 < c->ow; x2++) {
            size_t x;
            size_t y;
            switch (c->o) {
            case 2:
                x = c->w - 1 - x2;
                y = y2;
                break;
            case 3:
                x = c->w - 1 - x2;
                y = c->h - 1 - y2;
                break;
            case 4:
                x = x2;
                y = c->h - 1 - y2;
                break;
            case 5:
                x = y2;
                y = x2;
                break;
            case 6:
                x = y2;
                y = c->h - 1 - x2;
                break;
            case 7:
                x = c->w - 1 - y2;
                y = c->h - 1 - x2;
                break;
            default:
                x = c->w - 1 - y2;
                y = x2;
            }
            memcpy(c->out + (y2 * c->ow + x2) * 3, c->src + (y * c->w + x) * 3, 3 * sizeof *c->src);
        }
    }
    return PC_OK;
}

pc_status rx_orient(const uint16_t *src, size_t w, size_t h, uint16_t o, uint16_t **out, size_t *ow, size_t *oh,
                    pc_error *err)
{
    orient_ctx c;
    if (o < 2 || o > 8) {
        *out = NULL;
        *ow = w;
        *oh = h;
        return PC_OK;
    }
    c.src = src;
    c.w = w;
    c.h = h;
    c.o = o;
    c.ow = o >= 5 ? h : w;
    c.out = pc_malloc_array(w * h * 3 > 0 ? w * h * 3 : 1, sizeof *c.out);
    if (c.out == NULL) {
        return rx_nomem(err);
    }
    (void)pc_pool_for(pc_pool_global(), o >= 5 ? w : h, rx_band_rows(c.ow), orient_rows, &c);
    *out = c.out;
    *ow = c.ow;
    *oh = o >= 5 ? w : h;
    return PC_OK;
}

typedef struct cfa_ctx {
    const pc_raw_sensor *s;
    rx_padded *plane;
    float mult[3];
    bool periodic;
    size_t maxp;
    pc_error *errs;
} cfa_ctx;

static pc_status cfa_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    cfa_ctx *cc = ctx;
    const pc_raw_sensor *s = cc->s;
    pc_raw_rect c = s->crop;
    size_t w = c.width;
    size_t stride = cc->plane->stride;
    float *gains = NULL;
    float *row_buf = NULL;
    size_t r;
    if (s->ngain_maps > 0) {
        gains = pc_malloc_array(w > 0 ? w : 1, sizeof *gains);
        row_buf = pc_malloc_array(cc->maxp, sizeof *row_buf);
        if (gains == NULL || row_buf == NULL) {
            free(gains);
            free(row_buf);
            return rx_nomem(&cc->errs[chunk]);
        }
    }
    for (r = begin; r < end; r++) {
        float *row = cc->plane->data + (r + RX_PAD) * stride + RX_PAD;
        size_t y = c.y + r;
        size_t i;
        if (cc->periodic) {
            /* Per column parity: black and the combined scale x white-balance gain. */
            float kb[2];
            float kg[2];
            const uint16_t *src = s->data + y * s->width + c.x;
            int p;
            for (p = 0; p < 2; p++) {
                size_t x = c.x + (size_t)p;
                float black = pc_raw_black_at(&s->black, sat_sub(x, s->active.x), sat_sub(y, s->active.y), 0, 1);
                float range = s->white[0] - black;
                uint8_t col = pc_raw_cfa_color(&s->cfa, x, y);
                kb[p] = black;
                kg[p] = range >= 1.0f ? cc->mult[col < 2 ? col : 2] / range : 0.0f;
            }
            if (s->linearization == NULL) {
                for (i = 0; i < w; i++) {
                    row[i] = pc_clampf(((float)src[i] - kb[i & 1]) * kg[i & 1], 0.0f, 1.0f);
                }
            } else {
                for (i = 0; i < w; i++) {
                    uint16_t raw = src[i];
                    uint16_t l =
                        raw < s->nlinearization ? s->linearization[raw] : s->linearization[s->nlinearization - 1];
                    row[i] = pc_clampf(((float)l - kb[i & 1]) * kg[i & 1], 0.0f, 1.0f);
                }
            }
        } else {
            for (i = 0; i < w; i++) {
                size_t x = c.x + i;
                uint8_t col = pc_raw_cfa_color(&s->cfa, x, y);
                row[i] = pc_clampf(scale_value(s, x, y, 0) * cc->mult[col < 2 ? col : 2], 0.0f, 1.0f);
            }
        }
        /* Lens-shading gain maps, after the white-balance clip so clipped highlights stay neutral. */
        if (gains != NULL && gain_row(s, y, c.x, w, 0, gains, row_buf)) {
            for (i = 0; i < w; i++) {
                row[i] = pc_clampf(row[i] * gains[i], 0.0f, 1.0f);
            }
        }
    }
    free(gains);
    free(row_buf);
    return PC_OK;
}

typedef struct lin_ctx {
    const pc_raw_sensor *s;
    float *rgb;
    float mult[3];
    size_t maxp;
    pc_error *errs;
} lin_ctx;

static pc_status linear_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    lin_ctx *lc = ctx;
    const pc_raw_sensor *s = lc->s;
    pc_raw_rect c = s->crop;
    size_t w = c.width;
    float *gains = NULL;
    float *row_buf = NULL;
    size_t r;
    if (s->ngain_maps > 0) {
        gains = pc_malloc_array(w > 0 ? w : 1, sizeof *gains);
        row_buf = pc_malloc_array(lc->maxp, sizeof *row_buf);
        if (gains == NULL || row_buf == NULL) {
            free(gains);
            free(row_buf);
            return rx_nomem(&lc->errs[chunk]);
        }
    }
    for (r = begin; r < end; r++) {
        float *row = lc->rgb + r * w * 3;
        size_t y = c.y + r;
        size_t i;
        size_t k;
        for (i = 0; i < w; i++) {
            for (k = 0; k < 3; k++) {
                row[i * 3 + k] = pc_clampf(scale_value(s, c.x + i, y, k) * lc->mult[k], 0.0f, 1.0f);
            }
        }
        for (k = 0; k < 3; k++) {
            if (gains != NULL && gain_row(s, y, c.x, w, k, gains, row_buf)) {
                for (i = 0; i < w; i++) {
                    row[i * 3 + k] = pc_clampf(row[i * 3 + k] * gains[i], 0.0f, 1.0f);
                }
            }
        }
    }
    free(gains);
    free(row_buf);
    return PC_OK;
}

typedef struct out_ctx {
    const float *rgb;
    uint16_t *out;
    float m[3][3];
    const float *lut;
} out_ctx;

static pc_status encode_px(void *ctx, size_t chunk, size_t begin, size_t end)
{
    out_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        const float *p = c->rgb + i * 3;
        int k;
        for (k = 0; k < 3; k++) {
            c->out[i * 3 + (size_t)k] = rx_gamma_encode(c->lut, c->m[k][0] * p[0] + c->m[k][1] * p[1] + c->m[k][2] * p[2]);
        }
    }
    return PC_OK;
}

/* Runs fn over n items in grain-sized chunks, each with its own error slot; reports the lowest
 * failing chunk's error. */
static pc_status run(size_t n, size_t grain, pc_pool_fn fn, void *ctx, pc_error *errs, pc_error *err)
{
    pc_status st = pc_pool_for(pc_pool_global(), n, grain, fn, ctx);
    size_t i;
    if (st == PC_OK) {
        return PC_OK;
    }
    for (i = 0; i < pc_pool_chunks(n, grain); i++) {
        if (errs[i].code != PC_OK) {
            if (err != NULL) {
                *err = errs[i];
            }
            return errs[i].code;
        }
    }
    return rx_nomem(err);
}

pc_status pc_raw_develop_sensor(const pc_raw_sensor *s, const pc_raw_develop_options *opts, pc_raw_developed *out,
                                pc_error *err)
{
    pc_raw_rect c = s->crop;
    pc_raw_developed d;
    double as_shot[3];
    bool has_as_shot = false;
    double neutral[3];
    float mult[3];
    pc_raw_mat3 to_xyz;
    double gain;
    pc_raw_mat3 pro;
    pc_raw_mat3 om;
    float out_m[3][3];
    size_t w;
    size_t h;
    size_t band;
    float *rgb = NULL;
    uint8_t phase[4];
    float *lut = NULL;
    uint16_t *o = NULL;
    uint16_t *oriented = NULL;
    size_t ow;
    size_t oh;
    float nmax;
    pc_status st;
    pc_error *errs = NULL;
    size_t need;
    int k;
    memset(&d, 0, sizeof d);
    if (pc_raw_rect_is_empty(c) || c.x > SIZE_MAX - c.width || c.x + c.width > s->width ||
        c.y > SIZE_MAX - c.height || c.y + c.height > s->height || !pc_size_mul3(s->width, s->height, s->samples, &need) ||
        s->ndata < need || (s->data == NULL && need > 0)) {
        return rx_malformed(err, "crop lies outside the sensor data");
    }
    if (s->samples != 1 && s->samples != 3) {
        return rx_unsupported(err, "%zu samples per pixel", s->samples);
    }
    if (s->has_cfa && (s->cfa.width == 0 || s->cfa.height == 0)) {
        return rx_malformed(err, "crop lies outside the sensor data");
    }
    /* Working buffers: one f32 plane plus f32 RGB plus u16 RGB per pixel. */
    if ((st = pc_raw_limits_check(&opts->limits, c.width, c.height, 4 + 12 + 6, err)) != PC_OK) {
        return st;
    }
    if ((st = rx_strings_clone(&s->warnings, &d.warnings, err)) != PC_OK) {
        return st;
    }
    /* White balance: camera-space neutral, max component 1. */
    if (s->color.has_as_shot_neutral) {
        has_as_shot = normalize(s->color.as_shot_neutral, as_shot);
    } else {
        double n[3];
        bool have = false;
        if (s->color.has_as_shot_white_xy && rx_xy_to_neutral(&s->color, s->color.as_shot_white_xy, n)) {
            have = true;
        } else if (s->has_camera_wb) {
            for (k = 0; k < 3; k++) {
                n[k] = 1.0 / s->camera_wb[k];
            }
            have = true;
        }
        has_as_shot = have && normalize(n, as_shot);
    }
    switch (opts->white_balance.kind) {
    case PC_RAW_WB_AS_SHOT:
        if (has_as_shot) {
            memcpy(neutral, as_shot, sizeof neutral);
        } else {
            if ((st = rx_warn(&d.warnings, err,
                              "no as-shot white balance in the file; estimated automatically (grey world)")) != PC_OK ||
                (st = grey_world(s, neutral, err)) != PC_OK) {
                goto fail;
            }
        }
        break;
    case PC_RAW_WB_AUTO:
        if ((st = grey_world(s, neutral, err)) != PC_OK) {
            goto fail;
        }
        break;
    default: {
        double inv[3];
        for (k = 0; k < 3; k++) {
            inv[k] = 1.0 / opts->white_balance.multipliers[k];
        }
        if (!normalize(inv, neutral)) {
            st = rx_malformed(err, "white-balance multipliers must be positive");
            goto fail;
        }
    }
    }
    for (k = 0; k < 3; k++) {
        mult[k] = pc_f64_to_f32(1.0 / neutral[k]);
    }
    /* Balanced camera -> XYZ D50. */
    if (!pc_raw_balanced_to_xyz_d50(&s->color, neutral, &to_xyz)) {
        if (s->format != PC_RAW_DNG &&
            (st = rx_warn(&d.warnings, err,
                          "no colour calibration for this camera; camera colours are treated as sRGB primaries")) !=
                PC_OK) {
            goto fail;
        }
        to_xyz = rx_srgb_to_xyz_d50();
    }
    /* LLVM rewrites 2.0f64.powf(x) as exp2(x). */
    gain = exp2(s->baseline_exposure + pc_clampd(opts->exposure, -10.0, 10.0));
    pro = rx_xyz_d50_to_prophoto();
    om = rx_mul(&pro, &to_xyz);
    om = rx_scale(&om, gain);
    for (k = 0; k < 9; k++) {
        out_m[k / 3][k % 3] = pc_f64_to_f32(om.m[k / 3][k % 3]);
    }
    w = c.width;
    h = c.height;
    band = rx_band_rows(w);
    errs = pc_calloc_array(pc_pool_chunks(h, band) + 1, sizeof *errs);
    if (errs == NULL) {
        st = rx_nomem(err);
        goto fail;
    }
    if (s->has_cfa) {
        /* Normalized, balanced, clipped CFA plane. */
        rx_padded plane;
        cfa_ctx cc;
        const pc_raw_black *bl = &s->black;
        if ((st = rx_padded_new(w, h, &plane, err)) != PC_OK) {
            goto fail;
        }
        cc.s = s;
        cc.plane = &plane;
        memcpy(cc.mult, mult, sizeof mult);
        /* Fast path: black varies at most per 2x2 position (the Bayer period). */
        cc.periodic = bl->ndelta_h == 0 && bl->ndelta_v == 0 && (bl->rows == 1 || bl->rows == 2) &&
                      (bl->cols == 1 || bl->cols == 2);
        cc.maxp = max_points(s);
        cc.errs = errs;
        if ((st = run(h, band, cfa_rows, &cc, errs, err)) != PC_OK) {
            rx_padded_fini(&plane);
            goto fail;
        }
        rx_padded_fill_borders(&plane);
        pc_raw_cfa_phase(&s->cfa, c.x, c.y, phase);
        st = rx_demosaic(&plane, phase, opts->demosaic, &to_xyz, &rgb, err);
        rx_padded_fini(&plane);
        if (st != PC_OK) {
            goto fail;
        }
    } else {
        lin_ctx lc;
        rgb = pc_calloc_array(w * h * 3, sizeof *rgb);
        if (rgb == NULL) {
            st = rx_nomem(err);
            goto fail;
        }
        lc.s = s;
        lc.rgb = rgb;
        memcpy(lc.mult, mult, sizeof mult);
        lc.maxp = max_points(s);
        lc.errs = errs;
        if ((st = run(h, band, linear_rows, &lc, errs, err)) != PC_OK) {
            goto fail;
        }
    }
    /* Camera -> ProPhoto, exposure, the profile's tone curve, gamma 1.8, 16 bits. */
    lut = pc_malloc_array(RX_GAMMA_STEPS + 2, sizeof *lut);
    o = pc_malloc_array(w * h * 3, sizeof *o);
    if (lut == NULL || o == NULL) {
        st = rx_nomem(err);
        goto fail;
    }
    rx_gamma_table((const float(*)[2])s->tone_curve, s->ntone_curve, lut);
    {
        out_ctx oc;
        oc.rgb = rgb;
        oc.out = o;
        memcpy(oc.m, out_m, sizeof out_m);
        oc.lut = lut;
        (void)pc_pool_for(pc_pool_global(), w * h, band * w, encode_px, &oc);
    }
    free(rgb);
    rgb = NULL;
    ow = w;
    oh = h;
    if (opts->orient) {
        if ((st = rx_orient(o, w, h, s->orientation, &oriented, &ow, &oh, err)) != PC_OK) {
            goto fail;
        }
        if (oriented != NULL) {
            free(o);
            o = oriented;
        }
    }
    nmax = 3.4028234663852886e38f;
    for (k = 0; k < 3; k++) {
        nmax = pc_rs_minf(nmax, mult[k]);
    }
    nmax = pc_rs_maxf_k(nmax, 1e-6f);
    d.width = (uint32_t)ow;
    d.height = (uint32_t)oh;
    d.rgb = o;
    d.nrgb = w * h * 3;
    o = NULL;
    d.info.format = s->format;
    if (s->make != NULL && (d.info.make = pc_strndup(s->make, strlen(s->make))) == NULL) {
        st = rx_nomem(err);
        goto fail;
    }
    if (s->model != NULL && (d.info.model = pc_strndup(s->model, strlen(s->model))) == NULL) {
        st = rx_nomem(err);
        goto fail;
    }
    d.info.sensor_width = s->width;
    d.info.sensor_height = s->height;
    d.info.has_cfa = s->has_cfa;
    if (s->has_cfa) {
        static const char RGB[3] = {'R', 'G', 'B'};
        for (k = 0; k < 4; k++) {
            d.info.cfa[k] = RGB[phase[k] < 2 ? phase[k] : 2];
        }
        d.info.cfa[4] = '\0';
    }
    for (k = 0; k < 3; k++) {
        d.info.wb_multipliers[k] = (double)(mult[k] / nmax);
    }
    d.info.orientation = s->orientation;
    d.info.baseline_exposure = s->baseline_exposure;
    free(lut);
    free(errs);
    *out = d;
    return PC_OK;
fail:
    free(rgb);
    free(lut);
    free(o);
    free(errs);
    pc_raw_developed_fini(&d);
    return st;
}
