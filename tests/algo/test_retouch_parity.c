/* Bit-exact parity with the L8 part of crates/algo (stack, poisson, inpaint, content_aware, seam,
 * retouch, redeye, erase, paint): replays `rustref algo-retouch-vectors`
 * (tools/rustref/src/algo_retouch_vectors.rs) and compares every line of
 * tests/golden/algo-retouch-vectors.txt. Hashes are FNV-1a over little-endian f32 bits. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/buf.h"
#include "base/num.h"
#include "harness.h"
#include "raster/pixel.h"

#define GOLDEN "tests/golden/algo-retouch-vectors.txt"
#define H0 0xcbf29ce484222325ull
#define TAU_F 6.28318530717958647692f

static pc_buf OUT;

static void line(const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    (void)pc_buf_append_str(&OUT, tmp, NULL);
    (void)pc_buf_append_byte(&OUT, '\n', NULL);
}

static void fnv(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}
static void h64(uint64_t *h, uint64_t u)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(u >> (8 * i));
    }
    fnv(h, b, 8);
}
static void hf(uint64_t *h, const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t u;
        uint8_t b[4];
        memcpy(&u, &v[i], 4);
        b[0] = (uint8_t)u, b[1] = (uint8_t)(u >> 8), b[2] = (uint8_t)(u >> 16), b[3] = (uint8_t)(u >> 24);
        fnv(h, b, 4);
    }
}
static void hf1(uint64_t *h, float v)
{
    hf(h, &v, 1);
}
static void hi32(uint64_t *h, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
    fnv(h, b, 4);
}
static void hrect(uint64_t *h, pc_rect r)
{
    hi32(h, r.x0), hi32(h, r.y0), hi32(h, r.x1), hi32(h, r.y1);
}
static uint64_t of(const float *v, size_t n)
{
    uint64_t h = H0;
    hf(&h, v, n);
    return h;
}
static uint64_t hpx(const float (*v)[4], size_t n)
{
    uint64_t h = H0;
    size_t i;
    for (i = 0; i < n; i++) {
        hf(&h, v[i], 4);
    }
    return h;
}
static uint64_t hus(const size_t *v, size_t n)
{
    uint64_t h = H0;
    size_t i;
    for (i = 0; i < n; i++) {
        h64(&h, (uint64_t)v[i]);
    }
    return h;
}
static uint64_t hs(const pc_surface *s)
{
    uint64_t h = H0;
    pc_rect cb = pc_surface_content_bounds(s);
    float *v = NULL, def[PC_RASTER_MAX_CHANNELS];
    size_t len = 0;
    uint8_t tail[2];
    hrect(&h, cb);
    CHECK(pc_surface_read_region(s, cb, &v, &len, NULL) == PC_OK);
    hf(&h, v, len);
    free(v);
    pc_surface_default_pixel(s, def);
    hf(&h, def, pc_surface_channels(s));
    tail[0] = (uint8_t)pc_surface_format(s).alpha;
    tail[1] = (uint8_t)pc_surface_channels(s);
    fnv(&h, tail, 2);
    return h;
}

static float pat(int32_t x, int32_t y, size_t c, int32_t seed)
{
    int64_t v = ((int64_t)x * 7 + (int64_t)y * 13 + (int64_t)c * 29 + (int64_t)seed * 31) % 97;
    if (v < 0) {
        v += 97;
    }
    return (float)v / 96.0f;
}
static float alpha_at(int32_t x, int32_t y, int32_t seed)
{
    int64_t v = ((int64_t)x * 3 + (int64_t)y * 5 + seed) % 7;
    if (v < 0) {
        v += 7;
    }
    return (float)v / 6.0f;
}
static void surf(pc_surface *s, pc_pixel_format fmt, pc_rect r, int32_t seed, bool opaque)
{
    size_t n = pc_pixel_format_channels(fmt), k = 0, c;
    size_t len = (size_t)pc_rect_width(r) * pc_rect_height(r) * n;
    float *v = malloc(len * sizeof(float) + 1);
    int32_t x, y;
    pc_surface_init(s, fmt);
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            for (c = 0; c < n; c++) {
                v[k++] =
                    fmt.alpha && c == n - 1 ? (opaque ? 1.0f : alpha_at(x, y, seed)) : pat(x, y, c, seed);
            }
        }
    }
    CHECK(pc_surface_write_region(s, r, v, len, NULL) == PC_OK);
    free(v);
}
static float *img(size_t w, size_t h, size_t ch, int32_t seed)
{
    float *v = malloc(w * h * ch * sizeof(float) + 4);
    size_t x, y, c, k = 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            for (c = 0; c < ch; c++) {
                v[k++] = pat((int32_t)x, (int32_t)y, c, seed);
            }
        }
    }
    return v;
}
static float *smooth(size_t w, size_t h, size_t ch, int32_t seed)
{
    float *v = malloc(w * h * ch * sizeof(float) + 4);
    size_t x, y, c, k = 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            for (c = 0; c < ch; c++) {
                float t = sinf((float)x * 0.37f + (float)y * 0.21f + (float)c + (float)seed) * 0.15f;
                v[k++] = 0.3f + 0.004f * (float)x + 0.003f * (float)y + 0.05f * (float)c + t;
            }
        }
    }
    return v;
}
static float *noise(size_t len, uint32_t levels, uint32_t seed)
{
    float *v = malloc(len * sizeof(float) + 4);
    uint32_t s = seed | 1u;
    size_t i;
    for (i = 0; i < len; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        v[i] = (float)(s % levels) / (float)(levels - 1);
    }
    return v;
}
static bool *disc(size_t w, size_t h, float cx, float cy, float r)
{
    bool *m = malloc(w * h + 1);
    size_t i;
    for (i = 0; i < w * h; i++) {
        m[i] = hypotf((float)(i % w) - cx, (float)(i / w) - cy) < r;
    }
    return m;
}
static bool *rect_mask(size_t w, size_t h, size_t x0, size_t y0, size_t x1, size_t y1)
{
    bool *m = malloc(w * h + 1);
    size_t i;
    for (i = 0; i < w * h; i++) {
        m[i] = i % w >= x0 && i % w < x1 && i / w >= y0 && i / w < y1;
    }
    return m;
}
static bool *fill_mask(size_t n, bool v)
{
    bool *m = malloc(n + 1);
    memset(m, v, n);
    return m;
}
static float *texture(size_t w, size_t h)
{
    float *v = malloc(w * h * 3 * sizeof(float) + 4);
    uint64_t s = 7ull ^ 0x9E3779B97F4A7C15ull;
    size_t i;
    for (i = 0; i < w * h; i++) {
        uint64_t z;
        float x = (float)(i % w), y = (float)(i / w), n;
        s += 0x9E3779B97F4A7C15ull;
        z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        n = (float)(z % 1000) / 1000.0f * 0.04f - 0.02f;
        v[i * 3] = 0.3f + 0.004f * x + n;
        v[i * 3 + 1] = 0.5f + 0.002f * y + n;
        v[i * 3 + 2] = 0.4f + n;
    }
    return v;
}

static const char *const STAT_NAMES[] = {
    "Entropy", "Kurtosis", "Maximum",           "Mean",      "Median",  "Minimum",
    "Range",   "Skewness", "StandardDeviation", "Summation", "Variance"};

static void stack_vectors(void)
{
    static const size_t COUNTS[] = {1, 2, 3, 5, 8};
    size_t ci, k, i, s;
    for (ci = 0; ci < 5; ci++) {
        size_t count = COUNTS[ci], lens[8];
        float (*layers[8])[4];
        const float (*cl[8])[4];
        for (k = 0; k < count; k++) {
            lens[k] = 40 + (k * 7) % 13;
            layers[k] = malloc(lens[k] * sizeof *layers[k]);
            for (i = 0; i < lens[k]; i++) {
                int32_t x = (int32_t)i, y = (int32_t)k;
                layers[k][i][0] = pat(x, y, 0, 3);
                layers[k][i][1] = pat(x, y, 1, 3) * 1.2f - 0.1f;
                layers[k][i][2] = pat(x, y, 2, 3);
                layers[k][i][3] = alpha_at(x, y, 1);
            }
            cl[k] = (const float (*)[4])layers[k];
        }
        for (s = 0; s < 11; s++) {
            float (*out)[4] = NULL;
            size_t n = 0;
            pc_algo_stack_stat st;
            CHECK(pc_algo_stack_stat_from_name(STAT_NAMES[s], strlen(STAT_NAMES[s]), &st));
            CHECK(pc_algo_stack_combine(cl, lens, count, st, &out, &n, NULL) == PC_OK);
            if (st == PC_ALGO_STACK_ENTROPY) {
                uint64_t h = H0;
                for (i = 0; i < n * 4; i++) {
                    hi32(&h, pc_f32_to_i32(roundf(out[i / 4][i % 4] * 1e4f)));
                }
                line("stack %s/%zu %zu %016llx", STAT_NAMES[s], count, n, (unsigned long long)h);
            } else {
                line("stack %s/%zu %zu %016llx", STAT_NAMES[s], count, n,
                     (unsigned long long)hpx((const float (*)[4])out, n));
            }
            free(out);
        }
        for (k = 0; k < count; k++) {
            free(layers[k]);
        }
    }
}

static void poisson_vectors(void)
{
    static const struct {
        size_t w, h;
        float cx, cy, r;
    } CASES[6] = {{80, 60, 40, 30, 20}, {96, 72, 45, 36, 28}, {5, 5, 2, 2, 1.5f},
                  {33, 17, 0, 8, 9},    {7, 40, 3, 20, 15},   {64, 64, 32, 32, 60}};
    size_t ci, i, chi;
    for (ci = 0; ci < 6; ci++) {
        size_t w = CASES[ci].w, h = CASES[ci].h, n = w * h;
        bool *unknown = disc(w, h, CASES[ci].cx, CASES[ci].cy, CASES[ci].r);
        float *init = malloc(n * sizeof(float)), *v = malloc(n * sizeof(float));
        for (i = 0; i < n; i++) {
            init[i] = sinf((float)(i % w) * 0.21f) * 0.3f + cosf((float)(i / w) * 0.13f) * 0.2f + 0.5f;
        }
        memcpy(v, init, n * sizeof(float));
        CHECK(pc_algo_poisson_solve_membrane(w, h, unknown, v, NULL, NULL) == PC_OK);
        line("poisson membrane/%zu %016llx", ci, (unsigned long long)of(v, n));
        if (n <= 2000) {
            memcpy(v, init, n * sizeof(float));
            CHECK(pc_algo_poisson_solve_membrane_reference(w, h, unknown, v, 1e-5f, 300, NULL) == PC_OK);
            line("poisson reference/%zu %016llx", ci, (unsigned long long)of(v, n));
        }
        for (chi = 0; chi < 3; chi++) {
            size_t ch = chi == 0 ? 1 : chi + 2;
            float *src = smooth(w, h, ch, 1), *dst = img(w, h, ch, 2), *o = NULL;
            CHECK(pc_algo_poisson_seamless_clone(w, h, ch, src, dst, n * ch, unknown, n, &o, NULL) == PC_OK);
            line("poisson clone/%zu/%zu %016llx", ci, ch, (unsigned long long)of(o, n * ch));
            free(o);
            CHECK(pc_algo_poisson_membrane_fill(w, h, ch, dst, n * ch, unknown, n, NULL, &o, NULL) == PC_OK);
            line("poisson fill/%zu/%zu %016llx", ci, ch, (unsigned long long)of(o, n * ch));
            free(o);
            free(src);
            free(dst);
        }
        free(unknown);
        free(init);
        free(v);
    }
    {
        float *src = img(20, 10, 2, 4), *o = NULL;
        bool *all = fill_mask(200, true), *none = fill_mask(200, false);
        CHECK(pc_algo_poisson_membrane_fill(20, 10, 2, src, 400, all, 200, NULL, &o, NULL) == PC_OK);
        line("poisson fill/all %016llx", (unsigned long long)of(o, 400));
        free(o);
        CHECK(pc_algo_poisson_membrane_fill(20, 10, 2, src, 400, none, 200, NULL, &o, NULL) == PC_OK);
        line("poisson fill/none %016llx", (unsigned long long)of(o, 400));
        free(o);
        free(src), free(all), free(none);
    }
}

static void show_complete(const char *key, size_t w, size_t h, size_t ch, const float *im, const bool *hole,
                          const pc_algo_complete_params *p)
{
    float *o = NULL;
    pc_status st = pc_algo_inpaint_complete(w, h, ch, im, w * h * ch, hole, w * h, p, NULL, &o, NULL);
    CHECK(st == PC_OK);
    if (st != PC_OK) {
        line("inpaint %s ERR", key);
    } else if (o == NULL) {
        line("inpaint %s none", key);
    } else {
        line("inpaint %s %016llx", key, (unsigned long long)of(o, w * h * ch));
    }
    free(o);
}

static void offset_str(char *buf, size_t n, bool found, int32_t x, int32_t y)
{
    if (found) {
        (void)snprintf(buf, n, "%d,%d", x, y);
    } else {
        (void)snprintf(buf, n, "none");
    }
}

static void inpaint_vectors(void)
{
    pc_algo_complete_params d = pc_algo_complete_params_default(), p;
    size_t r, ri, mi;
    {
        float *tex = texture(48, 40);
        bool *hole = disc(48, 40, 24, 20, 7);
        show_complete("complete/tex", 48, 40, 3, tex, hole, &d);
        for (r = 1; r <= 2; r++) {
            char key[64];
            p = d, p.patch_radius = r, p.seed = 5;
            (void)snprintf(key, sizeof key, "complete/tex/r%zu", r);
            show_complete(key, 48, 40, 3, tex, hole, &p);
        }
        free(tex), free(hole);
    }
    {
        float *stripes = malloc(80 * 80 * sizeof(float));
        bool *hole = disc(80, 80, 40, 40, 7);
        size_t i;
        for (i = 0; i < 6400; i++) {
            stripes[i] = ((i % 80 / 4) % 2 == 0) ? 0.2f : 0.8f;
        }
        show_complete("complete/stripes", 80, 80, 1, stripes, hole, &d);
        free(stripes), free(hole);
    }
    {
        float *im4 = smooth(40, 36, 4, 3);
        bool *hole = rect_mask(40, 36, 0, 10, 9, 20);
        show_complete("complete/edge", 40, 36, 4, im4, hole, &d);
        free(im4), free(hole);
    }
    {
        float *im3 = smooth(120, 100, 2, 1);
        bool *hole = disc(120, 100, 60, 50, 20);
        p = d, p.refine_extent = 8, p.em_iters = 4;
        show_complete("complete/pyramid", 120, 100, 2, im3, hole, &p);
        show_complete("complete/pyramid-default", 120, 100, 2, im3, hole, &d);
        free(im3), free(hole);
    }
    {
        float *a = img(10, 8, 1, 1), *b = img(6, 6, 1, 1);
        bool *no = fill_mask(80, false), *all = fill_mask(80, true), *t = disc(6, 6, 3, 3, 1);
        show_complete("complete/nohole", 10, 8, 1, a, no, &d);
        show_complete("complete/allhole", 10, 8, 1, a, all, &d);
        show_complete("complete/tiny", 6, 6, 1, b, t, &d);
        free(a), free(b), free(no), free(all), free(t);
    }
    {
        static const size_t RINGS[] = {0, 1, 3, 6};
        static const int32_t MAXR[] = {-1, 0, 1, 5, 20, 47, 200};
        size_t w = 80, h = 60, i;
        float *per = malloc(w * h * sizeof(float)), *sm = smooth(w, h, 3, 2);
        bool *hole = disc(w, h, 40, 30, 5), *no = fill_mask(w * h, false), found;
        int32_t dx = 0, dy = 0;
        for (i = 0; i < w * h; i++) {
            per[i] = sinf((float)(i % w) * TAU_F / 10.0f) * 0.4f + 0.5f;
        }
        for (ri = 0; ri < 4; ri++) {
            for (mi = 0; mi < 7; mi++) {
                char a[32], b[32];
                CHECK(pc_algo_inpaint_best_offset(w, h, 1, per, w * h, hole, w * h, RINGS[ri], MAXR[mi],
                                                  &found, &dx, &dy, NULL) == PC_OK);
                offset_str(a, sizeof a, found, dx, dy);
                CHECK(pc_algo_inpaint_best_offset(w, h, 3, sm, w * h * 3, hole, w * h, RINGS[ri], MAXR[mi],
                                                  &found, &dx, &dy, NULL) == PC_OK);
                offset_str(b, sizeof b, found, dx, dy);
                line("inpaint offset/%zu/%d %s %s", RINGS[ri], MAXR[mi], a, b);
            }
        }
        CHECK(pc_algo_inpaint_best_offset(w, h, 1, per, w * h, no, w * h, 3, 20, &found, &dx, &dy, NULL) ==
              PC_OK);
        line("inpaint offset/nohole %s", found ? "false" : "true");
        free(per), free(sm), free(hole), free(no);
    }
    {
        static const size_t BLOCKS[] = {0, 4, 8, 13};
        static const uint64_t SEEDS[] = {3, 9};
        float *tex = texture(64, 64), *o = NULL, *t10 = texture(10, 10);
        bool *hole = disc(64, 64, 32, 32, 6), *h10 = disc(10, 10, 5, 5, 2);
        size_t bi, si;
        for (bi = 0; bi < 4; bi++) {
            for (si = 0; si < 2; si++) {
                CHECK(pc_algo_inpaint_synthesize(64, 64, 3, tex, 64 * 64 * 3, hole, 64 * 64, BLOCKS[bi],
                                                 SEEDS[si], &o, NULL) == PC_OK);
                line("inpaint synth/%zu/%llu %016llx", BLOCKS[bi], (unsigned long long)SEEDS[si],
                     (unsigned long long)of(o, 64 * 64 * 3));
                free(o);
            }
        }
        CHECK(pc_algo_inpaint_synthesize(10, 10, 3, t10, 300, h10, 100, 8, 1, &o, NULL) == PC_OK);
        line("inpaint synth/fallback %016llx", (unsigned long long)of(o, 300));
        free(o), free(tex), free(t10), free(hole), free(h10);
    }
}

static void ca_fill_line(const char *key, size_t w, size_t h, size_t ch, const float *im, const bool *hole,
                         const bool *src, const pc_algo_ca_fill_options *o)
{
    float *out = NULL;
    pc_status st = pc_algo_ca_fill(w, h, ch, im, w * h * ch, hole, src, w * h, o, NULL, &out, NULL);
    CHECK(st == PC_OK);
    line("ca fill/%s %016llx", key, st == PC_OK ? (unsigned long long)of(out, w * h * ch) : 0ull);
    free(out);
}

static void stripes_ca(size_t w, size_t h, float **im, bool **hole)
{
    size_t i;
    *im = malloc(w * h * sizeof(float));
    *hole = malloc(w * h);
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        (*im)[i] = x % 6 < 3 ? 0.2f : 0.8f;
        (*hole)[i] = (x > w / 2 ? x - w / 2 : w / 2 - x) < 4 && (y > h / 2 ? y - h / 2 : h / 2 - y) < 4;
    }
}

static void content_aware_vectors(void)
{
    static const char *const ROT[] = {"none", "low", "medium", "high", "full", "x"};
    static const char *const COL[] = {"none", "default", "high", "veryHigh", "x"};
    pc_algo_ca_fill_options d = pc_algo_ca_fill_options_default(), o;
    size_t i;
    for (i = 0; i < 6; i++) {
        float r[3];
        size_t n = pc_algo_ca_rotation_level(ROT[i], r);
        line("ca rotation/%s %016llx", ROT[i], (unsigned long long)of(r, n));
    }
    for (i = 0; i < 5; i++) {
        float c = pc_algo_ca_color_level(COL[i]);
        uint32_t u;
        memcpy(&u, &c, 4);
        line("ca color/%s %08x", COL[i], u);
    }
    {
        float *im;
        bool *hole, *all = fill_mask(48 * 32, true);
        stripes_ca(48, 32, &im, &hole);
        ca_fill_line("stripes", 48, 32, 1, im, hole, all, &d);
        o = d, o.color_adaptation = 0.0f;
        ca_fill_line("stripes0", 48, 32, 1, im, hole, all, &o);
        free(im), free(hole), free(all);
    }
    {
        size_t w = 40, h = 20;
        float *lr = malloc(w * h * sizeof(float)), *ramp = malloc(w * h * sizeof(float));
        bool *hole = rect_mask(w, h, 16, 6, 24, 14), *right = malloc(w * h), *none = fill_mask(w * h, false);
        bool *all = fill_mask(w * h, true), *hole2 = rect_mask(w, h, 14, 5, 26, 15);
        for (i = 0; i < w * h; i++) {
            lr[i] = i % w < 20 ? 0.1f : 0.9f;
            right[i] = i % w >= 24;
            ramp[i] = (float)(i % w) / (float)w;
        }
        o = d, o.color_adaptation = 0.0f;
        ca_fill_line("right", w, h, 1, lr, hole, right, &o);
        ca_fill_line("nosource", w, h, 1, lr, hole, none, &d);
        o = d, o.color_adaptation = 0.9f;
        ca_fill_line("ramp", w, h, 1, ramp, hole2, all, &o);
        free(lr), free(ramp), free(hole), free(right), free(none), free(all), free(hole2);
    }
    {
        size_t w = 32, h = 24;
        float *im, *rgb = malloc(w * h * 3 * sizeof(float)), low[3], full[3], r33 = 33.0f;
        bool *hole, *src = fill_mask(w * h, true), *no = fill_mask(w * h, false);
        size_t nl = pc_algo_ca_rotation_level("low", low), nf = pc_algo_ca_rotation_level("full", full);
        stripes_ca(w, h, &im, &hole);
        for (i = 0; i < w * h; i++) {
            rgb[i * 3] = im[i], rgb[i * 3 + 1] = im[i] * 0.5f, rgb[i * 3 + 2] = 1.0f - im[i];
        }
        o = (pc_algo_ca_fill_options){0.65f, low, nl, true, true, 7};
        ca_fill_line("adapt", w, h, 3, rgb, hole, src, &o);
        o = (pc_algo_ca_fill_options){0.35f, full, nf, false, false, 2};
        ca_fill_line("full", w, h, 3, rgb, hole, src, &o);
        o = (pc_algo_ca_fill_options){0.0f, NULL, 0, false, true, 1};
        ca_fill_line("mirror", w, h, 3, rgb, hole, src, &o);
        o = (pc_algo_ca_fill_options){0.35f, &r33, 1, true, false, 4};
        ca_fill_line("scale", w, h, 3, rgb, hole, src, &o);
        ca_fill_line("nohole", w, h, 3, rgb, no, src, &d);
        free(im), free(rgb), free(hole), free(src), free(no);
    }
}

static uint32_t SEED;
static size_t next_rand(size_t n)
{
    SEED ^= SEED << 13;
    SEED ^= SEED >> 17;
    SEED ^= SEED << 5;
    return (size_t)SEED % n;
}

static void seam_vectors(void)
{
    static const size_t CHS[3] = {1, 3, 4};
    static const uint32_t LEVELS[4] = {2, 5, 255, 65536}, ELEVELS[3] = {2, 3, 1000};
    static const size_t TARGETS[5][2] = {{20, 23}, {37, 12}, {60, 30}, {1, 1}, {74, 46}};
    uint32_t c;
    size_t i;
    SEED = 1;
    for (c = 0; c < 60; c++) {
        size_t w = 1 + next_rand(24), h = 1 + next_rand(18), ch = CHS[next_rand(3)], nw, nh, n = 0;
        uint32_t lv = LEVELS[next_rand(4)];
        float *im = noise(w * h * ch, lv, 11 + c), *prot = NULL, *o = NULL;
        if (next_rand(3) == 0) {
            prot = noise(w * h, 3, 101 + c);
        }
        nw = 1 + next_rand(2 * w + 2);
        nh = 1 + next_rand(2 * h + 2);
        CHECK(pc_algo_seam_carve(w, h, ch, im, w * h * ch, prot, nw, nh, NULL, &o, &n, NULL) == PC_OK);
        line("seam carve/%u %zux%zux%zu->%zux%zu %zu %016llx", c, w, h, ch, nw, nh, n,
             (unsigned long long)of(o, n));
        free(im), free(prot), free(o);
    }
    for (c = 0; c < 30; c++) {
        size_t w = 1 + next_rand(40), h = 1 + next_rand(30), *s = NULL;
        float *e = noise(w * h, ELEVELS[next_rand(3)], 7 + c), *im, *en = NULL;
        CHECK(pc_algo_seam_find(w, h, e, w * h, &s, NULL) == PC_OK);
        line("seam find/%u %016llx", c, (unsigned long long)hus(s, h));
        im = noise(w * h * 3, 7, 3 + c);
        CHECK(pc_algo_seam_energy(w, h, 3, im, w * h * 3, &en, NULL) == PC_OK);
        line("seam energy/%u %016llx", c, (unsigned long long)of(en, w * h));
        free(s), free(e), free(im), free(en);
    }
    {
        float *sm = smooth(37, 23, 3, 1);
        for (i = 0; i < 5; i++) {
            size_t nw = TARGETS[i][0], nh = TARGETS[i][1], n = 0;
            float *o = NULL;
            CHECK(pc_algo_seam_carve(37, 23, 3, sm, 37 * 23 * 3, NULL, nw, nh, NULL, &o, &n, NULL) == PC_OK);
            line("seam smooth/%zux%zu %016llx", nw, nh, (unsigned long long)of(o, n));
            free(o);
            CHECK(pc_algo_seam_resize_bilinear(37, 23, 3, sm, 37 * 23 * 3, nw, nh, &o, NULL) == PC_OK);
            line("seam bilinear/%zux%zu %016llx", nw, nh, (unsigned long long)of(o, nw * nh * 3));
            free(o);
        }
        free(sm);
    }
    {
        float *sk = img(17, 9, 4, 5), *sk2 = img(17, 9, 2, 5), tones[64 * 3], *o = NULL;
        CHECK(pc_algo_seam_skin_mask(17, 9, 4, sk, 17 * 9 * 4, &o, NULL) == PC_OK);
        line("seam skin %016llx", (unsigned long long)of(o, 17 * 9));
        free(o);
        CHECK(pc_algo_seam_skin_mask(17, 9, 2, sk2, 17 * 9 * 2, &o, NULL) == PC_OK);
        line("seam skin/2ch %016llx", (unsigned long long)of(o, 17 * 9));
        free(o);
        for (i = 0; i < 64; i++) {
            tones[i * 3] = 0.5f + (float)(i % 8) * 0.06f;
            tones[i * 3 + 1] = 0.2f + (float)(i / 8) * 0.05f;
            tones[i * 3 + 2] = 0.15f + (float)(i % 5) * 0.05f;
        }
        CHECK(pc_algo_seam_skin_mask(8, 8, 3, tones, 192, &o, NULL) == PC_OK);
        line("seam skin/tones %016llx", (unsigned long long)of(o, 64));
        free(o), free(sk), free(sk2);
    }
}

static void retouch_vectors(void)
{
    static const char *const RN[3] = {"s", "m", "h"};
    static const pc_algo_tone_range RR[3] = {PC_ALGO_TONE_SHADOWS, PC_ALGO_TONE_MIDTONES,
                                             PC_ALGO_TONE_HIGHLIGHTS};
    const float es[7] = {-0.5f, 0.0f, 0.1f, 0.5f, 1.0f, 2.0f, NAN};
    const float es2[4] = {0.0f, 0.2f, 0.8f, 1.0f};
    const float amounts[6] = {-1.0f, 0.0f, 0.3f, 0.5f, 1.0f, 3.0f};
    float vs[33];
    size_t i, ri, bi, pi, ei, si, vi;
    int32_t k;
    for (i = 0; i < 30; i++) {
        vs[i] = (float)i / 24.0f - 0.1f;
    }
    vs[30] = NAN, vs[31] = INFINITY, vs[32] = -0.0f;
    for (ri = 0; ri < 3; ri++) {
        for (bi = 0; bi < 2; bi++) {
            bool burn = bi == 1;
            uint64_t h = H0;
            for (ei = 0; ei < 7; ei++) {
                for (vi = 0; vi < 33; vi++) {
                    hf1(&h, pc_algo_retouch_tone_curve(vs[vi], es[ei], RR[ri], burn));
                }
            }
            line("retouch curve/%s/%s %016llx", RN[ri], burn ? "true" : "false", (unsigned long long)h);
            for (pi = 0; pi < 2; pi++) {
                bool protect = pi == 1;
                uint64_t g = H0;
                for (ei = 0; ei < 4; ei++) {
                    float o[3], zero[3] = {0, 0, 0}, odd[3] = {1.2f, 0.5f, -0.1f};
                    for (k = 0; k < 60; k++) {
                        float c[3] = {pat(k, 1, 0, 2) * 1.1f - 0.05f, pat(k, 2, 1, 2),
                                      pat(k, 3, 2, 2) * 0.9f};
                        pc_algo_retouch_dodge_burn(c, es2[ei], RR[ri], burn, protect, o);
                        hf(&g, o, 3);
                    }
                    pc_algo_retouch_dodge_burn(zero, es2[ei], RR[ri], burn, protect, o);
                    hf(&g, o, 3);
                    pc_algo_retouch_dodge_burn(odd, es2[ei], RR[ri], burn, protect, o);
                    hf(&g, o, 3);
                }
                line("retouch dodge/%s/%s/%s %016llx", RN[ri], burn ? "true" : "false",
                     protect ? "true" : "false", (unsigned long long)g);
            }
        }
    }
    for (si = 0; si < 2; si++) {
        for (vi = 0; vi < 2; vi++) {
            uint64_t h = H0;
            for (ei = 0; ei < 6; ei++) {
                for (k = 0; k < 60; k++) {
                    float c[3] = {pat(k, 1, 0, 4), pat(k, 2, 1, 4) * 1.05f, pat(k, 3, 2, 4)}, o[3];
                    pc_algo_retouch_sponge(c, amounts[ei], si == 1, vi == 1, o);
                    hf(&h, o, 3);
                }
            }
            line("retouch sponge/%s/%s %016llx", si ? "true" : "false", vi ? "true" : "false",
                 (unsigned long long)h);
        }
    }
    {
        uint64_t h = H0;
        for (k = 0; k < 60; k++) {
            float c[3] = {pat(k, 1, 0, 6), pat(k, 2, 1, 6), pat(k, 3, 2, 6)};
            hf1(&h, pc_algo_retouch_saturation(c));
            hf1(&h, pc_algo_retouch_luma(c));
        }
        line("retouch sat %016llx", (unsigned long long)h);
    }
    {
        static const size_t WINS[4][4] = {{0, 0, 16, 14}, {4, 4, 12, 12}, {13, 2, 16, 5}, {5, 5, 5, 9}};
        const float sig[5] = {0.3f, 1.0f, 1.5f, 3.7f, NAN};
        const float am[3] = {1.0f, 4.0f, -0.5f};
        const bool prot[3] = {false, true, true};
        size_t w = 16, hh = 14, chi, wi;
        for (chi = 0; chi < 3; chi++) {
            size_t ch = chi == 0 ? 1 : chi + 2, alpha = ch == 4 ? 3 : SIZE_MAX;
            float *data = img(w, hh, ch, 3);
            for (wi = 0; wi < 4; wi++) {
                size_t olen = (WINS[wi][2] - WINS[wi][0]) * (WINS[wi][3] - WINS[wi][1]) * ch, ai;
                for (si = 0; si < 5; si++) {
                    float *o = NULL;
                    CHECK(pc_algo_retouch_local_blur(data, w * hh * ch, w, hh, ch, alpha, WINS[wi], sig[si],
                                                     &o, NULL) == PC_OK);
                    line("retouch blur/%zu/%zu/%zu %016llx", ch, wi, si, (unsigned long long)of(o, olen));
                    free(o);
                }
                for (ai = 0; ai < 3; ai++) {
                    float *o = NULL;
                    CHECK(pc_algo_retouch_local_sharpen(data, w * hh * ch, w, hh, ch, alpha, WINS[wi], am[ai],
                                                        prot[ai], &o, NULL) == PC_OK);
                    line("retouch sharpen/%zu/%zu/%zu %016llx", ch, wi, ai, (unsigned long long)of(o, olen));
                    free(o);
                }
            }
            free(data);
        }
    }
}

static bool disc_px(size_t x, size_t y, float cx, float cy, float r)
{
    return hypotf((float)x - cx, (float)y - cy) <= r;
}

static float (*portrait(size_t w))[4]
{
    float (*px)[4] = malloc(w * w * sizeof *px);
    size_t i;
    for (i = 0; i < w * w; i++) {
        size_t x = i % w, y = i / w;
        const float white[4] = {1, 1, 1, 1}, red[4] = {0.95f, 0.10f, 0.10f, 1},
                    iris[4] = {0.38f, 0.28f, 0.16f, 1};
        memcpy(px[i], disc_px(x, y, 16, 16, 1.2f) ? white : (disc_px(x, y, 16, 16, 10) ? red : iris), 16);
    }
    return px;
}

static void redeye_vectors(void)
{
    const float pr[10] = {-5.0f, 0.0f, 1.0f, 10.0f, 50.0f, 99.9f, 100.0f, 150.0f, NAN, INFINITY};
    const float sides[11] = {-500.0f, -5.0f, 0.0f, 1.0f, 50.0f, 100.0f, 200.0f, 205.0f, 210.0f, 1e9f, NAN};
    const float pd[6][2] = {{50, 100}, {50, 50}, {1, 50}, {10, 0}, {NAN, NAN}, {100, 250}};
    static const int32_t CO[4][4] = {{16, 16, 0, 0}, {20, 14, 0, 0}, {116, 116, 100, 100}, {60, 60, 0, 0}};
    uint64_t h = H0;
    size_t i, pi, ci;
    uint32_t n = 0;
    line("redeye halo %d", pc_algo_redeye_halo_radius());
    for (i = 0; i < 10; i++) {
        hf1(&h, pc_algo_redeye_search_radius(pr[i]));
    }
    line("redeye radius %016llx", (unsigned long long)h);
    for (i = 0; i < 11; i++) {
        uint32_t s = 0;
        bool ok = pc_algo_redeye_unclamped_window_side(sides[i], &s);
        line("redeye side/%zu %lld", i, ok ? (long long)s : -1ll);
    }
    for (pi = 0; pi < 6; pi++) {
        for (ci = 0; ci < 4; ci++) {
            float (*px)[4] = portrait(32);
            CHECK(pc_algo_redeye_apply(px, 1024, 32, 32, CO[ci][0], CO[ci][1], CO[ci][2], CO[ci][3],
                                       pd[pi][0], pd[pi][1], &n, NULL) == PC_OK);
            line("redeye portrait/%zu/%zu %u %016llx", pi, ci, n,
                 (unsigned long long)hpx((const float (*)[4])px, 1024));
            free(px);
        }
    }
    {
        size_t w = 128;
        float (*half)[4] = malloc(w * w * sizeof *half), (*noisy)[4] = malloc(1200 * sizeof *noisy);
        int32_t k;
        for (i = 0; i < w * w; i++) {
            const float a[4] = {0.95f, 0.10f, 0.10f, 1}, b[4] = {0.38f, 0.28f, 0.16f, 1};
            memcpy(half[i], i % w < 64 ? a : b, 16);
        }
        CHECK(pc_algo_redeye_apply(half, w * w, w, w, 60, 64, 0, 0, 50, 50, &n, NULL) == PC_OK);
        line("redeye half %u %016llx", n, (unsigned long long)hpx((const float (*)[4])half, w * w));
        for (k = 0; k < 1200; k++) {
            noisy[k][0] = pat(k, 0, 0, 1), noisy[k][1] = pat(k, 0, 1, 1) * 0.5f,
            noisy[k][2] = pat(k, 0, 2, 1) * 0.4f;
            noisy[k][3] = 1.0f;
        }
        CHECK(pc_algo_redeye_apply(noisy, 1200, 40, 30, 20, 15, 0, 0, 30, 70, &n, NULL) == PC_OK);
        line("redeye noisy %u %016llx", n, (unsigned long long)hpx((const float (*)[4])noisy, 1200));
        free(half), free(noisy);
    }
}

typedef struct fmt_entry {
    const char *name;
    pc_pixel_format f;
} fmt_entry;

static const fmt_entry FORMATS[9] = {
    {"rgba8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true}},
    {"rgba16", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}},
    {"rgba32", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}},
    {"rgb8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false}},
    {"gray8", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false}},
    {"graya16", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, true}},
    {"cmyka8", {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true}},
    {"laba16", {PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U16, true}},
    {"laba32", {PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_F32, true}},
};

static void region(pc_algo_region *rg, pc_rect bbox, int32_t seed)
{
    size_t w = pc_rect_width(bbox), h = pc_rect_height(bbox), i;
    rg->bbox = bbox;
    rg->len = w * h;
    rg->mask = malloc(w * h + 1);
    for (i = 0; i < w * h; i++) {
        float v = pat((int32_t)i, (int32_t)(i / 5), 0, seed);
        rg->mask[i] = v < 0.2f ? 0 : (v > 0.7f ? 255 : pc_f32_to_u8(v * 255.0f));
    }
}

static void gray_sel(pc_surface *s, int32_t w, int32_t h, int32_t seed)
{
    float *v = malloc((size_t)(w * h) * sizeof(float));
    int32_t i;
    for (i = 0; i < w * h; i++) {
        v[i] = pat(i, i / w, 0, seed);
    }
    pc_surface_init(s, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_write_region(s, pc_rect_make(0, 0, w, h), v, (size_t)(w * h), NULL) == PC_OK);
    free(v);
}

static void erase_vectors(void)
{
    const struct {
        float amount;
        bool sel, lock;
    } CASES[5] = {{1.0f, false, false},
                  {0.5f, false, false},
                  {0.7f, true, false},
                  {0.6f, false, true},
                  {NAN, false, false}};
    const float lock_c[4] = {0.9f, 0.1f, 0.4f, 0.2f};
    pc_surface sel, s;
    pc_algo_region rg;
    size_t fi, ai;
    gray_sel(&sel, 200, 200, 9);
    region(&rg, pc_rect_make(-5, 3, 270, 260), 4);
    for (fi = 0; fi < 9; fi++) {
        for (ai = 0; ai < 5; ai++) {
            pc_rect d = PC_RECT_EMPTY;
            uint64_t h = H0;
            surf(&s, FORMATS[fi].f, pc_rect_make(-20, -10, 300, 270), 2, false);
            CHECK(pc_algo_erase_magic(&s, &rg, CASES[ai].amount, CASES[ai].sel ? &sel : NULL,
                                      CASES[ai].lock ? lock_c : NULL, 4, &d, NULL) == PC_OK);
            hrect(&h, d);
            line("erase %s/%zu %016llx %016llx", FORMATS[fi].name, ai, (unsigned long long)h,
                 (unsigned long long)hs(&s));
            pc_surface_fini(&s);
        }
    }
    {
        pc_algo_region bad = {pc_rect_make(0, 0, 4, 4), NULL, 3};
        uint8_t m[3] = {255, 255, 255};
        pc_rect d = pc_rect_make(1, 1, 2, 2);
        bad.mask = m;
        surf(&s, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 8, 8), 1, true);
        CHECK(pc_algo_erase_magic(&s, &bad, 1.0f, NULL, NULL, 0, &d, NULL) == PC_OK);
        line("erase mismatch %s %016llx", pc_rect_is_empty(d) ? "true" : "false", (unsigned long long)hs(&s));
        pc_surface_fini(&s);
    }
    free(rg.mask);
    pc_surface_fini(&sel);
}

static void pattern_src(void *ctx, int32_t x, int32_t y, float out[4])
{
    (void)ctx;
    out[0] = pat(x, y, 0, 1), out[1] = pat(x, y, 1, 1), out[2] = pat(x, y, 2, 1), out[3] = 0.5f;
}

static void blue_src(void *ctx, int32_t x, int32_t y, float out[4])
{
    (void)ctx, (void)x, (void)y;
    out[0] = 0.2f, out[1] = 0.4f, out[2] = 0.8f, out[3] = 1.0f;
}

static void paint_vectors(void)
{
    static const char *const SN[5] = {"linear", "radial", "angle", "reflected", "diamond"};
    const float ft[3][4] = {{0, 0, 10, 0}, {3.5f, -2.0f, -7.25f, 11.0f}, {1, 1, 1, 1}};
    const pc_algo_gradient_stop stops3[4] = {
        {0.0f, {0, 0, 0, 1}}, {0.3f, {1, 0, 0, 0.5f}}, {0.3f, {0, 1, 0, 1}}, {1.0f, {1, 1, 1, 0}}};
    const struct {
        const char *key;
        pc_algo_gradient_shape shape;
        bool rev, dither;
        pc_blend_mode mode;
        bool sel, lock;
    } GC[4] = {{"a", PC_ALGO_GRADIENT_LINEAR, false, false, PC_BLEND_NORMAL, false, false},
               {"b", PC_ALGO_GRADIENT_RADIAL, true, true, PC_BLEND_MULTIPLY, true, false},
               {"c", PC_ALGO_GRADIENT_ANGLE, false, true, PC_BLEND_SCREEN, false, true},
               {"d", PC_ALGO_GRADIENT_DIAMOND, true, false, PC_BLEND_OVERLAY, true, true}};
    const struct {
        bool contig, aa;
        float tol;
    } BC[3] = {{true, false, 10.0f}, {false, true, 30.0f}, {true, true, 0.0f}};
    const float red[4] = {1, 0, 0, 1}, from[2] = {3.0f, 4.0f}, to[2] = {250.5f, 80.0f};
    pc_surface sel;
    size_t si, fi, gi, bi, n;
    int i;
    for (si = 0; si < 5; si++) {
        uint64_t h = H0;
        size_t c;
        pc_algo_gradient_shape shape;
        CHECK(pc_algo_gradient_shape_from_name(SN[si], strlen(SN[si]), &shape));
        for (c = 0; c < 3; c++) {
            for (i = 0; i < 80; i++) {
                float x = (float)(i % 9) * 1.7f - 4.0f, y = (float)(i / 9) * 1.3f - 3.0f;
                hf1(&h, pc_algo_paint_gradient_t(shape, ft[c], ft[c] + 2, x, y));
            }
        }
        line("paint t/%s %016llx", SN[si], (unsigned long long)h);
    }
    {
        uint64_t h = H0;
        static const size_t NS[4] = {0, 1, 2, 4};
        for (n = 0; n < 4; n++) {
            for (i = 0; i < 25; i++) {
                float o[4];
                pc_algo_paint_sample_stops(stops3, NS[n], (float)i / 20.0f - 0.1f, o);
                hf(&h, o, 4);
            }
        }
        line("paint stops %016llx", (unsigned long long)h);
    }
    gray_sel(&sel, 150, 100, 3);
    for (fi = 0; fi < 9; fi++) {
        pc_pixel_format fmt = FORMATS[fi].f;
        pc_surface s, t;
        float c1[PC_RASTER_MAX_CHANNELS], c2[PC_RASTER_MAX_CHANNELS];
        const float rgba1[4] = {0.9f, 0.9f, 0.9f, 1.0f}, rgba2[4] = {0.1f, 0.2f, 0.3f, 1.0f};
        pc_rect a = pc_rect_make(0, 0, 40, 30);
        pc_algo_region rg;
        bool ok = false;
        size_t nc;
        for (gi = 0; gi < 4; gi++) {
            surf(&s, fmt, pc_rect_make(0, 0, 260, 90), 5, false);
            CHECK(pc_algo_paint_gradient(&s, pc_rect_make(-10, -5, 290, 120), from, to, GC[gi].shape, stops3,
                                         4, GC[gi].rev, 0.8f, GC[gi].mode, GC[gi].dither,
                                         GC[gi].sel ? &sel : NULL, GC[gi].lock, NULL) == PC_OK);
            line("paint gradient/%s/%s %016llx", FORMATS[fi].name, GC[gi].key, (unsigned long long)hs(&s));
            pc_surface_fini(&s);
        }
        pc_surface_init(&s, fmt);
        nc = pc_from_rgba(fmt, rgba1, c1);
        (void)pc_from_rgba(fmt, rgba2, c2);
        CHECK(pc_surface_fill_rect(&s, a, c1, nc, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&s, pc_rect_make(18, 0, 22, 30), c2, nc, NULL) == PC_OK);
        for (bi = 0; bi < 3; bi++) {
            CHECK(pc_surface_clone(&s, &t, NULL) == PC_OK);
            CHECK(pc_algo_paint_bucket_fill(&t, a, 3, 4, BC[bi].tol, BC[bi].contig, BC[bi].aa, red, 0.75f,
                                            &sel, false, &ok, NULL) == PC_OK);
            line("paint bucket/%s/%zu %s %016llx", FORMATS[fi].name, bi, ok ? "true" : "false",
                 (unsigned long long)hs(&t));
            pc_surface_fini(&t);
        }
        CHECK(pc_surface_clone(&s, &t, NULL) == PC_OK);
        CHECK(pc_algo_paint_bucket_fill_src(&t, a, 30, 10, 5.0f, true, false, 1.0f, NULL, true, pattern_src,
                                            NULL, &ok, NULL) == PC_OK);
        line("paint bucketsrc/%s %s %016llx", FORMATS[fi].name, ok ? "true" : "false",
             (unsigned long long)hs(&t));
        pc_surface_fini(&t);
        CHECK(pc_surface_clone(&s, &t, NULL) == PC_OK);
        CHECK(pc_algo_paint_bucket_fill(&t, a, 50, 1, 10.0f, true, false, red, 1.0f, NULL, false, &ok,
                                        NULL) == PC_OK);
        line("paint bucket/%s/outside %s", FORMATS[fi].name, ok ? "true" : "false");
        pc_surface_fini(&t);
        CHECK(pc_surface_clone(&s, &t, NULL) == PC_OK);
        region(&rg, pc_rect_make(-3, 4, 25, 20), 6);
        CHECK(pc_algo_paint_bucket_fill_region_src(&t, &rg, 0.5f, &sel, false, blue_src, NULL, NULL) ==
              PC_OK);
        line("paint region/%s %016llx", FORMATS[fi].name, (unsigned long long)hs(&t));
        free(rg.mask);
        pc_surface_fini(&t);
        pc_surface_fini(&s);
    }
    pc_surface_fini(&sel);
}

/* ------------------------------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *data = NULL;
    long n;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = malloc((size_t)n + 2);
        if (data != NULL) {
            *len = fread(data, 1, (size_t)n, f);
            data[*len] = '\0';
        }
    }
    (void)fclose(f);
    return data;
}

static void test_retouch_parity(void)
{
    size_t glen = 0, lines = 0;
    char *golden = read_file(GOLDEN, &glen), *want, *got, *wsave = NULL, *gsave = NULL;
    CHECK(golden != NULL);
    if (golden == NULL) {
        return;
    }
    pc_buf_init(&OUT);
    stack_vectors();
    poisson_vectors();
    inpaint_vectors();
    content_aware_vectors();
    seam_vectors();
    retouch_vectors();
    redeye_vectors();
    erase_vectors();
    paint_vectors();
    want = strtok_r(golden, "\n", &wsave);
    got = strtok_r((char *)OUT.data, "\n", &gsave);
    while (want != NULL || got != NULL) {
        lines++;
        if (want == NULL || got == NULL || strcmp(want, got) != 0) {
            PC_T_FAIL("line %zu:\n  want %.400s\n  got  %.400s", lines, want != NULL ? want : "(end)",
                      got != NULL ? got : "(end)");
        } else {
            pc_t_checks++;
        }
        want = want != NULL ? strtok_r(NULL, "\n", &wsave) : NULL;
        got = got != NULL ? strtok_r(NULL, "\n", &gsave) : NULL;
    }
    pc_buf_fini(&OUT);
    free(golden);
}

int main(int argc, char **argv)
{
    RUN(test_retouch_parity);
    return pc_t_finish();
}
