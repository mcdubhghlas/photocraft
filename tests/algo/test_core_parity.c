/* Bit-exact parity with the L4 part of crates/algo: replays `rustref algo-core-vectors`
 * (tools/rustref/src/algo_core_vectors.rs) and compares every line of
 * tests/golden/algo-core-vectors.txt. Hashes are FNV-1a over little-endian f32/f64 bits.
 *
 * Lines that need filters of later chunks (Preserve Details enlargements: Unsharp Mask, L5) are
 * skipped while those filters aren't registered; the run prints how many. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/buf.h"
#include "base/json.h"
#include "harness.h"
#include "raster/pixel.h"
#include "testdefs.h"

#define GOLDEN "tests/golden/algo-core-vectors.txt"
#define H0 0xcbf29ce484222325ull

static pc_buf OUT;

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
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
static void h64(uint64_t *h, uint64_t u)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(u >> (8 * i));
    }
    fnv(h, b, 8);
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
static void hi32(uint64_t *h, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
    fnv(h, b, 4);
}
static void hu32(uint64_t *h, uint32_t u)
{
    hi32(h, (int32_t)u);
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

/* Content bounds, the samples inside them and the default pixel. */
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
/* fnv of a sub-hash's little-endian bytes. */
static void hh(uint64_t *h, uint64_t sub)
{
    h64(h, sub);
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
    int64_t v = (int64_t)x * 7 + (int64_t)y * 13 + (int64_t)c * 29 + (int64_t)seed * 31;
    v %= 97;
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
static float *buf(size_t n, int32_t seed)
{
    float *v = malloc(n * sizeof(float) + 4);
    size_t i;
    for (i = 0; i < n; i++) {
        v[i] = pat((int32_t)i, (int32_t)(i / 7), i % 3, seed);
    }
    return v;
}
static float (*rgba_px(size_t n, int32_t seed))[4]
{
    float (*v)[4] = malloc(n * sizeof *v + 16);
    size_t i;
    for (i = 0; i < n; i++) {
        v[i][0] = pat((int32_t)i, 1, 0, seed);
        v[i][1] = pat((int32_t)i, 2, 1, seed);
        v[i][2] = pat((int32_t)i, 3, 2, seed);
        v[i][3] = alpha_at((int32_t)i, 0, seed);
    }
    return v;
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
#define NFMT (sizeof FORMATS / sizeof FORMATS[0])

/* ------------------------------------------------------------------------------------------ */

static void enum_line(const pc_algo_enum_desc *d)
{
    char tmp[1024];
    size_t used = 0, i;
    tmp[0] = '\0';
    for (i = 0; i < d->n; i++) {
        used += (size_t)snprintf(tmp + used, sizeof tmp - used, "%s\"%s\"", i > 0 ? "," : "", d->names[i]);
    }
    line("enum %s %s def=\"%s\"", d->rust_name, tmp, pc_algo_enum_name(d, d->def));
}

static void json_of(yyjson_mut_doc *d, yyjson_mut_val *v, char *out, size_t cap)
{
    pc_buf b;
    pc_buf_init(&b);
    out[0] = '\0';
    if (v != NULL && pc_json_write(v, 0, &b, NULL) == PC_OK) {
        (void)snprintf(out, cap, "%s", pc_buf_cstr(&b));
    }
    pc_buf_fini(&b);
    (void)d;
}

static void enums(void)
{
    static const pc_algo_enum_desc *const all[] = {
        &PC_ALGO_ENUM_RADIAL_METHOD,   &PC_ALGO_ENUM_RADIAL_QUALITY,  &PC_ALGO_ENUM_DISTRIBUTION,
        &PC_ALGO_ENUM_UNDEFINED_AREAS, &PC_ALGO_ENUM_PRESERVE,        &PC_ALGO_ENUM_SPHERIZE_MODE,
        &PC_ALGO_ENUM_WAVE_TYPE,       &PC_ALGO_ENUM_RIPPLE_SIZE,     &PC_ALGO_ENUM_POLAR_MODE,
        &PC_ALGO_ENUM_MEZZOTINT_TYPE,  &PC_ALGO_ENUM_DIFFUSE_MODE,    &PC_ALGO_ENUM_EXTRUDE_TYPE,
        &PC_ALGO_ENUM_TILE_FILL,       &PC_ALGO_ENUM_WIND_METHOD,     &PC_ALGO_ENUM_ZIGZAG_STYLE,
        &PC_ALGO_ENUM_LENS_TYPE,       &PC_ALGO_ENUM_LIGHT_KIND,      &PC_ALGO_ENUM_TEXTURE_CHANNEL,
        &PC_ALGO_ENUM_BLUR_QUALITY,    &PC_ALGO_ENUM_SMART_BLUR_MODE, &PC_ALGO_ENUM_DEPTH_SOURCE,
        &PC_ALGO_ENUM_BLUR_SHAPE,      &PC_ALGO_ENUM_HSB_MODEL,       &PC_ALGO_ENUM_RESAMPLE,
        &PC_ALGO_ENUM_INTERP,
    };
    static const char *const parse_in[] = {"nearest", "nearestNeighbor", "bilinear", "bicubic", "x", ""};
    size_t i;
    char js[2048];
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    pc_algo_light l = pc_algo_light_default();
    td_iris iris;
    td_field field;
    td_spin spin;
    td_path path;
    pc_algo_iris_pin ip = pc_algo_iris_pin_default();
    pc_algo_field_pin fp = pc_algo_field_pin_default();
    pc_algo_spin_pin sp = pc_algo_spin_pin_default();
    pc_algo_blur_path bp;
    for (i = 0; i < sizeof all / sizeof all[0]; i++) {
        enum_line(all[i]);
    }
    for (i = 0; i < 6; i++) {
        line("interp_parse \"%s\" \"%s\"", parse_in[i],
             pc_algo_enum_name(&PC_ALGO_ENUM_INTERP, (int)pc_algo_interp_parse(parse_in[i])));
    }
    json_of(d, pc_algo_light_to_json(d, &l), js, sizeof js);
    line("light_default %s", js);
    /* The pin defaults through one-element vectors (the generic writer), unwrapped. */
    iris.pins.v = &ip, iris.pins.n = 1;
    field.pins.v = &fp, field.pins.n = 1;
    spin.pins.v = &sp, spin.pins.n = 1;
    CHECK(pc_algo_blur_path_default(&bp, NULL) == PC_OK);
    path.paths.v = &bp, path.paths.n = 1;
    {
        struct {
            const char *name;
            const pc_algo_jfield *f;
            const void *base;
        } pins[] = {{"iris_default", TD_IRIS_F, &iris},
                    {"field_default", TD_FIELD_F, &field},
                    {"spin_default", TD_SPIN_F, &spin},
                    {"path_default", TD_PATH_F, &path}};
        for (i = 0; i < 4; i++) {
            yyjson_mut_val *o = yyjson_mut_obj(d);
            char *a, *z;
            CHECK(pc_algo_jfields_write(pins[i].f, 1, d, o, pins[i].base));
            json_of(d, o, js, sizeof js);
            /* {"pins":[{...}]} -> {...} */
            a = strchr(js, '[');
            z = strrchr(js, ']');
            if (a != NULL && z != NULL && z > a) {
                *z = '\0';
                line("%s %s", pins[i].name, a + 1);
            } else {
                line("%s ?", pins[i].name);
            }
        }
    }
    pc_algo_blur_path_fini(&bp);
    yyjson_mut_doc_free(d);
}

static void filter_line(const char *prefix, const pc_algo_filter *f)
{
    pc_buf b;
    pc_buf_init(&b);
    if (pc_algo_filter_to_json_text(f, &b, NULL) == PC_OK) {
        line("%s%s", prefix, pc_buf_cstr(&b));
    } else {
        line("%s?", prefix);
    }
    pc_buf_fini(&b);
}

static void filter_serde(const char *golden)
{
    /* The inputs are the golden's `de` lines themselves (text up to the tab). */
    const char *p = golden;
    while ((p = strstr(p, "\nde ")) != NULL) {
        const char *s = p + 4, *tab = strchr(s, '\t');
        yyjson_doc *doc = NULL;
        pc_algo_filter f;
        pc_error err;
        char in[1024];
        size_t n;
        if (tab == NULL) {
            break;
        }
        n = (size_t)(tab - s);
        if (n >= sizeof in) {
            n = sizeof in - 1;
        }
        memcpy(in, s, n);
        in[n] = '\0';
        p = tab;
        if (pc_json_parse(in, n, &doc, &err) != PC_OK) {
            line("de %s\tPARSE %s", in, err.msg);
            continue;
        }
        if (pc_algo_filter_from_json_in(yyjson_doc_get_root(doc), TD_ALL, &f, &err) == PC_OK) {
            char pre[1100];
            (void)snprintf(pre, sizeof pre, "de %s\t", in);
            filter_line(pre, &f);
            pc_algo_filter_fini(&f);
        } else {
            line("de %s\tERR %s", in, err.msg);
        }
        yyjson_doc_free(doc);
    }
    {
        const float radii[] = {0.1f,        1e-7f, 1e20f,    3.4028235e38f, -0.0f,
                               16777216.0f, NAN,   INFINITY, 1.0f / 3.0f};
        size_t i;
        td_offset o = {INT32_MIN, INT32_MAX, PC_ALGO_UNDEF_TRANSPARENT};
        td_noise nz = {1.5f, PC_ALGO_DIST_UNIFORM, true, UINT32_MAX};
        pc_algo_filter f;
        for (i = 0; i < sizeof radii / sizeof radii[0]; i++) {
            td_radius r = {radii[i]};
            f = td_make(&TD_GAUSS, &r);
            filter_line("ser ", &f);
            pc_algo_filter_fini(&f);
        }
        f = td_make(&TD_OFFSET, &o);
        filter_line("ser ", &f);
        pc_algo_filter_fini(&f);
        f = td_make(&TD_NOISE, &nz);
        filter_line("ser ", &f);
        pc_algo_filter_fini(&f);
    }
    {
        td_radius g1 = {2.0f}, g2 = {-1.0f}, g3 = {NAN}, g4 = {1e30f}, bx = {2.5f};
        td_motion mo = {0.0f, -7.0f};
        td_mosaic ms = {0.2f};
        td_emboss em = {1.0f, -3.5f, 1.0f};
        td_twirl tw = {1.0f};
        struct {
            const pc_algo_filter_def *d;
            const void *p;
        } cases[] = {{&TD_GAUSS, &g1},       {&TD_GAUSS, &g2},   {&TD_GAUSS, &g3},  {&TD_GAUSS, &g4},
                     {&TD_BOX, &bx},         {&TD_MOTION, &mo},  {&TD_MOSAIC, &ms}, {&TD_EMBOSS, &em},
                     {&TD_FIND_EDGES, NULL}, {&TD_INVERT, NULL}, {&TD_TWIRL, &tw}};
        size_t i;
        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            pc_algo_filter f = td_make(cases[i].d, cases[i].p);
            pc_algo_halo h = pc_algo_filter_halo(&f);
            pc_rect content = pc_rect_make(10, 10, 20, 20), r = pc_rect_make(0, 0, 40, 30);
            pc_rect sb = pc_rect_make(0, 0, 12, 12);
            uint64_t x = H0;
            pc_buf b;
            char hs_[32];
            hrect(&x, pc_algo_output_area(&f, content, r, NULL));
            hrect(&x, pc_algo_output_area(&f, content, r, &sb));
            hrect(&x, pc_algo_output_area(&f, PC_RECT_EMPTY, r, NULL));
            if (h.bounds) {
                (void)snprintf(hs_, sizeof hs_, "bounds");
            } else {
                (void)snprintf(hs_, sizeof hs_, "r%d", (int)h.radius);
            }
            pc_buf_init(&b);
            (void)pc_algo_filter_to_json_text(&f, &b, NULL);
            line("halo %s %s %s %016llx", pc_buf_cstr(&b), pc_algo_filter_label(&f), hs_,
                 (unsigned long long)x);
            pc_buf_fini(&b);
            pc_algo_filter_fini(&f);
        }
    }
}

/* ------------------------------------------------------------------------------------------ */

static void image_vectors(void)
{
    size_t fi;
    for (fi = 0; fi < NFMT; fi++) {
        pc_surface s;
        uint64_t h = H0;
        const pc_rect rs[] = {pc_rect_make(0, 0, 10, 10), pc_rect_make(-10, -5, 50, 40),
                              pc_rect_make(30, 25, 300, 270), PC_RECT_EMPTY};
        const pc_rect es[] = {pc_rect_make(0, 0, 30, 20), pc_rect_make(-3, 2, 37, 29),
                              pc_rect_make(5, 5, 6, 6), pc_rect_make(100, 100, 110, 110)};
        size_t i, j;
        surf(&s, FORMATS[fi].f, pc_rect_make(-3, 2, 37, 29), 5, false);
        for (i = 0; i < 4; i++) {
            pc_algo_image a;
            CHECK(pc_algo_image_read(&s, rs[i], &a, NULL) == PC_OK);
            hf(&h, a.data, a.len);
            pc_algo_image_fini(&a);
            for (j = 0; j < 4; j++) {
                pc_algo_image b;
                CHECK(pc_algo_image_read_clamped(&s, rs[i], es[j], &b, NULL) == PC_OK);
                hrect(&h, b.rect);
                hf(&h, b.data, b.len);
                pc_algo_image_fini(&b);
            }
        }
        line("image_read %s %016llx", FORMATS[fi].name, (unsigned long long)h);
        pc_surface_fini(&s);
    }
    {
        pc_surface s;
        pc_algo_image img;
        const pc_rect areas[] = {pc_rect_make(2, 3, 9, 8), pc_rect_make(0, 0, 5, 5), PC_RECT_EMPTY,
                                 pc_rect_make(3, 4, 4, 5)};
        int e;
        float *v;
        size_t len;
        uint64_t p1, p2;
        surf(&s, PC_PIXEL_FORMAT_RGBA32F, pc_rect_make(2, 3, 9, 8), 1, false);
        CHECK(pc_algo_image_read(&s, pc_rect_make(2, 3, 9, 8), &img, NULL) == PC_OK);
        for (e = 0; e < 3; e++) {
            uint64_t h = H0;
            size_t a;
            int32_t x, y;
            size_t c;
            for (a = 0; a < 4; a++) {
                for (y = -6; y < 15; y++) {
                    for (x = -7; x < 16; x++) {
                        for (c = 0; c < 4; c++) {
                            hf1(&h, pc_algo_image_get_edge(&img, x, y, c, (pc_algo_edge)e, areas[a]));
                        }
                    }
                }
            }
            line("get_edge %d %016llx", e, (unsigned long long)h);
            h = H0;
            for (a = 0; a < 4; a++) {
                int al;
                for (al = 0; al < 2; al++) {
                    float fy = -2.3f;
                    while (fy < 11.0f) {
                        float fx = -3.1f;
                        while (fx < 12.0f) {
                            float out[4];
                            pc_algo_image_sample(&img, fx, fy, (pc_algo_edge)e, areas[a], al == 1, out, 4);
                            hf(&h, out, 4);
                            fx += 0.37f;
                        }
                        fy += 0.41f;
                    }
                }
            }
            line("sample %d %016llx", e, (unsigned long long)h);
        }
        CHECK(pc_algo_image_crop(&img, pc_rect_make(0, 0, 12, 10), &v, &len, NULL) == PC_OK);
        p1 = of(v, len);
        free(v);
        CHECK(pc_algo_image_crop(&img, pc_rect_make(4, 4, 6, 7), &v, &len, NULL) == PC_OK);
        p2 = of(v, len);
        free(v);
        line("crop %016llx %016llx", (unsigned long long)p1, (unsigned long long)p2);
        pc_algo_premultiply(img.data, img.len, 4, true);
        p1 = of(img.data, img.len);
        pc_algo_unpremultiply(img.data, img.len, 4, true);
        line("premul %016llx %016llx", (unsigned long long)p1, (unsigned long long)of(img.data, img.len));
        pc_algo_image_fini(&img);
        pc_surface_fini(&s);
    }
}

static void fxutil_vectors(void)
{
    uint64_t h = H0;
    int32_t x, y;
    size_t i;
    static const uint32_t cs[4][2] = {{0, 0}, {11, 7}, {12, 0xffffffffu}, {3, 12345}};
    for (y = -20; y < 20; y++) {
        for (x = -20; x < 20; x++) {
            for (i = 0; i < 4; i++) {
                hf1(&h, pc_algo_hash01(x * 977, y * 131, cs[i][0], cs[i][1]));
            }
        }
    }
    hf1(&h, pc_algo_hash01(INT32_MIN, INT32_MAX, UINT32_MAX, UINT32_MAX));
    line("hash01 %016llx", (unsigned long long)h);
    h = H0;
    for (y = -5; y < 5; y++) {
        for (x = -5; x < 5; x++) {
            float px, py;
            pc_algo_cell_point(x, y, 6.0f, 1, 3.5f, -2.25f, &px, &py);
            hf1(&h, px), hf1(&h, py);
            pc_algo_cell_point(x, y, 17.5f, 9, 3.5f, -2.25f, &px, &py);
            hf1(&h, px), hf1(&h, py);
        }
    }
    line("cell_point %016llx", (unsigned long long)h);
    {
        static const float sig[] = {0.0f, 0.2f, 0.3f,  0.5f,    0.577f, 0.6f, 1.0f, 1.5f,
                                    2.5f, 7.3f, 30.0f, 1000.0f, 1e9f,   NAN,  -5.0f};
        static const char *const names[] = {"0",   "0.2", "0.3", "0.5",  "0.577",      "0.6", "1", "1.5",
                                            "2.5", "7.3", "30",  "1000", "1000000000", "NaN", "-5"};
        char tmp[1024];
        size_t used = 0;
        for (i = 0; i < sizeof sig / sizeof sig[0]; i++) {
            size_t r[3];
            pc_algo_gauss_box_radii(sig[i], r);
            used += (size_t)snprintf(tmp + used, sizeof tmp - used, " %s:%zu,%zu,%zu", names[i], r[0], r[1],
                                     r[2]);
        }
        line("gauss_box_radii%s", tmp);
    }
    {
        static const size_t dims[5][3] = {{17, 11, 1}, {9, 23, 3}, {1, 7, 4}, {7, 1, 2}, {40, 30, 5}};
        static const size_t radii[] = {0, 1, 2, 5, 30};
        static const float sigmas[] = {0.2f, 0.5f, 1.0f, 2.5f, 7.3f, 30.0f};
        size_t d, k;
        for (d = 0; d < 5; d++) {
            size_t w = dims[d][0], hh_ = dims[d][1], n = dims[d][2], len = w * hh_ * n;
            uint64_t out = H0;
            for (k = 0; k < 5; k++) {
                float *b = buf(len, (int32_t)radii[k]);
                CHECK(pc_algo_box_blur_n(b, w, hh_, n, radii[k], NULL) == PC_OK);
                hf(&out, b, len);
                free(b);
            }
            for (k = 0; k < 6; k++) {
                float *b = buf(len, 3);
                CHECK(pc_algo_gauss_blur_n(b, w, hh_, n, sigmas[k], NULL) == PC_OK);
                hf(&out, b, len);
                free(b);
            }
            line("box_gauss %zux%zux%zu %016llx", w, hh_, n, (unsigned long long)out);
        }
    }
    {
        static const float e0s[] = {0.0f, 0.25f, 1.0f}, e1s[] = {0.0f, 0.5f, 1.0f};
        size_t a, b;
        h = H0;
        for (a = 0; a < 3; a++) {
            for (b = 0; b < 3; b++) {
                float fx = -0.5f;
                while (fx < 1.5f) {
                    hf1(&h, pc_algo_smoothstep(e0s[a], e1s[b], fx));
                    fx += 0.0625f;
                }
            }
        }
        hf1(&h, pc_algo_smoothstep(0.0f, 1.0f, NAN));
        line("smoothstep %016llx", (unsigned long long)h);
    }
    {
        pc_surface s;
        pc_algo_image img;
        const pc_rect clips[] = {pc_rect_make(0, 0, 9, 7), pc_rect_make(2, 1, 6, 5), pc_rect_make(4, 4, 5, 5),
                                 PC_RECT_EMPTY};
        const pc_rect wins[] = {pc_rect_make(-2, -2, 11, 9), pc_rect_make(3, 3, 4, 4), PC_RECT_EMPTY};
        size_t c, al;
        surf(&s, PC_PIXEL_FORMAT_RGBA32F, pc_rect_make(0, 0, 9, 7), 2, false);
        CHECK(pc_algo_image_read(&s, pc_rect_make(0, 0, 9, 7), &img, NULL) == PC_OK);
        h = H0;
        for (c = 0; c < 4; c++) {
            for (al = 0; al < 2; al++) {
                float acc[4] = {0, 0, 0, 0};
                float fy = -1.3f;
                while (fy < 9.0f) {
                    float fx = -1.7f;
                    while (fx < 11.0f) {
                        pc_algo_add_sample_premul(&img, fx, fy, clips[c], al == 1, acc);
                        fx += 0.53f;
                    }
                    fy += 0.61f;
                }
                pc_algo_add_sample_premul(&img, NAN, 1.0f, clips[c], al == 1, acc);
                pc_algo_add_sample_premul(&img, 1.0f, INFINITY, clips[c], al == 1, acc);
                hf(&h, acc, 4);
            }
        }
        line("add_sample_premul %016llx", (unsigned long long)h);
        h = H0;
        for (c = 0; c < 3; c++) {
            for (al = 0; al < 2; al++) {
                float *v = NULL;
                size_t len = 0, k;
                CHECK(pc_algo_premul_window(&img, wins[c], al == 1, &v, &len, NULL) == PC_OK);
                hf(&h, v, len);
                for (k = 0; k + 4 <= len; k += 4) {
                    pc_algo_unpremul_px(v + k, 4, al == 1);
                }
                hf(&h, v, len);
                free(v);
            }
        }
        line("premul_window %016llx", (unsigned long long)h);
        pc_algo_image_fini(&img);
        pc_surface_fini(&s);
    }
    {
        static const pc_color_mode modes[] = {
            PC_COLOR_MODE_RGB,    PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,    PC_COLOR_MODE_LAB,
            PC_COLOR_MODE_BITMAP, PC_COLOR_MODE_DUOTONE,   PC_COLOR_MODE_INDEXED, PC_COLOR_MODE_MULTICHANNEL};
        size_t m;
        int al;
        for (m = 0; m < 8; m++) {
            for (al = 0; al < 2; al++) {
                pc_algo_ctx ctx = {pc_rect_make(0, 0, 10, 10), modes[m], al == 1};
                pc_pixel_format pf = {modes[m], PC_SAMPLE_TYPE_F32, al == 1};
                size_t n = pc_pixel_format_channels(pf), c;
                int32_t k;
                h = H0;
                for (k = 0; k < 64; k++) {
                    float px[8], q[8], c4[4], rgba[4], nat[PC_ALGO_MAXC];
                    for (c = 0; c < n; c++) {
                        px[c] = pat(k, 3, c, 4);
                    }
                    hf1(&h, pc_algo_luma(&ctx, px, n));
                    pc_algo_rgba(&ctx, px, n, rgba);
                    hf(&h, rgba, 4);
                    c4[0] = pat(k, 0, 0, 1), c4[1] = pat(k, 0, 1, 1), c4[2] = pat(k, 0, 2, 1),
                    c4[3] = alpha_at(k, 0, 1);
                    memcpy(q, px, n * sizeof(float));
                    pc_algo_set_rgba(&ctx, q, n, c4);
                    hf(&h, q, n);
                    pc_algo_native(&ctx, c4, nat);
                    hf(&h, nat, PC_ALGO_MAXC);
                }
                line("ctx_colour %s %s %016llx", pc_color_mode_name(modes[m]), al ? "true" : "false",
                     (unsigned long long)h);
            }
        }
    }
    {
        int32_t k;
        h = H0;
        for (k = 0; k < 400; k++) {
            float c[3], hsl[3], o[3], t[3];
            c[0] = pat(k, 0, 0, 2), c[1] = pat(k, 1, 1, 3), c[2] = pat(k, 2, 2, 5);
            if (k % 17 == 0) {
                c[1] = c[0], c[2] = c[0];
            } else if (k % 13 == 0) {
                c[1] = c[0];
            }
            pc_algo_rgb_to_hsl(c, hsl);
            hf(&h, hsl, 3);
            pc_algo_hsl_to_rgb(hsl, o);
            hf(&h, o, 3);
            pc_algo_hsb_to_rgb(c, o);
            hf(&h, o, 3);
            t[0] = c[0] * 3.0f - 1.0f, t[1] = c[1], t[2] = c[2];
            pc_algo_hsl_to_rgb(t, o);
            hf(&h, o, 3);
        }
        line("hsl %016llx", (unsigned long long)h);
    }
    {
        float (*px)[4] = rgba_px(300, 4);
        static const float fz[] = {0.0f, 10.0f, 40.0f, 200.0f, -5.0f};
        const float col[3] = {0.5f, 0.25f, 0.75f};
        float mask[300];
        h = H0;
        for (i = 0; i < 5; i++) {
            pc_algo_color_range((const float (*)[4])px, 300, col, fz[i], mask);
            hf(&h, mask, 300);
        }
        line("color_range %016llx", (unsigned long long)h);
        free(px);
    }
}

static void histogram_vectors(void)
{
    float (*px)[4] = rgba_px(500, 6);
    static const size_t ns[] = {0, 1, 50, 500};
    pc_algo_rgb_histogram *hg = malloc(sizeof *hg);
    size_t i, k;
    uint64_t x;
    for (i = 0; i < 500; i++) {
        switch (i % 23) {
        case 0:
            px[i][0] = NAN;
            break;
        case 1:
            px[i][3] = INFINITY;
            break;
        case 2:
            px[i][1] = -0.5f;
            break;
        case 3:
            px[i][2] = 3.0f;
            break;
        case 4:
            px[i][0] = 1.0f;
            break;
        case 5:
            px[i][1] = 0.0f;
            break;
        case 6:
            px[i][3] = -1.0f;
            break;
        case 7:
            px[i][2] = -INFINITY;
            break;
        default:
            break;
        }
    }
    for (i = 0; i < 4; i++) {
        pc_algo_rgb_histogram_from_rgba((const float (*)[4])px, ns[i], hg);
        x = H0;
        for (k = 0; k < 3; k++) {
            size_t b;
            for (b = 0; b < PC_ALGO_HIST_BINS; b++) {
                h64(&x, hg->channels[k][b]);
            }
        }
        h64(&x, hg->samples), h64(&x, hg->transparent), h64(&x, hg->invalid);
        for (k = 0; k < 3; k++) {
            h64(&x, hg->underflow[k]);
        }
        for (k = 0; k < 3; k++) {
            h64(&x, hg->overflow[k]);
        }
        for (k = 0; k < 3; k++) {
            h64(&x, hg->shadows[k]);
        }
        for (k = 0; k < 3; k++) {
            h64(&x, hg->highlights[k]);
        }
        line("histogram %zu %016llx %llu %llu %llu", ns[i], (unsigned long long)x,
             (unsigned long long)hg->samples, (unsigned long long)hg->transparent,
             (unsigned long long)hg->invalid);
    }
    x = H0;
    for (i = 0; i < 500; i++) {
        uint8_t sh[2];
        pc_algo_clipping(px[i], &sh[0], &sh[1]);
        fnv(&x, sh, 2);
    }
    line("clipping %016llx", (unsigned long long)x);
    free(hg);
    free(px);
}

static float (*ramp(size_t w, size_t h))[4]
{
    float (*v)[4] = malloc(w * h * sizeof *v);
    size_t i;
    for (i = 0; i < w * h; i++) {
        float x = (float)(i % w) / (float)(w - 1);
        v[i][0] = x, v[i][1] = x * 0.8f, v[i][2] = x * 0.6f + 0.1f, v[i][3] = 1.0f;
    }
    return v;
}

static float (*dup_px(const float (*v)[4], size_t n))[4]
{
    float (*d)[4] = malloc(n * sizeof *d + 16);
    memcpy(d, v, n * sizeof *d);
    return d;
}

static void tone_vectors(void)
{
    const size_t w = 37, h = 23, n = w * h;
    float (*base)[4] = rgba_px(n, 8);
    pc_algo_shadows_highlights_params sh[5];
    size_t i;
    for (i = 0; i < 5; i++) {
        sh[i] = pc_algo_shadows_highlights_default();
    }
    sh[1].black_clip = 0.0f, sh[1].white_clip = 0.0f;
    sh[2].shadow_amount = 0.0f, sh[2].highlight_amount = 80.0f, sh[2].black_clip = 0.0f,
    sh[2].white_clip = 0.0f;
    sh[3].shadow_amount = 70.0f, sh[3].shadow_radius = 3.0f, sh[3].highlight_amount = 40.0f;
    sh[3].highlight_radius = 9.0f, sh[3].color = -40.0f, sh[3].midtone = 35.0f, sh[3].black_clip = 2.0f;
    sh[3].white_clip = 5.0f;
    sh[4].shadow_tone = 0.0f, sh[4].highlight_tone = 0.0f, sh[4].midtone = -100.0f, sh[4].black_clip = 50.0f;
    sh[4].white_clip = 60.0f;
    for (i = 0; i < 5; i++) {
        float (*a)[4] = dup_px((const float (*)[4])base, n);
        float (*r)[4] = ramp(32, 8);
        CHECK(pc_algo_shadows_highlights(a, n, w, h, &sh[i], NULL) == PC_OK);
        CHECK(pc_algo_shadows_highlights(r, 256, 32, 8, &sh[i], NULL) == PC_OK);
        line("shadows_highlights %zu %016llx %016llx", i, (unsigned long long)hpx((const float (*)[4])a, n),
             (unsigned long long)hpx((const float (*)[4])r, 256));
        free(a);
        free(r);
    }
    {
        static const float rc[3][7] = {{1.0f, 0.0f, 0.0f, 40.0f, 120.0f, 0.0f, 0.0f},
                                       {0.5f, 0.4f, 0.3f, 80.0f, -45.0f, 30.0f, -20.0f},
                                       {0.2f, 0.9f, 0.1f, 200.0f, 400.0f, -60.0f, 70.0f}};
        for (i = 0; i < 3; i++) {
            float (*a)[4] = dup_px((const float (*)[4])base, n);
            float *m = malloc(n * sizeof(float));
            pc_algo_replace_color(a, n, rc[i], rc[i][3], rc[i][4], rc[i][5], rc[i][6], m);
            line("replace_color %zu %016llx %016llx", i, (unsigned long long)hpx((const float (*)[4])a, n),
                 (unsigned long long)of(m, n));
            free(a);
            free(m);
        }
    }
    {
        float *mask = malloc(n / 2 * sizeof(float));
        const float zeros[2] = {0.0f, 0.0f};
        pc_algo_lab_stats s[4], target, src;
        bool ok[4];
        char t[4][40];
        float (*r)[4] = ramp(16, 4);
        static const float mc[4][4] = {{100.0f, 100.0f, 0.0f, 0.0f},
                                       {100.0f, 100.0f, 0.0f, 1.0f},
                                       {150.0f, 20.0f, 30.0f, 0.0f},
                                       {1.0f, 200.0f, 100.0f, 1.0f}};
        for (i = 0; i < n / 2; i++) {
            mask[i] = pat((int32_t)i, 0, 0, 9);
        }
        ok[0] = pc_algo_lab_stats_compute((const float (*)[4])base, n, NULL, 0, &s[0]);
        ok[1] = pc_algo_lab_stats_compute((const float (*)[4])base, n, mask, n / 2, &s[1]);
        ok[2] = pc_algo_lab_stats_compute((const float (*)[4])base, 0, NULL, 0, &s[2]);
        ok[3] = pc_algo_lab_stats_compute((const float (*)[4])base, 3, zeros, 2, &s[3]);
        for (i = 0; i < 4; i++) {
            if (ok[i]) {
                float v[6];
                memcpy(v, s[i].mean, sizeof s[i].mean);
                memcpy(v + 3, s[i].std, sizeof s[i].std);
                (void)snprintf(t[i], sizeof t[i], "%016llx", (unsigned long long)of(v, 6));
            } else {
                (void)snprintf(t[i], sizeof t[i], "none");
            }
        }
        line("lab_stats %s %s %s %s", t[0], t[1], t[2], t[3]);
        target = s[0];
        if (!pc_algo_lab_stats_compute((const float (*)[4])r, 64, NULL, 0, &src)) {
            src = target;
        }
        for (i = 0; i < 4; i++) {
            pc_algo_match_color o = {mc[i][0], mc[i][1], mc[i][2], mc[i][3] != 0.0f};
            float (*a)[4] = dup_px((const float (*)[4])base, n);
            float (*b)[4] = dup_px((const float (*)[4])base, n);
            pc_algo_match_color_apply(a, n, &target, &src, &o);
            pc_algo_match_color_apply(b, n, &target, NULL, &o);
            line("match_color %zu %016llx %016llx", i, (unsigned long long)hpx((const float (*)[4])a, n),
                 (unsigned long long)hpx((const float (*)[4])b, n));
            free(a);
            free(b);
        }
        free(r);
        free(mask);
    }
    {
        pc_algo_hdr_toning_params hp[5];
        static const float c2[2][2] = {{0.0f, 1.0f}, {1.0f, 0.0f}};
        static const float c3[6][2] = {{0.7f, 0.9f}, {0.2f, 0.1f},   {NAN, 0.5f},
                                       {0.2f, 0.3f}, {-0.0f, 0.05f}, {0.0f, 0.0f}};
        static const float c4[1][2] = {{0.5f, 0.5f}};
        for (i = 0; i < 5; i++) {
            hp[i] = pc_algo_hdr_toning_default();
        }
        hp[1].strength = 2.0f, hp[1].detail = 0.0f, hp[1].saturation = 0.0f;
        hp[2].strength = 0.0f, hp[2].detail = 0.0f, hp[2].saturation = 0.0f, hp[2].curve = c2,
        hp[2].curve_len = 2;
        hp[3].radius = 3.0f, hp[3].gamma = 0.4f, hp[3].exposure = 1.5f, hp[3].detail = 120.0f,
        hp[3].shadow = 60.0f;
        hp[3].highlight = 40.0f, hp[3].vibrance = 50.0f, hp[3].saturation = -30.0f, hp[3].curve = c3;
        hp[3].curve_len = 6;
        hp[4].radius = 500.0f, hp[4].strength = 4.0f, hp[4].gamma = 0.0f, hp[4].exposure = -5.0f;
        hp[4].detail = -100.0f, hp[4].curve = c4, hp[4].curve_len = 1;
        for (i = 0; i < 5; i++) {
            float (*a)[4] = dup_px((const float (*)[4])base, n);
            float (*r)[4] = ramp(40, 10);
            CHECK(pc_algo_hdr_toning(a, n, w, h, &hp[i], NULL) == PC_OK);
            CHECK(pc_algo_hdr_toning(r, 400, 40, 10, &hp[i], NULL) == PC_OK);
            line("hdr_toning %zu %016llx %016llx", i, (unsigned long long)hpx((const float (*)[4])a, n),
                 (unsigned long long)hpx((const float (*)[4])r, 400));
            free(a);
            free(r);
        }
    }
    free(base);
}

/* ------------------------------------------------------------------------------------------ */

static int SKIPPED;

/* The reference's filter list (indices printed in the golden). */
static const pc_algo_resample RS5[5] = {PC_ALGO_RESAMPLE_NEAREST, PC_ALGO_RESAMPLE_BILINEAR,
                                        PC_ALGO_RESAMPLE_BICUBIC, PC_ALGO_RESAMPLE_LANCZOS,
                                        PC_ALGO_RESAMPLE_PRESERVE_DETAILS};

static void resample_vectors(void)
{
    static const double ks[8][2] = {{0.013, 0.013}, {0.37, 0.37}, {0.5, 0.5}, {1.0, 1.0},
                                    {1.37, 1.37},   {2.0, 2.0},   {2.3, 0.7}, {1.0, 0.01}};
    bool have_usm = pc_algo_filter_def_find("unsharpMask") != NULL;
    size_t fi, f, k;
    for (fi = 0; fi < NFMT; fi++) {
        pc_surface s;
        surf(&s, FORMATS[fi].f, pc_rect_make(-5, 3, 61, 37), 2, false);
        for (f = 0; f < 5; f++) {
            uint64_t h = H0, hp = H0;
            for (k = 0; k < 8; k++) {
                bool up = RS5[f] == PC_ALGO_RESAMPLE_PRESERVE_DETAILS && (ks[k][0] > 1.0 || ks[k][1] > 1.0);
                pc_surface o;
                if (up && !have_usm) {
                    continue;
                }
                hh(up ? &hp : &h,
                   hs_st(pc_algo_resize_surface(&s, ks[k][0], ks[k][1], RS5[f], &o, NULL), &o));
            }
            line("resize %s %zu %016llx", FORMATS[fi].name, f, (unsigned long long)h);
            if (RS5[f] == PC_ALGO_RESAMPLE_PRESERVE_DETAILS) {
                if (have_usm) {
                    line("resize_pd %s %016llx", FORMATS[fi].name, (unsigned long long)hp);
                } else {
                    line("resize_pd %s SKIP", FORMATS[fi].name);
                }
            }
        }
        pc_surface_fini(&s);
    }
    {
        pc_rect canvas = pc_rect_make(0, 0, 30, 20);
        static const float fillv[5] = {0.2f, 0.6f, 1.0f, 1.0f, 0.5f};
        static const double cks[4] = {2.0, 1.37, 0.5, 0.11};
        for (fi = 0; fi < NFMT; fi++) {
            pc_surface s, inner;
            pc_surface_init(&s, FORMATS[fi].f);
            CHECK(pc_surface_fill_rect(&s, canvas, fillv, pc_pixel_format_channels(FORMATS[fi].f), NULL) ==
                  PC_OK);
            surf(&inner, FORMATS[fi].f, pc_rect_make(10, 5, 20, 15), 4, false);
            for (f = 0; f < 4; f++) {
                uint64_t h = H0;
                for (k = 0; k < 4; k++) {
                    pc_surface o;
                    double kk = cks[k];
                    hh(&h, hs_st(pc_algo_resize_surface_in_canvas(&s, kk, kk, RS5[f], canvas, &o, NULL), &o));
                    hh(&h,
                       hs_st(pc_algo_resize_surface_in_canvas(&inner, kk, kk * 1.1, RS5[f], canvas, &o, NULL),
                             &o));
                }
                line("resize_canvas %s %zu %016llx", FORMATS[fi].name, f, (unsigned long long)h);
            }
            pc_surface_fini(&s);
            pc_surface_fini(&inner);
        }
    }
    {
        pc_surface mask, sparse;
        const float one = 1.0f, zero = 0.0f, quarter = 0.25f;
        const float white[4] = {1, 1, 1, 1}, other[4] = {0.5f, 0.25f, 1.0f, 1.0f};
        static const double mk[3] = {0.013, 0.37, 1.6};
        static const char *const dn[4][2] = {{"NaN", "1"}, {"0", "1"}, {"-1", "-1"}, {"1", "-0.5"}};
        const double dk[4][2] = {{NAN, 1.0}, {0.0, 1.0}, {-1.0, -1.0}, {1.0, -0.5}};
        pc_surface_init_default(&mask, PC_PIXEL_FORMAT_GRAY8, &one, 1);
        CHECK(pc_surface_fill_rect(&mask, pc_rect_make(0, 0, 40, 30), &zero, 1, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&mask, pc_rect_make(500, 900, 530, 940), &quarter, 1, NULL) == PC_OK);
        pc_surface_init(&sparse, PC_PIXEL_FORMAT_RGBA32F);
        CHECK(pc_surface_fill_rect(&sparse, pc_rect_make(0, 0, 256, 256), white, 4, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&sparse, pc_rect_make(99840, 99840, 100096, 100096), other, 4, NULL) ==
              PC_OK);
        for (f = 0; f < 4; f++) {
            uint64_t h = H0;
            pc_surface o;
            double kk = 1.0 / 100096.0;
            for (k = 0; k < 3; k++) {
                hh(&h, hs_st(pc_algo_resize_surface(&mask, mk[k], mk[k], RS5[f], &o, NULL), &o));
            }
            hh(&h, hs_st(pc_algo_resize_surface(&sparse, kk, kk, RS5[f], &o, NULL), &o));
            hh(&h, hs_st(pc_algo_resize_surface(&sparse, 1000.0 / 100096.0, kk, RS5[f], &o, NULL), &o));
            line("resize_sparse %zu %016llx", f, (unsigned long long)h);
        }
        for (k = 0; k < 4; k++) {
            pc_surface o;
            line("resize_degenerate %s %s %016llx", dn[k][0], dn[k][1],
                 (unsigned long long)hs_st(
                     pc_algo_resize_surface(&mask, dk[k][0], dk[k][1], PC_ALGO_RESAMPLE_BICUBIC, &o, NULL),
                     &o));
        }
        pc_surface_fini(&mask);
        pc_surface_fini(&sparse);
    }
    {
        const pc_rect rs[] = {pc_rect_make(-3, -3, 3, 3), pc_rect_make(0, 0, 1, 1),
                              pc_rect_make(-100, 5, 7, 900), PC_RECT_EMPTY};
        const double ss[4][2] = {{0.5, 0.5}, {1.0 / 3.0, 3.0}, {1e-9, 1e9}, {2.5, 0.013}};
        uint64_t h = H0;
        pc_surface s;
        const int32_t tr[4][2] = {{5, -3}, {256, 0}, {-300, 512}, {0, 0}};
        const pc_rect keeps[] = {pc_rect_make(0, 0, 10, 10), pc_rect_make(-5, 20, 70, 25), PC_RECT_EMPTY};
        for (k = 0; k < 4; k++) {
            for (f = 0; f < 4; f++) {
                hrect(&h, pc_algo_scaled_rect(rs[k], ss[f][0], ss[f][1]));
            }
        }
        line("scaled_rect %016llx", (unsigned long long)h);
        surf(&s, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 64, 32), 3, false);
        h = H0;
        for (k = 0; k < 4; k++) {
            pc_surface o;
            hh(&h, hs_st(pc_algo_translate_surface(&s, tr[k][0], tr[k][1], &o, NULL), &o));
        }
        for (k = 0; k < 3; k++) {
            pc_surface o;
            hh(&h, hs_st(pc_algo_crop_surface(&s, keeps[k], &o, NULL), &o));
        }
        line("translate_crop %016llx", (unsigned long long)h);
        pc_surface_fini(&s);
    }
}

static void transform_vectors(void)
{
    const double r[4] = {10.0, 20.0, 110.0, 70.0};
    static const double quads[6][4][2] = {
        {{0.0, 0.0}, {200.0, 10.0}, {180.0, 150.0}, {-20.0, 120.0}},
        {{10.0, 20.0}, {110.0, 20.0}, {110.0, 70.0}, {10.0, 70.0}},
        {{30.0, -10.0}, {30.0, 30.0}, {10.0, 30.0}, {10.0, -10.0}},
        {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
        {{0.0, 0.0}, {10.0, 0.0}, {20.0, 0.0}, {30.0, 0.0}},
        {{5.0, 5.0}, {6.0, 5.5}, {5.5, 6.5}, {5.0, 6.0}},
    };
    static const double pts[4][2] = {{10.0, 20.0}, {90.0, 60.0}, {-1e6, 3.0}, {55.5, 1e-9}};
    uint64_t h = H0;
    size_t q, i;
    for (q = 0; q < 6; q++) {
        pc_algo_homography m, inv;
        if (pc_algo_homography_rect_to_quad(r, quads[q], &m)) {
            hd(&h, m.m, 9);
            if (pc_algo_homography_inverse(&m, &inv)) {
                pc_algo_homography p = pc_algo_homography_mul(&m, &inv);
                hd(&h, inv.m, 9);
                hd(&h, p.m, 9);
            } else {
                fnv(&h, "noinv", 5);
            }
            for (i = 0; i < 4; i++) {
                double o[3];
                pc_algo_homography_apply(&m, pts[i][0], pts[i][1], &o[0], &o[1]);
                o[2] = pc_algo_homography_local_scale(&m, pts[i][0], pts[i][1]);
                hd(&h, o, 3);
            }
        } else {
            fnv(&h, "none", 4);
        }
    }
    {
        const double rr[2][4] = {{0.0, 0.0, 0.0, 10.0}, {5.0, 5.0, 1.0, 9.0}};
        for (i = 0; i < 2; i++) {
            pc_algo_homography m;
            fnv(&h, pc_algo_homography_rect_to_quad(rr[i], quads[0], &m) ? "some" : "none", 4);
        }
    }
    line("homography %016llx", (unsigned long long)h);
    {
        static const fmt_entry fmts[] = {
            {"rgba8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true}},
            {"rgba16", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}},
            {"rgba32", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}},
            {"rgb8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false}},
            {"gray8", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false}},
            {"cmyka8", {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true}},
        };
        static const double sr[4] = {4.0, 4.0, 44.0, 24.0};
        static const double qs[5][4][2] = {
            {{0.0, 0.0}, {80.0, 0.0}, {80.0, 40.0}, {0.0, 40.0}},
            {{34.0, -6.0}, {34.0, 34.0}, {14.0, 34.0}, {14.0, -6.0}},
            {{0.0, 0.0}, {70.0, 10.0}, {60.0, 50.0}, {-5.0, 40.0}},
            {{0.0, 0.0}, {5.0, 0.0}, {5.0, 2.5}, {0.0, 2.5}},
            {{1.0, 1.0}, {14.0, 2.0}, {13.0, 8.0}, {0.0, 7.0}},
        };
        static const char *const names[] = {"identity", "translate",   "subpixel", "scale2",
                                            "rotate",   "perspective", "shrink8",  "shrink3",
                                            "flip",     "singular",    "infinite", "far"};
        pc_algo_homography ms[12];
        pc_rect src_r = pc_rect_make(4, 4, 44, 24);
        size_t fi, mi;
        ms[0] = PC_ALGO_HOMOGRAPHY_IDENTITY;
        ms[1] = (pc_algo_homography){{1.0, 0.0, 7.0, 0.0, 1.0, -3.0, 0.0, 0.0, 1.0}};
        ms[2] = (pc_algo_homography){{1.0, 0.0, 7.25, 0.0, 1.0, -3.6, 0.0, 0.0, 1.0}};
        for (i = 0; i < 5; i++) {
            if (!pc_algo_homography_rect_to_quad(sr, qs[i], &ms[3 + i])) {
                ms[3 + i] = PC_ALGO_HOMOGRAPHY_IDENTITY;
            }
        }
        ms[8] = (pc_algo_homography){{-1.0, 0.0, 50.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};
        ms[9] = (pc_algo_homography){{1.0, 2.0, 0.0, 2.0, 4.0, 0.0, 0.0, 0.0, 1.0}};
        ms[10] = (pc_algo_homography){{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0}};
        ms[11] = (pc_algo_homography){{1.0, 0.0, 3.0e6, 0.0, 1.0, -2.0e6, 0.0, 0.0, 1.0}};
        for (fi = 0; fi < 6; fi++) {
            pc_surface s;
            surf(&s, fmts[fi].f, src_r, 7, false);
            for (mi = 0; mi < 12; mi++) {
                uint64_t x = H0;
                int it;
                pc_surface o;
                for (it = 0; it < 3; it++) {
                    hh(&x, hs_st(pc_algo_warp_surface(&s, src_r, &ms[mi], (pc_algo_interp)it, &o, NULL), &o));
                }
                hh(&x,
                   hs_st(pc_algo_warp_surface(&s, PC_RECT_EMPTY, &ms[mi], PC_ALGO_INTERP_BICUBIC, &o, NULL),
                         &o));
                hh(&x, hs_st(pc_algo_warp_surface(&s, pc_rect_make(10, 6, 30, 20), &ms[mi],
                                                  PC_ALGO_INTERP_BILINEAR, &o, NULL),
                             &o));
                line("warp %s %s %016llx", fmts[fi].name, names[mi], (unsigned long long)x);
            }
            pc_surface_fini(&s);
        }
    }
}

static void hash_level(uint64_t *h, const pc_algo_level *l, size_t ch)
{
    hu32(h, (uint32_t)l->w);
    hu32(h, (uint32_t)l->h);
    hf(h, l->px, l->w * l->h * ch);
}

static void pyramid_vectors(void)
{
    static const size_t dims[6][3] = {{37, 23, 2}, {1, 1, 3}, {1, 9, 1}, {16, 1, 4}, {64, 32, 1}, {5, 5, 0}};
    size_t d, i;
    for (d = 0; d < 6; d++) {
        size_t w = dims[d][0], h = dims[d][1], ch = dims[d][2];
        pc_algo_level l = {w, h, buf(w * h * ch, 3)}, r;
        pc_algo_pyramid g, lp;
        float *e = NULL, *back = NULL;
        size_t blen = 0;
        uint64_t x = H0;
        uint8_t wh[2];
        CHECK(pc_algo_pyramid_reduce(&l, ch, &r, NULL) == PC_OK);
        CHECK(pc_algo_pyramid_expand(&l, ch, w * 2 + 1, h * 2, &e, NULL) == PC_OK);
        CHECK(pc_algo_pyramid_gaussian(w, h, ch, l.px, 5, &g, NULL) == PC_OK);
        CHECK(pc_algo_pyramid_laplacian(w, h, ch, l.px, 4, &lp, NULL) == PC_OK);
        CHECK(pc_algo_pyramid_collapse(&lp, ch, &back, &blen, NULL) == PC_OK);
        hf(&x, r.px, r.w * r.h * ch);
        wh[0] = (uint8_t)r.w, wh[1] = (uint8_t)r.h;
        fnv(&x, wh, 2);
        hf(&x, e, (w * 2 + 1) * h * 2 * ch);
        for (i = 0; i < g.n; i++) {
            hash_level(&x, &g.levels[i], ch);
        }
        for (i = 0; i < lp.n; i++) {
            hash_level(&x, &lp.levels[i], ch);
        }
        hf(&x, back, blen);
        line("pyramid %zux%zux%zu %zu %016llx", w, h, ch, g.n, (unsigned long long)x);
        free(back);
        free(e);
        pc_algo_pyramid_fini(&g);
        pc_algo_pyramid_fini(&lp);
        pc_algo_level_fini(&r);
        pc_algo_level_fini(&l);
    }
    {
        static const size_t al[6][2] = {{0, 0}, {8, 100}, {9, 9}, {17, 1000}, {100000, 100000}, {5000, 3000}};
        char tmp[128];
        size_t used = 0;
        for (i = 0; i < 6; i++) {
            used += (size_t)snprintf(tmp + used, sizeof tmp - used, " %zu",
                                     pc_algo_pyramid_auto_levels(al[i][0], al[i][1]));
        }
        line("auto_levels%s", tmp);
    }
    {
        const size_t w = 48, h = 40;
        size_t ch;
        for (ch = 1; ch <= 3; ch += 2) {
            float *a = buf(w * h * ch, 1), *b = buf(w * h * ch, 2), *c = malloc(w * h * ch * sizeof(float));
            float *wa = malloc(w * h * sizeof(float)), *wb = malloc(w * h * sizeof(float)),
                  *wc = malloc(w * h * sizeof(float));
            const size_t levels[5] = {0, 1, 3, pc_algo_pyramid_auto_levels(w, h), 12};
            uint64_t x = H0;
            float *out;
            for (i = 0; i < w * h * ch; i++) {
                c[i] = pat((int32_t)i, 5, 0, 3) * 0.5f;
            }
            for (i = 0; i < w * h; i++) {
                wa[i] = i % w < 24 ? 1.0f : 0.0f;
                wb[i] = 1.0f - wa[i];
                wc[i] = pat((int32_t)i, 0, 0, 7);
            }
            for (i = 0; i < 5; i++) {
                const float *im2[2] = {a, b}, *w2[2] = {wa, wb}, *im3[3] = {a, b, c}, *w3[3] = {wa, wb, wc};
                CHECK(pc_algo_pyramid_blend(w, h, ch, im2, w2, 2, levels[i], &out, NULL) == PC_OK);
                hf(&x, out, w * h * ch);
                free(out);
                CHECK(pc_algo_pyramid_blend(w, h, ch, im3, w3, 3, levels[i], &out, NULL) == PC_OK);
                hf(&x, out, w * h * ch);
                free(out);
            }
            CHECK(pc_algo_pyramid_blend(w, h, ch, NULL, NULL, 0, 3, &out, NULL) == PC_OK);
            hf(&x, out, w * h * ch);
            free(out);
            line("blend %zu %016llx", ch, (unsigned long long)x);
            free(a), free(b), free(c), free(wa), free(wb), free(wc);
        }
    }
    {
        static const size_t dd[4][2] = {{48, 48}, {200, 130}, {3, 2}, {1, 1}};
        for (d = 0; d < 4; d++) {
            size_t w = dd[d][0], h = dd[d][1], n = w * h;
            float *i0 = malloc(n * sizeof(float)), *i1 = malloc(n * sizeof(float));
            float *ones = malloc(n * sizeof(float)), *half = malloc(n * sizeof(float));
            float *outs[3] = {NULL, NULL, NULL};
            uint64_t x = H0;
            for (i = 0; i < n; i++) {
                size_t px = i % w, py = i / w;
                float chk = ((px / 2 + py / 2) % 2 == 0) ? 0.1f : 0.9f;
                i0[i] = px < w / 2 ? chk : 0.5f;
                i1[i] = px >= w / 2 ? chk : pat((int32_t)i, 1, 1, 1);
                ones[i] = 1.0f;
                half[i] = (py % 3 == 0) ? 0.0f : 1.0f;
            }
            {
                const float *l2[2] = {i0, i1}, *a2[2] = {ones, half}, *a3[3] = {ones, half, i0};
                CHECK(pc_algo_stack_weights(w, h, l2, a2, 2, outs, NULL) == PC_OK);
                hf(&x, outs[0], n), hf(&x, outs[1], n);
                free(outs[0]), free(outs[1]);
                CHECK(pc_algo_panorama_weights(w, h, a3, 3, outs, NULL) == PC_OK);
                hf(&x, outs[0], n), hf(&x, outs[1], n), hf(&x, outs[2], n);
                free(outs[0]), free(outs[1]), free(outs[2]);
            }
            line("weights %zux%zu %016llx", w, h, (unsigned long long)x);
            free(i0), free(i1), free(ones), free(half);
        }
    }
}

static void apply_vectors(void)
{
    pc_rect r = pc_rect_make(0, 0, 40, 30);
    td_offset ow = {7, -3, PC_ALGO_UNDEF_WRAP}, orp = {-45, 31, PC_ALGO_UNDEF_REPEAT};
    td_offset ot = {5, 0, PC_ALGO_UNDEF_TRANSPARENT};
    struct {
        const char *name;
        const pc_algo_filter_def *d;
        const void *p;
    } fl[] = {{"invert", &TD_INVERT, NULL},        {"solarize", &TD_SOLARIZE, NULL},
              {"findEdges", &TD_FIND_EDGES, NULL}, {"offsetWrap", &TD_OFFSET, &ow},
              {"offsetRepeat", &TD_OFFSET, &orp},  {"offsetTransparent", &TD_OFFSET, &ot}};
    pc_surface sel_half, sel_grad, sel_none;
    const float half = 0.5f, one = 1.0f;
    pc_pixel_format grayf = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, false};
    size_t fi, k;
    pc_surface_init_default(&sel_half, grayf, &half, 1);
    CHECK(pc_surface_fill_rect(&sel_half, pc_rect_make(20, 0, 40, 10), &one, 1, NULL) == PC_OK);
    surf(&sel_grad, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(-5, -5, 45, 35), 3, false);
    pc_surface_init(&sel_none, PC_PIXEL_FORMAT_GRAY8);
    for (fi = 0; fi < NFMT; fi++) {
        pc_surface s, opaque;
        surf(&s, FORMATS[fi].f, pc_rect_make(-4, 2, 37, 33), 9, false);
        surf(&opaque, FORMATS[fi].f, r, 1, true);
        for (k = 0; k < 6; k++) {
            pc_algo_filter f = td_make(fl[k].d, fl[k].p);
            uint64_t h = H0;
            pc_rect area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), r, NULL);
            const int32_t tiles[4] = {256, 7, 1, 64};
            const pc_surface *sels[3] = {&sel_half, &sel_grad, &sel_none};
            pc_rect ext2 = pc_rect_make(-2, -2, 30, 31);
            size_t t;
            pc_surface o;
            hrect(&h, area);
            for (t = 0; t < 4; t++) {
                hh(&h, hs_st(pc_algo_apply_tiled(&s, &f, area, r, NULL, tiles[t], NULL, &o, NULL), &o));
            }
            hh(&h, hs_st(pc_algo_apply(&s, &f, area, r, NULL, &o, NULL), &o));
            for (t = 0; t < 3; t++) {
                hh(&h, hs_st(pc_algo_apply(&s, &f, area, r, sels[t], &o, NULL), &o));
            }
            hh(&h, hs_st(pc_algo_apply_in(&s, &f, area, r, NULL, r, &o, NULL), &o));
            hh(&h,
               hs_st(pc_algo_apply_in(&opaque, &f,
                                      pc_algo_output_area(&f, pc_surface_content_bounds(&opaque), r, NULL), r,
                                      &sel_grad, r, &o, NULL),
                     &o));
            hh(&h,
               hs_st(pc_algo_apply_tiled(&s, &f, pc_rect_make(3, 3, 3, 9), r, NULL, 16, &r, &o, NULL), &o));
            hh(&h, hs_st(pc_algo_apply_tiled(&s, &f, pc_rect_make(-9, -9, 50, 50), pc_rect_make(5, 5, 25, 20),
                                             NULL, 9, &ext2, &o, NULL),
                         &o));
            line("apply %s %s %016llx", FORMATS[fi].name, fl[k].name, (unsigned long long)h);
            pc_algo_filter_fini(&f);
        }
        pc_surface_fini(&s);
        pc_surface_fini(&opaque);
    }
    pc_surface_fini(&sel_half);
    pc_surface_fini(&sel_grad);
    pc_surface_fini(&sel_none);
}

/* ------------------------------------------------------------------------------------------ */

#include "core_parity_r5.h"

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

static void test_core_parity(void)
{
    size_t glen = 0, lines = 0;
    char *golden = read_file(GOLDEN, &glen), *want, *got, *wsave = NULL, *gsave = NULL;
    char *golden_de;
    CHECK(golden != NULL);
    if (golden == NULL) {
        return;
    }
    golden_de = malloc(glen + 2);
    golden_de[0] = '\n';
    memcpy(golden_de + 1, golden, glen + 1);
    pc_buf_init(&OUT);
    enums();
    filter_serde(golden_de);
    image_vectors();
    fxutil_vectors();
    histogram_vectors();
    tone_vectors();
    resample_vectors();
    transform_vectors();
    pyramid_vectors();
    apply_vectors();
    r5_vectors();
    want = strtok_r(golden, "\n", &wsave);
    got = strtok_r((char *)OUT.data, "\n", &gsave);
    while (want != NULL || got != NULL) {
        lines++;
        if (want != NULL && got != NULL && strncmp(want, "resize_pd ", 10) == 0 &&
            strstr(got, " SKIP") != NULL) {
            SKIPPED++;
        } else if (want != NULL && got != NULL && strstr(got, "\tERR ") != NULL &&
                   strlen(strstr(got, "\tERR ") + 5) == PC_ERROR_MSG_MAX - 1 &&
                   strncmp(want, got, strlen(got)) == 0) {
            pc_t_checks++; /* pc_error keeps the first 255 bytes of a long message (plan section 7) */
        } else if (want == NULL || got == NULL || strcmp(want, got) != 0) {
            PC_T_FAIL("line %zu:\n  want %.400s\n  got  %.400s", lines, want != NULL ? want : "(end)",
                      got != NULL ? got : "(end)");
        } else {
            pc_t_checks++;
        }
        want = want != NULL ? strtok_r(NULL, "\n", &wsave) : NULL;
        got = got != NULL ? strtok_r(NULL, "\n", &gsave) : NULL;
    }
    if (SKIPPED > 0) {
        (void)fprintf(stderr,
                      "test_core_parity: %d Preserve Details lines skipped (Unsharp Mask not registered)\n",
                      SKIPPED);
    }
    pc_buf_fini(&OUT);
    free(golden);
    free(golden_de);
}

int main(int argc, char **argv)
{
    RUN(test_core_parity);
    return pc_t_finish();
}
