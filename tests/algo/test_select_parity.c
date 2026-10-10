/* Bit-exact parity with the L7 part of crates/algo (selection, magnetic, matting, segment): replays
 * `rustref algo-select-vectors` (tools/rustref/src/algo_select_vectors.rs) with the same synthetic
 * inputs and compares every line of tests/golden/algo-select-vectors.txt. Hashes are FNV-1a over
 * little-endian f32/f64 bits. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/buf.h"
#include "base/num.h"
#include "base/rmath.h"
#include "harness.h"

#define GOLDEN "tests/golden/algo-select-vectors.txt"
#define H0 0xcbf29ce484222325ull

static pc_buf OUT;

static void line(const char *fmt, ...)
{
    char tmp[8192];
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
static void hd(uint64_t *h, const double *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint64_t u;
        memcpy(&u, &v[i], 8);
        h64(h, u);
    }
}
static void hd1(uint64_t *h, double v)
{
    hd(h, &v, 1);
}
static void hi32(uint64_t *h, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
    fnv(h, b, 4);
}
static void hrect(uint64_t *h, pc_rect r)
{
    hi32(h, r.x0);
    hi32(h, r.y0);
    hi32(h, r.x1);
    hi32(h, r.y1);
}
static uint64_t of(const float *v, size_t n)
{
    uint64_t h = H0;
    hf(&h, v, n);
    return h;
}
static uint64_t ob(const bool *v, size_t n)
{
    uint64_t h = H0;
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t b = v[i] ? 1 : 0;
        fnv(&h, &b, 1);
    }
    return h;
}
static uint64_t o8(const uint8_t *v, size_t n)
{
    uint64_t h = H0;
    fnv(&h, v, n);
    return h;
}
static uint64_t opts(const double (*p)[2], size_t n)
{
    uint64_t h = H0;
    size_t i;
    h64(&h, n);
    for (i = 0; i < n; i++) {
        hd(&h, p[i], 2);
    }
    return h;
}
static const char *oreg(const pc_algo_region *r, char *buf)
{
    uint64_t h = H0;
    if (pc_algo_region_is_none(r)) {
        return "none";
    }
    hrect(&h, r->bbox);
    fnv(&h, r->mask, r->len);
    (void)sprintf(buf, "%016llx", (unsigned long long)h);
    return buf;
}
static uint64_t hs(const pc_surface *s)
{
    uint64_t h = H0;
    pc_rect cb = pc_surface_content_bounds(s);
    float *v = NULL, dp[PC_RASTER_MAX_CHANNELS];
    size_t len = 0;
    uint8_t b[2];
    hrect(&h, cb);
    if (pc_surface_read_region(s, cb, &v, &len, NULL) == PC_OK) {
        hf(&h, v, len);
    }
    free(v);
    pc_surface_default_pixel(s, dp);
    hf(&h, dp, pc_surface_channels(s));
    b[0] = pc_surface_format(s).alpha ? 1 : 0;
    b[1] = (uint8_t)pc_surface_channels(s);
    fnv(&h, b, 2);
    return h;
}
static const char *osurf(bool some, const pc_surface *s, char *buf)
{
    if (!some) {
        return "none";
    }
    (void)sprintf(buf, "%016llx", (unsigned long long)hs(s));
    return buf;
}
static uint64_t orgb(const pc_algo_rgb_image *img)
{
    uint64_t h = H0;
    h64(&h, img->w);
    h64(&h, img->h);
    hf(&h, (const float *)img->px, img->w * img->h * 3);
    return h;
}
static uint64_t ogmm(const pc_algo_gmm *g)
{
    uint64_t h = H0;
    size_t i;
    int k;
    h64(&h, g->n);
    for (i = 0; i < g->n; i++) {
        hf1(&h, g->comps[i].weight);
        hf(&h, g->comps[i].mean, 3);
        for (k = 0; k < 3; k++) {
            hf(&h, g->comps[i].cov[k], 3);
        }
    }
    for (k = 0; k < 27; k++) {
        float z[3] = {(float)(k % 3) / 2.0f, (float)((k / 3) % 3) / 2.0f, (float)(k / 9) / 2.0f};
        hd1(&h, pc_algo_gmm_log_prob(g, z));
        hf1(&h, pc_algo_gmm_neg_log(g, z));
        h64(&h, pc_algo_gmm_component_of(g, z));
    }
    return h;
}
#define X(v) ((unsigned long long)(v))

/* ---- synthetic inputs (as in the rustref module) ---- */

static int64_t rem_euclid(int64_t a, int64_t b)
{
    int64_t r = a % b;
    return r < 0 ? r + b : r;
}
static float pat(int32_t x, int32_t y, size_t c, int32_t seed)
{
    return (float)rem_euclid((int64_t)x * 7 + (int64_t)y * 13 + (int64_t)c * 29 + (int64_t)seed * 31, 97) /
           96.0f;
}
static float (*rgba_img(int32_t w, int32_t h, int32_t seed))[4]
{
    float (*v)[4] = malloc((size_t)(w * h) * sizeof *v);
    int32_t x, y, i = 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++, i++) {
            float b = (float)((x / 4 + y / 3 + seed) % 3) / 2.0f, n = pat(x, y, 0, seed) * 0.05f;
            float a = rem_euclid(x * 3 + y * 5 + seed, 17) == 0 ? 0.0f : 1.0f;
            v[i][0] = b + n;
            v[i][1] = 1.0f - b;
            v[i][2] = pc_rs_minf_k(b * 0.5f + pat(x, y, 2, seed) * 0.1f, 1.0f);
            v[i][3] = a;
        }
    }
    return v;
}
static uint8_t (*blobs(int32_t w, int32_t h))[4]
{
    uint8_t (*v)[4] = malloc((size_t)(w * h) * 4);
    int32_t i;
    for (i = 0; i < w * h; i++) {
        int32_t x = i % w, y = i / w;
        uint8_t val = (uint8_t)((x * 7 + y * 3) % 23 + ((x / 5 + y / 4) % 3 == 0 ? 120 : 0));
        v[i][0] = val;
        v[i][1] = val / 2;
        v[i][2] = (uint8_t)(255 - val);
        v[i][3] = x % 11 == 0 ? 128 : 255;
    }
    return v;
}
static float *mask_field(int32_t w, int32_t h, int32_t seed)
{
    float cx = (float)w * 0.45f, cy = (float)h * 0.5f, r = (float)(w < h ? w : h) * 0.3f;
    float *v = malloc((size_t)(w * h) * sizeof(float) + 1);
    int32_t x, y, i = 0;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++, i++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy) - r;
            float m = pc_clampf(0.5f - d, 0.0f, 1.0f);
            if (rem_euclid(x * 5 + y * 7 + seed, 41) == 0) {
                m = 1.0f;
            }
            v[i] = m;
        }
    }
    return v;
}
static void disc_scene(size_t w, size_t h, float cx, float cy, float r, uint64_t seed, pc_algo_rgb_image *img)
{
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(seed);
    size_t x, y;
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, n, *p = img->px[y * w + x];
            bool inside = dx * dx + dy * dy <= r * r;
            int k;
            n = 0.05f * pc_algo_segment_rng_normal(&rng);
            if (inside) {
                float t = pc_clampf(sinf((float)(x + y) * 0.35f) * 0.5f + 0.5f, 0.0f, 1.0f);
                p[0] = 0.85f + 0.05f * t + n;
                p[1] = 0.2f + 0.3f * t + n;
                p[2] = 0.15f + n;
            } else {
                p[0] = 0.25f + n;
                p[1] = 0.45f + n;
                p[2] = 0.7f + n;
            }
            for (k = 0; k < 3; k++) {
                p[k] = pc_clampf(p[k], 0.0f, 1.0f);
            }
        }
    }
}
static void two_regions(size_t w, size_t h, pc_algo_rgb_image *img)
{
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(5);
    size_t x, y;
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float n = 0.04f * pc_algo_segment_rng_normal(&rng), *p = img->px[y * w + x];
            float t = (sinf((float)x * 0.5f) * cosf((float)y * 0.3f)) * 0.05f;
            int k;
            if (x < 60) {
                p[0] = 0.8f + t + n;
                p[1] = 0.5f + n;
                p[2] = 0.3f + t;
            } else {
                p[0] = 0.2f + n;
                p[1] = 0.35f + t;
                p[2] = 0.6f + n;
            }
            for (k = 0; k < 3; k++) {
                p[k] = pc_clampf(p[k], 0.0f, 1.0f);
            }
        }
    }
}
static void landscape(size_t w, size_t h, pc_algo_rgb_image *img)
{
    size_t horizon = h * 3 / 5, x, y;
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float cx = (float)w * 0.7f, cy = (float)h * 0.35f, rr = (float)h * 0.18f, *p = img->px[y * w + x];
            float ddx = (float)x - cx, ddy = (float)y - cy;
            bool in_crown = sqrtf(ddx * ddx + ddy * ddy) < rr;
            bool in_trunk = fabsf((float)x - cx) < 3.0f && (float)y > cy && y < horizon;
            float n = ((float)((x * 7919 + y * 104729) % 97) / 97.0f - 0.5f) * 0.25f;
            if (in_crown) {
                p[0] = 0.1f + n * 0.5f;
                p[1] = 0.35f + n;
                p[2] = 0.1f + n * 0.5f;
            } else if (in_trunk) {
                p[0] = 0.3f + n * 0.3f;
                p[1] = 0.2f;
                p[2] = 0.1f;
            } else if (y < horizon) {
                float t = (float)y / (float)horizon;
                p[0] = 0.25f + 0.4f * t;
                p[1] = 0.5f + 0.3f * t;
                p[2] = 0.9f + 0.05f * t;
            } else {
                p[0] = 0.35f + n;
                p[1] = 0.3f + n;
                p[2] = 0.15f + n * 0.5f;
            }
        }
    }
}
static void surf(pc_pixel_format fmt, pc_rect r, int32_t seed, pc_surface *s)
{
    size_t n = pc_pixel_format_channels(fmt), c, i = 0;
    float *v = malloc((size_t)pc_rect_width(r) * pc_rect_height(r) * n * sizeof(float) + 4);
    int32_t x, y;
    pc_surface_init(s, fmt);
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            for (c = 0; c < n; c++) {
                v[i++] = fmt.alpha && c == n - 1 ? (float)rem_euclid(x * 3 + y * 5 + seed, 7) / 6.0f
                                                 : pat(x, y, c, seed);
            }
        }
    }
    (void)pc_surface_write_region(s, r, v, i, NULL);
    free(v);
}
typedef struct fmtname {
    const char *name;
    pc_pixel_format f;
} fmtname;
static const fmtname FORMATS[] = {
    {"rgba8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true}},
    {"rgba16", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}},
    {"rgba32", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}},
    {"rgb8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false}},
    {"gray8", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false}},
    {"cmyka8", {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true}},
    {"laba16", {PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U16, true}},
};
#define NFORMATS (sizeof FORMATS / sizeof FORMATS[0])
static const char *MODE_NAMES[] = {"Replace", "Add", "Subtract", "Intersect"};

#include "select_parity_cases.h"

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

static void test_select_parity(void)
{
    size_t glen = 0, lines = 0;
    char *golden = read_file(GOLDEN, &glen), *want, *got, *wsave = NULL, *gsave = NULL;
    CHECK(golden != NULL);
    if (golden == NULL) {
        return;
    }
    pc_buf_init(&OUT);
    selection_cases();
    magnetic_cases();
    matting_cases();
    segment_core_cases();
    segment_algo_cases();
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
    RUN(test_select_parity);
    return pc_t_finish();
}
