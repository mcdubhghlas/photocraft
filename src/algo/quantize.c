/* Indexed colour and bitmap conversion (port of crates/algo/src/quantize.rs). */
#include "algo/quantize.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/quantize_nearest.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "color/convert.h"

static uint8_t to8(float v)
{
    return pc_f32_to_u8(roundf(pc_clampf(v, 0.0f, 1.0f) * 255.0f));
}

bool pc_algo_palette_kind_from_id(const char *s, pc_algo_palette_kind *out)
{
    static const struct {
        const char *id;
        pc_algo_palette_kind k;
    } T[] = {{"exact", PC_ALGO_PALETTE_EXACT},
             {"system", PC_ALGO_PALETTE_SYSTEM_MAC},
             {"systemMac", PC_ALGO_PALETTE_SYSTEM_MAC},
             {"systemMacOs", PC_ALGO_PALETTE_SYSTEM_MAC},
             {"systemWindows", PC_ALGO_PALETTE_SYSTEM_WINDOWS},
             {"web", PC_ALGO_PALETTE_WEB},
             {"uniform", PC_ALGO_PALETTE_UNIFORM},
             {"perceptual", PC_ALGO_PALETTE_PERCEPTUAL},
             {"localPerceptual", PC_ALGO_PALETTE_PERCEPTUAL},
             {"selective", PC_ALGO_PALETTE_SELECTIVE},
             {"localSelective", PC_ALGO_PALETTE_SELECTIVE},
             {"adaptive", PC_ALGO_PALETTE_ADAPTIVE},
             {"localAdaptive", PC_ALGO_PALETTE_ADAPTIVE}};
    size_t i;
    for (i = 0; s != NULL && i < sizeof T / sizeof T[0]; i++) {
        if (strcmp(s, T[i].id) == 0) {
            *out = T[i].k;
            return true;
        }
    }
    return false;
}

bool pc_algo_halftone_shape_from_id(const char *s, pc_algo_halftone_shape *out)
{
    static const struct {
        const char *id;
        pc_algo_halftone_shape k;
    } T[] = {{"round", PC_ALGO_HALFTONE_ROUND},     {"ellipse", PC_ALGO_HALFTONE_ELLIPSE},
             {"line", PC_ALGO_HALFTONE_LINE},       {"square", PC_ALGO_HALFTONE_SQUARE},
             {"diamond", PC_ALGO_HALFTONE_DIAMOND}, {"cross", PC_ALGO_HALFTONE_CROSS}};
    size_t i;
    if (s == NULL) {
        return false;
    }
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (strcmp(s, T[i].id) == 0) {
            *out = T[i].k;
            return true;
        }
    }
    return false;
}

static void pal_push(pc_algo_palette *p, uint8_t r, uint8_t g, uint8_t b)
{
    if (p->n < 256) {
        p->c[p->n][0] = r, p->c[p->n][1] = g, p->c[p->n][2] = b;
        p->n++;
    }
}

static bool pal_contains(const pc_algo_palette *p, const uint8_t c[3])
{
    size_t i;
    for (i = 0; i < p->n; i++) {
        if (p->c[i][0] == c[0] && p->c[i][1] == c[1] && p->c[i][2] == c[2]) {
            return true;
        }
    }
    return false;
}

void pc_algo_quantize_web_palette(pc_algo_palette *out)
{
    unsigned r, g, b;
    out->n = 0;
    for (r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                pal_push(out, (uint8_t)(r * 51), (uint8_t)(g * 51), (uint8_t)(b * 51));
            }
        }
    }
}

void pc_algo_quantize_mac_palette(pc_algo_palette *out)
{
    /* Ramps use the values between cube steps (17-unit spacing, skipping multiples of 51),
     * brightest first. */
    uint8_t steps[15];
    size_t ns = 0, i;
    unsigned k;
    pc_algo_quantize_web_palette(out);
    for (k = 1; k <= 15; k++) {
        if ((k * 17) % 51 != 0) {
            steps[ns++] = (uint8_t)(k * 17);
        }
    }
    for (i = ns; i-- > 0;) {
        pal_push(out, steps[i], 0, 0);
    }
    for (i = ns; i-- > 0;) {
        pal_push(out, 0, steps[i], 0);
    }
    for (i = ns; i-- > 0;) {
        pal_push(out, 0, 0, steps[i]);
    }
    for (i = ns; i-- > 0;) {
        pal_push(out, steps[i], steps[i], steps[i]);
    }
}

void pc_algo_quantize_windows_palette(pc_algo_palette *out)
{
    static const uint8_t SYS[20][3] = {{0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},
                                       {0, 0, 128},     {128, 0, 128},   {0, 128, 128},   {192, 192, 192},
                                       {192, 220, 192}, {166, 202, 240}, {255, 251, 240}, {160, 160, 164},
                                       {128, 128, 128}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
                                       {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255}};
    pc_algo_palette web;
    size_t i;
    uint8_t g = 8;
    out->n = 0;
    for (i = 0; i < 20; i++) {
        pal_push(out, SYS[i][0], SYS[i][1], SYS[i][2]);
    }
    pc_algo_quantize_web_palette(&web);
    for (i = 0; i < web.n; i++) {
        if (!pal_contains(out, web.c[i])) {
            pal_push(out, web.c[i][0], web.c[i][1], web.c[i][2]);
        }
    }
    /* g steps through 64 distinct values (12 and 256 share a factor 4), enough to fill 256. */
    for (i = 0; out->n < 256 && i < 4096; i++) {
        uint8_t c[3] = {g, g, g};
        if (!pal_contains(out, c)) {
            pal_push(out, g, g, g);
        }
        g = (uint8_t)(g + 12u);
    }
}

pc_status pc_algo_quantize_uniform_palette(size_t n, uint8_t **out, size_t *count, pc_error *err)
{
    size_t k = pc_f32_to_size(floorf(pc_cbrtf((float)n))), total, r, g, b, i = 0;
    uint8_t *v;
    if (k < 2) {
        k = 2;
    }
    if (k > 256 || !pc_size_mul3(k, k, k, &total)) {
        return pc_fail(err, PC_ERR_LIMIT, "uniform palette of %zu colours is too large", n);
    }
    v = pc_malloc_array(total, 3);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a uniform palette");
    }
#define LV(q) pc_f32_to_u8(roundf(((float)(q) / (float)(k - 1)) * 255.0f))
    for (r = 0; r < k; r++) {
        for (g = 0; g < k; g++) {
            for (b = 0; b < k; b++) {
                v[i++] = LV(r);
                v[i++] = LV(g);
                v[i++] = LV(b);
            }
        }
    }
#undef LV
    *out = v;
    *count = total;
    return PC_OK;
}

static void forced_colors(pc_algo_forced f, pc_algo_palette *out)
{
    static const uint8_t PRIM[8][3] = {{0, 0, 0},   {255, 255, 255}, {255, 0, 0},   {0, 255, 0},
                                       {0, 0, 255}, {0, 255, 255},   {255, 0, 255}, {255, 255, 0}};
    size_t i;
    out->n = 0;
    switch (f) {
    case PC_ALGO_FORCED_BLACK_WHITE:
        pal_push(out, 0, 0, 0);
        pal_push(out, 255, 255, 255);
        break;
    case PC_ALGO_FORCED_PRIMARIES:
        for (i = 0; i < 8; i++) {
            pal_push(out, PRIM[i][0], PRIM[i][1], PRIM[i][2]);
        }
        break;
    case PC_ALGO_FORCED_WEB:
        pc_algo_quantize_web_palette(out);
        break;
    default:
        break;
    }
}

/* ---- histogram ---- */

typedef struct hist_entry {
    uint32_t rgb; /* r << 16 | g << 8 | b */
    uint32_t count;
} hist_entry;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Unique 8-bit colours of the opaque pixels with counts, sorted by colour. */
static pc_status histogram(const float (*px)[4], size_t n, hist_entry **out, size_t *count, pc_error *err)
{
    uint32_t *keys = pc_malloc_array(n > 0 ? n : 1, sizeof(uint32_t));
    hist_entry *h;
    size_t m = 0, i, nh = 0;
    if (keys == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the colour histogram");
    }
    for (i = 0; i < n; i++) {
        if (px[i][3] >= 0.5f) {
            keys[m++] = (uint32_t)to8(px[i][0]) << 16 | (uint32_t)to8(px[i][1]) << 8 | to8(px[i][2]);
        }
    }
    qsort(keys, m, sizeof *keys, cmp_u32);
    h = pc_malloc_array(m > 0 ? m : 1, sizeof *h);
    if (h == NULL) {
        free(keys);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the colour histogram");
    }
    for (i = 0; i < m; i++) {
        if (nh > 0 && h[nh - 1].rgb == keys[i]) {
            h[nh - 1].count++;
        } else {
            h[nh].rgb = keys[i];
            h[nh].count = 1;
            nh++;
        }
    }
    free(keys);
    *out = h;
    *count = nh;
    return PC_OK;
}

static void rgb_of(uint32_t k, uint8_t c[3])
{
    c[0] = (uint8_t)(k >> 16), c[1] = (uint8_t)(k >> 8), c[2] = (uint8_t)k;
}

/* ---- median cut + k-means ---- */

typedef struct wpt {
    float v[3];
    uint32_t w;
} wpt;

static int32_t total_key(float f)
{
    uint32_t u;
    int32_t i;
    memcpy(&u, &f, sizeof u);
    i = pc_u32_as_i32(u);
    return i ^ pc_u32_as_i32((uint32_t)(i >> 31) >> 1);
}

/* Stable merge sort of p[0..n) by total_cmp on axis `a` (Rust's sort_by is stable). */
static void sort_axis(wpt *p, wpt *tmp, size_t n, int a)
{
    size_t width, i;
    for (width = 1; width < n; width *= 2) {
        for (i = 0; i < n; i += 2 * width) {
            size_t mid = i + width < n ? i + width : n, end = i + 2 * width < n ? i + 2 * width : n;
            size_t l = i, r = mid, k = i;
            while (l < mid && r < end) {
                tmp[k++] = total_key(p[r].v[a]) < total_key(p[l].v[a]) ? p[r++] : p[l++];
            }
            while (l < mid) {
                tmp[k++] = p[l++];
            }
            while (r < end) {
                tmp[k++] = p[r++];
            }
        }
        memcpy(p, tmp, n * sizeof *p);
    }
}

typedef struct box {
    size_t start, len;
} box;

/* Median cut of pts (reordered in place) into at most n boxes; centres get the box means. */
static pc_status median_cut(wpt *pts, size_t np, size_t n, float (*centers)[3], size_t *nc, pc_error *err)
{
    box *boxes;
    wpt *tmp;
    size_t nb = 1, i, a;
    *nc = 0;
    if (np == 0 || n == 0) {
        return PC_OK;
    }
    boxes = pc_malloc_array(n + 1, sizeof *boxes);
    tmp = pc_malloc_array(np, sizeof *tmp);
    if (boxes == NULL || tmp == NULL) {
        free(boxes), free(tmp);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for median cut");
    }
    boxes[0].start = 0, boxes[0].len = np;
    while (nb < n) {
        size_t bi = SIZE_MAX;
        int axis = 0;
        float score = -1.0f;
        box b, rest;
        uint64_t total = 0, acc = 0;
        size_t cut = 1;
        /* Split the box with the largest weighted extent (first strictly larger wins). */
        for (i = 0; i < nb; i++) {
            const wpt *q = pts + boxes[i].start;
            size_t len = boxes[i].len, j;
            int best_a = 0;
            float best = -1.0f, wsum = -0.0f, sc;
            if (len <= 1) {
                continue;
            }
            for (a = 0; a < 3; a++) {
                float lo = 3.40282347e+38f, hi = -3.40282347e+38f;
                for (j = 0; j < len; j++) {
                    lo = pc_rs_minf(lo, q[j].v[a]);
                    hi = pc_rs_maxf(hi, q[j].v[a]);
                }
                if (hi - lo > best) {
                    best_a = (int)a;
                    best = hi - lo;
                }
            }
            for (j = 0; j < len; j++) {
                wsum += (float)q[j].w;
            }
            sc = best * sqrtf(wsum);
            if (sc > score) {
                bi = i;
                axis = best_a;
                score = sc;
            }
        }
        if (bi == SIZE_MAX || !(score > 0.0f)) {
            break;
        }
        b = boxes[bi];
        boxes[bi] = boxes[nb - 1]; /* swap_remove */
        nb--;
        sort_axis(pts + b.start, tmp, b.len, axis);
        for (i = 0; i < b.len; i++) {
            total += pts[b.start + i].w;
        }
        for (i = 0; i < b.len; i++) {
            acc += pts[b.start + i].w;
            if (acc * 2 >= total) {
                cut = i + 1 < 1 ? 1 : (i + 1 > b.len - 1 ? b.len - 1 : i + 1);
                break;
            }
        }
        rest.start = b.start + cut, rest.len = b.len - cut;
        b.len = cut;
        boxes[nb++] = b;
        boxes[nb++] = rest;
    }
    for (i = 0; i < nb; i++) {
        const wpt *q = pts + boxes[i].start;
        size_t j;
        double w = -0.0;
        for (j = 0; j < boxes[i].len; j++) {
            w += (double)q[j].w;
        }
        for (a = 0; a < 3; a++) {
            double s = -0.0;
            for (j = 0; j < boxes[i].len; j++) {
                s += (double)q[j].v[a] * (double)q[j].w;
            }
            centers[i][a] = (float)(s / w);
        }
    }
    *nc = nb;
    free(boxes), free(tmp);
    return PC_OK;
}

/* Lloyd refinement of centres over weighted points. */
static pc_status kmeans(const wpt *pts, size_t np, float (*centers)[3], size_t nc, size_t iters,
                        pc_error *err)
{
    double (*sum)[3], *cnt;
    size_t it, i, a;
    pc_algo_nearest lookup;
    if (nc == 0) { /* the forced colours fill the palette: nothing to refine */
        return PC_OK;
    }
    sum = pc_malloc_array(nc, sizeof *sum);
    cnt = pc_malloc_array(nc, sizeof *cnt);
    if (sum == NULL || cnt == NULL) {
        free(sum), free(cnt);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for k-means");
    }
    for (it = 0; it < iters; it++) {
        if (pc_algo_nearest_init(&lookup, (const float (*)[3])centers, nc) != PC_OK) {
            free(sum), free(cnt);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for k-means");
        }
        for (i = 0; i < nc; i++) {
            sum[i][0] = sum[i][1] = sum[i][2] = 0.0;
            cnt[i] = 0.0;
        }
        for (i = 0; i < np; i++) {
            size_t k = pc_algo_nearest_find(&lookup, pts[i].v);
            for (a = 0; a < 3; a++) {
                sum[k][a] += (double)pts[i].v[a] * (double)pts[i].w;
            }
            cnt[k] += (double)pts[i].w;
        }
        pc_algo_nearest_fini(&lookup);
        for (i = 0; i < nc; i++) {
            if (cnt[i] > 0.0) {
                for (a = 0; a < 3; a++) {
                    centers[i][a] = (float)(sum[i][a] / cnt[i]);
                }
            }
        }
    }
    free(sum), free(cnt);
    return PC_OK;
}

static unsigned abs_diff(uint8_t a, uint8_t b)
{
    return a > b ? (unsigned)(a - b) : (unsigned)(b - a);
}

pc_status pc_algo_quantize_build_palette(const float (*px)[4], size_t n, pc_algo_palette_kind kind,
                                         size_t colors, pc_algo_forced forced, pc_algo_palette *out,
                                         pc_error *err)
{
    pc_algo_palette pal, comp;
    size_t room, nforced, i;
    hist_entry *hist = NULL;
    size_t nh = 0;
    wpt *pts = NULL;
    float (*centers)[3] = NULL;
    size_t nc = 0;
    pc_status st = PC_OK;
    colors = colors < 2 ? 2 : (colors > 256 ? 256 : colors);
    forced_colors(forced, &pal);
    nforced = pal.n;
    room = colors > pal.n ? colors - pal.n : 0;
    comp.n = 0;
    /* Fixed palettes, or computed palettes already filled by forced colours, need no scan.
     * Exact still counts every colour to enforce its 256-colour limit. */
    if (kind == PC_ALGO_PALETTE_EXACT ||
        (room > 0 && (kind == PC_ALGO_PALETTE_ADAPTIVE || kind == PC_ALGO_PALETTE_PERCEPTUAL ||
                      kind == PC_ALGO_PALETTE_SELECTIVE))) {
        st = histogram(px, n, &hist, &nh, err);
        if (st != PC_OK) {
            return st;
        }
    }
    switch (kind) {
    case PC_ALGO_PALETTE_EXACT:
        if (nh > 256) {
            st = pc_fail(err, PC_ERR_INVALID, "the image has %zu colours; Exact needs 256 or fewer", nh);
            goto cleanup;
        }
        for (i = 0; i < nh; i++) {
            uint8_t c[3];
            rgb_of(hist[i].rgb, c);
            pal_push(&comp, c[0], c[1], c[2]);
        }
        break;
    case PC_ALGO_PALETTE_SYSTEM_MAC:
        pc_algo_quantize_mac_palette(&comp);
        break;
    case PC_ALGO_PALETTE_SYSTEM_WINDOWS:
        pc_algo_quantize_windows_palette(&comp);
        break;
    case PC_ALGO_PALETTE_WEB:
        pc_algo_quantize_web_palette(&comp);
        break;
    case PC_ALGO_PALETTE_UNIFORM: {
        uint8_t *u = NULL;
        size_t nu = 0;
        st = pc_algo_quantize_uniform_palette(room > 8 ? room : 8, &u, &nu, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        for (i = 0; i < nu && i < 256; i++) { /* room <= 256 gives at most 216 entries */
            pal_push(&comp, u[3 * i], u[3 * i + 1], u[3 * i + 2]);
        }
        free(u);
        break;
    }
    case PC_ALGO_PALETTE_ADAPTIVE:
    case PC_ALGO_PALETTE_PERCEPTUAL:
    case PC_ALGO_PALETTE_SELECTIVE:
    default:
        if (nh <= room) {
            for (i = 0; i < nh; i++) {
                uint8_t c[3];
                rgb_of(hist[i].rgb, c);
                pal_push(&comp, c[0], c[1], c[2]);
            }
            break;
        }
        pts = pc_malloc_array(nh, sizeof *pts);
        centers = pc_malloc_array(room > 0 ? room : 1, sizeof *centers);
        if (pts == NULL || centers == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for the palette");
            goto cleanup;
        }
        for (i = 0; i < nh; i++) {
            uint8_t c[3];
            rgb_of(hist[i].rgb, c);
            pts[i].w = hist[i].count;
            if (kind == PC_ALGO_PALETTE_ADAPTIVE) {
                pts[i].v[0] = (float)c[0], pts[i].v[1] = (float)c[1], pts[i].v[2] = (float)c[2];
            } else {
                float rgb[3] = {(float)c[0] / 255.0f, (float)c[1] / 255.0f, (float)c[2] / 255.0f};
                pc_srgb_to_lab(rgb, pts[i].v);
            }
        }
        /* k-means visits the points in histogram (colour) order, so it gets its own copy:
         * median cut reorders `pts`. */
        {
            wpt *mc = pc_malloc_array(nh, sizeof *mc);
            if (mc == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for the palette");
                goto cleanup;
            }
            memcpy(mc, pts, nh * sizeof *mc);
            st = median_cut(mc, nh, room, centers, &nc, err);
            free(mc);
        }
        if (st == PC_OK) {
            st = kmeans(pts, nh, centers, nc, kind == PC_ALGO_PALETTE_ADAPTIVE ? 2 : 4, err);
        }
        if (st != PC_OK) {
            goto cleanup;
        }
        for (i = 0; i < nc; i++) {
            uint8_t c[3];
            if (kind == PC_ALGO_PALETTE_ADAPTIVE) {
                c[0] = pc_f32_to_u8(pc_clampf(roundf(centers[i][0]), 0.0f, 255.0f));
                c[1] = pc_f32_to_u8(pc_clampf(roundf(centers[i][1]), 0.0f, 255.0f));
                c[2] = pc_f32_to_u8(pc_clampf(roundf(centers[i][2]), 0.0f, 255.0f));
            } else {
                float rgb[3];
                pc_lab_to_srgb(centers[i], rgb);
                c[0] = to8(rgb[0]), c[1] = to8(rgb[1]), c[2] = to8(rgb[2]);
                if (kind == PC_ALGO_PALETTE_SELECTIVE) {
                    /* Favour web-safe colours: snap entries within ~10 levels of one. */
                    uint8_t s[3];
                    int q;
                    bool near = true;
                    for (q = 0; q < 3; q++) {
                        s[q] = pc_f32_to_u8(roundf((float)c[q] / 51.0f) * 51.0f);
                        near = near && abs_diff(c[q], s[q]) <= 10;
                    }
                    if (near) {
                        memcpy(c, s, 3);
                    }
                }
            }
            pal_push(&comp, c[0], c[1], c[2]);
        }
        break;
    }
    for (i = 0; i < comp.n; i++) {
        if (pal.n >= 256) {
            break;
        }
        if (!pal_contains(&pal, comp.c[i])) {
            pal_push(&pal, comp.c[i][0], comp.c[i][1], comp.c[i][2]);
        }
    }
    if (kind == PC_ALGO_PALETTE_ADAPTIVE || kind == PC_ALGO_PALETTE_PERCEPTUAL ||
        kind == PC_ALGO_PALETTE_SELECTIVE || kind == PC_ALGO_PALETTE_UNIFORM) {
        size_t keep = colors > nforced ? colors : nforced;
        if (pal.n > keep) {
            pal.n = keep;
        }
    }
    if (pal.n == 0) {
        pal_push(&pal, 0, 0, 0);
    }
    *out = pal;
cleanup:
    free(hist), free(pts), free(centers);
    return st;
}

float pc_algo_quantize_bayer8(size_t x, size_t y)
{
    static const uint8_t M[64] = {0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
                                  12, 44, 4,  36, 14, 46, 6,  38, 60, 28, 52, 20, 62, 30, 54, 22,
                                  3,  35, 11, 43, 1,  33, 9,  41, 51, 19, 59, 27, 49, 17, 57, 25,
                                  15, 47, 7,  39, 13, 45, 5,  37, 63, 31, 55, 23, 61, 29, 53, 21};
    return ((float)M[(y % 8) * 8 + x % 8] + 0.5f) / 64.0f - 0.5f;
}

/* quantize.rs's own position hash (not fxutil's or photo_util's). */
static float qhash01(size_t x, size_t y, uint32_t seed)
{
    uint32_t h = ((uint32_t)x * 0x8da6b343u) ^ ((uint32_t)y * 0xd8163841u) ^ (seed * 0xcb1ab31fu);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xffffu) / 65535.0f;
}

/* The palette lookup: entries as f32 0..255, queries scaled by 255 (same distances as the
 * reference's former linear scan, which nearest.rs reproduces exactly). */
static size_t nearest(const pc_algo_nearest *lookup, const float c[3])
{
    float q[3] = {c[0] * 255.0f, c[1] * 255.0f, c[2] * 255.0f};
    return pc_algo_nearest_find(lookup, q);
}

typedef struct q_ctx {
    float (*px)[4];
    size_t n, stride;
    const uint8_t *pal;
    const pc_algo_nearest *lookup;
    pc_algo_dither dither;
    float spread;
    bool has_t;
    size_t t;
    uint8_t *idx;
} q_ctx;

/* One row of the dither-free / ordered / noise mapping (rows independent: parallel). */
static pc_status q_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    q_ctx *q = vctx;
    size_t y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        size_t x, i0 = y * q->stride;
        for (x = 0; x < q->stride && i0 + x < q->n; x++) {
            size_t i = i0 + x, k;
            float *p = q->px[i], pv[4], c[3], e[3];
            int a;
            memcpy(pv, p, sizeof pv);
            if (pv[3] < 0.5f) {
                if (q->has_t) {
                    q->idx[i] = (uint8_t)q->t;
                    p[0] = p[1] = p[2] = p[3] = 0.0f;
                    continue;
                }
                if (pv[3] <= 0.0f) {
                    continue;
                }
            }
            for (a = 0; a < 3; a++) {
                c[a] = pv[a];
                if (q->dither == PC_ALGO_DITHER_PATTERN) {
                    c[a] = c[a] + pc_algo_quantize_bayer8(x, y) * q->spread;
                } else if (q->dither == PC_ALGO_DITHER_NOISE) {
                    c[a] = c[a] + (qhash01(x, y, 7) - 0.5f) * q->spread;
                }
                c[a] = pc_clampf(c[a], 0.0f, 1.0f);
            }
            k = nearest(q->lookup, c);
            q->idx[i] = (uint8_t)k;
            for (a = 0; a < 3; a++) {
                e[a] = (float)q->pal[3 * k + (size_t)a] / 255.0f;
            }
            p[0] = e[0], p[1] = e[1], p[2] = e[2];
            p[3] = q->has_t ? 1.0f : pv[3];
        }
    }
    return PC_OK;
}

pc_status pc_algo_quantize(float (*px)[4], size_t n, size_t w, const uint8_t *pal, size_t npal,
                           pc_algo_dither dither, float amount, bool has_transparent, size_t transparent,
                           uint8_t *idx, pc_error *err)
{
    size_t h = w > 0 ? n / w : 0, x, y;
    float spread, a;
    float (*errbuf)[3], (*pf)[3];
    pc_algo_nearest lookup;
    pc_status st;
    if (npal == 0 || pal == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "quantize needs a non-empty palette");
    }
    pf = pc_malloc_array(npal, sizeof *pf);
    if (pf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the palette lookup");
    }
    for (x = 0; x < npal; x++) {
        pf[x][0] = (float)pal[3 * x], pf[x][1] = (float)pal[3 * x + 1], pf[x][2] = (float)pal[3 * x + 2];
    }
    st = pc_algo_nearest_init(&lookup, (const float (*)[3])pf, npal);
    free(pf);
    if (st != PC_OK) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the palette lookup");
    }
    memset(idx, 0, n);
    /* Typical palette spacing sets the ordered/noise dither amplitude. */
    spread =
        pc_rs_minf_k(1.0f / pc_rs_maxf_k(pc_cbrtf((float)npal), 1.0f), 0.5f) * pc_clampf(amount, 0.0f, 1.0f);
    if (dither != PC_ALGO_DITHER_DIFFUSION) {
        q_ctx q;
        size_t stride = w > 0 ? w : 1;
        q.px = px, q.n = n, q.stride = stride, q.pal = pal, q.lookup = &lookup, q.dither = dither;
        q.spread = spread, q.has_t = has_transparent, q.t = transparent, q.idx = idx;
        /* chunks_mut: a short last row is mapped too. */
        st = pc_algo_par_for((n + stride - 1) / stride, 8, q_rows, &q);
        pc_algo_nearest_fini(&lookup);
        return st;
    }
    if (w > (PC_MAX_ALLOC / sizeof(float[3]) - 4) / 2) {
        pc_algo_nearest_fini(&lookup);
        return pc_fail(err, PC_ERR_LIMIT, "image too wide to dither");
    }
    errbuf = pc_calloc_array(2 * (w + 2), sizeof *errbuf);
    if (errbuf == NULL) {
        pc_algo_nearest_fini(&lookup);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for error diffusion");
    }
    a = pc_clampf(amount, 0.0f, 1.0f);
    for (y = 0; y < h; y++) {
        float (*cur)[3] = errbuf, (*nxt)[3] = errbuf + (w + 2);
        /* Rows ping-pong: `cur` is row y, `nxt` row y + 1 (offset by one for x = -1). */
        memcpy(cur, nxt, (w + 2) * sizeof *cur);
        memset(nxt, 0, (w + 2) * sizeof *nxt);
        for (x = 0; x < w; x++) {
            size_t i = y * w + x, k;
            float p[4], c[3], e[3], d[3];
            int q;
            memcpy(p, px[i], sizeof p);
            if (p[3] < 0.5f) {
                if (has_transparent) {
                    idx[i] = (uint8_t)transparent;
                    px[i][0] = px[i][1] = px[i][2] = px[i][3] = 0.0f;
                    continue;
                }
                if (p[3] <= 0.0f) {
                    continue;
                }
            }
            for (q = 0; q < 3; q++) {
                c[q] = pc_clampf(p[q] + cur[x + 1][q], 0.0f, 1.0f);
            }
            k = nearest(&lookup, c);
            idx[i] = (uint8_t)k;
            for (q = 0; q < 3; q++) {
                e[q] = (float)pal[3 * k + (size_t)q] / 255.0f;
                d[q] = (c[q] - e[q]) * a;
            }
            for (q = 0; q < 3; q++) {
                cur[x + 2][q] += d[q] * 7.0f / 16.0f;
                nxt[x][q] += d[q] * 3.0f / 16.0f;
                nxt[x + 1][q] += d[q] * 5.0f / 16.0f;
                nxt[x + 2][q] += d[q] / 16.0f;
            }
            px[i][0] = e[0], px[i][1] = e[1], px[i][2] = e[2];
            px[i][3] = has_transparent ? 1.0f : p[3];
        }
    }
    free(errbuf);
    pc_algo_nearest_fini(&lookup);
    return PC_OK;
}

/* Spot function in 0..1 for (u, v) in -1..1 within a halftone cell. */
static float spot(pc_algo_halftone_shape shape, float u, float v)
{
    float s;
    switch (shape) {
    case PC_ALGO_HALFTONE_ELLIPSE:
        s = 1.0f - (u * u + 0.6f * v * v) / 1.6f;
        break;
    case PC_ALGO_HALFTONE_LINE:
        s = 1.0f - fabsf(v);
        break;
    case PC_ALGO_HALFTONE_SQUARE:
        s = 1.0f - pc_rs_maxf(fabsf(u), fabsf(v));
        break;
    case PC_ALGO_HALFTONE_DIAMOND:
        s = 1.0f - (fabsf(u) + fabsf(v)) / 2.0f;
        break;
    case PC_ALGO_HALFTONE_CROSS:
        s = 1.0f - pc_rs_minf(fabsf(u), fabsf(v));
        break;
    case PC_ALGO_HALFTONE_ROUND:
    default:
        s = 1.0f - (u * u + v * v) / 2.0f;
        break;
    }
    return pc_clampf(s, 0.0f, 1.0f);
}

static int cmp_total(const void *pa, const void *pb)
{
    int32_t a = total_key(*(const float *)pa), b = total_key(*(const float *)pb);
    return a < b ? -1 : (a > b ? 1 : 0);
}

pc_status pc_algo_quantize_to_bitmap(float *gray, size_t n, size_t w, pc_algo_bitmap_method method,
                                     pc_error *err)
{
    size_t h = w > 0 ? n / w : 0, i, x, y;
    switch (method.kind) {
    case PC_ALGO_BITMAP_THRESHOLD:
        for (i = 0; i < n; i++) {
            gray[i] = gray[i] >= 0.5f ? 1.0f : 0.0f;
        }
        return PC_OK;
    case PC_ALGO_BITMAP_PATTERN:
        if (w == 0 && n > 0) {
            return pc_fail(err, PC_ERR_INVALID, "bitmap conversion needs a non-zero width");
        }
        for (i = 0; i < n; i++) {
            gray[i] = gray[i] > pc_algo_quantize_bayer8(i % w, i / w) + 0.5f ? 1.0f : 0.0f;
        }
        return PC_OK;
    case PC_ALGO_BITMAP_DIFFUSION: {
        float *buf = pc_malloc_array(n > 0 ? n : 1, sizeof(float));
        if (buf == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for error diffusion");
        }
        memcpy(buf, gray, n * sizeof(float));
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t k = y * w + x;
                float old = buf[k], nw = old >= 0.5f ? 1.0f : 0.0f, e = old - nw;
                gray[k] = nw;
                if (x + 1 < w) {
                    buf[k + 1] += e * 7.0f / 16.0f;
                }
                if (y + 1 < h) {
                    if (x > 0) {
                        buf[k + w - 1] += e * 3.0f / 16.0f;
                    }
                    buf[k + w] += e * 5.0f / 16.0f;
                    if (x + 1 < w) {
                        buf[k + w + 1] += e / 16.0f;
                    }
                }
            }
        }
        free(buf);
        return PC_OK;
    }
    case PC_ALGO_BITMAP_HALFTONE:
    default: {
        float cell = pc_rs_maxf_k(method.cell, 2.0f), rad = method.angle * (3.14159265358979323846f / 180.0f);
        float s = sinf(rad), c = cosf(rad), table[32 * 32];
        if (w == 0 && n > 0) {
            return pc_fail(err, PC_ERR_INVALID, "bitmap conversion needs a non-zero width");
        }
        /* Rank the spot function over the cell so coverage is proportional to the tone. */
        for (i = 0; i < 32 * 32; i++) {
            table[i] = spot(method.shape, ((float)(i % 32) + 0.5f) / 16.0f - 1.0f,
                            ((float)(i / 32) + 0.5f) / 16.0f - 1.0f);
        }
        qsort(table, 32 * 32, sizeof(float), cmp_total);
        for (i = 0; i < n; i++) {
            float fx = (float)(i % w) + 0.5f, fy = (float)(i / w) + 0.5f;
            float rx = fx * c + fy * s, ry = -fx * s + fy * c;
            float u = pc_algo_rem_euclidf(rx / cell, 1.0f) * 2.0f - 1.0f;
            float vv = pc_algo_rem_euclidf(ry / cell, 1.0f) * 2.0f - 1.0f;
            float sp = spot(method.shape, u, vv), cdf;
            size_t lo = 0, hi = 32 * 32;
            while (lo < hi) { /* partition_point(|t| t < sp) */
                size_t mid = lo + (hi - lo) / 2;
                if (table[mid] < sp) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            cdf = (float)lo / 1024.0f;
            /* Ink (black) covers where the spot function exceeds the gray level. */
            gray[i] = cdf >= gray[i] ? 0.0f : 1.0f;
        }
        return PC_OK;
    }
    }
}
