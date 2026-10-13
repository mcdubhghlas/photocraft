/* A per-file colour rendering ("camera look") fitted to the camera's own embedded JPEG, for raw files
 * without colour calibration (port of crates/raw/src/look.rs; method after LightCraft's prose
 * description, an independent implementation against this module's pipeline).
 *
 * The sensor data and the camera JPEG are reduced to a common grid of at most PC_RAW_PROXY_EDGE
 * cells on the long edge (the upright frame). Each cell pairs the white-balanced camera RGB c with
 * the JPEG's colour in linear ProPhoto y; the model is the develop pipeline itself, y ~ f(M c), M a
 * 3x3 matrix scaled so the balanced white keeps the exposure gain's luminance, f one monotone curve
 * per channel. They are fitted by alternating: f by binned (square-root domain) pool-adjacent-
 * violators isotonic regression, M by ridge-regularised weighted least squares of f^-1(y) on c with
 * Tukey biweights. The look is accepted only when its matrix is plausible and it lowers the
 * held-out mean CIE76 dE to at most 85 % of the neutral fallback's.
 *
 * Float operations follow the reference one for one (sum orders, chunked parallel sums combined in
 * order), so a fit is bit-identical to the reference's. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rfmt.h"
#include "base/rmath.h"
#include "raw/internal.h"

/* Samples per parallel chunk of the least-squares accumulation. */
#define CHUNK 4096
/* Bins of the tone-curve fit, uniform in sqrt(x). */
#define CURVE_BINS 32
static const double RIDGES[3] = {0.01, 0.05, 0.15};
/* The Y row of linear ProPhoto -> XYZ, for luminance. */
static const double PRO_Y[3] = {0.288040, 0.711874, 0.000086};

/* ---- the tone curve ---- */

/* Rust's partition_point on knots[.][col] < t (the knots are sorted on col). */
static size_t lower_bound(const double (*k)[2], size_t n, int col, double t)
{
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (k[mid][col] < t) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static size_t clamp_index(size_t i, size_t n)
{
    size_t hi = n > 1 ? n - 1 : 1;
    return i < 1 ? 1 : i > hi ? hi : i;
}

bool pc_raw_look_curve_new(const double (*knots)[2], size_t n, pc_raw_look_curve *out)
{
    size_t i;
    if (n < 2 || n > PC_RAW_TONE_MAX_KNOTS) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (!isfinite(knots[i][0]) || !isfinite(knots[i][1]) || knots[i][0] < 0.0 || knots[i][0] > 1.0 ||
            knots[i][1] < 0.0 || knots[i][1] > 1.0) {
            return false;
        }
        if (i > 0 && !(knots[i][0] > knots[i - 1][0] && knots[i][1] >= knots[i - 1][1])) {
            return false;
        }
    }
    if (!(knots[0][0] == 0.0 && knots[0][1] == 0.0 && knots[n - 1][0] == 1.0 && knots[n - 1][1] == 1.0)) {
        return false;
    }
    out->n = n;
    for (i = 0; i < n; i++) {
        out->knots[i][0] = sqrt(knots[i][0]);
        out->knots[i][1] = sqrt(knots[i][1]);
    }
    return true;
}

void pc_raw_look_curve_knots(const pc_raw_look_curve *c, double (*out)[2])
{
    size_t i;
    for (i = 0; i < c->n; i++) {
        out[i][0] = c->knots[i][0] * c->knots[i][0];
        out[i][1] = c->knots[i][1] * c->knots[i][1];
    }
}

/* Piecewise-linear interpolation of column to at t on column from. */
static double interp(const double (*k)[2], size_t n, double t, int from, int to)
{
    size_t i = clamp_index(lower_bound(k, n, from, t), n);
    double d;
    if (i >= n) {
        return t;
    }
    d = k[i][from] - k[i - 1][from];
    if (d <= 0.0) {
        return k[i - 1][to];
    }
    return k[i - 1][to] + (k[i][to] - k[i - 1][to]) * pc_clampd((t - k[i - 1][from]) / d, 0.0, 1.0);
}

double pc_raw_look_curve_eval(const pc_raw_look_curve *c, double x)
{
    double t = isnan(x) ? 0.0 : sqrt(pc_clampd(x, 0.0, 1.0));
    double s = interp((const double (*)[2])c->knots, c->n, t, 0, 1);
    return s * s;
}

/* (x, f'(x)) for x = f^-1(y), from one segment lookup. */
static void inverse_and_slope(const pc_raw_look_curve *c, double y, double *x, double *d)
{
    double t = isnan(y) ? 0.0 : sqrt(pc_clampd(y, 0.0, 1.0));
    const double (*k)[2] = (const double (*)[2])c->knots;
    size_t i = clamp_index(lower_bound(k, c->n, 1, t), c->n);
    double dx;
    double dy;
    double sx;
    double slope;
    if (i >= c->n) {
        *x = y;
        *d = 1.0;
        return;
    }
    dx = k[i][0] - k[i - 1][0];
    dy = k[i][1] - k[i - 1][1];
    if (dy <= 0.0 || dx <= 0.0) {
        *x = k[i - 1][0] * k[i - 1][0];
        *d = 0.0;
        return;
    }
    sx = k[i - 1][0] + dx * pc_clampd((t - k[i - 1][1]) / dy, 0.0, 1.0);
    slope = dy / dx;
    *d = sx > 1e-9 ? slope * t / sx : slope * slope;
    *x = sx * sx;
}

bool rx_look_is_usable(const pc_raw_camera_look *l)
{
    pc_raw_mat3 inv;
    double one[3] = {1.0, 1.0, 1.0};
    double w[3];
    int i;
    for (i = 0; i < 9; i++) {
        double v = l->to_xyz.m[i / 3][i % 3];
        if (!isfinite(v) || !(fabs(v) < 64.0)) {
            return false;
        }
    }
    if (!rx_invert(&l->to_xyz, &inv)) {
        return false;
    }
    rx_apply(&l->to_xyz, one, w);
    return w[1] > 1e-3;
}

/* ---- rejection text ---- */

#define REJECT_PREFIX "colour not fitted to the camera JPEG: "

static pc_status reject(pc_error *err, const char *why)
{
    return pc_fail(err, PC_ERR_INVALID, REJECT_PREFIX "%s", why);
}

/* Rust's {:.N} for an f64 (NaN and inf spelt its way). */
static void fmt_prec(char *out, size_t cap, double v, int prec)
{
    if (isnan(v)) {
        (void)snprintf(out, cap, "NaN");
    } else if (isinf(v)) {
        (void)snprintf(out, cap, "%sinf", v < 0 ? "-" : "");
    } else {
        (void)snprintf(out, cap, "%.*f", prec, v);
    }
}

/* Rust's {:?} for an f64: plain decimal (with ".0") for 1e-4 <= |v| < 1e16 or 0, else the shortest
 * digits in exponent form ("1e-7", "-2.5e16"). */
static void fmt_debug(char *out, size_t cap, double v)
{
    double a = fabs(v);
    if (isnan(v) || isinf(v) || a == 0.0 || (a >= 1e-4 && a < 1e16)) {
        char t[PC_RUST_F64_TEXT_MAX];
        size_t n = pc_rust_display_f64(v, t);
        if (isfinite(v) && strchr(t, '.') == NULL && n + 2 < sizeof t) {
            memcpy(t + n, ".0", 3);
        }
        (void)snprintf(out, cap, "%.60s", t);
        return;
    } else {
        char e[64];
        int p;
        char *x;
        char mant[40];
        for (p = 0; p < 17; p++) {
            (void)snprintf(e, sizeof e, "%.*e", p, v);
            if (strtod(e, NULL) == v) {
                break;
            }
        }
        x = strchr(e, 'e');
        if (x == NULL) {
            (void)snprintf(out, cap, "%s", e);
            return;
        }
        (void)snprintf(mant, sizeof mant, "%.*s", (int)(x - e), e);
        (void)snprintf(out, cap, "%se%d", mant, atoi(x + 1));
    }
}

/* ---- the fit grid ---- */

typedef struct cell {
    double cam[3];      /* white-balanced camera RGB, 0..1 */
    bool cam_clipped;   /* any sampled sensor value at or near clipping */
    double rgb[3];      /* the JPEG's colour, linear ProPhoto */
    double lum;         /* luminance (Y, D50) of rgb */
    double ref_clipped; /* fraction of the JPEG pixels with a channel at 250 or more */
    double edge_weight; /* weight from the JPEG's local contrast */
} cell;

typedef struct scell {
    double cam[3];
    bool clipped;
    bool some;
} scell;

typedef struct rcell {
    double srgb[3];
    double clipped;
    double edge;
    bool some;
} rcell;

typedef struct grid_ctx {
    const pc_raw_sensor *s;
    const pc_raw_reference *r;
    double mult[3];
    size_t gw, gh;       /* sensor-orientation grid */
    size_t gw_up, gh_up; /* upright grid */
    double lut[256];
    scell *sensor;
    rcell *ref;
} grid_ctx;

/* Mean white-balanced camera RGB over the box (sampling at most about 8 x 8 Bayer quads), and
 * whether any sample was near clipping. */
static scell sensor_cell(const grid_ctx *g, size_t x0, size_t x1, size_t y0, size_t y1)
{
    const pc_raw_sensor *s = g->s;
    size_t sx = ((x1 > x0 ? x1 - x0 : 0) + 15) / 16;
    size_t sy = ((y1 > y0 ? y1 - y0 : 0) + 15) / 16;
    double sum[3] = {0.0, 0.0, 0.0};
    uint32_t n[3] = {0, 0, 0};
    scell out;
    size_t x;
    size_t y;
    int c;
    static const size_t D[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    sx = (sx < 1 ? 1 : sx) * 2;
    sy = (sy < 1 ? 1 : sy) * 2;
    memset(&out, 0, sizeof out);
    for (y = y0; y < y1; y += sy) {
        for (x = x0; x < x1; x += sx) {
            int q;
            for (q = 0; q < 4; q++) {
                size_t px = x + D[q][0];
                size_t py = y + D[q][1];
                int k;
                if (px >= x1 || py >= y1) {
                    continue;
                }
                for (k = 0; k < 3; k++) {
                    size_t ch;
                    double v;
                    double b;
                    if (s->has_cfa && s->samples == 1) {
                        uint8_t col;
                        if (k > 0) {
                            break;
                        }
                        col = pc_raw_cfa_color(&s->cfa, px, py);
                        ch = col < 2 ? col : 2;
                        v = (double)rx_scale_value(s, px, py, 0);
                    } else {
                        ch = (size_t)k;
                        v = (double)rx_scale_value(s, px, py, (size_t)k);
                    }
                    b = v * g->mult[ch];
                    out.clipped = out.clipped || v >= 0.98 || b >= 0.98;
                    sum[ch] += pc_clampd(b, 0.0, 1.0);
                    n[ch]++;
                }
            }
        }
    }
    out.some = n[0] > 0 && n[1] > 0 && n[2] > 0;
    for (c = 0; c < 3 && out.some; c++) {
        out.cam[c] = sum[c] / (double)n[c];
    }
    return out;
}

/* Mean linear sRGB of a reference box, the fraction of its pixels with a clipped channel and an
 * edge weight from its luminance spread. */
static rcell reference_cell(const grid_ctx *g, size_t x0, size_t x1, size_t y0, size_t y1)
{
    const pc_raw_reference *r = g->r;
    size_t rw = r->width;
    double sum[3] = {0.0, 0.0, 0.0};
    double l1 = 0.0;
    double l2 = 0.0;
    size_t n = 0;
    size_t clipped = 0;
    rcell out;
    size_t x;
    size_t y;
    double nf;
    double mean;
    double sd;
    double cv;
    double q;
    int c;
    memset(&out, 0, sizeof out);
    for (y = y0; y < y1; y++) {
        if ((y * rw + x1) * 3 > r->len || x1 < x0) {
            return out;
        }
        for (x = x0; x < x1; x++) {
            const uint8_t *p = r->rgb + (y * rw + x) * 3;
            double v[3];
            double l;
            for (c = 0; c < 3; c++) {
                v[c] = g->lut[p[c]];
                sum[c] += v[c];
            }
            l = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
            l1 += l;
            l2 += l * l;
            n++;
            clipped += (p[0] >= 250 || p[1] >= 250 || p[2] >= 250) ? 1 : 0;
        }
    }
    if (n == 0) {
        return out;
    }
    nf = (double)n;
    mean = l1 / nf;
    sd = sqrt(pc_rs_max_k(l2 / nf - mean * mean, 0.0));
    cv = sd / (mean + 0.01);
    q = cv / 0.15;
    for (c = 0; c < 3; c++) {
        out.srgb[c] = sum[c] / nf;
    }
    out.clipped = (double)clipped / nf;
    out.edge = 1.0 / (1.0 + q * q);
    out.some = true;
    return out;
}

static pc_status sensor_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    grid_ctx *g = ctx;
    pc_raw_rect c = g->s->crop;
    size_t j;
    (void)chunk;
    for (j = begin; j < end; j++) {
        size_t y0 = c.y + j * c.height / g->gh;
        size_t y1 = c.y + (j + 1) * c.height / g->gh;
        size_t i;
        for (i = 0; i < g->gw; i++) {
            size_t x0 = c.x + i * c.width / g->gw;
            size_t x1 = c.x + (i + 1) * c.width / g->gw;
            g->sensor[j * g->gw + i] = sensor_cell(g, x0, x1, y0, y1);
        }
    }
    return PC_OK;
}

static pc_status ref_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    grid_ctx *g = ctx;
    size_t rw = g->r->width;
    size_t rh = g->r->height;
    size_t j;
    (void)chunk;
    for (j = begin; j < end; j++) {
        size_t y0 = j * rh / g->gh_up;
        size_t y1 = (j + 1) * rh / g->gh_up;
        size_t i;
        y1 = y1 > y0 + 1 ? y1 : y0 + 1;
        for (i = 0; i < g->gw_up; i++) {
            size_t x0 = i * rw / g->gw_up;
            size_t x1 = (i + 1) * rw / g->gw_up;
            x1 = x1 > x0 + 1 ? x1 : x0 + 1;
            g->ref[j * g->gw_up + i] = reference_cell(g, x0, x1 < rw ? x1 : rw, y0, y1 < rh ? y1 : rh);
        }
    }
    return PC_OK;
}

static double srgb_to_linear(double v)
{
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

/* The upright fit grid: *gw_out cells across, *cells_out row-major (malloc'd). */
static pc_status grid(const pc_raw_sensor *s, const pc_raw_develop_options *opts, const pc_raw_reference *r,
                      size_t *gw_out, cell **cells_out, size_t *ncells, pc_error *err)
{
    pc_raw_rect c = s->crop;
    size_t need;
    size_t rw = r->width;
    size_t rh = r->height;
    size_t n3;
    bool swap;
    size_t uw;
    size_t uh;
    double aspect;
    size_t lng;
    size_t edge;
    grid_ctx g;
    double neutral[3];
    pc_raw_strings warnings = {NULL, 0};
    pc_raw_mat3 to_pro;
    pc_raw_mat3 pro_to_xyz;
    pc_raw_mat3 a;
    pc_raw_mat3 b;
    cell *cells;
    size_t i;
    size_t j;
    pc_status st;
    pc_error e2;
    char why[200];
    if (pc_raw_rect_is_empty(c) || c.x > SIZE_MAX - c.width || c.x + c.width > s->width ||
        c.y > SIZE_MAX - c.height || c.y + c.height > s->height ||
        !pc_size_mul3(s->width, s->height, s->samples, &need) || s->ndata < need || s->data == NULL) {
        return reject(err, "the crop lies outside the sensor data");
    }
    if (!(s->samples == 3 || (s->samples == 1 && s->has_cfa))) {
        return reject(err, "unsupported sensor layout");
    }
    if (rw < 32 || rh < 32 || !pc_size_mul3(rw, rh, 3, &n3) || n3 != r->len || r->rgb == NULL) {
        return reject(err, "the reference image is too small or its buffer does not match its size");
    }
    swap = s->orientation >= 5 && s->orientation <= 8;
    uw = swap ? c.height : c.width;
    uh = swap ? c.width : c.height;
    aspect = ((double)rw / (double)rh) / ((double)uw / (double)uh);
    if (!(isfinite(aspect) && fabs(log(aspect)) <= 0.03)) {
        (void)snprintf(why, sizeof why,
                       "the reference (%zux%zu) does not have the raw frame's shape (%zux%zu)", rw, rh, uw,
                       uh);
        return reject(err, why);
    }
    /* Cells at least 4 sensor pixels and 2 reference pixels across. */
    lng = uw > uh ? uw : uh;
    edge = PC_RAW_PROXY_EDGE;
    edge = lng / 4 < edge ? lng / 4 : edge;
    edge = (rw > rh ? rw : rh) / 2 < edge ? (rw > rh ? rw : rh) / 2 : edge;
    g.gw_up = (uw * edge + lng - 1) / lng;
    g.gh_up = (uh * edge + lng - 1) / lng;
    g.gw_up = g.gw_up < 1 ? 1 : g.gw_up;
    g.gh_up = g.gh_up < 1 ? 1 : g.gh_up;
    if (g.gw_up < 16 || g.gh_up < 16) {
        return reject(err, "the image is too small to fit");
    }
    memset(&e2, 0, sizeof e2);
    st = rx_resolve_neutral(s, opts, &warnings, neutral, &e2);
    pc_raw_strings_fini(&warnings);
    if (st == PC_ERR_NOMEM) {
        return rx_nomem(err);
    }
    if (st != PC_OK) {
        return reject(err, e2.msg);
    }
    g.s = s;
    g.r = r;
    for (i = 0; i < 3; i++) {
        g.mult[i] = 1.0 / neutral[i];
    }
    g.gw = swap ? g.gh_up : g.gw_up;
    g.gh = swap ? g.gw_up : g.gh_up;
    for (i = 0; i < 256; i++) {
        g.lut[i] = srgb_to_linear((double)i / 255.0);
    }
    g.sensor = pc_malloc_array(g.gw * g.gh, sizeof *g.sensor);
    g.ref = pc_malloc_array(g.gw_up * g.gh_up, sizeof *g.ref);
    cells = pc_malloc_array(g.gw_up * g.gh_up, sizeof *cells);
    if (g.sensor == NULL || g.ref == NULL || cells == NULL) {
        free(g.sensor);
        free(g.ref);
        free(cells);
        return rx_nomem(err);
    }
    (void)pc_pool_for(pc_pool_global(), g.gh, 1, sensor_rows, &g);
    (void)pc_pool_for(pc_pool_global(), g.gh_up, 1, ref_rows, &g);
    a = rx_xyz_d50_to_prophoto();
    b = rx_srgb_to_xyz_d50();
    to_pro = rx_mul(&a, &b);
    pro_to_xyz = rx_rgb_to_xyz(RX_ROMM, RX_D50_XY);
    for (j = 0; j < g.gh_up; j++) {
        for (i = 0; i < g.gw_up; i++) {
            cell *o = &cells[j * g.gw_up + i];
            const rcell *rc = &g.ref[j * g.gw_up + i];
            size_t sx;
            size_t sy;
            const scell *sc;
            double xyz[3];
            memset(o, 0, sizeof *o);
            rx_source_xy(s->orientation, g.gw, g.gh, i, j, &sx, &sy);
            sc = sy < g.gh && sx < g.gw ? &g.sensor[sy * g.gw + sx] : NULL;
            if (sc == NULL || !sc->some || !rc->some) {
                o->cam_clipped = true;
                continue;
            }
            memcpy(o->cam, sc->cam, sizeof o->cam);
            o->cam_clipped = sc->clipped;
            rx_apply(&to_pro, rc->srgb, o->rgb);
            rx_apply(&pro_to_xyz, o->rgb, xyz);
            o->lum = xyz[1];
            o->ref_clipped = rc->clipped;
            o->edge_weight = rc->edge;
        }
    }
    free(g.sensor);
    free(g.ref);
    *gw_out = g.gw_up;
    *cells_out = cells;
    *ncells = g.gw_up * g.gh_up;
    return PC_OK;
}

/* ---- the fit ---- */

typedef struct sample {
    double cam[3];
    double rgb[3];
    double lum;
    double perceptual;
    double weight;
} sample;

typedef struct lab {
    pc_raw_mat3 to_xyz;
    double white[3];
} lab;

static void lab_new(lab *l)
{
    double one[3] = {1.0, 1.0, 1.0};
    l->to_xyz = rx_rgb_to_xyz(RX_ROMM, RX_D50_XY);
    rx_apply(&l->to_xyz, one, l->white);
}

static double labf(double t)
{
    return t > 216.0 / 24389.0 ? pc_cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0;
}

static void lab_of(const lab *l, const double rgb[3], double out[3])
{
    double xyz[3];
    double f[3];
    int k;
    rx_apply(&l->to_xyz, rgb, xyz);
    for (k = 0; k < 3; k++) {
        f[k] = labf(xyz[k] / l->white[k]);
    }
    out[0] = 116.0 * f[1] - 16.0;
    out[1] = 500.0 * (f[0] - f[1]);
    out[2] = 200.0 * (f[1] - f[2]);
}

static double delta_e(const lab *l, const double a[3], const double b[3])
{
    double p[3];
    double q[3];
    lab_of(l, a, p);
    lab_of(l, b, q);
    return sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) +
                (p[2] - q[2]) * (p[2] - q[2]));
}

/* The develop pipeline's output for one sample (clipped to 0..1 like the 16-bit encode). */
static void render(const pc_raw_mat3 *m, const pc_raw_look_curve *tone, const double cam[3], double out[3])
{
    int k;
    rx_apply(m, cam, out);
    for (k = 0; k < 3; k++) {
        double v = isfinite(out[k]) ? pc_clampd(out[k], 0.0, 1.0) : 0.0;
        out[k] = tone != NULL ? pc_raw_look_curve_eval(tone, v) : v;
    }
}

typedef struct de_ctx {
    const lab *lab;
    const sample *const *samples;
    size_t n;
    const pc_raw_mat3 *m;
    const pc_raw_look_curve *tone;
    double *parts;
} de_ctx;

static pc_status de_chunks(void *ctx, size_t chunk, size_t begin, size_t end)
{
    de_ctx *c = ctx;
    size_t ci;
    (void)chunk;
    for (ci = begin; ci < end; ci++) {
        size_t lo = ci * CHUNK;
        size_t hi = lo + CHUNK < c->n ? lo + CHUNK : c->n;
        double sum = -0.0;
        size_t i;
        for (i = lo; i < hi; i++) {
            double o[3];
            render(c->m, c->tone, c->samples[i]->cam, o);
            sum += delta_e(c->lab, o, c->samples[i]->rgb);
        }
        c->parts[ci] = sum;
    }
    return PC_OK;
}

/* Mean dE of the rendering against the JPEG (inf for no samples); NaN on out of memory. */
static double mean_delta_e(const lab *l, const sample *const *samples, size_t n, const pc_raw_mat3 *m,
                           const pc_raw_look_curve *tone, bool *nomem)
{
    de_ctx c;
    size_t nc = (n + CHUNK - 1) / CHUNK;
    double total = -0.0;
    size_t i;
    if (n == 0) {
        return INFINITY;
    }
    c.lab = l;
    c.samples = samples;
    c.n = n;
    c.m = m;
    c.tone = tone;
    c.parts = pc_malloc_array(nc, sizeof *c.parts);
    if (c.parts == NULL) {
        *nomem = true;
        return NAN;
    }
    (void)pc_pool_for(pc_pool_global(), nc, 1, de_chunks, &c);
    for (i = 0; i < nc; i++) {
        total += c.parts[i];
    }
    free(c.parts);
    return total / (double)n;
}

/* Error-weight scale turning a linear difference near luminance lum into roughly L* units. */
static double perceptual(double lum)
{
    return (116.0 / 3.0) * pow(pc_rs_max_k(lum, 0.0) + 0.008, -2.0 / 3.0);
}

/* A plausible white-balanced camera -> ProPhoto matrix (exposure removed). */
static bool plausible(const pc_raw_mat3 *rel)
{
    const double (*r)[3] = rel->m;
    double det;
    int i;
    int j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            double v = r[i][j];
            if (i == j ? !(v >= 0.2 && v <= 3.0) : !(fabs(v) <= 2.0)) {
                return false;
            }
        }
    }
    det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
          r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
          r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    for (i = 0; i < 3; i++) {
        double s = r[i][0] + r[i][1] + r[i][2];
        if (!(s >= 0.6 && s <= 1.6)) {
            return false;
        }
    }
    return det > 0.05;
}

/* Pearson correlation of (a, b) pairs (NaN without spread). */
static double correlation(const double (*p)[2], size_t n)
{
    double nf = (double)n;
    double sa = -0.0;
    double sb = -0.0;
    double ma;
    double mb;
    double sab = 0.0;
    double saa = 0.0;
    double sbb = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        sa += p[i][0];
    }
    for (i = 0; i < n; i++) {
        sb += p[i][1];
    }
    ma = sa / nf;
    mb = sb / nf;
    for (i = 0; i < n; i++) {
        sab += (p[i][0] - ma) * (p[i][1] - mb);
        saa += (p[i][0] - ma) * (p[i][0] - ma);
        sbb += (p[i][1] - mb) * (p[i][1] - mb);
    }
    return sab / sqrt(saa * sbb);
}

/* Fits the tone curve to (M cam, rgb) pairs of all channels by weighted bin means in the
 * square-root domain made monotone (pool adjacent violators). */
static bool fit_curve(const sample *s, const double *robust, size_t n, const pc_raw_mat3 *m,
                      pc_raw_look_curve *out)
{
    double bins[CURVE_BINS][3];
    double total = 0.0;
    double pts[CURVE_BINS][3];
    size_t np = 0;
    double blocks[CURVE_BINS][2];
    size_t bcount[CURVE_BINS];
    size_t nb = 0;
    double knots[CURVE_BINS + 2][2];
    size_t nk = 0;
    size_t i;
    size_t b;
    memset(bins, 0, sizeof bins);
    for (i = 0; i < n; i++) {
        double x[3];
        double w = s[i].weight * robust[i];
        int k;
        if (w <= 0.0) {
            continue;
        }
        rx_apply(m, s[i].cam, x);
        for (k = 0; k < 3; k++) {
            double xv = x[k];
            double yv = s[i].rgb[k];
            double sx;
            double sy;
            if (!(xv > 1e-5 && xv < 1.0 && isfinite(yv))) {
                continue;
            }
            sx = sqrt(xv);
            sy = sqrt(pc_clampd(yv, 0.0, 1.0));
            b = pc_f64_to_size(sx * (double)CURVE_BINS);
            b = b < CURVE_BINS - 1 ? b : CURVE_BINS - 1;
            bins[b][0] += w;
            bins[b][1] += w * sx;
            bins[b][2] += w * sy;
            total += w;
        }
    }
    if (isnan(total) || total <= 0.0) {
        return false;
    }
    for (b = 0; b < CURVE_BINS; b++) {
        if (bins[b][0] > total * 2e-4) {
            pts[np][0] = bins[b][0];
            pts[np][1] = bins[b][1] / bins[b][0];
            pts[np][2] = bins[b][2] / bins[b][0];
            np++;
        }
    }
    if (np < 4) {
        return false;
    }
    for (i = 0; i < np; i++) {
        blocks[nb][0] = pts[i][0];
        blocks[nb][1] = pts[i][2];
        bcount[nb] = 1;
        nb++;
        while (nb >= 2 && !(blocks[nb - 2][1] <= blocks[nb - 1][1])) {
            double w = blocks[nb - 2][0] + blocks[nb - 1][0];
            double y = (blocks[nb - 2][0] * blocks[nb - 2][1] + blocks[nb - 1][0] * blocks[nb - 1][1]) / w;
            size_t cnt = bcount[nb - 2] + bcount[nb - 1];
            nb -= 2;
            blocks[nb][0] = w;
            blocks[nb][1] = y;
            bcount[nb] = cnt;
            nb++;
        }
    }
    {
        size_t at = 0;
        for (b = 0; b < nb; b++) {
            size_t q;
            for (q = 0; q < bcount[b] && at + q < np; q++) {
                pts[at + q][2] = blocks[b][1];
            }
            at += bcount[b];
        }
    }
    knots[nk][0] = 0.0;
    knots[nk][1] = 0.0;
    nk++;
    for (i = 0; i < np; i++) {
        double x = pts[i][1] * pts[i][1];
        double y = pc_clampd(pts[i][2] * pts[i][2], 0.0, 1.0);
        double px = knots[nk - 1][0];
        double py = knots[nk - 1][1];
        if (x > px + 1e-6 && x < 1.0 - 1e-6) {
            knots[nk][0] = x;
            knots[nk][1] = pc_rs_max(y, py);
            nk++;
        }
    }
    knots[nk][0] = 1.0;
    knots[nk][1] = 1.0;
    nk++;
    return pc_raw_look_curve_new((const double (*)[2])knots, nk, out);
}

/* Per row: normal matrix, right-hand side, level. */
typedef struct normal {
    double a[3][3][3];
    double b[3][3];
    double l[3];
} normal;

typedef struct fm_ctx {
    const sample *s;
    const double *robust;
    size_t n;
    const pc_raw_look_curve *tone;
    normal *parts;
} fm_ctx;

static pc_status fm_chunks(void *ctx, size_t chunk, size_t begin, size_t end)
{
    fm_ctx *c = ctx;
    size_t ci;
    (void)chunk;
    for (ci = begin; ci < end; ci++) {
        normal *acc = &c->parts[ci];
        size_t lo = ci * CHUNK;
        size_t hi = lo + CHUNK < c->n ? lo + CHUNK : c->n;
        size_t q;
        memset(acc, 0, sizeof *acc);
        for (q = lo; q < hi; q++) {
            const double *cm = c->s[q].cam;
            double avg = (cm[0] + cm[1] + cm[2]) / 3.0;
            double level = avg * avg;
            int i;
            for (i = 0; i < 3; i++) {
                double y = c->s[q].rgb[i];
                double t;
                double slope;
                double qq;
                double w;
                int j;
                int k;
                if (!(y > 0.0 && y < 0.995)) {
                    continue;
                }
                inverse_and_slope(c->tone, y, &t, &slope);
                qq = slope * c->s[q].perceptual;
                w = c->s[q].weight * c->robust[q] * qq * qq;
                if (!(isfinite(w) && w > 0.0)) {
                    continue;
                }
                acc->l[i] += w * level;
                for (j = 0; j < 3; j++) {
                    for (k = 0; k < 3; k++) {
                        acc->a[i][j][k] += w * cm[j] * cm[k];
                    }
                    acc->b[i][j] += w * cm[j] * t;
                }
            }
        }
    }
    return PC_OK;
}

/* Weighted least squares for M from f^-1(rgb) on cam, ridge-pulled toward prior, then scaled so the
 * balanced white keeps luminance gain. false on out of memory. */
static bool fit_matrix(const sample *s, const double *robust, size_t n, const pc_raw_look_curve *tone,
                       const pc_raw_mat3 *prior, double gain, double ridge, pc_raw_mat3 *out)
{
    fm_ctx c;
    size_t nc = (n + CHUNK - 1) / CHUNK;
    pc_raw_mat3 m = *prior;
    double one[3] = {1.0, 1.0, 1.0};
    double wv[3];
    double white_y;
    size_t i;
    c.s = s;
    c.robust = robust;
    c.n = n;
    c.tone = tone;
    c.parts = pc_malloc_array(nc > 0 ? nc : 1, sizeof *c.parts);
    if (c.parts == NULL) {
        return false;
    }
    (void)pc_pool_for(pc_pool_global(), nc, 1, fm_chunks, &c);
    for (i = 0; i < 3; i++) {
        pc_raw_mat3 a;
        pc_raw_mat3 inv;
        double b[3] = {0.0, 0.0, 0.0};
        double level = 0.0;
        double lambda;
        size_t p;
        int j;
        int k;
        memset(&a, 0, sizeof a);
        for (p = 0; p < nc; p++) {
            for (j = 0; j < 3; j++) {
                for (k = 0; k < 3; k++) {
                    a.m[j][k] += c.parts[p].a[i][j][k];
                }
                b[j] += c.parts[p].b[i][j];
            }
            level += c.parts[p].l[i];
        }
        lambda = ridge * ridge * level + 1e-12;
        for (j = 0; j < 3; j++) {
            a.m[j][j] += lambda;
            b[j] += lambda * prior->m[i][j];
        }
        if (rx_invert(&a, &inv)) {
            double x[3];
            rx_apply(&inv, b, x);
            if (isfinite(x[0]) && isfinite(x[1]) && isfinite(x[2])) {
                memcpy(m.m[i], x, sizeof x);
            }
        }
    }
    free(c.parts);
    rx_apply(&m, one, wv);
    white_y = PRO_Y[0] * wv[0] + PRO_Y[1] * wv[1] + PRO_Y[2] * wv[2];
    *out = isfinite(white_y) && white_y > 1e-6 ? rx_scale(&m, gain / white_y) : *prior;
    return true;
}

static bool less_f64(void *ctx, size_t a, size_t b)
{
    const double *v = ctx;
    int64_t ia;
    int64_t ib;
    memcpy(&ia, &v[a], 8);
    memcpy(&ib, &v[b], 8);
    ia ^= (int64_t)((uint64_t)(ia >> 63) >> 1);
    ib ^= (int64_t)((uint64_t)(ib >> 63) >> 1);
    return ia < ib;
}

/* Tukey biweights from each sample's L*-scaled residual. */
static bool update_robust(const sample *s, double *robust, size_t n, const pc_raw_mat3 *m,
                          const pc_raw_look_curve *tone)
{
    double *errs = pc_malloc_array(n > 0 ? n : 1, sizeof *errs);
    double *sorted = pc_malloc_array(n > 0 ? n : 1, sizeof *sorted);
    size_t *idx = pc_malloc_array(n > 0 ? n : 1, sizeof *idx);
    size_t ns = 0;
    size_t i;
    double c;
    if (errs == NULL || sorted == NULL || idx == NULL) {
        free(errs);
        free(sorted);
        free(idx);
        return false;
    }
    for (i = 0; i < n; i++) {
        double o[3];
        double sum = -0.0;
        int k;
        render(m, tone, s[i].cam, o);
        for (k = 0; k < 3; k++) {
            sum += (o[k] - s[i].rgb[k]) * (o[k] - s[i].rgb[k]);
        }
        errs[i] = sqrt(sum) * s[i].perceptual;
        if (isfinite(errs[i])) {
            sorted[ns] = errs[i];
            idx[ns] = ns;
            ns++;
        }
    }
    if (ns > 0) {
        size_t mid = ns / 2;
        rx_select_nth(idx, ns, mid, less_f64, sorted);
        c = 4.685 * pc_rs_max_k(1.4826 * sorted[idx[mid]], 0.5);
        for (i = 0; i < n; i++) {
            double u = errs[i] / c;
            robust[i] = isfinite(u) && u < 1.0 ? (1.0 - u * u) * (1.0 - u * u) : 0.0;
        }
    }
    free(errs);
    free(sorted);
    free(idx);
    return true;
}

/* Alternates tone-curve and matrix fits with Tukey reweighting. 1: fitted, 0: no curve, -1: no
 * memory. */
static int fit_model(const sample *train, size_t n, const pc_raw_mat3 *fallback, double gain, double ridge,
                     pc_raw_mat3 *m_out, pc_raw_look_curve *tone)
{
    pc_raw_mat3 m = *fallback;
    double *robust = pc_malloc_array(n > 0 ? n : 1, sizeof *robust);
    size_t i;
    int it;
    if (robust == NULL) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        robust[i] = 1.0;
    }
    if (!fit_curve(train, robust, n, &m, tone)) {
        free(robust);
        return 0;
    }
    for (it = 0; it < 6; it++) {
        if (!fit_matrix(train, robust, n, tone, fallback, gain, ridge, &m)) {
            free(robust);
            return -1;
        }
        if (!fit_curve(train, robust, n, &m, tone)) {
            free(robust);
            return 0;
        }
        if (!update_robust(train, robust, n, &m, tone)) {
            free(robust);
            return -1;
        }
    }
    free(robust);
    *m_out = m;
    return 1;
}

typedef struct fit_state {
    sample *train;
    sample *test;
    sample *all;
    const sample **refs;
    double (*pairs)[2];
} fit_state;

static void fit_state_fini(fit_state *f)
{
    free(f->train);
    free(f->test);
    free(f->all);
    free(f->refs);
    free(f->pairs);
}

static pc_status fit_cells(const cell *cells, size_t ncells, size_t gw, double baseline_exposure,
                           pc_raw_camera_look *out, pc_error *err)
{
    double gain = exp2(isfinite(baseline_exposure) ? pc_clampd(baseline_exposure, -10.0, 10.0) : 0.0);
    pc_raw_mat3 a = rx_xyz_d50_to_prophoto();
    pc_raw_mat3 b = rx_srgb_to_xyz_d50();
    pc_raw_mat3 fallback;
    lab lb;
    fit_state f;
    size_t ntrain = 0;
    size_t ntest = 0;
    size_t nall = 0;
    size_t nusable;
    size_t coloured = 0;
    size_t k;
    double r;
    double held_fallback;
    char why[700];
    char t1[64];
    char t2[64];
    bool nomem = false;
    bool found = false;
    pc_raw_mat3 m;
    pc_raw_mat3 rel;
    pc_raw_look_curve *tone = NULL;
    pc_raw_camera_look look;
    pc_status st = PC_OK;
    size_t ri;
    fallback = rx_mul(&a, &b);
    fallback = rx_scale(&fallback, gain);
    lab_new(&lb);
    memset(&f, 0, sizeof f);
    why[0] = '\0';
    f.train = pc_malloc_array(ncells > 0 ? ncells : 1, sizeof *f.train);
    f.test = pc_malloc_array(ncells > 0 ? ncells : 1, sizeof *f.test);
    f.all = pc_malloc_array(ncells > 0 ? ncells : 1, sizeof *f.all);
    f.refs = pc_malloc_array(ncells > 0 ? ncells : 1, sizeof *f.refs);
    f.pairs = pc_malloc_array(ncells > 0 ? ncells : 1, sizeof *f.pairs);
    tone = malloc(sizeof *tone);
    if (f.train == NULL || f.test == NULL || f.all == NULL || f.refs == NULL || f.pairs == NULL ||
        tone == NULL) {
        st = rx_nomem(err);
        goto done;
    }
    for (k = 0; k < ncells; k++) {
        const cell *c = &cells[k];
        sample s;
        size_t x;
        size_t y;
        bool usable;
        int q;
        bool finite = true;
        for (q = 0; q < 3; q++) {
            finite = finite && isfinite(c->cam[q]) && isfinite(c->rgb[q]);
        }
        if (!finite) {
            continue;
        }
        memcpy(s.cam, c->cam, sizeof s.cam);
        memcpy(s.rgb, c->rgb, sizeof s.rgb);
        s.lum = c->lum;
        s.perceptual = perceptual(c->lum);
        s.weight = c->edge_weight;
        f.all[nall++] = s;
        usable = !c->cam_clipped && c->ref_clipped < 0.01 && c->lum > 0.002 && c->cam[0] > 1e-4 &&
                 c->cam[1] > 1e-4 && c->cam[2] > 1e-4 && c->edge_weight > 0.05;
        if (!usable) {
            continue;
        }
        x = k % (gw > 0 ? gw : 1);
        y = k / (gw > 0 ? gw : 1);
        if ((x + 2 * y) % 3 == 0) {
            f.test[ntest++] = s;
        } else {
            f.train[ntrain++] = s;
        }
    }
    if (ntrain < 300 || ntest < 100) {
        (void)snprintf(why, sizeof why, "too few usable cells (%zu + %zu held out)", ntrain, ntest);
        st = reject(err, why);
        goto done;
    }
    /* The reference must show this picture (its lightness follows the raw's) in colour. */
    nusable = ntrain + ntest;
    for (k = 0; k < nusable; k++) {
        const sample *s = k < ntrain ? &f.train[k] : &f.test[k - ntrain];
        double v[3];
        double lab3[3];
        rx_apply(&fallback, s->cam, v);
        f.pairs[k][0] = sqrt(pc_rs_max_k(PRO_Y[0] * v[0] + PRO_Y[1] * v[1] + PRO_Y[2] * v[2], 0.0));
        f.pairs[k][1] = sqrt(pc_rs_max_k(s->lum, 0.0));
        lab_of(&lb, s->rgb, lab3);
        coloured += hypot(lab3[1], lab3[2]) > 4.0 ? 1 : 0;
    }
    r = correlation((const double (*)[2])f.pairs, nusable);
    if (isnan(r) || r < 0.7) {
        fmt_prec(t1, sizeof t1, r, 2);
        (void)snprintf(why, sizeof why,
                       "the reference does not match the raw picture (lightness correlation %s)", t1);
        st = reject(err, why);
        goto done;
    }
    if (coloured * 50 < nusable) {
        st = reject(err, "the reference is (nearly) colourless");
        goto done;
    }
    /* Weakest ridge first; a stronger pull toward the fallback when the matrix comes out
     * implausible. */
    for (k = 0; k < ntest; k++) {
        f.refs[k] = &f.test[k];
    }
    held_fallback = mean_delta_e(&lb, f.refs, ntest, &fallback, NULL, &nomem);
    for (ri = 0; ri < 3 && !nomem; ri++) {
        double held_fit;
        int fr = fit_model(f.train, ntrain, &fallback, gain, RIDGES[ri], &m, tone);
        if (fr < 0) {
            nomem = true;
            break;
        }
        if (fr == 0) {
            st = reject(err, "no tone curve could be fitted");
            goto done;
        }
        rel = rx_scale(&m, 1.0 / gain);
        if (!plausible(&rel)) {
            size_t o = (size_t)snprintf(why, sizeof why, "implausible matrix [");
            int i;
            for (i = 0; i < 9 && o < sizeof why; i++) {
                fmt_debug(t1, sizeof t1, rel.m[i / 3][i % 3]);
                o += (size_t)snprintf(why + o, sizeof why - o, "%s%s%s%s",
                                      i % 3 == 0 ? (i > 0 ? ", [" : "[") : ", ", t1, i % 3 == 2 ? "]" : "",
                                      i == 8 ? "]" : "");
            }
            continue;
        }
        held_fit = mean_delta_e(&lb, f.refs, ntest, &m, tone, &nomem);
        if (isnan(held_fit) || held_fit > 0.85 * held_fallback) {
            fmt_prec(t1, sizeof t1, held_fit, 1);
            fmt_prec(t2, sizeof t2, held_fallback, 1);
            (void)snprintf(why, sizeof why,
                           "the fit does not improve enough on the neutral fallback (held-out \xCE\x94"
                           "E %s vs %s)",
                           t1, t2);
            continue;
        }
        found = true;
        break;
    }
    if (nomem) {
        st = rx_nomem(err);
        goto done;
    }
    if (!found) {
        st = reject(err, why);
        goto done;
    }
    for (k = 0; k < nall; k++) {
        f.refs[k] = &f.all[k];
    }
    memset(&look, 0, sizeof look);
    look.delta_e = mean_delta_e(&lb, f.refs, nall, &m, tone, &nomem);
    look.delta_e_fallback = mean_delta_e(&lb, f.refs, nall, &fallback, NULL, &nomem);
    if (nomem) {
        st = rx_nomem(err);
        goto done;
    }
    a = rx_rgb_to_xyz(RX_ROMM, RX_D50_XY);
    look.to_xyz = rx_mul(&a, &rel);
    look.has_tone = true;
    look.tone = *tone;
    if (!rx_look_is_usable(&look)) {
        st = reject(err, "the fitted matrix is not usable");
        goto done;
    }
    *out = look;
done:
    free(tone);
    fit_state_fini(&f);
    return st;
}

pc_status pc_raw_fit_look(const pc_raw_sensor *s, const pc_raw_develop_options *o, const pc_raw_reference *r,
                          pc_raw_camera_look *out, pc_error *err)
{
    size_t gw;
    cell *cells;
    size_t ncells;
    pc_status st = grid(s, o, r, &gw, &cells, &ncells, err);
    if (st != PC_OK) {
        return st;
    }
    st = fit_cells(cells, ncells, gw, s->baseline_exposure, out, err);
    free(cells);
    return st;
}
