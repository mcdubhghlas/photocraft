/* Lens Correction (port of crates/algo/src/lens.rs). See lens.h. */
#include "algo/lens.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/l9util.h"
#include "algo/photo_util.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "raster/pixel.h"

#define PI_D 3.14159265358979323846

/* Rust's `{:.0}` of an f64 (NaN is "NaN"; infinities "inf" / "-inf" as in C). */
static void fmt0(char *buf, size_t cap, double v)
{
    if (isnan(v)) {
        (void)snprintf(buf, cap, "NaN");
    } else {
        (void)snprintf(buf, cap, "%.0f", v);
    }
}

void pc_algo_lens_generic_profile(double focal_35mm, pc_algo_lens_profile *out)
{
    /* (focal, k1, k2, v1, v2) */
    static const double TABLE[7][5] = {
        {12.0, -0.16, 0.04, -0.60, 0.15},  {16.0, -0.11, 0.025, -0.50, 0.12},
        {24.0, -0.06, 0.012, -0.38, 0.08}, {35.0, -0.025, 0.004, -0.28, 0.05},
        {50.0, 0.0, 0.0, -0.22, 0.04},     {85.0, 0.012, 0.0, -0.16, 0.02},
        {200.0, 0.02, 0.0, -0.10, 0.01},
    };
    double f = pc_clampd(focal_35mm, TABLE[0][0], TABLE[6][0]), t;
    const double *a, *b;
    size_t k = 5, i;
    char num[320]; /* "-" and 309 digits of the largest f64 */
    for (i = 0; i < 6; i++) {
        if (f <= TABLE[i + 1][0]) {
            k = i;
            break;
        }
    }
    a = TABLE[k];
    b = TABLE[k + 1];
    /* Interpolate on log focal length. */
    t = (log(f) - log(a[0])) / (log(b[0]) - log(a[0]));
    memset(out, 0, sizeof *out);
    fmt0(num, sizeof num, focal_35mm);
    (void)snprintf(out->name, sizeof out->name, "Generic Lens (%s mm equivalent)", num);
#define L(x, y) ((x) + ((y) - (x)) * t)
    out->k1 = L(a[1], b[1]);
    out->k2 = L(a[2], b[2]);
    out->vignette[0] = L(a[3], b[3]);
    out->vignette[1] = L(a[4], b[4]);
#undef L
}

void pc_algo_lens_correction_default(pc_algo_lens_correction *lc)
{
    memset(lc, 0, sizeof *lc);
    lc->correct_distortion = true;
    lc->correct_vignette = true;
    lc->correct_ca = true;
    lc->vignette_midpoint = 50.0;
    lc->scale = 100.0;
    lc->edge.kind = PC_ALGO_LENS_EDGE_TRANSPARENCY;
}

void pc_algo_lens_map_init(pc_algo_lens_map *m, const pc_algo_lens_correction *lc, pc_rect frame)
{
    pc_algo_lens_profile zero;
    const pc_algo_lens_profile *prof = &zero;
    double g, h, a;
    memset(&zero, 0, sizeof zero);
    if (lc->has_profile) {
        prof = &lc->profile;
    }
    memset(m, 0, sizeof *m);
    m->c[0] = (double)pc_i32_wrapping_add(frame.x0, frame.x1) / 2.0;
    m->c[1] = (double)pc_i32_wrapping_add(frame.y0, frame.y1) / 2.0;
    m->r = pc_rs_max_k(hypot((double)pc_rect_width(frame), (double)pc_rect_height(frame)), 2.0) / 2.0;
    /* Keystone x' = x / (1 + h x + g y), y' = y / (1 + h x + g y) (normalised by r); the inverse
     * of [[1,0,0],[0,1,0],[h,g,1]] is [[1,0,0],[0,1,0],[-h,-g,1]]. */
    g = -lc->vertical / 100.0 * 0.5;
    h = -lc->horizontal / 100.0 * 0.5;
    m->hinv[0] = 1.0;
    m->hinv[4] = 1.0;
    m->hinv[6] = -h;
    m->hinv[7] = -g;
    m->hinv[8] = 1.0;
    a = lc->angle * (PI_D / 180.0);
    m->k1 = (lc->correct_distortion ? prof->k1 : 0.0) - lc->distortion / 100.0 * 0.25;
    m->k2 = lc->correct_distortion ? prof->k2 : 0.0;
    m->p1 = lc->correct_distortion ? prof->p1 : 0.0;
    m->p2 = lc->correct_distortion ? prof->p2 : 0.0;
    m->ca_r = (lc->correct_ca ? prof->ca_red : 0.0) - lc->red_cyan * 4e-5;
    m->ca_b = (lc->correct_ca ? prof->ca_blue : 0.0) - lc->blue_yellow * 4e-5;
    if (lc->correct_vignette) {
        memcpy(m->vig_profile, prof->vignette, sizeof m->vig_profile);
    }
    m->vig_amount = lc->vignette_amount / 100.0;
    m->vig_mid = pc_clampd(lc->vignette_midpoint, 0.0, 100.0) / 100.0;
    m->rot_s = sin(a);
    m->rot_c = cos(a);
    m->scale = pc_clampd(lc->scale / 100.0, 0.05, 20.0);
}

bool pc_algo_lens_map_sample(const pc_algo_lens_map *m, double x, double y, pc_algo_lens_sample *out)
{
    /* Undo scale and rotation (about the centre). */
    double dx = (x - m->c[0]) / m->scale, dy = (y - m->c[1]) / m->scale;
    double rx = m->rot_c * dx + m->rot_s * dy, ry = -m->rot_s * dx + m->rot_c * dy;
    double nx = rx / m->r, ny = ry / m->r, w, ux, uy, r2, radial, sx, sy, rs2, falloff, start, t, ramp, gain;
    const double *v = m->vig_profile;
    /* Undo the keystone. */
    w = m->hinv[6] * nx + m->hinv[7] * ny + m->hinv[8];
    if (w <= 0.05) {
        return false;
    }
    ux = nx / w;
    uy = ny / w;
    /* Lens distortion (Brown-Conrady). */
    r2 = ux * ux + uy * uy;
    radial = 1.0 + m->k1 * r2 + m->k2 * r2 * r2;
    sx = ux * radial + 2.0 * m->p1 * ux * uy + m->p2 * (r2 + 2.0 * ux * ux);
    sy = uy * radial + m->p1 * (r2 + 2.0 * uy * uy) + 2.0 * m->p2 * ux * uy;
    /* Vignetting on the source radius. */
    rs2 = sx * sx + sy * sy;
    falloff = pc_rs_max_k(1.0 + v[0] * rs2 + v[1] * rs2 * rs2 + v[2] * rs2 * rs2 * rs2, 0.05);
    start = (1.0 - m->vig_mid) * 0.9;
    t = pc_clampd((sqrt(rs2) - start) / (1.0 - start + 1e-9), 0.0, 1.0);
    ramp = t * t * (3.0 - 2.0 * t);
    gain = exp2(m->vig_amount * 1.5 * ramp) / falloff;
    out->p[0] = m->c[0] + sx * 1.0 * m->r;
    out->p[1] = m->c[1] + sy * 1.0 * m->r;
    out->pr[0] = m->c[0] + sx * (1.0 + m->ca_r) * m->r;
    out->pr[1] = m->c[1] + sy * (1.0 + m->ca_r) * m->r;
    out->pb[0] = m->c[0] + sx * (1.0 + m->ca_b) * m->r;
    out->pb[1] = m->c[1] + sy * (1.0 + m->ca_b) * m->r;
    out->gain = pc_f64_to_f32(gain);
    return true;
}

static bool covers(const pc_algo_lens_correction *lc, pc_rect frame, double scale)
{
    pc_algo_lens_correction c = *lc;
    pc_algo_lens_map m;
    double x0 = frame.x0, y0 = frame.y0, x1 = frame.x1, y1 = frame.y1;
    int i, k;
    c.scale = scale;
    pc_algo_lens_map_init(&m, &c, frame);
    for (i = 0; i <= 40; i++) {
        double t = (double)i / 40.0;
        double pts[4][2] = {{x0 + t * (x1 - x0), y0},
                            {x0 + t * (x1 - x0), y1},
                            {x0, y0 + t * (y1 - y0)},
                            {x1, y0 + t * (y1 - y0)}};
        for (k = 0; k < 4; k++) {
            pc_algo_lens_sample s;
            if (!pc_algo_lens_map_sample(&m, pts[k][0], pts[k][1], &s) ||
                !(s.p[0] >= x0 - 0.5 && s.p[0] <= x1 + 0.5 && s.p[1] >= y0 - 0.5 && s.p[1] <= y1 + 0.5)) {
                return false;
            }
        }
    }
    return true;
}

/* The reference grows the bound by 1.25x until it covers or passes 400 %, which never ends for a
 * scale <= 0; 64 steps take any positive scale above 1e-4 % past 400 %. */
#define AUTO_SCALE_MAX_GROW 64

double pc_algo_lens_auto_scale(const pc_algo_lens_correction *lc, pc_rect frame)
{
    double lo = lc->scale, hi = lc->scale;
    int i;
    if (covers(lc, frame, lc->scale)) {
        /* Shrink while it still covers (never below the user's 100 % baseline when it covers). */
        return lc->scale;
    }
    for (i = 0; i < AUTO_SCALE_MAX_GROW && !covers(lc, frame, hi) && hi < 400.0; i++) {
        hi *= 1.25;
    }
    for (i = 0; i < 30; i++) {
        double mid = (lo + hi) / 2.0;
        if (covers(lc, frame, mid)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

double pc_algo_lens_straighten_angle(const double a[2], const double b[2])
{
    double deg = atan2(b[1] - a[1], b[0] - a[0]) * (180.0 / PI_D);
    /* Nearest multiple of 90 degrees. */
    double target = round(deg / 90.0) * 90.0;
    return -(deg - target);
}

/* ---- remap ---- */

#define TILE 128

typedef struct remap_ctx {
    const pc_surface *src;
    pc_rect frame, area;
    pc_algo_lens_edge edge;
    pc_algo_lens_map_fn map;
    void *mctx;
    pc_pixel_format sfmt, ofmt;
    size_t n_in, n_out, cc, tiles_x;
    bool rgb, subtractive, lab;
    float edge_px[PC_RASTER_MAX_CHANNELS + 1];
    size_t base;     /* first tile of the running group */
    float **results; /* per tile, tw * th * n_out */
} remap_ctx;

static pc_rect tile_rect(pc_rect area, size_t tiles_x, size_t k)
{
    int64_t tx = (int64_t)area.x0 + (int64_t)(k % tiles_x) * TILE;
    int64_t ty = (int64_t)area.y0 + (int64_t)(k / tiles_x) * TILE;
    return pc_rect_make((int32_t)tx, (int32_t)ty, (int32_t)(tx + TILE < area.x1 ? tx + TILE : area.x1),
                        (int32_t)(ty + TILE < area.y1 ? ty + TILE : area.y1));
}

typedef struct win_t {
    pc_rect r;
    size_t ww, n_in;
    const float *buf;
    double fx0, fy0, fx1, fy1;
} win_t;

static float bicubic(const win_t *w, size_t c, const double p[2])
{
    /* Pixel centres at +0.5; clamp to the frame (edge extension). */
    double x = pc_clampd(p[0] - 0.5, w->fx0, w->fx1 - 1.0), y = pc_clampd(p[1] - 0.5, w->fy0, w->fy1 - 1.0);
    double xi = floor(x), yi = floor(y);
    float wx[4], wy[4], acc = 0.0f;
    int i, j;
    pc_algo_photo_catmull_rom(pc_f64_to_f32(x - xi), wx);
    pc_algo_photo_catmull_rom(pc_f64_to_f32(y - yi), wy);
    for (j = 0; j < 4; j++) {
        int32_t yy =
            pc_clamp_i32(pc_i32_wrapping_add(pc_f64_to_i32(yi), j - 1), w->r.y0, w->r.y1 - 1) - w->r.y0;
        for (i = 0; i < 4; i++) {
            int32_t xx =
                pc_clamp_i32(pc_i32_wrapping_add(pc_f64_to_i32(xi), i - 1), w->r.x0, w->r.x1 - 1) - w->r.x0;
            acc += wx[i] * wy[j] * w->buf[((size_t)yy * w->ww + (size_t)xx) * w->n_in + c];
        }
    }
    return acc;
}

/* All n channels sampled at p together (lens::sample_channels): the 16 tap addresses and weights
 * once, each channel keeping bicubic()'s summation order. False when not applicable. */
static bool bicubic_all(const win_t *w, const double p[2], float out[8])
{
    size_t n = w->n_in, c;
    double x, y, xi, yi;
    float wx[4], wy[4];
    int i, j;
    if (n == 0 || n > 8 || n == 6 || n == 7 || pc_rect_is_empty(w->r) || w->fx0 >= w->fx1 ||
        w->fy0 >= w->fy1 || !isfinite(p[0]) || !isfinite(p[1])) {
        return false;
    }
    x = pc_clampd(p[0] - 0.5, w->fx0, w->fx1 - 1.0);
    y = pc_clampd(p[1] - 0.5, w->fy0, w->fy1 - 1.0);
    xi = floor(x);
    yi = floor(y);
    pc_algo_photo_catmull_rom(pc_f64_to_f32(x - xi), wx);
    pc_algo_photo_catmull_rom(pc_f64_to_f32(y - yi), wy);
    for (c = 0; c < 8; c++) {
        out[c] = 0.0f;
    }
    for (j = 0; j < 4; j++) {
        int32_t yy =
            pc_clamp_i32(pc_i32_wrapping_add(pc_f64_to_i32(yi), j - 1), w->r.y0, w->r.y1 - 1) - w->r.y0;
        for (i = 0; i < 4; i++) {
            int32_t xx =
                pc_clamp_i32(pc_i32_wrapping_add(pc_f64_to_i32(xi), i - 1), w->r.x0, w->r.x1 - 1) - w->r.x0;
            const float *px = w->buf + ((size_t)yy * w->ww + (size_t)xx) * n;
            float weight = wx[i] * wy[j];
            for (c = 0; c < n; c++) {
                out[c] += weight * px[c];
            }
        }
    }
    return true;
}

static bool same_point(const double a[2], const double b[2])
{
    return a[0] == b[0] && a[1] == b[1];
}

static pc_status remap_tile(remap_ctx *rc, size_t k, pc_error *err)
{
    pc_rect t = tile_rect(rc->area, rc->tiles_x, k);
    size_t tw = pc_rect_width(t), th = pc_rect_height(t), n = tw * th, i, c;
    pc_algo_lens_sample *samples = NULL;
    bool *ok = NULL;
    double bx0 = 1.7976931348623157e308, by0 = 1.7976931348623157e308, bx1 = -1.7976931348623157e308,
           by1 = -1.7976931348623157e308;
    float *data = NULL, *buf = NULL;
    size_t buflen = 0;
    win_t w;
    pc_status st = PC_OK;
    samples = pc_malloc_array(n, sizeof *samples);
    ok = pc_malloc_array(n, sizeof *ok);
    data = pc_calloc_array(n * rc->n_out, sizeof *data);
    if (samples == NULL || ok == NULL || data == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }
    for (i = 0; i < n; i++) {
        ok[i] = rc->map(rc->mctx, (double)t.x0 + (double)(i % tw) + 0.5,
                        (double)t.y0 + (double)(i / tw) + 0.5, &samples[i]);
        if (ok[i]) {
            const double *ps[3] = {samples[i].p, samples[i].pr, samples[i].pb};
            int q;
            for (q = 0; q < 3; q++) {
                bx0 = pc_rs_min(bx0, ps[q][0]);
                by0 = pc_rs_min(by0, ps[q][1]);
                bx1 = pc_rs_max(bx1, ps[q][0]);
                by1 = pc_rs_max(by1, ps[q][1]);
            }
        }
    }
    /* Source window: bbox of every sample, clamped to the frame. */
    if (bx0 > bx1) {
        w.r = PC_RECT_EMPTY;
    } else {
        pc_rect f = rc->frame;
        w.r = pc_rect_make(pc_clamp_i32(pc_f64_to_i32(floor(bx0) - 3.0), f.x0, f.x1),
                           pc_clamp_i32(pc_f64_to_i32(floor(by0) - 3.0), f.y0, f.y1),
                           pc_clamp_i32(pc_f64_to_i32(ceil(bx1) + 4.0), f.x0, f.x1),
                           pc_clamp_i32(pc_f64_to_i32(ceil(by1) + 4.0), f.y0, f.y1));
    }
    w.ww = pc_rect_width(w.r);
    w.n_in = rc->n_in;
    if (!pc_rect_is_empty(w.r)) {
        if ((st = pc_surface_read_region(rc->src, w.r, &buf, &buflen, err)) != PC_OK) {
            goto done;
        }
        /* Premultiply colour by alpha so transparent pixels don't bleed. */
        if (rc->sfmt.alpha) {
            size_t q;
            for (q = 0; q + rc->n_in <= buflen; q += rc->n_in) {
                float a = buf[q + rc->n_in - 1];
                for (c = 0; c + 1 < rc->n_in; c++) {
                    buf[q + c] *= a;
                }
            }
        }
    }
    w.buf = buf;
    w.fx0 = rc->frame.x0;
    w.fy0 = rc->frame.y0;
    w.fx1 = rc->frame.x1;
    w.fy1 = rc->frame.y1;
    for (i = 0; i < n; i++) {
        float *o = data + i * rc->n_out, alpha, inside, col[8] = {0}, a_out, center[8];
        bool have_center;
        const pc_algo_lens_sample *s = &samples[i];
        if (!ok[i] || pc_rect_is_empty(w.r)) {
            memcpy(o, rc->edge_px, rc->n_out * sizeof *o);
            continue;
        }
        /* Inside-frame weight (1 px soft edge) unless edges extend. */
        if (rc->edge.kind == PC_ALGO_LENS_EDGE_EXTENSION) {
            inside = 1.0f;
        } else {
            double fx = pc_clampd(pc_rs_min(s->p[0] - w.fx0, w.fx1 - s->p[0]) + 0.5, 0.0, 1.0);
            double fy = pc_clampd(pc_rs_min(s->p[1] - w.fy0, w.fy1 - s->p[1]) + 0.5, 0.0, 1.0);
            inside = pc_f64_to_f32(fx * fy);
        }
        /* Colour and coverage usually sample the same location: prepare the taps once. */
        have_center = (!rc->rgb || (same_point(s->pr, s->p) && same_point(s->pb, s->p))) &&
                      bicubic_all(&w, s->p, center);
        alpha = rc->sfmt.alpha ? pc_clampf(have_center && rc->n_in - 1 < 8 && same_point(s->p, s->p)
                                               ? center[rc->n_in - 1]
                                               : bicubic(&w, rc->n_in - 1, s->p),
                                           0.0f, 1.0f)
                               : 1.0f;
        for (c = 0; c < rc->cc && c < 8; c++) {
            const double *p = rc->rgb && c == 0 ? s->pr : (rc->rgb && c == 2 ? s->pb : s->p);
            float pm = have_center && same_point(p, s->p) ? center[c] : bicubic(&w, c, p), straight, v;
            straight = rc->sfmt.alpha ? (alpha > 1e-6f ? pm / alpha : 0.0f) : pm;
            if (rc->subtractive) {
                v = 1.0f - (1.0f - straight) * s->gain;
            } else if (rc->lab) {
                v = c == 0 ? straight * s->gain : straight;
            } else {
                v = straight * s->gain;
            }
            if (rc->sfmt.sample != PC_SAMPLE_TYPE_F32) {
                v = pc_clampf(v, 0.0f, 1.0f);
            }
            col[c] = v;
        }
        a_out = alpha * inside;
        if (rc->edge.kind == PC_ALGO_LENS_EDGE_COLOR && inside < 1.0f) {
            /* Blend towards the edge colour by the outside share. */
            float ea = rc->ofmt.alpha ? rc->edge_px[rc->n_out - 1] : 1.0f;
            float oa = alpha * inside + ea * (1.0f - inside);
            for (c = 0; c < rc->cc; c++) {
                float mixed = col[c] * alpha * inside + rc->edge_px[c] * ea * (1.0f - inside);
                o[c] = oa > 1e-6f ? mixed / oa : 0.0f;
            }
            if (rc->ofmt.alpha) {
                o[rc->n_out - 1] = oa;
            }
        } else {
            memcpy(o, col, rc->cc * sizeof *o);
            if (rc->ofmt.alpha) {
                o[rc->n_out - 1] = a_out;
            }
        }
    }
    rc->results[k] = data;
    data = NULL;
done:
    free(samples), free(ok), free(data), free(buf);
    return st;
}

static pc_status remap_tiles(void *vc, size_t chunk, size_t k0, size_t k1)
{
    remap_ctx *rc = vc;
    size_t k;
    (void)chunk;
    for (k = k0; k < k1; k++) {
        pc_status st = remap_tile(rc, rc->base + k, NULL);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

pc_status pc_algo_lens_remap(const pc_surface *src, pc_rect frame, pc_rect out_area, pc_algo_lens_edge edge,
                             pc_algo_lens_map_fn map, void *ctx, pc_surface *out, pc_error *err)
{
    remap_ctx rc;
    pc_surface res;
    size_t tiles_y, ntiles, k;
    pc_status st = PC_OK;
    memset(&rc, 0, sizeof rc);
    rc.sfmt = pc_surface_format(src);
    rc.ofmt = rc.sfmt;
    if (edge.kind == PC_ALGO_LENS_EDGE_TRANSPARENCY && !rc.sfmt.alpha) {
        rc.ofmt.alpha = true;
    }
    pc_surface_init(&res, rc.ofmt);
    if (pc_rect_is_empty(out_area) || pc_rect_is_empty(frame)) {
        *out = res;
        return PC_OK;
    }
    rc.src = src;
    rc.frame = frame;
    rc.area = out_area;
    rc.edge = edge;
    rc.map = map;
    rc.mctx = ctx;
    rc.n_in = pc_pixel_format_channels(rc.sfmt);
    rc.n_out = pc_pixel_format_channels(rc.ofmt);
    rc.cc = pc_color_mode_channels(rc.sfmt.mode);
    rc.rgb = (rc.sfmt.mode == PC_COLOR_MODE_RGB || rc.sfmt.mode == PC_COLOR_MODE_INDEXED) && rc.cc == 3;
    rc.subtractive = rc.sfmt.mode == PC_COLOR_MODE_CMYK;
    rc.lab = rc.sfmt.mode == PC_COLOR_MODE_LAB;
    if (edge.kind == PC_ALGO_LENS_EDGE_COLOR) {
        (void)pc_from_rgba(rc.ofmt, edge.color, rc.edge_px);
    }
    rc.tiles_x = (pc_rect_width(out_area) + TILE - 1) / TILE;
    tiles_y = (pc_rect_height(out_area) + TILE - 1) / TILE;
    if (!pc_size_mul(rc.tiles_x, tiles_y, &ntiles) || ntiles > PC_MAX_ALLOC / sizeof(float *)) {
        pc_surface_fini(&res);
        return pc_fail(err, PC_ERR_LIMIT, "output area too large");
    }
    rc.results = pc_calloc_array(ntiles, sizeof *rc.results);
    if (rc.results == NULL) {
        pc_surface_fini(&res);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    /* Tiles run in bounded groups so the held results stay small. */
    for (k = 0; k < ntiles && st == PC_OK; k += 256) {
        size_t end = k + 256 < ntiles ? k + 256 : ntiles, q;
        rc.base = k;
        st = pc_algo_par_for(end - k, 1, remap_tiles, &rc);
        if (st != PC_OK) {
            (void)pc_fail(err, st, st == PC_ERR_NOMEM ? "out of memory" : "lens remap failed");
            break;
        }
        for (q = k; q < end && st == PC_OK; q++) {
            pc_rect t = tile_rect(out_area, rc.tiles_x, q);
            st = pc_surface_write_region(&res, t, rc.results[q],
                                         (size_t)pc_rect_width(t) * pc_rect_height(t) * rc.n_out, err);
            free(rc.results[q]);
            rc.results[q] = NULL;
        }
    }
    for (k = 0; k < ntiles; k++) {
        free(rc.results[k]);
    }
    free(rc.results);
    if (st != PC_OK) {
        pc_surface_fini(&res);
        return st;
    }
    pc_surface_prune(&res);
    *out = res;
    return PC_OK;
}

static bool lens_map_cb(void *ctx, double x, double y, pc_algo_lens_sample *out)
{
    return pc_algo_lens_map_sample(ctx, x, y, out);
}

pc_status pc_algo_lens_correct(const pc_surface *src, pc_rect frame, const pc_algo_lens_correction *lc,
                               pc_surface *out, pc_error *err)
{
    pc_algo_lens_map m;
    pc_algo_lens_map_init(&m, lc, frame);
    return pc_algo_lens_remap(src, frame, frame, lc->edge, lens_map_cb, &m, out, err);
}
