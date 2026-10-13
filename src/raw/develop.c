/* Raw development (port of crates/raw/src/develop.rs): linearize, subtract black, scale to white,
 * white balance, clip highlights, gain maps, demosaic (Bayer methods or the generic CFA path),
 * camera -> XYZ (D50) -> linear ProPhoto (or a camera look fitted to the JPEG; monochrome data stays
 * grey), exposure, Dehaze, Shadows / Highlights / Clarity / Texture and the black point (tone.c), the
 * profile's or the look's tone curve, gamma 1.8 to 16 bits, orientation. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"
#include "raw/internal.h"

static size_t sat_sub(size_t a, size_t b) { return a > b ? a - b : 0; }

/* Linearization + black/white scaling for one sample (pre white balance), 0..1. */
float rx_scale_value(const pc_raw_sensor *s, size_t x, size_t y, size_t sample)
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
                float v = rx_scale_value(s, x, y, k);
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
float rx_tone(const float (*curve)[2], size_t n, float v)
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
        table[i] = powf(pc_clampf(rx_tone(curve, n, s * s), 0.0f, 1.0f), 1.0f / 1.8f) * 65535.0f;
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

void rx_source_xy(uint16_t o, size_t w, size_t h, size_t x2, size_t y2, size_t *x, size_t *y)
{
    size_t wl = sat_sub(w, 1);
    size_t hl = sat_sub(h, 1);
    switch (o) {
    case 2:
        *x = sat_sub(wl, x2);
        *y = y2;
        break;
    case 3:
        *x = sat_sub(wl, x2);
        *y = sat_sub(hl, y2);
        break;
    case 4:
        *x = x2;
        *y = sat_sub(hl, y2);
        break;
    case 5:
        *x = y2;
        *y = x2;
        break;
    case 6:
        *x = y2;
        *y = sat_sub(hl, x2);
        break;
    case 7:
        *x = sat_sub(wl, y2);
        *y = sat_sub(hl, x2);
        break;
    case 8:
        *x = sat_sub(wl, y2);
        *y = x2;
        break;
    default:
        *x = x2;
        *y = y2;
    }
}

pc_status rx_resolve_neutral(const pc_raw_sensor *s, const pc_raw_develop_options *opts,
                             pc_raw_strings *warnings, double out[3], pc_error *err)
{
    double as_shot[3];
    bool has_as_shot = false;
    pc_status st;
    int k;
    /* Monochrome data has no colour to balance. */
    if (pc_raw_sensor_is_monochrome(s)) {
        out[0] = out[1] = out[2] = 1.0;
        return PC_OK;
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
            memcpy(out, as_shot, sizeof as_shot);
            return PC_OK;
        }
        if ((st = rx_warn(warnings, err,
                          "no as-shot white balance in the file; estimated automatically (grey world)")) !=
            PC_OK) {
            return st;
        }
        return grey_world(s, out, err);
    case PC_RAW_WB_AUTO:
        return grey_world(s, out, err);
    default: {
        double inv[3];
        for (k = 0; k < 3; k++) {
            inv[k] = 1.0 / opts->white_balance.multipliers[k];
        }
        if (!normalize(inv, out)) {
            return rx_malformed(err, "white-balance multipliers must be positive");
        }
        return PC_OK;
    }
    }
}

/* The gamma-1.8 encoding after a fitted look curve (NULL: plain gamma 1.8). */
static void gamma_table_look(const pc_raw_look_curve *tone, float table[RX_GAMMA_STEPS + 2])
{
    size_t i;
    for (i = 0; i <= RX_GAMMA_STEPS + 1; i++) {
        double s = pc_rs_min_k((double)i / (double)RX_GAMMA_STEPS, 1.0);
        if (tone != NULL) {
            table[i] = (float)(pow(pc_raw_look_curve_eval(tone, s * s), 1.0 / 1.8) * 65535.0);
        } else {
            table[i] = powf((float)s, 2.0f / 1.8f) * 65535.0f;
        }
    }
}

typedef struct cfa_ctx {
    const pc_raw_sensor *s;
    rx_padded *plane;
    float mult[3];
    bool periodic;
    size_t period;
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
            /* Per column of the period: black and the combined scale x white-balance gain. */
            float kb[2 * PC_RAW_CFA_MAX] = {0.0f};
            float kg[2 * PC_RAW_CFA_MAX] = {0.0f};
            const uint16_t *src = s->data + y * s->width + c.x;
            size_t p;
            size_t q = 0;
            for (p = 0; p < cc->period; p++) {
                size_t x = c.x + p;
                float black = pc_raw_black_at(&s->black, sat_sub(x, s->active.x), sat_sub(y, s->active.y), 0, 1);
                float range = s->white[0] - black;
                uint8_t col = pc_raw_cfa_color(&s->cfa, x, y);
                kb[p] = black;
                kg[p] = range >= 1.0f ? cc->mult[col < 2 ? col : 2] / range : 0.0f;
            }
            for (i = 0; i < w; i++) {
                uint16_t raw = src[i];
                float v = (float)raw;
                if (s->linearization != NULL) {
                    v = (float)(raw < s->nlinearization ? s->linearization[raw]
                                                        : s->linearization[s->nlinearization - 1]);
                }
                row[i] = pc_clampf((v - kb[q]) * kg[q], 0.0f, 1.0f);
                q = q + 1 == cc->period ? 0 : q + 1;
            }
        } else {
            for (i = 0; i < w; i++) {
                size_t x = c.x + i;
                uint8_t col = pc_raw_cfa_color(&s->cfa, x, y);
                row[i] = pc_clampf(rx_scale_value(s, x, y, 0) * cc->mult[col < 2 ? col : 2], 0.0f, 1.0f);
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
    size_t last = s->samples > 0 ? s->samples - 1 : 0;
    size_t ks = s->samples < 3 ? s->samples : 3;
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
                /* Monochrome: the one sample in all three channels. */
                size_t sample = k < last ? k : last;
                row[i * 3 + k] = pc_clampf(rx_scale_value(s, c.x + i, y, sample) * lc->mult[k], 0.0f, 1.0f);
            }
        }
        for (k = 0; k < ks; k++) {
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

/* The output stage: camera -> ProPhoto (or grey), Dehaze, the tone map, the gamma encoding. */
typedef struct out_ctx {
    const float *rgb;
    uint16_t *out;
    size_t w;
    float m[3][3];
    bool mono;
    float grey_gain;
    float yrow[3];
    const rx_haze *haze;   /* NULL: none */
    const rx_tonemap *map; /* NULL: none */
    const float *lut;
} out_ctx;

static void pro(const out_ctx *c, size_t i, float v[3])
{
    const float *p = c->rgb + i * 3;
    int k;
    if (c->mono) {
        v[0] = v[1] = v[2] = c->grey_gain * p[0];
        return;
    }
    for (k = 0; k < 3; k++) {
        v[k] = c->m[k][0] * p[0] + c->m[k][1] * p[1] + c->m[k][2] * p[2];
    }
}

/* Linear ProPhoto of pixel i after Dehaze. */
static void linear(const out_ctx *c, size_t i, float v[3])
{
    pro(c, i, v);
    if (c->haze != NULL) {
        float o[3];
        rx_haze_apply(c->haze, i % c->w, i / c->w, v, o);
        memcpy(v, o, sizeof o);
    }
}

static float lumf(const out_ctx *c, const float v[3])
{
    return c->yrow[0] * v[0] + c->yrow[1] * v[1] + c->yrow[2] * v[2];
}

static void px_pro(const void *ctx, size_t i, float out[3])
{
    pro(ctx, i, out);
}

static void px_lum_linear(const void *ctx, size_t i, float out[3])
{
    float v[3];
    linear(ctx, i, v);
    out[0] = lumf(ctx, v);
}

static void px_lum_pro(const void *ctx, size_t i, float out[3])
{
    float v[3];
    pro(ctx, i, v);
    out[0] = lumf(ctx, v);
}

static pc_status encode_px(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const out_ctx *c = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        float v[3];
        int k;
        linear(c, i, v);
        if (c->map != NULL) {
            float g = rx_tonemap_gain(c->map, i % c->w, i / c->w, lumf(c, v));
            /* The black point first, as Camera Raw: lifting before it would flatten the deepest
             * shadows' contrast. */
            for (k = 0; k < 3; k++) {
                v[k] = rx_tonemap_unblack(c->map, v[k]) * g;
            }
        }
        for (k = 0; k < 3; k++) {
            c->out[i * 3 + (size_t)k] = rx_gamma_encode(c->lut, v[k]);
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

/* Rust's Display of an f64 with one decimal (NaN / inf spelt its way). */
static void fmt1(char *out, size_t cap, double v)
{
    if (isnan(v)) {
        (void)snprintf(out, cap, "NaN");
    } else if (isinf(v)) {
        (void)snprintf(out, cap, "%sinf", v < 0 ? "-" : "");
    } else {
        (void)snprintf(out, cap, "%.1f", v);
    }
}

/* Balanced camera -> XYZ D50: a look fitted to the camera's JPEG, the file's calibration, or the
 * neutral fallback (with the warnings the reference gives). */
static pc_status choose_to_xyz(const pc_raw_sensor *s, const pc_raw_develop_options *opts,
                               const double neutral[3], bool mono, pc_raw_strings *warnings,
                               pc_raw_mat3 *to_xyz, const pc_raw_camera_look **look, pc_error *err)
{
    pc_raw_mat3 m;
    bool calibrated = pc_raw_balanced_to_xyz_d50(&s->color, neutral, &m);
    pc_status st;
    *look =
        opts->camera_look != NULL && !mono && rx_look_is_usable(opts->camera_look) ? opts->camera_look : NULL;
    if (opts->camera_look != NULL && *look == NULL && !mono &&
        (st = rx_warn(warnings, err,
                      "the supplied camera look is not usable (non-finite or singular); ignored")) != PC_OK) {
        return st;
    }
    if (*look != NULL) {
        char a[64];
        char b[64];
        fmt1(a, sizeof a, (*look)->delta_e);
        fmt1(b, sizeof b, (*look)->delta_e_fallback);
        if ((st = rx_warn(warnings, err,
                          "colour fitted to the camera's embedded JPEG (mean \xCE\x94"
                          "E %s against it; %s with the neutral fallback)",
                          a, b)) != PC_OK) {
            return st;
        }
        *to_xyz = (*look)->to_xyz;
        return PC_OK;
    }
    if (calibrated) {
        *to_xyz = m;
        return PC_OK;
    }
    if (s->format != PC_RAW_DNG && !mono &&
        (st = rx_warn(
             warnings, err,
             "no colour calibration for this camera; camera colours are treated as sRGB primaries")) !=
            PC_OK) {
        return st;
    }
    *to_xyz = rx_srgb_to_xyz_d50();
    return PC_OK;
}

/* The CFA path: a normalised, balanced, clipped plane, demosaiced (Bayer methods, or the generic
 * edge-directed one for other patterns). Writes the pattern's name into info. */
static pc_status develop_cfa(const pc_raw_sensor *s, const pc_raw_develop_options *opts, const float mult[3],
                             const pc_raw_mat3 *to_xyz, bool bayer, pc_error *errs, char *name, float **rgb,
                             pc_error *err)
{
    static const char RGB[3] = {'R', 'G', 'B'};
    pc_raw_rect c = s->crop;
    rx_padded plane;
    cfa_ctx cc;
    const pc_raw_black *bl = &s->black;
    pc_status st;
    size_t k;
    if ((st = rx_padded_new(c.width, c.height, &plane, err)) != PC_OK) {
        return st;
    }
    cc.s = s;
    cc.plane = &plane;
    memcpy(cc.mult, mult, sizeof cc.mult);
    /* Fast path: black varies at most per 2x2 position, so black and gain repeat every `period`
     * columns (the CFA width, made even). */
    cc.periodic = bl->ndelta_h == 0 && bl->ndelta_v == 0 && (bl->rows == 1 || bl->rows == 2) &&
                  (bl->cols == 1 || bl->cols == 2) && s->cfa.width <= 16;
    cc.period = s->cfa.width % 2 == 0 ? (s->cfa.width > 2 ? s->cfa.width : 2) : s->cfa.width * 2;
    cc.periodic = cc.periodic && cc.period <= 2 * PC_RAW_CFA_MAX;
    cc.maxp = max_points(s);
    cc.errs = errs;
    if ((st = run(c.height, rx_band_rows(c.width), cfa_rows, &cc, errs, err)) != PC_OK) {
        rx_padded_fini(&plane);
        return st;
    }
    rx_padded_fill_borders(&plane);
    if (bayer) {
        uint8_t phase[4];
        pc_raw_cfa_phase(&s->cfa, c.x, c.y, phase);
        for (k = 0; k < 4; k++) {
            name[k] = RGB[phase[k] < 2 ? phase[k] : 2];
        }
        name[4] = '\0';
        st = rx_demosaic(&plane, phase, opts->demosaic, to_xyz, rgb, err);
    } else {
        rx_pattern pat;
        size_t o = 0;
        rx_pattern_of(&s->cfa, c.x, c.y, &pat);
        for (k = 0; k < pat.w * pat.h; k++) {
            if (k > 0 && k % pat.w == 0) {
                name[o++] = '/';
            }
            name[o++] = RGB[pat.colors[k] < 2 ? pat.colors[k] : 2];
        }
        name[o] = '\0';
        st = rx_demosaic_cfa(&plane, &pat, rgb, err);
    }
    rx_padded_fini(&plane);
    return st;
}

/* Camera -> ProPhoto, exposure, Dehaze, Shadows / Highlights / Clarity / Texture and the black point,
 * the look's (or the profile's) tone curve, gamma 1.8, 16 bits. */
static pc_status develop_output(const pc_raw_sensor *s, const pc_raw_develop_options *opts, out_ctx *oc,
                                const pc_raw_camera_look *look, pc_error *err)
{
    size_t w = s->crop.width;
    size_t h = s->crop.height;
    rx_haze haze;
    rx_tonemap map;
    bool has_haze = false;
    bool has_map = false;
    float *lut = NULL;
    pc_status st;
    if ((st = rx_haze_new(w, h, px_pro, oc, opts->tone.dehaze, &haze, &has_haze, err)) != PC_OK) {
        return st;
    }
    oc->haze = has_haze ? &haze : NULL;
    if ((st = rx_tonemap_new(w, h, px_lum_linear, oc, &opts->tone, s->black_point, &map, &has_map, err)) !=
        PC_OK) {
        goto done;
    }
    oc->map = has_map ? &map : NULL;
    /* The black point belongs to the default rendering: Dehaze does not move it. */
    if (has_map && has_haze &&
        (st = rx_black_level(w, h, px_lum_pro, oc, s->black_point, &map.black, err)) != PC_OK) {
        goto done;
    }
    lut = pc_malloc_array(RX_GAMMA_STEPS + 2, sizeof *lut);
    if (lut == NULL) {
        st = rx_nomem(err);
        goto done;
    }
    if (look != NULL) {
        gamma_table_look(look->has_tone ? &look->tone : NULL, lut);
    } else {
        rx_gamma_table((const float (*)[2])s->tone_curve, s->ntone_curve, lut);
    }
    oc->lut = lut;
    (void)pc_pool_for(pc_pool_global(), w * h, rx_band_rows(w) * w, encode_px, oc);
    st = PC_OK;
done:
    free(lut);
    if (has_map) {
        rx_tonemap_fini(&map);
    }
    if (has_haze) {
        rx_haze_fini(&haze);
    }
    oc->haze = NULL;
    oc->map = NULL;
    return st;
}

pc_status pc_raw_develop_sensor(const pc_raw_sensor *s, const pc_raw_develop_options *opts, pc_raw_developed *out,
                                pc_error *err)
{
    pc_raw_rect c = s->crop;
    pc_raw_developed d;
    double neutral[3];
    float mult[3];
    pc_raw_mat3 to_xyz;
    const pc_raw_camera_look *look;
    double gain;
    pc_raw_mat3 pm;
    pc_raw_mat3 om;
    out_ctx oc;
    size_t w;
    size_t h;
    float *rgb = NULL;
    uint16_t *o = NULL;
    uint16_t *oriented = NULL;
    size_t ow;
    size_t oh;
    float nmax;
    pc_status st;
    pc_error *errs = NULL;
    size_t need;
    bool bayer;
    bool mono;
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
    /* Working buffers: one f32 plane plus f32 RGB plus u16 RGB per pixel, and a green plane for
     * non-Bayer patterns. */
    bayer = !s->has_cfa || pc_raw_cfa_is_bayer(&s->cfa);
    if ((st = pc_raw_limits_check(&opts->limits, c.width, c.height, bayer ? 4 + 12 + 6 : 4 + 4 + 12 + 6,
                                  err)) != PC_OK) {
        return st;
    }
    if ((st = rx_strings_clone(&s->warnings, &d.warnings, err)) != PC_OK) {
        return st;
    }
    if ((st = rx_resolve_neutral(s, opts, &d.warnings, neutral, err)) != PC_OK) {
        goto fail;
    }
    for (k = 0; k < 3; k++) {
        mult[k] = pc_f64_to_f32(1.0 / neutral[k]);
    }
    /* Monochrome data stays neutral grey: no camera look and no colour calibration. */
    mono = pc_raw_sensor_is_monochrome(s);
    if ((st = choose_to_xyz(s, opts, neutral, mono, &d.warnings, &to_xyz, &look, err)) != PC_OK) {
        goto fail;
    }
    /* LLVM rewrites 2.0f64.powf(x) as exp2(x). */
    gain = exp2(s->baseline_exposure + pc_clampd(opts->exposure, -10.0, 10.0));
    pm = rx_xyz_d50_to_prophoto();
    om = rx_mul(&pm, &to_xyz);
    om = rx_scale(&om, gain);
    memset(&oc, 0, sizeof oc);
    for (k = 0; k < 9; k++) {
        oc.m[k / 3][k % 3] = pc_f64_to_f32(om.m[k / 3][k % 3]);
    }
    /* Neutral grey through the matrix is the gain alone, so the three channels stay equal. */
    oc.grey_gain = (float)gain;
    oc.mono = mono;
    w = c.width;
    h = c.height;
    errs = pc_calloc_array(pc_pool_chunks(h, rx_band_rows(w)) + 1, sizeof *errs);
    if (errs == NULL) {
        st = rx_nomem(err);
        goto fail;
    }
    if (s->has_cfa) {
        if ((st = develop_cfa(s, opts, mult, &to_xyz, bayer, errs, d.info.cfa, &rgb, err)) != PC_OK) {
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
        if ((st = run(h, rx_band_rows(w), linear_rows, &lc, errs, err)) != PC_OK) {
            goto fail;
        }
    }
    o = pc_malloc_array(w * h * 3, sizeof *o);
    if (o == NULL) {
        st = rx_nomem(err);
        goto fail;
    }
    {
        pc_raw_mat3 y = rx_rgb_to_xyz(RX_ROMM, RX_D50_XY);
        for (k = 0; k < 3; k++) {
            oc.yrow[k] = (float)y.m[1][k];
        }
    }
    oc.rgb = rgb;
    oc.out = o;
    oc.w = w;
    if ((st = develop_output(s, opts, &oc, look, err)) != PC_OK) {
        goto fail;
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
    for (k = 0; k < 3; k++) {
        d.info.wb_multipliers[k] = (double)(mult[k] / nmax);
    }
    d.info.orientation = s->orientation;
    d.info.baseline_exposure = s->baseline_exposure;
    free(errs);
    *out = d;
    return PC_OK;
fail:
    free(rgb);
    free(o);
    free(errs);
    pc_raw_developed_fini(&d);
    return st;
}
