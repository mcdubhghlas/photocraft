/* Filter > Liquify (port of crates/algo/src/liquify.rs): the displacement field, its brush tools,
 * the final bicubic resample and the preview proxy. JSON lives in liquify_puppet_json.c.
 *
 * Integer arithmetic that the reference does in usize / isize / i32 and that can overflow on
 * hostile input wraps here as it does in the reference's release build (size_t is the reference's
 * usize on its 64-bit targets). */
#include "algo/liquify.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* ---- tools ---- */

static const char *const TOOL_NAMES[PC_ALGO_LIQUIFY_TOOL_COUNT] = {
    "forwardWarp", "reconstruct",    "smooth",    "twirlCw", "twirlCcw",
    "pucker",      "bloat",          "pushLeft",  "freeze",  "thaw",
    "lassoMask",   "reconstructAll", "freezeAll", "thawAll", "invertFreeze"};

const pc_algo_enum_desc pc_algo_liquify_tool_desc = {"LiquifyTool", TOOL_NAMES, PC_ALGO_LIQUIFY_TOOL_COUNT,
                                                     PC_ALGO_LIQUIFY_FORWARD_WARP};

const pc_algo_liquify_tool pc_algo_liquify_tools_all[11] = {
    PC_ALGO_LIQUIFY_FORWARD_WARP, PC_ALGO_LIQUIFY_RECONSTRUCT, PC_ALGO_LIQUIFY_SMOOTH,
    PC_ALGO_LIQUIFY_TWIRL_CW,     PC_ALGO_LIQUIFY_TWIRL_CCW,   PC_ALGO_LIQUIFY_PUCKER,
    PC_ALGO_LIQUIFY_BLOAT,        PC_ALGO_LIQUIFY_PUSH_LEFT,   PC_ALGO_LIQUIFY_FREEZE,
    PC_ALGO_LIQUIFY_THAW,         PC_ALGO_LIQUIFY_LASSO_MASK};

const char *pc_algo_liquify_tool_label(pc_algo_liquify_tool t)
{
    static const char *const labels[PC_ALGO_LIQUIFY_TOOL_COUNT] = {
        "Forward Warp", "Reconstruct",     "Smooth",    "Twirl Clockwise", "Twirl Counterclockwise",
        "Pucker",       "Bloat",           "Push Left", "Freeze Mask",     "Thaw Mask",
        "Freeze Lasso", "Reconstruct All", "Mask All",  "Mask None",       "Invert Mask"};
    return (unsigned)t < PC_ALGO_LIQUIFY_TOOL_COUNT ? labels[t] : "";
}

bool pc_algo_liquify_tool_is_global(pc_algo_liquify_tool t)
{
    return t == PC_ALGO_LIQUIFY_RECONSTRUCT_ALL || t == PC_ALGO_LIQUIFY_FREEZE_ALL ||
           t == PC_ALGO_LIQUIFY_THAW_ALL || t == PC_ALGO_LIQUIFY_INVERT_FREEZE;
}

bool pc_algo_liquify_tool_is_stationary(pc_algo_liquify_tool t)
{
    return !(t == PC_ALGO_LIQUIFY_FORWARD_WARP || t == PC_ALGO_LIQUIFY_PUSH_LEFT ||
             t == PC_ALGO_LIQUIFY_LASSO_MASK);
}

/* ---- strokes ---- */

/* Rust's `{}` for an f64 (copied from src/doc/analysis.c, which algo may not include). */
/* Large enough for any f64 in plain decimal: 5e-324 is "0." + 323 zeros + "5", f64::MAX has 309
 * integer digits, and %.4f adds 5 characters. */
enum { NUM_TEXT_MAX = 400 };

/* Digits of |v| in scientific form: exact[0] is the leading digit, exact[1..] the following
 * ones (EXACT_DIGITS in all, enough for the exact value of any double, at most 767 significant
 * digits); *exp10 is the exponent of the leading digit. */
enum { EXACT_DIGITS = 800 };

static bool exact_digits(double v, char digits[EXACT_DIGITS], int *exp10)
{
    char buf[EXACT_DIGITS + 16];
    int n = snprintf(buf, sizeof buf, "%.*e", EXACT_DIGITS - 1, fabs(v));
    const char *e;
    if (n <= 0 || (size_t)n >= sizeof buf || buf[1] != '.') {
        return false;
    }
    e = strchr(buf, 'e');
    if (e == NULL || e - buf != EXACT_DIGITS + 1) {
        return false;
    }
    digits[0] = buf[0];
    memcpy(digits + 1, buf + 2, EXACT_DIGITS - 1);
    *exp10 = (int)strtol(e + 1, NULL, 10);
    return true;
}

/* Whether the k-digit decimal d[0..k) x 10^(exp10 - k + 1) parses back to exactly |v|. */
static bool round_trips(const char *d, size_t k, int exp10, double av)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%.*se%d", (int)k, d, exp10 - (int)k + 1);
    return n > 0 && (size_t)n < sizeof buf && strtod(buf, NULL) == av;
}

/* Rust's `{}` (Display) for an f64: the shortest digits that parse back to v, never in exponent
 * form ("0.0000001", "1000000000000000000000"), "NaN", "inf", "-inf"; no ".0" on integers.
 *
 * Rust's flt2dec (Grisu with a Dragon4 fallback) picks the shortest length k, then of the two
 * k-digit neighbours of v the one inside the rounding interval, the closer when both are, and
 * on an exact tie the one ABOVE (ryu, and so serde's text, rounds such ties to even: Rust prints
 * 3578819939694.65625 as ...6563 where ryu gives ...6562). This reproduces that from glibc's
 * exact printf digits and its correctly rounded strtod. */
static size_t rust_display_f64(double v, char out[NUM_TEXT_MAX])
{
    char d[EXACT_DIGITS];
    char lo[24];
    char hi[24];
    const char *best = NULL;
    int e = 0;
    int ehi;
    int ebest = 0;
    size_t k;
    size_t nb = 0;
    size_t o = 0;
    size_t i;
    double av = fabs(v);

    if (isnan(v)) {
        memcpy(out, "NaN", 4);
        return 3;
    }
    if (isinf(v)) {
        memcpy(out, v < 0 ? "-inf" : "inf", v < 0 ? 5 : 4);
        return v < 0 ? 4 : 3;
    }
    if (signbit(v)) {
        out[o++] = '-';
    }
    if (av == 0.0 || !exact_digits(v, d, &e)) {
        out[o++] = '0';
        out[o] = '\0';
        return o;
    }
    for (k = 1; k <= 17 && best == NULL; k++) {
        bool lo_ok;
        bool hi_ok;
        size_t j;
        memcpy(lo, d, k);
        lo[k] = '\0';
        /* The k-digit neighbour above: lo plus one unit in the last place, with carry. */
        memcpy(hi, d, k);
        hi[k] = '\0';
        ehi = e;
        for (j = k; j > 0 && hi[j - 1] == '9'; j--) {
            hi[j - 1] = '0';
        }
        if (j == 0) {
            hi[0] = '1'; /* 99..9 + 1 = 100..0: one more power of ten, same digit count */
            ehi = e + 1;
        } else {
            hi[j - 1] = (char)(hi[j - 1] + 1);
        }
        lo_ok = round_trips(lo, k, e, av);
        hi_ok = round_trips(hi, k, ehi, av);
        if (lo_ok && hi_ok) {
            /* The closer one; remainder >= half (including an exact tie) rounds up. */
            bool up = d[k] >= '5';
            best = up ? hi : lo;
            ebest = up ? ehi : e;
        } else if (lo_ok) {
            best = lo;
            ebest = e;
        } else if (hi_ok) {
            best = hi;
            ebest = ehi;
        }
        nb = k;
    }
    if (best == NULL) {
        /* Unreachable: 17 significant digits always round-trip. */
        best = d;
        nb = 17;
        ebest = e;
    }
    while (nb > 1 && best[nb - 1] == '0') {
        nb--;
    }
    /* value = 0.best[0..nb) x 10^(ebest + 1) */
    {
        long p = (long)ebest + 1;
        if (p <= 0) {
            out[o++] = '0';
            out[o++] = '.';
            for (; p < 0; p++) {
                out[o++] = '0';
            }
            for (i = 0; i < nb; i++) {
                out[o++] = best[i];
            }
        } else {
            long q = 0;
            for (i = 0; i < nb; i++, q++) {
                if (q == p) {
                    out[o++] = '.';
                }
                out[o++] = best[i];
            }
            for (; q < p; q++) {
                out[o++] = '0';
            }
        }
    }
    out[o] = '\0';
    return o;
}

pc_algo_liquify_stroke pc_algo_liquify_stroke_make(pc_algo_liquify_tool tool, double size)
{
    pc_algo_liquify_stroke s;
    memset(&s, 0, sizeof s);
    s.tool = tool;
    s.size = size;
    s.density = 50.0;
    s.pressure = 100.0;
    s.rate = 80.0;
    return s;
}

void pc_algo_liquify_stroke_fini(pc_algo_liquify_stroke *s)
{
    if (s == NULL) {
        return;
    }
    free(s->coords);
    free(s->off);
    s->coords = NULL;
    s->off = NULL;
    s->npoints = 0;
}

pc_status pc_algo_liquify_stroke_push(pc_algo_liquify_stroke *s, const double *v, size_t n, pc_error *err)
{
    size_t have = s->npoints > 0 ? s->off[s->npoints] : 0, need, np;
    double *c;
    size_t *o;
    if (!pc_size_add(have, n, &need) || !pc_size_add(s->npoints, 2, &np)) {
        return pc_fail(err, PC_ERR_LIMIT, "liquify: too many stroke points");
    }
    c = realloc(s->coords, need > 0 ? need * sizeof *c : 1);
    if (c == NULL || need > PC_MAX_ALLOC / sizeof *c) {
        if (c != NULL) {
            s->coords = c;
        }
        return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
    }
    s->coords = c;
    if (np > PC_MAX_ALLOC / sizeof *o) {
        return pc_fail(err, PC_ERR_LIMIT, "liquify: too many stroke points");
    }
    o = realloc(s->off, np * sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
    }
    s->off = o;
    if (n > 0) {
        memcpy(s->coords + have, v, n * sizeof *v);
    }
    o[0] = 0;
    o[s->npoints + 1] = need;
    s->npoints++;
    return PC_OK;
}

pc_status pc_algo_liquify_stroke_copy(const pc_algo_liquify_stroke *s, pc_algo_liquify_stroke *out,
                                      pc_error *err)
{
    pc_algo_liquify_stroke c = *s;
    size_t nc = s->npoints > 0 ? s->off[s->npoints] : 0;
    c.coords = NULL;
    c.off = NULL;
    if (s->npoints > 0) {
        c.coords = pc_malloc_array(nc > 0 ? nc : 1, sizeof(double));
        c.off = pc_malloc_array(s->npoints + 1, sizeof(size_t));
        if (c.coords == NULL || c.off == NULL) {
            pc_algo_liquify_stroke_fini(&c);
            return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
        }
        if (nc > 0) {
            memcpy(c.coords, s->coords, nc * sizeof(double));
        }
        memcpy(c.off, s->off, (s->npoints + 1) * sizeof(size_t));
    }
    *out = c;
    return PC_OK;
}

bool pc_algo_liquify_stroke_eq(const pc_algo_liquify_stroke *a, const pc_algo_liquify_stroke *b)
{
    size_t i;
    if (a->tool != b->tool || a->size != b->size || a->density != b->density || a->pressure != b->pressure ||
        a->rate != b->rate || a->npoints != b->npoints || a->has_amount != b->has_amount ||
        (a->has_amount && a->amount != b->amount)) {
        return false;
    }
    for (i = 0; i < a->npoints; i++) {
        size_t k, la = a->off[i + 1] - a->off[i];
        if (la != b->off[i + 1] - b->off[i]) {
            return false;
        }
        for (k = 0; k < la; k++) {
            if (a->coords[a->off[i] + k] != b->coords[b->off[i] + k]) {
                return false;
            }
        }
    }
    return true;
}

pc_status pc_algo_liquify_stroke_validate(const pc_algo_liquify_stroke *s, pc_error *err)
{
    size_t i;
    if (!(isfinite(s->size) && s->size >= 1.0 && s->size <= 15000.0)) {
        char num[NUM_TEXT_MAX];
        (void)rust_display_f64(s->size, num);
        return pc_fail(err, PC_ERR_INVALID, "brush size %s out of range 1..15000", num);
    }
    for (i = 0; i < s->npoints; i++) {
        size_t k, n = s->off[i + 1] - s->off[i];
        bool bad = n < 2 || n > 3;
        for (k = 0; k < n && !bad; k++) {
            bad = !isfinite(s->coords[s->off[i] + k]);
        }
        if (bad) {
            return pc_fail(err, PC_ERR_INVALID, "stroke points are [x, y] or [x, y, pressure]");
        }
    }
    return PC_OK;
}

/* ---- field ---- */

double pc_algo_liquify_auto_cell(pc_rect bounds)
{
    return pc_size_area(pc_rect_size(bounds)) <= 4200000u ? 2.0 : 4.0;
}

pc_status pc_algo_liquify_field_init(pc_algo_liquify_field *f, pc_rect bounds, double cell, pc_error *err)
{
    pc_algo_liquify_field g;
    size_t n;
    g.bounds = bounds;
    g.cell = pc_clampd(cell, 1.0, 64.0);
    g.w = pc_f64_to_size(ceil((double)pc_rect_width(bounds) / g.cell)) + 2;
    g.h = pc_f64_to_size(ceil((double)pc_rect_height(bounds) / g.cell)) + 2;
    if (!pc_size_mul(g.w, g.h, &n) || n > PC_MAX_ALLOC / (3 * sizeof(float))) {
        return pc_fail(err, PC_ERR_LIMIT, "liquify: the field over %ux%u px is too large",
                       pc_rect_width(bounds), pc_rect_height(bounds));
    }
    g.d = pc_calloc_array(n, sizeof *g.d);
    g.freeze = pc_calloc_array(n, sizeof *g.freeze);
    if (g.d == NULL || g.freeze == NULL) {
        free(g.d);
        free(g.freeze);
        return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
    }
    *f = g;
    return PC_OK;
}

void pc_algo_liquify_field_fini(pc_algo_liquify_field *f)
{
    if (f == NULL) {
        return;
    }
    free(f->d);
    free(f->freeze);
    f->d = NULL;
    f->freeze = NULL;
    f->w = f->h = 0;
}

pc_status pc_algo_liquify_field_clone(const pc_algo_liquify_field *f, pc_algo_liquify_field *out,
                                      pc_error *err)
{
    pc_algo_liquify_field g = *f;
    size_t n = f->w * f->h;
    g.d = pc_malloc_array(n > 0 ? n : 1, sizeof *g.d);
    g.freeze = pc_malloc_array(n > 0 ? n : 1, sizeof *g.freeze);
    if (g.d == NULL || g.freeze == NULL) {
        free(g.d);
        free(g.freeze);
        return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
    }
    if (n > 0) {
        memcpy(g.d, f->d, n * sizeof *g.d);
        memcpy(g.freeze, f->freeze, n * sizeof *g.freeze);
    }
    *out = g;
    return PC_OK;
}

bool pc_algo_liquify_field_eq(const pc_algo_liquify_field *a, const pc_algo_liquify_field *b)
{
    size_t i, n = a->w * a->h;
    if (!pc_rect_eq(a->bounds, b->bounds) || a->cell != b->cell || a->w != b->w || a->h != b->h) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (a->d[i][0] != b->d[i][0] || a->d[i][1] != b->d[i][1] || a->freeze[i] != b->freeze[i]) {
            return false;
        }
    }
    return true;
}

void pc_algo_liquify_node_pos(const pc_algo_liquify_field *f, size_t i, size_t j, double out[2])
{
    out[0] = (double)f->bounds.x0 + 0.5 + (double)i * f->cell;
    out[1] = (double)f->bounds.y0 + 0.5 + (double)j * f->cell;
}

static double grid_x(const pc_algo_liquify_field *f, double x)
{
    return (x - (double)f->bounds.x0 - 0.5) / f->cell;
}

static double grid_y(const pc_algo_liquify_field *f, double y)
{
    return (y - (double)f->bounds.y0 - 0.5) / f->cell;
}

bool pc_algo_liquify_is_identity(const pc_algo_liquify_field *f)
{
    size_t i, n = f->w * f->h;
    for (i = 0; i < n; i++) {
        if (!(f->d[i][0] == 0.0f && f->d[i][1] == 0.0f)) {
            return false;
        }
    }
    return true;
}

float pc_algo_liquify_max_displacement(const pc_algo_liquify_field *f)
{
    size_t i, n = f->w * f->h;
    float m = 0.0f;
    for (i = 0; i < n; i++) {
        m = pc_rs_maxf(m, hypotf(f->d[i][0], f->d[i][1]));
    }
    return m;
}

/* bilinear() of the reference: zero outside (-1, w) x (-1, h). */
static void bilinear(const float (*d)[2], size_t w, size_t h, double gx, double gy, double out[2])
{
    size_t i, j, i1, j1, k;
    double fx, fy;
    if (!(gx > -1.0 && gy > -1.0 && gx < (double)w && gy < (double)h)) {
        out[0] = 0.0;
        out[1] = 0.0;
        return;
    }
    gx = pc_clampd(gx, 0.0, (double)(w - 1));
    gy = pc_clampd(gy, 0.0, (double)(h - 1));
    i = pc_f64_to_size(gx);
    j = pc_f64_to_size(gy);
    i = i < (w >= 2 ? w - 2 : 0) ? i : (w >= 2 ? w - 2 : 0);
    j = j < (h >= 2 ? h - 2 : 0) ? j : (h >= 2 ? h - 2 : 0);
    fx = gx - (double)i;
    fy = gy - (double)j;
    i1 = i + 1 < w - 1 ? i + 1 : w - 1;
    j1 = j + 1 < h - 1 ? j + 1 : h - 1;
    for (k = 0; k < 2; k++) {
        double a = (double)d[j * w + i][k], b = (double)d[j * w + i1][k];
        double c = (double)d[j1 * w + i][k], e = (double)d[j1 * w + i1][k];
        out[k] = (a * (1.0 - fx) + b * fx) * (1.0 - fy) + (c * (1.0 - fx) + e * fx) * fy;
    }
}

void pc_algo_liquify_sample(const pc_algo_liquify_field *f, double x, double y, double out[2])
{
    bilinear((const float (*)[2])f->d, f->w, f->h, grid_x(f, x), grid_y(f, y), out);
}

float pc_algo_liquify_freeze_at(const pc_algo_liquify_field *f, double x, double y)
{
    double gx = pc_clampd(grid_x(f, x), 0.0, (double)(f->w - 1));
    double gy = pc_clampd(grid_y(f, y), 0.0, (double)(f->h - 1));
    size_t i = pc_f64_to_size(gx), j = pc_f64_to_size(gy), w = f->w;
    float fx, fy;
    const float *z = f->freeze;
    i = i < f->w - 2 ? i : f->w - 2;
    j = j < f->h - 2 ? j : f->h - 2;
    fx = pc_f64_to_f32(gx - (double)i);
    fy = pc_f64_to_f32(gy - (double)j);
    return (z[j * w + i] * (1.0f - fx) + z[j * w + i + 1] * fx) * (1.0f - fy) +
           (z[(j + 1) * w + i] * (1.0f - fx) + z[(j + 1) * w + i + 1] * fx) * fy;
}

static int cmp_total(const void *pa, const void *pb)
{
    double a = *(const double *)pa, b = *(const double *)pb;
    int64_t ia, ib;
    memcpy(&ia, &a, 8);
    memcpy(&ib, &b, 8);
    /* f64::total_cmp: flip the magnitude bits of negatives so the integers order like the floats. */
    ia ^= (int64_t)((uint64_t)(ia >> 63) >> 1);
    ib ^= (int64_t)((uint64_t)(ib >> 63) >> 1);
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

/* Lasso: sets the freeze mask to amount (1 / none freezes, 0 thaws) at every node inside the
 * closed polygon (even-odd; non-finite points skipped), one pass over the edges per node row. */
static pc_status apply_lasso(pc_algo_liquify_field *f, const pc_algo_liquify_stroke *s, pc_rect *dirty,
                             pc_error *err)
{
    double (*poly)[2] = NULL, *xs = NULL;
    size_t np = 0, i, nxs, j, j_first, j_last;
    double px0 = INFINITY, py0 = INFINITY, px1 = -INFINITY, py1 = -INFINITY, gy0, gy1, last_row, last_col, j0,
           j1;
    float v;
    *dirty = PC_RECT_EMPTY;
    if (s->npoints > 0) {
        poly = pc_malloc_array(s->npoints, sizeof *poly);
        xs = pc_malloc_array(s->npoints, sizeof *xs);
        if (poly == NULL || xs == NULL) {
            free(poly);
            free(xs);
            return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
        }
    }
    for (i = 0; i < s->npoints; i++) {
        const double *p = s->coords + s->off[i];
        if (s->off[i + 1] - s->off[i] >= 2 && isfinite(p[0]) && isfinite(p[1])) {
            poly[np][0] = p[0];
            poly[np][1] = p[1];
            np++;
        }
    }
    if (np < 3 || f->w == 0 || f->h == 0) {
        goto done;
    }
    for (i = 0; i < np; i++) {
        px0 = pc_rs_min(px0, poly[i][0]);
        py0 = pc_rs_min(py0, poly[i][1]);
        px1 = pc_rs_max(px1, poly[i][0]);
        py1 = pc_rs_max(py1, poly[i][1]);
    }
    gy0 = grid_y(f, py0);
    gy1 = grid_y(f, py1);
    last_row = (double)(f->h - 1);
    last_col = (double)(f->w - 1);
    j0 = pc_rs_max_k(ceil(gy0), 0.0);
    j1 = pc_rs_min(floor(gy1), last_row);
    if (j1 < j0) {
        goto done;
    }
    v = (s->has_amount ? s->amount : 1.0) > 0.5 ? 1.0f : 0.0f;
    j_first = pc_f64_to_size(j0);
    j_last = pc_f64_to_size(j1);
    for (j = j_first; j <= j_last; j++) {
        double y = (double)f->bounds.y0 + 0.5 + (double)j * f->cell;
        const double *prev = poly[np - 1];
        size_t k;
        nxs = 0;
        for (i = 0; i < np; i++) {
            const double *a = poly[i];
            if ((a[1] > y) != (prev[1] > y)) {
                xs[nxs++] = a[0] + (y - a[1]) * (prev[0] - a[0]) / (prev[1] - a[1]);
            }
            prev = a;
        }
        qsort(xs, nxs, sizeof *xs, cmp_total);
        /* Even-odd: nodes with xs[2k] <= x < xs[2k + 1] are inside. */
        for (k = 0; k + 1 < nxs; k += 2) {
            double ga = grid_x(f, xs[k]), gb = grid_x(f, xs[k + 1]);
            double i0 = pc_rs_max_k(ceil(ga), 0.0), i1 = pc_rs_min(ceil(gb) - 1.0, last_col);
            size_t row = j * f->w, a0, a1, x;
            if (i1 < i0) {
                continue;
            }
            a0 = pc_f64_to_size(i0);
            a1 = pc_f64_to_size(i1);
            /* freeze.get_mut(row + i0 ..= row + i1): skipped when out of range. */
            if (a0 > a1 || a1 >= f->w) {
                continue;
            }
            for (x = a0; x <= a1; x++) {
                f->freeze[row + x] = v;
            }
        }
        if (j == SIZE_MAX) {
            break;
        }
    }
    *dirty = pc_rect_intersect(pc_rect_make(pc_f64_to_i32(floor(px0)), pc_f64_to_i32(floor(py0)),
                                            pc_f64_to_i32(ceil(px1)), pc_f64_to_i32(ceil(py1))),
                               f->bounds);
done:
    free(poly);
    free(xs);
    return PC_OK;
}

/* One dab at c with brush motion delta; *dirty receives the document rect it affected. */
static pc_status dab(pc_algo_liquify_field *f, const pc_algo_liquify_stroke *s, const double c[2],
                     const double delta[2], double point_pressure, pc_rect *dirty, pc_error *err)
{
    pc_algo_liquify_tool tool = s->tool;
    double r, strength, rate, density, inv_cell;
    int64_t i1s, j1s;
    size_t i0, j0, i1, j1, m, si0, sj0, si1, sj1, sw, sh, i, j, nw = 0;
    float (*snap)[2] = NULL;
    struct lq_write {
        size_t idx;
        float d[2];
        float f;
    } *writes = NULL;
    int32_t pad;
    *dirty = PC_RECT_EMPTY;
    if (tool == PC_ALGO_LIQUIFY_LASSO_MASK) {
        return PC_OK; /* the lasso is polygon-only; never a brush dab */
    }
    r = pc_rs_max_k(s->size / 2.0, 0.5);
    strength = pc_clampd(s->pressure / 100.0, 0.0, 1.0) * point_pressure;
    rate = pc_clampd(s->rate / 100.0, 0.0, 1.0);
    density = pc_clampd(s->density / 100.0, 0.0, 1.0);
    if (strength <= 0.0) {
        return PC_OK;
    }
    i0 = pc_f64_to_size(pc_rs_max_k(floor(grid_x(f, c[0] - r)), 0.0));
    j0 = pc_f64_to_size(pc_rs_max_k(floor(grid_y(f, c[1] - r)), 0.0));
    i1s = pc_f64_to_i64(pc_rs_max_k(ceil(grid_x(f, c[0] + r)), -1.0));
    j1s = pc_f64_to_i64(pc_rs_max_k(ceil(grid_y(f, c[1] + r)), -1.0));
    i1s = i1s < (int64_t)f->w - 1 ? i1s : (int64_t)f->w - 1;
    j1s = j1s < (int64_t)f->h - 1 ? j1s : (int64_t)f->h - 1;
    /* The reference compares `i1 < i0 as isize`; a start beyond isize::MAX (a brush centre past
     * ~1e19 px) wraps negative there and the snapshot slice then panics. Unsigned here. */
    if (i1s < 0 || j1s < 0 || (uint64_t)i1s < (uint64_t)i0 || (uint64_t)j1s < (uint64_t)j0) {
        return PC_OK;
    }
    i1 = (size_t)i1s;
    j1 = (size_t)j1s;
    /* Snapshot of the affected nodes plus a margin (sampling reads around them). */
    m = pc_f64_to_size(ceil((pc_rs_max(fabs(delta[0]), fabs(delta[1])) + r * 0.2) / f->cell)) + 2;
    si0 = i0 > m ? i0 - m : 0;
    sj0 = j0 > m ? j0 - m : 0;
    si1 = i1 + m < f->w - 1 ? i1 + m : f->w - 1;
    sj1 = j1 + m < f->h - 1 ? j1 + m : f->h - 1;
    sw = si1 - si0 + 1;
    sh = sj1 - sj0 + 1;
    snap = pc_malloc_array(sw * sh, sizeof *snap);
    writes = pc_malloc_array((i1 - i0 + 1) * (j1 - j0 + 1), sizeof *writes);
    if (snap == NULL || writes == NULL) {
        free(snap);
        free(writes);
        return pc_fail(err, PC_ERR_NOMEM, "liquify: out of memory");
    }
    for (j = sj0; j <= sj1; j++) {
        memcpy(snap + (j - sj0) * sw, f->d + j * f->w + si0, sw * sizeof *snap);
    }
    inv_cell = 1.0 / f->cell;
    for (j = j0; j <= j1; j++) {
        for (i = i0; i <= i1; i++) {
            double p[2], dx, dy, t2, soft, fall, k_raw, k;
            size_t idx = j * f->w + i;
            float cur[2], nw_d[2];
            pc_algo_liquify_node_pos(f, i, j, p);
            dx = p[0] - c[0];
            dy = p[1] - c[1];
            t2 = (dx * dx + dy * dy) / (r * r);
            if (t2 >= 1.0) {
                continue;
            }
            soft = (1.0 - t2) * (1.0 - t2);
            fall = (1.0 - density) + density * soft;
            k_raw = strength * fall;
            if (tool == PC_ALGO_LIQUIFY_FREEZE || tool == PC_ALGO_LIQUIFY_THAW) {
                double fz = (double)f->freeze[idx];
                double v = tool == PC_ALGO_LIQUIFY_FREEZE ? pc_rs_min_k(fz + k_raw, 1.0)
                                                          : pc_rs_max_k(fz - k_raw, 0.0);
                writes[nw].idx = idx;
                writes[nw].d[0] = f->d[idx][0];
                writes[nw].d[1] = f->d[idx][1];
                writes[nw].f = pc_f64_to_f32(v);
                nw++;
                continue;
            }
            k = k_raw * (1.0 - (double)f->freeze[idx]);
            if (k <= 0.0) {
                continue;
            }
            cur[0] = f->d[idx][0];
            cur[1] = f->d[idx][1];
            if (tool == PC_ALGO_LIQUIFY_RECONSTRUCT) {
                float a = pc_f64_to_f32(1.0 - k * (0.1 + 0.4 * rate));
                nw_d[0] = cur[0] * a;
                nw_d[1] = cur[1] * a;
            } else if (tool == PC_ALGO_LIQUIFY_SMOOTH) {
                static const int OFS[8][2] = {{-1, 0},  {1, 0},  {0, -1}, {0, 1},
                                              {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
                int64_t gx = (int64_t)(i - si0), gy = (int64_t)(j - sj0);
                float mean[2] = {0.0f, 0.0f}, a;
                int o;
                for (o = 0; o < 8; o++) {
                    int64_t x = pc_clamp_i64(gx + OFS[o][0], 0, (int64_t)sw - 1);
                    int64_t y = pc_clamp_i64(gy + OFS[o][1], 0, (int64_t)sh - 1);
                    const float *vv = snap[(size_t)y * sw + (size_t)x];
                    mean[0] += vv[0] / 8.0f;
                    mean[1] += vv[1] / 8.0f;
                }
                a = pc_f64_to_f32(k * (0.1 + 0.4 * rate));
                nw_d[0] = cur[0] + (mean[0] - cur[0]) * a;
                nw_d[1] = cur[1] + (mean[1] - cur[1]) * a;
            } else {
                double q[2], qx, qy, lx, ly, dq[2];
                if (tool == PC_ALGO_LIQUIFY_FORWARD_WARP) {
                    q[0] = p[0] - k * delta[0];
                    q[1] = p[1] - k * delta[1];
                } else if (tool == PC_ALGO_LIQUIFY_PUSH_LEFT) {
                    /* Left of the stroke direction (y down): (dx, dy) -> (dy, -dx). */
                    q[0] = p[0] - k * delta[1];
                    q[1] = p[1] + k * delta[0];
                } else if (tool == PC_ALGO_LIQUIFY_TWIRL_CW || tool == PC_ALGO_LIQUIFY_TWIRL_CCW) {
                    double th = k * rate * 0.12 * (tool == PC_ALGO_LIQUIFY_TWIRL_CW ? 1.0 : -1.0);
                    /* Content turns by +th, so sample at the point turned by -th. LLVM folds
                     * the reference's (-th).sin_cos() to (-sin th, cos th): identical for numbers,
                     * and this keeps the sign of a NaN the same too. */
                    double sn = -sin(th), cs = cos(th);
                    q[0] = c[0] + dx * cs - dy * sn;
                    q[1] = c[1] + dx * sn + dy * cs;
                } else if (tool == PC_ALGO_LIQUIFY_PUCKER) {
                    double a = 1.0 + k * rate * 0.06;
                    q[0] = c[0] + dx * a;
                    q[1] = c[1] + dy * a;
                } else {
                    /* Bloat, and (as in the reference) any global tool reaching a dab. */
                    double a = 1.0 - k * rate * 0.06;
                    q[0] = c[0] + dx * a;
                    q[1] = c[1] + dy * a;
                }
                qx = (q[0] - p[0]) * inv_cell + (double)i;
                qy = (q[1] - p[1]) * inv_cell + (double)j;
                lx = qx - (double)si0;
                ly = qy - (double)sj0;
                if (lx >= 0.0 && ly >= 0.0 && lx <= (double)(sw - 1) && ly <= (double)(sh - 1)) {
                    bilinear((const float (*)[2])snap, sw, sh, lx, ly, dq);
                } else {
                    /* Outside the snapshot nothing changed: read the live field. */
                    bilinear((const float (*)[2])f->d, f->w, f->h, qx, qy, dq);
                }
                nw_d[0] = pc_f64_to_f32(q[0] + dq[0] - p[0]);
                nw_d[1] = pc_f64_to_f32(q[1] + dq[1] - p[1]);
            }
            writes[nw].idx = idx;
            writes[nw].d[0] = nw_d[0];
            writes[nw].d[1] = nw_d[1];
            writes[nw].f = f->freeze[idx];
            nw++;
        }
    }
    for (i = 0; i < nw; i++) {
        f->d[writes[i].idx][0] = writes[i].d[0];
        f->d[writes[i].idx][1] = writes[i].d[1];
        f->freeze[writes[i].idx] = writes[i].f;
    }
    free(snap);
    free(writes);
    pad = pc_i32_wrapping_add(pc_f64_to_i32(ceil(r)), 2);
    *dirty = pc_rect_intersect(pc_rect_make(pc_i32_wrapping_sub(pc_f64_to_i32(floor(c[0])), pad),
                                            pc_i32_wrapping_sub(pc_f64_to_i32(floor(c[1])), pad),
                                            pc_i32_wrapping_add(pc_f64_to_i32(ceil(c[0])), pad),
                                            pc_i32_wrapping_add(pc_f64_to_i32(ceil(c[1])), pad)),
                               f->bounds);
    return PC_OK;
}

pc_status pc_algo_liquify_stroke_begin(pc_algo_liquify_field *f, const pc_algo_liquify_stroke *s,
                                       const double p[3], pc_rect *dirty, pc_error *err)
{
    pc_rect d = PC_RECT_EMPTY;
    pc_status st = PC_OK;
    if (pc_algo_liquify_tool_is_stationary(s->tool) && !pc_algo_liquify_tool_is_global(s->tool)) {
        double c[2] = {p[0], p[1]}, zero[2] = {0.0, 0.0};
        st = dab(f, s, c, zero, pc_clampd(p[2], 0.0, 1.0), &d, err);
    }
    if (st == PC_OK && dirty != NULL) {
        *dirty = d;
    }
    return st;
}

/* The steps k0..=k1 of a segment walked in `steps` whose dab can touch a field node; false when
 * none can. Step k centres its dab at a + (b - a)(k - o)/steps (o = 1 for the dragging tools, 0
 * for the stationary ones); the window pads the reach by a node and a step. */
static bool reachable_steps(const pc_algo_liquify_field *f, const double a[3], const double b[3],
                            size_t steps, double r, bool stationary, size_t *k0_out, size_t *k1_out)
{
    double pad, lo[2], hi[2], n, o, k0 = 1.0, k1;
    int ax;
    if (f->w == 0 || f->h == 0) {
        return false;
    }
    pad = r + 2.0 * f->cell;
    lo[0] = (double)f->bounds.x0 + 0.5 - pad;
    lo[1] = (double)f->bounds.y0 + 0.5 - pad;
    hi[0] = lo[0] + (double)(f->w - 1) * f->cell + 2.0 * pad;
    hi[1] = lo[1] + (double)(f->h - 1) * f->cell + 2.0 * pad;
    n = (double)steps;
    o = stationary ? 0.0 : 1.0;
    k1 = n;
    for (ax = 0; ax < 2; ax++) {
        double d = b[ax] - a[ax], u0, u1;
        if (d == 0.0) {
            if (isnan(a[ax]) || a[ax] < lo[ax] || a[ax] > hi[ax]) {
                return false;
            }
            continue;
        }
        u0 = (lo[ax] - a[ax]) / d;
        u1 = (hi[ax] - a[ax]) / d;
        k0 = pc_rs_max(k0, floor(pc_rs_min(u0, u1) * n + o) - 1.0);
        k1 = pc_rs_min(k1, ceil(pc_rs_max(u0, u1) * n + o) + 1.0);
    }
    /* NaN: a segment whose length overflowed. */
    if (isnan(k0) || isnan(k1) || k0 > k1) {
        return false;
    }
    *k0_out = pc_f64_to_size(k0);
    *k1_out = pc_f64_to_size(k1);
    return true;
}

pc_status pc_algo_liquify_stroke_segment(pc_algo_liquify_field *f, const pc_algo_liquify_stroke *s,
                                         const double a[3], const double b[3], pc_rect *dirty, pc_error *err)
{
    pc_rect acc = PC_RECT_EMPTY, d;
    double pa = pc_clampd(a[2], 0.0, 1.0), pb = pc_clampd(b[2], 0.0, 1.0);
    double len = hypot(b[0] - a[0], b[1] - a[1]), r, spacing;
    bool stationary = pc_algo_liquify_tool_is_stationary(s->tool);
    size_t steps, k0, k1, k;
    pc_status st;
    if (dirty != NULL) {
        *dirty = PC_RECT_EMPTY;
    }
    if (len == 0.0) {
        if (stationary) {
            double c[2] = {b[0], b[1]}, zero[2] = {0.0, 0.0};
            st = dab(f, s, c, zero, pb, &d, err);
            if (st == PC_OK && dirty != NULL) {
                *dirty = d;
            }
            return st;
        }
        return PC_OK;
    }
    /* An infinite end point: every dab centre on the segment is then infinite or NaN and changes
     * nothing, but the reference's step window becomes NaN and it walks 2^64 dabs (or panics on
     * a dab centred at +inf). Nothing to do. */
    if (isinf(a[0]) || isinf(a[1]) || isinf(b[0]) || isinf(b[1])) {
        return PC_OK;
    }
    r = pc_rs_max_k(s->size / 2.0, 0.5);
    spacing = pc_rs_max_k(r * 0.2, 0.5);
    steps = pc_f64_to_size(pc_rs_max_k(ceil(len / spacing), 1.0));
    /* Walk only the steps whose dab can reach a field node: a segment to a far-off point has
     * trillions of them (#937). */
    if (!reachable_steps(f, a, b, steps, r, stationary, &k0, &k1)) {
        return PC_OK;
    }
    for (k = k0; k <= k1; k++) {
        double t0 = (double)(k - 1) / (double)steps, t1 = (double)k / (double)steps;
        double c[2] = {a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1};
        double delta[2] = {(b[0] - a[0]) * (t1 - t0), (b[1] - a[1]) * (t1 - t0)};
        double pr = pa + (pb - pa) * t1, centre[2];
        /* Forward Warp centres the dab where the brush was (it drags what is under it). */
        if (stationary) {
            centre[0] = c[0];
            centre[1] = c[1];
        } else {
            centre[0] = c[0] - delta[0];
            centre[1] = c[1] - delta[1];
        }
        st = dab(f, s, centre, delta, pr, &d, err);
        if (st != PC_OK) {
            return st;
        }
        acc = pc_rect_union(acc, d);
        if (k == SIZE_MAX) {
            break;
        }
    }
    if (dirty != NULL) {
        *dirty = acc;
    }
    return PC_OK;
}

pc_status pc_algo_liquify_apply_stroke(pc_algo_liquify_field *f, const pc_algo_liquify_stroke *s,
                                       pc_rect *dirty, pc_error *err)
{
    size_t i, n = f->w * f->h, first;
    pc_rect acc, d;
    double prev[3];
    pc_status st;
    if (dirty != NULL) {
        *dirty = PC_RECT_EMPTY;
    }
    switch (s->tool) {
    case PC_ALGO_LIQUIFY_LASSO_MASK:
        st = apply_lasso(f, s, &d, err);
        if (st == PC_OK && dirty != NULL) {
            *dirty = d;
        }
        return st;
    case PC_ALGO_LIQUIFY_RECONSTRUCT_ALL: {
        float k = pc_f64_to_f32(pc_clampd((s->has_amount ? s->amount : 100.0) / 100.0, 0.0, 1.0));
        for (i = 0; i < n; i++) {
            float m = 1.0f - k * (1.0f - f->freeze[i]);
            f->d[i][0] *= m;
            f->d[i][1] *= m;
        }
        break;
    }
    case PC_ALGO_LIQUIFY_FREEZE_ALL:
        for (i = 0; i < n; i++) {
            f->freeze[i] = 1.0f;
        }
        break;
    case PC_ALGO_LIQUIFY_THAW_ALL:
        for (i = 0; i < n; i++) {
            f->freeze[i] = 0.0f;
        }
        break;
    case PC_ALGO_LIQUIFY_INVERT_FREEZE:
        for (i = 0; i < n; i++) {
            f->freeze[i] = 1.0f - f->freeze[i];
        }
        break;
    default:
        break;
    }
    if (pc_algo_liquify_tool_is_global(s->tool)) {
        if (dirty != NULL) {
            *dirty = f->bounds;
        }
        return PC_OK;
    }
    /* Points with at least two values, pressure 1 when absent. */
    for (first = 0; first < s->npoints && s->off[first + 1] - s->off[first] < 2; first++) {
    }
    if (first == s->npoints) {
        return PC_OK;
    }
    {
        const double *p = s->coords + s->off[first];
        prev[0] = p[0];
        prev[1] = p[1];
        prev[2] = s->off[first + 1] - s->off[first] > 2 ? p[2] : 1.0;
    }
    st = pc_algo_liquify_stroke_begin(f, s, prev, &acc, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = first + 1; i < s->npoints; i++) {
        const double *p = s->coords + s->off[i];
        size_t len = s->off[i + 1] - s->off[i];
        double cur[3];
        if (len < 2) {
            continue;
        }
        cur[0] = p[0];
        cur[1] = p[1];
        cur[2] = len > 2 ? p[2] : 1.0;
        st = pc_algo_liquify_stroke_segment(f, s, prev, cur, &d, err);
        if (st != PC_OK) {
            return st;
        }
        acc = pc_rect_union(acc, d);
        memcpy(prev, cur, sizeof prev);
    }
    if (dirty != NULL) {
        *dirty = acc;
    }
    return PC_OK;
}

pc_status pc_algo_liquify_field_from_strokes(pc_rect bounds, double cell,
                                             const pc_algo_liquify_stroke *strokes, size_t n,
                                             pc_algo_liquify_field *out, pc_error *err)
{
    pc_algo_liquify_field f;
    size_t i;
    pc_status st = pc_algo_liquify_field_init(&f, bounds, cell, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n; i++) {
        st = pc_algo_liquify_apply_stroke(&f, &strokes[i], NULL, err);
        if (st != PC_OK) {
            pc_algo_liquify_field_fini(&f);
            return st;
        }
    }
    *out = f;
    return PC_OK;
}
