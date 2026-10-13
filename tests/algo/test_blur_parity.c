/* Parity with the L5 part of crates/algo: replays `rustref algo-blur-vectors`
 * (tools/rustref/src/algo_blur_vectors.rs) and compares every line of
 * tests/golden/algo-blur-vectors.txt. Hashes are FNV-1a over little-endian f32 bits; the filter
 * cases are the golden file's own `case` JSON, re-serialised here and compared as text.
 *
 * Every line must match bit for bit except the `fft` lines: the FFT Motion Blur uses pocketfft
 * where the reference uses rustfft, so its samples are compared within 2e-5 (absolute) plus 1e-5
 * relative (REWRITE_PLAN.md section 7). They are computed with the FFT path forced, so the
 * comparison doesn't depend on the thread count that decides the path in production. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "algo/blur_int.h"
#include "base/buf.h"
#include "base/num.h"
#include "base/rmath.h"
#include "harness.h"
#include "raster/pixel.h"

#define GOLDEN "tests/golden/algo-blur-vectors.txt"
#define H0 0xcbf29ce484222325ull
#define MAX_CASES 256

static pc_buf OUT;

static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char *fmt, ...)
{
    char tmp[16384];
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
static void hf(uint64_t *h, const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t u;
        uint8_t b[4];
        memcpy(&u, &v[i], 4);
        if (isnan(v[i])) {
            u = 0x7fc00000u; /* NaN sign and payload depend on the compiler's operand order */
        }
        b[0] = (uint8_t)u, b[1] = (uint8_t)(u >> 8), b[2] = (uint8_t)(u >> 16), b[3] = (uint8_t)(u >> 24);
        fnv(h, b, 4);
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
static void hrect(uint64_t *h, pc_rect r)
{
    int32_t v[4] = {r.x0, r.y0, r.x1, r.y1};
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t u = (uint32_t)v[i];
        uint8_t b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
        fnv(h, b, 4);
    }
}
static uint64_t of(const float *v, size_t n)
{
    uint64_t h = H0;
    hf(&h, v, n);
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
/* A failed operation hashes as a marker that never matches a reference value. */
static uint64_t hs_st(pc_status st, pc_surface *s)
{
    uint64_t h;
    if (st != PC_OK) {
        return 0xbad;
    }
    h = hs(s);
    pc_surface_fini(s);
    return h;
}

static float pat(int32_t x, int32_t y, size_t c, int32_t seed)
{
    int64_t v = ((int64_t)x * 7 + (int64_t)y * 13 + (int64_t)c * 29 + (int64_t)seed * 31) % 97;
    return (float)(v < 0 ? v + 97 : v) / 96.0f;
}
static float alpha_at(int32_t x, int32_t y, int32_t seed)
{
    int64_t v = ((int64_t)x * 3 + (int64_t)y * 5 + seed) % 7;
    return (float)(v < 0 ? v + 7 : v) / 6.0f;
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

typedef struct fmt_entry {
    const char *name;
    pc_pixel_format f;
} fmt_entry;

static const fmt_entry FORMATS[] = {
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
#define NFORMATS (sizeof FORMATS / sizeof FORMATS[0])

/* ---- the cases (from the golden file) ---- */

static char *CASE_JSON[MAX_CASES];
static size_t NCASES;
static pc_algo_image DEPTH;

static void depth_map_init(void)
{
    pc_rect r = pc_rect_make(-30, -30, 70, 60);
    size_t w = pc_rect_width(r), i;
    CHECK(pc_algo_image_init(&DEPTH, r, 1, NULL) == PC_OK);
    for (i = 0; i < DEPTH.len; i++) {
        DEPTH.data[i] = pat(r.x0 + (int32_t)(i % w), r.y0 + (int32_t)(i / w), 0, 5);
    }
}

static bool parse(size_t i, pc_algo_filter *f)
{
    pc_error err;
    if (pc_algo_filter_from_json_str(CASE_JSON[i], strlen(CASE_JSON[i]), f, &err) != PC_OK) {
        PC_T_FAIL("case %zu: %s", i, err.msg);
        return false;
    }
    if (f->def == &PC_ALGO_FILTER_LENS_BLUR) {
        pc_algo_lens_blur_params *p = f->params;
        if (p->depth == PC_ALGO_DEPTH_LAYER_MASK) {
            p->depth_map = &DEPTH;
        }
    }
    return true;
}

static void halo_text(pc_algo_halo h, char *out, size_t cap)
{
    if (h.bounds) {
        (void)snprintf(out, cap, "bounds");
    } else {
        (void)snprintf(out, cap, "r%d", (int)h.radius);
    }
}

static void case_lines(void)
{
    size_t i;
    for (i = 0; i < NCASES; i++) {
        pc_algo_filter f;
        pc_buf text;
        char ht[32];
        if (!parse(i, &f)) {
            continue;
        }
        pc_buf_init(&text);
        CHECK(pc_algo_filter_to_json_text(&f, &text, NULL) == PC_OK);
        (void)pc_buf_append_byte(&text, 0, NULL);
        line("case %zu %s", i, (const char *)text.data);
        pc_buf_fini(&text);
        halo_text(pc_algo_filter_halo(&f), ht, sizeof ht);
        line("halo %zu %s %s", i, ht, pc_algo_filter_label(&f));
        pc_algo_filter_fini(&f);
    }
}

static void apply_lines(void)
{
    pc_rect r = pc_rect_make(0, 0, 40, 30), ext = pc_rect_make(-2, -2, 30, 31);
    pc_surface sel_grad, sel_none;
    size_t fi, i;
    surf(&sel_grad, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(-5, -5, 45, 35), 3, false);
    pc_surface_init(&sel_none, PC_PIXEL_FORMAT_GRAY8);
    for (fi = 0; fi < NFORMATS; fi++) {
        pc_surface s;
        surf(&s, FORMATS[fi].f, pc_rect_make(-4, 2, 37, 33), 9, false);
        for (i = 0; i < NCASES; i++) {
            pc_algo_filter f;
            pc_surface o;
            pc_rect area;
            uint64_t h = H0;
            if (!parse(i, &f)) {
                continue;
            }
            area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), r, NULL);
            hrect(&h, area);
            h64(&h, hs_st(pc_algo_apply_tiled(&s, &f, area, r, NULL, 256, NULL, &o, NULL), &o));
            h64(&h, hs_st(pc_algo_apply_tiled(&s, &f, area, r, NULL, 7, NULL, &o, NULL), &o));
            h64(&h, hs_st(pc_algo_apply(&s, &f, area, r, &sel_grad, &o, NULL), &o));
            h64(&h, hs_st(pc_algo_apply(&s, &f, area, r, &sel_none, &o, NULL), &o));
            h64(&h, hs_st(pc_algo_apply_in(&s, &f, area, r, NULL, r, &o, NULL), &o));
            h64(&h, hs_st(pc_algo_apply_tiled(&s, &f, pc_rect_make(-9, -9, 50, 50),
                                              pc_rect_make(5, 5, 25, 20), NULL, 9, &ext, &o, NULL),
                          &o));
            line("apply %s %zu %016llx", FORMATS[fi].name, i, (unsigned long long)h);
            pc_algo_filter_fini(&f);
        }
        pc_surface_fini(&s);
    }
    pc_surface_fini(&sel_grad), pc_surface_fini(&sel_none);
}

static void kernel_lines(void)
{
    pc_rect bounds = pc_rect_make(-6, -4, 34, 26), out = pc_rect_make(2, 3, 21, 15);
    pc_pixel_format fmt = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true};
    pc_algo_ctx ctx = {bounds, PC_COLOR_MODE_RGB, true};
    const float vals[3] = {NAN, -INFINITY, -1e30f};
    pc_surface s;
    size_t i;
    int vi;
    surf(&s, fmt, pc_rect_make(-40, -40, 80, 70), 4, false);
    for (i = 0; i < NCASES; i++) {
        for (vi = 0; vi < 4; vi++) {
            pc_algo_filter f;
            pc_algo_halo halo;
            pc_algo_image src;
            float *data = NULL;
            size_t len = 0;
            char ht[32];
            uint64_t h = 0xbad;
            if (!parse(i, &f)) {
                continue;
            }
            if (vi > 0 && f.def->nfields > 0 && f.def->fields[0].kind == PC_ALGO_JF_F32) {
                memcpy((char *)f.params + f.def->fields[0].off, &vals[vi - 1], sizeof(float));
            }
            halo = pc_algo_filter_halo(&f);
            CHECK(pc_algo_image_read(
                      &s, halo.bounds ? pc_rect_union(bounds, out) : pc_rect_inflate(out, halo.radius), &src,
                      NULL) == PC_OK);
            if (pc_algo_kernel(&f, &src, out, &ctx, &data, &len, NULL) == PC_OK) {
                h = of(data, len);
            }
            halo_text(halo, ht, sizeof ht);
            line("kernel %zu %d %s %016llx", i, vi, ht, (unsigned long long)h);
            free(data);
            pc_algo_image_fini(&src);
            pc_algo_filter_fini(&f);
        }
    }
    pc_surface_fini(&s);
}

/* ---- Smooth and Feather ---- */

static float *mask(const char *kind, size_t w, size_t h, int32_t seed)
{
    size_t n = w * h, i;
    float *m = malloc(n * sizeof(float) + 4);
    float cx = (float)w * 0.4f, cy = (float)h * 0.55f,
          rad = pc_rs_maxf_k((float)(w < h ? w : h) * 0.4f, 1.0f);
    for (i = 0; i < n; i++) {
        int32_t x = (int32_t)(i % w), y = (int32_t)(i / w);
        if (strcmp(kind, "binary") == 0) {
            m[i] = pat(x, y, 0, seed) < 0.5f ? 1.0f : 0.0f;
        } else if (strcmp(kind, "soft") == 0) {
            m[i] = pat(x * 3, y, 1, seed);
        } else if (strcmp(kind, "blob") == 0) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
            m[i] = pc_clampf(rad - sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)), 0.0f, 1.0f);
        } else if (strcmp(kind, "sparse") == 0) {
            int64_t v = ((int64_t)x * 7 + (int64_t)y * 11 + seed) % 53;
            m[i] = v == 0 ? 1.0f : 0.0f;
        } else {
            m[i] = (i % w) * 3 >= w && (i / w) * 4 < h * 3 ? 1.0f : 0.0f;
        }
    }
    return m;
}

static uint64_t sel_hash(bool feather, const float *m, size_t len, size_t w, size_t h, float r)
{
    float *o = NULL;
    uint64_t v;
    pc_status st = feather ? pc_algo_selection_feather(m, len, w, h, r, &o, NULL)
                           : pc_algo_selection_smooth(m, len, w, h, r, &o, NULL);
    if (st != PC_OK) {
        return 0xbad;
    }
    v = of(o, len);
    free(o);
    return v;
}

static void selection_lines(void)
{
    static const char *const kinds[5] = {"binary", "soft", "blob", "sparse", "rect"};
    static const float sr[8] = {0.0f, 0.4f, 1.0f, 2.0f, 5.0f, 16.0f, 50.0f, 500.0f};
    static const char *const srn[8] = {"0", "0.4", "1", "2", "5", "16", "50", "500"};
    static const float fr[11] = {0.1f, 0.3f, 1.0f, 2.5f, 8.0f, 9.0f, 17.0f, 25.0f, 50.0f, 120.0f, 1000.0f};
    static const char *const frn[11] = {"0.1", "0.3", "1", "2.5", "8", "9", "17", "25", "50", "120", "1000"};
    static const size_t dims[4][3] = {{97, 61, 1}, {40, 150, 2}, {13, 7, 3}, {1, 1, 4}};
    const float hv[5] = {NAN, INFINITY, -INFINITY, -5.0f, 1e30f};
    static const char *const hn[5] = {"NaN", "inf", "-inf", "-5", "1000000000000000000000000000000"};
    float *m;
    size_t d, k, i;
    for (d = 0; d < 4; d++) {
        size_t w = dims[d][0], h = dims[d][1];
        for (k = 0; k < 5; k++) {
            m = mask(kinds[k], w, h, (int32_t)dims[d][2]);
            for (i = 0; i < 8; i++) {
                line("smooth %zux%zu %s %s %016llx", w, h, kinds[k], srn[i],
                     (unsigned long long)sel_hash(false, m, w * h, w, h, sr[i]));
            }
            for (i = 0; i < 11; i++) {
                line("feather %zux%zu %s %s %016llx", w, h, kinds[k], frn[i],
                     (unsigned long long)sel_hash(true, m, w * h, w, h, fr[i]));
            }
            free(m);
        }
    }
    m = malloc(50 * 40 * sizeof(float));
    for (i = 0; i < 50 * 40; i++) {
        m[i] = (float)(i % 7) / 6.0f;
    }
    for (i = 0; i < 5; i++) {
        line("smooth hostile %s %016llx", hn[i], (unsigned long long)sel_hash(false, m, 2000, 50, 40, hv[i]));
        line("feather hostile %s %016llx", hn[i], (unsigned long long)sel_hash(true, m, 2000, 50, 40, hv[i]));
    }
    line("smooth mismatched %016llx", (unsigned long long)sel_hash(false, m, 2000, 49, 40, 5.0f));
    line("feather mismatched %016llx", (unsigned long long)sel_hash(true, m, 2000, 50, 41, 5.0f));
    free(m);
}

/* ---- FFT Motion Blur ---- */

static void fft_lines(void)
{
    pc_rect area = pc_rect_make(0, 0, 1500, 1000);
    pc_surface sel;
    struct {
        const char *name;
        pc_pixel_format f;
        float angle, distance;
        bool extras;
    } cases[3] = {
        {"rgba-30-1000", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}, 30.0f, 1000.0f, false},
        {"graya-0-2000", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, true}, 0.0f, 2000.0f, false},
        {"rgba-45-900-sel-extent", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}, 45.0f, 900.0f, true},
    };
    size_t ci;
    surf(&sel, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(-5, -5, 1505, 1005), 3, false);
    for (ci = 0; ci < 3; ci++) {
        pc_algo_motion_params mp = {cases[ci].angle, cases[ci].distance};
        pc_algo_filter f;
        pc_surface s, o;
        bool handled = false;
        pc_algo_apply_args a;
        size_t n = pc_pixel_format_channels(cases[ci].f), c;
        int32_t k;
        pc_buf text;
        surf(&s, cases[ci].f, area, 11, false);
        CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_MOTION_BLUR, &mp, &f, NULL) == PC_OK);
        a.surface = &s;
        a.filter = &f;
        a.area = area;
        a.bounds = area;
        a.selection = cases[ci].extras ? &sel : NULL;
        a.tile = 1024;
        a.extent = cases[ci].extras ? &area : NULL;
        a.ctl = NULL;
        CHECK(pc_algo_motion_apply_ex(&a, true, &o, &handled, NULL) == PC_OK && handled);
        pc_buf_init(&text);
        {
            char head[64];
            (void)snprintf(head, sizeof head, "fft %s %zu", cases[ci].name, n);
            (void)pc_buf_append_str(&text, head, NULL);
        }
        for (k = 0; k < 97; k++) {
            float px[PC_RASTER_MAX_CHANNELS] = {0};
            if (handled) {
                pc_surface_read_pixel(&o, (k * 131) % 1500, (k * 71) % 1000, px);
            }
            for (c = 0; c < n; c++) {
                uint32_t u;
                char hex[16];
                memcpy(&u, &px[c], 4);
                (void)snprintf(hex, sizeof hex, " %08x", (unsigned)u);
                (void)pc_buf_append_str(&text, hex, NULL);
            }
        }
        (void)pc_buf_append_byte(&text, 0, NULL);
        line("%s", (const char *)text.data);
        pc_buf_fini(&text);
        if (handled) {
            pc_surface_fini(&o);
        }
        pc_surface_fini(&s);
        pc_algo_filter_fini(&f);
    }
    pc_surface_fini(&sel);
}

/* fft lines: same name and count, samples within tolerance. */
static bool fft_close(const char *want, const char *got, double *worst)
{
    char *a = strdup(want), *b = strdup(got), *sa = NULL, *sb = NULL, *ta, *tb;
    int field = 0;
    bool ok = a != NULL && b != NULL;
    ta = ok ? strtok_r(a, " ", &sa) : NULL;
    tb = ok ? strtok_r(b, " ", &sb) : NULL;
    while (ok && (ta != NULL || tb != NULL)) {
        if (ta == NULL || tb == NULL) {
            ok = false;
            break;
        }
        if (field < 3) {
            ok = strcmp(ta, tb) == 0;
        } else {
            uint32_t ua = (uint32_t)strtoul(ta, NULL, 16), ub = (uint32_t)strtoul(tb, NULL, 16);
            float fa, fb;
            double d;
            memcpy(&fa, &ua, 4);
            memcpy(&fb, &ub, 4);
            d = fabs((double)fa - (double)fb);
            *worst = d > *worst ? d : *worst;
            ok = d <= 2e-5 + 1e-5 * fabs((double)fa);
        }
        field++;
        ta = strtok_r(NULL, " ", &sa);
        tb = strtok_r(NULL, " ", &sb);
    }
    free(a);
    free(b);
    return ok;
}

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

static void test_blur_parity(void)
{
    size_t glen = 0, lines = 0, exact = 0, near = 0;
    char *golden = read_file(GOLDEN, &glen), *copy, *want, *got, *wsave = NULL, *gsave = NULL, *p;
    double worst = 0.0;
    CHECK(golden != NULL);
    if (golden == NULL) {
        return;
    }
    /* The cases' JSON. */
    copy = strdup(golden);
    for (p = strtok_r(copy, "\n", &wsave); p != NULL && NCASES < MAX_CASES;
         p = strtok_r(NULL, "\n", &wsave)) {
        if (strncmp(p, "case ", 5) == 0) {
            char *json = strchr(p + 5, ' ');
            if (json != NULL) {
                CASE_JSON[NCASES++] = strdup(json + 1);
            }
        }
    }
    free(copy);
    wsave = NULL;
    depth_map_init();
    pc_buf_init(&OUT);
    case_lines();
    apply_lines();
    kernel_lines();
    selection_lines();
    fft_lines();
    (void)pc_buf_append_byte(&OUT, 0, NULL);
    want = strtok_r(golden, "\n", &wsave);
    got = strtok_r((char *)OUT.data, "\n", &gsave);
    while (want != NULL || got != NULL) {
        lines++;
        if (want != NULL && got != NULL && strncmp(want, "fft ", 4) == 0) {
            if (fft_close(want, got, &worst)) {
                pc_t_checks++;
                near++;
            } else {
                PC_T_FAIL("line %zu (fft):\n  want %.300s\n  got  %.300s", lines, want, got);
            }
        } else if (want == NULL || got == NULL || strcmp(want, got) != 0) {
            PC_T_FAIL("line %zu:\n  want %.400s\n  got  %.400s", lines, want != NULL ? want : "(end)",
                      got != NULL ? got : "(end)");
        } else {
            pc_t_checks++;
            exact++;
        }
        want = want != NULL ? strtok_r(NULL, "\n", &wsave) : NULL;
        got = got != NULL ? strtok_r(NULL, "\n", &gsave) : NULL;
    }
    (void)fprintf(stderr,
                  "test_blur_parity: %zu lines, %zu bit-exact, %zu fft within tolerance (worst %.3g)\n",
                  lines, exact, near, worst);
    pc_buf_fini(&OUT);
    pc_algo_image_fini(&DEPTH);
    while (NCASES > 0) {
        free(CASE_JSON[--NCASES]);
    }
    free(golden);
}

int main(int argc, char **argv)
{
    RUN(test_blur_parity);
    return pc_t_finish();
}
