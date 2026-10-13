/* Bit-exact parity with crates/algo/src/{liquify,puppet}.rs: replays every line of
 * tests/golden/algo-warp-vectors.txt (`rustref algo-warp-vectors`, tools/rustref/src/
 * algo_warp_vectors.rs). Each line carries its inputs (JSON, or f64 bit patterns in hex) and the
 * reference result; hashes are FNV-1a over little-endian f32/f64/integer bytes. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/buf.h"
#include "base/json.h"
#include "harness.h"

#define GOLDEN "tests/golden/algo-warp-vectors.txt"
#define H0 0xcbf29ce484222325ull

/* ---- hashing ---- */

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
static void h32(uint64_t *h, uint32_t u)
{
    uint8_t b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
    fnv(h, b, 4);
}
static void hf(uint64_t *h, const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t u = 0x7fc00000u; /* NaNs hash canonically (see the rustref module) */
        if (!isnan(v[i])) {
            memcpy(&u, &v[i], 4);
        }
        h32(h, u);
    }
}
static void hd(uint64_t *h, const double *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint64_t u = 0x7ff8000000000000ull;
        if (!isnan(v[i])) {
            memcpy(&u, &v[i], 8);
        }
        h64(h, u);
    }
}
static void hi32(uint64_t *h, int32_t v)
{
    h32(h, (uint32_t)v);
}
static void hrect(uint64_t *h, pc_rect r)
{
    hi32(h, r.x0), hi32(h, r.y0), hi32(h, r.x1), hi32(h, r.y1);
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

/* ---- inputs ---- */

static double fhex(const char *s)
{
    uint64_t u = strtoull(s, NULL, 16);
    double d;
    memcpy(&d, &u, 8);
    return d;
}
static pc_rect rect_of(const char *s)
{
    int a = 0, b = 0, c = 0, d = 0;
    CHECK(sscanf(s, "%d,%d,%d,%d", &a, &b, &c, &d) == 4);
    return pc_rect_make(a, b, c, d);
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
static void surf(pc_pixel_format fmt, pc_rect r, int32_t seed, bool opaque, pc_surface *s)
{
    size_t n = pc_pixel_format_channels(fmt), k = 0, c;
    size_t len = (size_t)pc_rect_width(r) * pc_rect_height(r) * n;
    float *v = malloc(len > 0 ? len * sizeof *v : 1);
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
static pc_pixel_format format_named(const char *name)
{
    pc_pixel_format f = {PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_F32, true};
    static const struct {
        const char *name;
        pc_pixel_format f;
    } T[] = {{"rgba8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true}},
             {"rgba16", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}},
             {"rgba32", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}},
             {"rgb8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false}},
             {"gray8", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false}},
             {"graya16", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, true}},
             {"cmyka8", {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true}}};
    size_t i;
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (strcmp(T[i].name, name) == 0) {
            return T[i].f;
        }
    }
    return f;
}

static void write_px(pc_surface *s, int32_t x, int32_t y, const float *px, size_t n)
{
    CHECK(pc_surface_write_pixel(s, x, y, px, n, NULL) == PC_OK);
}
/* The L shape of the reference's tests. */
static void blob(pc_sample_type st, bool alpha, pc_surface *s)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, st, alpha};
    int32_t x, y;
    pc_surface_init(s, f);
    for (y = 10; y < 70; y++) {
        for (x = 20; x < 36; x++) {
            float px[4] = {(float)((x + y) % 7) / 6.0f, 0.5f, (float)y / 80.0f, 1.0f};
            write_px(s, x, y, px, alpha ? 4 : 3);
        }
    }
    for (y = 56; y < 70; y++) {
        for (x = 36; x < 70; x++) {
            float px[4] = {0.2f, (float)((x * 3) % 5) / 4.0f, 0.8f, 1.0f};
            write_px(s, x, y, px, alpha ? 4 : 3);
        }
    }
}
static void islands(pc_surface *s)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true};
    int32_t x, y;
    pc_surface_init(s, f);
    for (y = 0; y < 40; y++) {
        for (x = 0; x < 120; x++) {
            if ((x < 40 && y > 5) || (x > 80 && y < 30)) {
                float px[4] = {pat(x, y, 0, 1), pat(x, y, 1, 1), pat(x, y, 2, 1),
                               0.03f + (float)(x % 5) * 0.2f};
                write_px(s, x, y, px, 4);
            }
        }
    }
}
static void surface_named(const char *name, pc_surface *s)
{
    if (strcmp(name, "blob8") == 0) {
        blob(PC_SAMPLE_TYPE_U8, true, s);
    } else if (strcmp(name, "blob32") == 0) {
        blob(PC_SAMPLE_TYPE_F32, true, s);
    } else if (strcmp(name, "blobrgb") == 0) {
        blob(PC_SAMPLE_TYPE_U8, false, s);
    } else {
        islands(s);
    }
}

static yyjson_doc *parse(const char *js)
{
    yyjson_doc *d = NULL;
    CHECK(pc_json_parse(js, strlen(js), &d, NULL) == PC_OK);
    return d;
}

static bool strokes_of(const char *js, pc_algo_liquify_stroke **s, size_t *n)
{
    yyjson_doc *d = parse(js);
    pc_error e;
    bool ok = d != NULL && pc_algo_liquify_strokes_from_json(yyjson_doc_get_root(d), s, n, &e) == PC_OK;
    CHECK(ok);
    yyjson_doc_free(d);
    return ok;
}

static void hfield(uint64_t *h, const pc_algo_liquify_field *f)
{
    hd(h, &f->cell, 1);
    h64(h, f->w);
    h64(h, f->h);
    hf(h, &f->d[0][0], f->w * f->h * 2);
    hf(h, f->freeze, f->w * f->h);
}

/* ---- cases ---- */

static char *case_field(char **a)
{
    pc_rect b = rect_of(a[1]);
    double cell = fhex(a[2]);
    pc_algo_liquify_stroke *st = NULL;
    size_t n = 0, i;
    pc_algo_liquify_field f;
    uint64_t h = H0;
    int i2, j2;
    char *out = malloc(32);
    if (!strokes_of(a[3], &st, &n) || pc_algo_liquify_field_init(&f, b, cell, NULL) != PC_OK) {
        snprintf(out, 32, "error");
        return out;
    }
    for (i = 0; i < n; i++) {
        pc_rect d = PC_RECT_EMPTY;
        CHECK(pc_algo_liquify_apply_stroke(&f, &st[i], &d, NULL) == PC_OK);
        hrect(&h, d);
    }
    hfield(&h, &f);
    for (j2 = -3; j2 <= 12; j2++) {
        for (i2 = -3; i2 <= 16; i2++) {
            double x = (double)b.x0 + (double)i2 * 7.3 - 2.0, y = (double)b.y0 + (double)j2 * 6.1 - 2.5, s[2];
            float fz;
            pc_algo_liquify_sample(&f, x, y, s);
            hd(&h, s, 2);
            fz = pc_algo_liquify_freeze_at(&f, x, y);
            hf(&h, &fz, 1);
        }
    }
    {
        float m = pc_algo_liquify_max_displacement(&f);
        uint8_t id = (uint8_t)pc_algo_liquify_is_identity(&f);
        hf(&h, &m, 1);
        fnv(&h, &id, 1);
    }
    pc_algo_liquify_field_fini(&f);
    pc_algo_liquify_strokes_free(st, n);
    snprintf(out, 32, "%016llx", (unsigned long long)h);
    return out;
}

static char *case_segment(char **a)
{
    static const pc_algo_liquify_tool tools[] = {
        PC_ALGO_LIQUIFY_FORWARD_WARP, PC_ALGO_LIQUIFY_RECONSTRUCT, PC_ALGO_LIQUIFY_SMOOTH,
        PC_ALGO_LIQUIFY_TWIRL_CW,     PC_ALGO_LIQUIFY_PUCKER,      PC_ALGO_LIQUIFY_BLOAT,
        PC_ALGO_LIQUIFY_PUSH_LEFT,    PC_ALGO_LIQUIFY_FREEZE,      PC_ALGO_LIQUIFY_THAW,
        PC_ALGO_LIQUIFY_LASSO_MASK,   PC_ALGO_LIQUIFY_FREEZE_ALL};
    int ti = atoi(a[1]);
    double pa[3], pb[3], w0[3] = {20.0, 20.0, 1.0}, w1[3] = {60.0, 40.0, 1.0};
    char *p = a[3], *q = a[4], *out = malloc(32);
    int k;
    pc_algo_liquify_field f;
    pc_algo_liquify_stroke s = pc_algo_liquify_stroke_make(tools[ti], fhex(a[2]));
    pc_algo_liquify_stroke pre = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_FORWARD_WARP, 30.0);
    pc_rect d = PC_RECT_EMPTY;
    uint64_t h = H0;
    for (k = 0; k < 3; k++) {
        pa[k] = fhex(p);
        pb[k] = fhex(q);
        p += 17;
        q += 17;
    }
    CHECK(pc_algo_liquify_field_init(&f, pc_rect_make(-7, 3, 89, 67), 2.0, NULL) == PC_OK);
    CHECK(pc_algo_liquify_stroke_segment(&f, &pre, w0, w1, NULL, NULL) == PC_OK);
    CHECK(pc_algo_liquify_stroke_begin(&f, &s, pa, &d, NULL) == PC_OK);
    hrect(&h, d);
    CHECK(pc_algo_liquify_stroke_segment(&f, &s, pa, pb, &d, NULL) == PC_OK);
    hrect(&h, d);
    hfield(&h, &f);
    pc_algo_liquify_field_fini(&f);
    snprintf(out, 32, "%016llx", (unsigned long long)h);
    return out;
}

static char *case_apply(char **a)
{
    pc_pixel_format fmt = format_named(a[1]);
    pc_rect b = rect_of(a[2]);
    int seed = pc_rect_eq(b, pc_rect_make(0, 0, 96, 64)) ? 3 : 4;
    pc_surface src, res;
    pc_algo_liquify_stroke *st = NULL;
    size_t n = 0;
    pc_algo_liquify_field f;
    char *out = malloc(32);
    surf(fmt, pc_rect_make(b.x0 + 2, b.y0 + 1, b.x1 - 3, b.y1 - 2), seed, a[3][0] == '1', &src);
    snprintf(out, 32, "error");
    if (strokes_of(a[4], &st, &n) &&
        pc_algo_liquify_field_from_strokes(b, pc_algo_liquify_auto_cell(b), st, n, &f, NULL) == PC_OK) {
        if (pc_algo_liquify_apply(&src, &f, &res, NULL) == PC_OK) {
            snprintf(out, 32, "%016llx", (unsigned long long)hs(&res));
            pc_surface_fini(&res);
        }
        pc_algo_liquify_field_fini(&f);
    }
    pc_algo_liquify_strokes_free(st, n);
    pc_surface_fini(&src);
    return out;
}

static char *case_proxy(char **a)
{
    const char *js = "[{\"tool\":\"bloat\",\"size\":40,\"points\":[[48,32],[48,32],[48,32],[48,32]]},"
                     "{\"tool\":\"forwardWarp\",\"size\":30,\"points\":[[20,20],[30,25]]}]";
    pc_pixel_format fmt = format_named(a[1]);
    pc_rect b = rect_of(a[2]), rs[4];
    size_t max_side = (size_t)strtoull(a[3], NULL, 10), n = 0, i, r1[4] = {0, 0, 0, 0}, r2[4] = {1, 2, 5, 3};
    pc_surface src;
    pc_algo_liquify_stroke *st = NULL;
    pc_algo_liquify_field f;
    pc_algo_liquify_proxy p;
    uint64_t h = H0;
    uint8_t (*o)[4];
    char *out = malloc(32);
    snprintf(out, 32, "error");
    surf(fmt, pc_rect_make(-10, -10, 140, 90), 5, false, &src);
    if (!strokes_of(js, &st, &n) || pc_algo_liquify_field_from_strokes(b, 2.0, st, n, &f, NULL) != PC_OK) {
        pc_surface_fini(&src);
        return out;
    }
    if (pc_algo_liquify_proxy_init(&p, &src, b, max_side, NULL) == PC_OK) {
        hd(&h, &p.scale, 1);
        h64(&h, p.w);
        h64(&h, p.h);
        hf(&h, &p.px[0][0], p.w * p.h * 4);
        o = calloc(p.w * p.h, sizeof *o);
        r1[2] = p.w;
        r1[3] = p.h;
        CHECK(pc_algo_liquify_proxy_render(&p, &f, r1, o, p.w * p.h, NULL) == PC_OK);
        CHECK(pc_algo_liquify_proxy_render(&p, &f, r2, o, p.w * p.h, NULL) == PC_OK);
        fnv(&h, o, p.w * p.h * 4);
        free(o);
        rs[0] = pc_rect_make(0, 0, 10, 10);
        rs[1] = pc_rect_make(-100, -100, 5000, 5000);
        rs[2] = pc_rect_make(INT32_MIN, 3, INT32_MAX, 9);
        rs[3] = b;
        for (i = 0; i < 4; i++) {
            size_t q[4];
            int k;
            pc_algo_liquify_proxy_rect(&p, rs[i], q);
            for (k = 0; k < 4; k++) {
                h64(&h, q[k]);
            }
        }
        pc_algo_liquify_proxy_fini(&p);
        snprintf(out, 32, "%016llx", (unsigned long long)h);
    }
    pc_algo_liquify_field_fini(&f);
    pc_algo_liquify_strokes_free(st, n);
    pc_surface_fini(&src);
    return out;
}

/* R6: render_full_resolution, the result flag then the output bytes. */
static char *case_fullres(char **a)
{
    const char *js = "[{\"tool\":\"bloat\",\"size\":40,\"points\":[[48,32],[48,32],[48,32],[48,32]]},"
                     "{\"tool\":\"forwardWarp\",\"size\":30,\"points\":[[20,20],[30,25],[60,40]]}]";
    pc_pixel_format fmt = format_named(a[1]);
    pc_rect canvas = pc_rect_make(0, 0, 96, 64), bounds = rect_of(a[2]), region = rect_of(a[3]), b;
    pc_surface src;
    pc_algo_liquify_stroke *st = NULL;
    pc_algo_liquify_field f;
    pc_algo_liquify_scratch scratch = {NULL, 0};
    size_t n = 0, len;
    uint8_t (*o)[4];
    uint64_t h = H0, ok;
    char *out = malloc(32);
    snprintf(out, 32, "error");
    surf(fmt, pc_rect_make(-10, -10, 140, 90), 5, false, &src);
    if (!strokes_of(js, &st, &n) ||
        pc_algo_liquify_field_from_strokes(canvas, 2.0, st, n, &f, NULL) != PC_OK) {
        pc_surface_fini(&src);
        return out;
    }
    b = pc_rect_intersect(bounds, f.bounds);
    len = (size_t)pc_rect_width(b) * pc_rect_height(b);
    o = calloc(len > 0 ? len : 1, sizeof *o);
    ok = pc_algo_liquify_render_full_resolution(&src, &f, bounds, region, o, len, &scratch) ? 1u : 0u;
    h64(&h, ok);
    fnv(&h, o, len * 4);
    snprintf(out, 32, "%016llx", (unsigned long long)h);
    free(o), free(scratch.px);
    pc_algo_liquify_field_fini(&f);
    pc_algo_liquify_strokes_free(st, n);
    pc_surface_fini(&src);
    return out;
}

static char *dup_buf(pc_buf *b)
{
    size_t len = 0;
    char *s = (char *)pc_buf_take(b, &len);
    char *o = malloc(len + 1);
    if (len > 0) {
        memcpy(o, s, len);
    }
    o[len] = '\0';
    free(s);
    return o;
}

static char *json_result(int kind, const char *js)
{
    yyjson_doc *d = parse(js);
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    pc_error e;
    pc_buf b;
    pc_status st;
    size_t i;
    pc_buf_init(&b);
    if (kind == 0) {
        pc_algo_liquify_stroke *s = NULL;
        size_t n = 0;
        st = pc_algo_liquify_strokes_from_json(yyjson_doc_get_root(d), &s, &n, &e);
        if (st == PC_OK) {
            root = yyjson_mut_arr(m);
            for (i = 0; i < n; i++) {
                yyjson_mut_arr_append(root, pc_algo_liquify_stroke_to_json(m, &s[i]));
            }
            pc_algo_liquify_strokes_free(s, n);
        }
    } else if (kind == 1) {
        pc_algo_puppet_pin *p = NULL;
        size_t n = 0;
        st = pc_algo_puppet_pins_from_json(yyjson_doc_get_root(d), &p, &n, &e);
        if (st == PC_OK) {
            root = yyjson_mut_arr(m);
            for (i = 0; i < n; i++) {
                yyjson_mut_arr_append(root, pc_algo_puppet_pin_to_json(m, &p[i]));
            }
            free(p);
        }
    } else {
        pc_algo_puppet_warp w;
        st = pc_algo_puppet_warp_from_json(yyjson_doc_get_root(d), &w, &e);
        if (st == PC_OK) {
            root = pc_algo_puppet_warp_to_json(m, &w);
            pc_algo_puppet_warp_fini(&w);
        }
    }
    if (st == PC_OK) {
        pc_buf_append_str(&b, "ok:", NULL);
        CHECK(pc_json_write(root, 0, &b, NULL) == PC_OK);
    } else {
        pc_buf_appendf(&b, NULL, "err:%s", e.msg);
    }
    yyjson_mut_doc_free(m);
    yyjson_doc_free(d);
    return dup_buf(&b);
}

static void hmesh(uint64_t *h, const pc_algo_puppet_mesh *m)
{
    size_t i;
    hd(h, &m->spacing, 1);
    h64(h, m->nverts);
    h64(h, m->ntris);
    if (m->nverts > 0) {
        hd(h, &m->verts[0][0], m->nverts * 2);
    }
    for (i = 0; i < m->ntris; i++) {
        h64(h, m->tris[i][0]);
        h64(h, m->tris[i][1]);
        h64(h, m->tris[i][2]);
    }
}

static char *hex_out(uint64_t h)
{
    char *o = malloc(32);
    snprintf(o, 32, "%016llx", (unsigned long long)h);
    return o;
}

static char *case_mesh(char **a)
{
    pc_surface s;
    pc_algo_puppet_mesh m;
    uint64_t h = H0;
    char *out;
    surface_named(a[1], &s);
    if (pc_algo_puppet_build_mesh(&s, rect_of(a[2]), (pc_algo_puppet_density)atoi(a[3]), fhex(a[4]), &m,
                                  NULL) != PC_OK) {
        pc_surface_fini(&s);
        out = malloc(8);
        snprintf(out, 8, "error");
        return out;
    }
    hmesh(&h, &m);
    pc_algo_puppet_mesh_fini(&m);
    pc_surface_fini(&s);
    return hex_out(h);
}

typedef struct pinset {
    double (*src)[2], (*dst)[2];
    bool *fixed;
    pc_algo_puppet_rot *rot;
    int32_t *depth;
    size_t n;
} pinset;

static void pinset_of(const char *js, pinset *ps)
{
    yyjson_doc *d = parse(js);
    pc_algo_puppet_pin *p = NULL;
    size_t i, n = 0;
    CHECK(pc_algo_puppet_pins_from_json(yyjson_doc_get_root(d), &p, &n, NULL) == PC_OK);
    yyjson_doc_free(d);
    ps->n = n;
    ps->src = calloc(n + 1, sizeof *ps->src);
    ps->dst = calloc(n + 1, sizeof *ps->dst);
    ps->fixed = calloc(n + 1, sizeof *ps->fixed);
    ps->rot = calloc(n + 1, sizeof *ps->rot);
    ps->depth = calloc(n + 1, sizeof *ps->depth);
    for (i = 0; i < n; i++) {
        memcpy(ps->src[i], p[i].src, 16);
        memcpy(ps->dst[i], p[i].dst, 16);
        ps->fixed[i] = p[i].rotate.set;
        ps->rot[i] = p[i].rotate;
        ps->depth[i] = p[i].depth;
    }
    free(p);
}
static void pinset_fini(pinset *ps)
{
    free(ps->src);
    free(ps->dst);
    free(ps->fixed);
    free(ps->rot);
    free(ps->depth);
}

static const pc_algo_puppet_mode MODES[3] = {PC_ALGO_PUPPET_RIGID, PC_ALGO_PUPPET_NORMAL,
                                             PC_ALGO_PUPPET_DISTORT};

static char *case_solve(char **a)
{
    pc_surface s;
    pc_algo_puppet_mesh m;
    pc_algo_puppet_solver *sv = NULL;
    pinset ps;
    double (*v)[2] = NULL, *e = NULL, (*v2)[2] = NULL, *e2 = NULL, en = 0.0;
    size_t iters = (size_t)atoi(a[4]), *order = NULL, nv;
    uint64_t h = H0;
    surface_named(a[1], &s);
    pinset_of(a[5], &ps);
    CHECK(pc_algo_puppet_build_mesh(&s, pc_surface_content_bounds(&s), (pc_algo_puppet_density)atoi(a[2]),
                                    2.0, &m, NULL) == PC_OK);
    CHECK(pc_algo_puppet_solver_new(&m, (const double (*)[2])ps.src, ps.n, ps.fixed, ps.n, &sv, NULL) ==
          PC_OK);
    nv = pc_algo_puppet_solver_mesh(sv)->nverts;
    CHECK(pc_algo_puppet_solve(sv, (const double (*)[2])ps.dst, ps.n, ps.rot, ps.n, MODES[atoi(a[3])], iters,
                               NULL, 0, &v, &e, NULL) == PC_OK);
    if (nv > 0) {
        hd(&h, &v[0][0], nv * 2);
    }
    if (iters > 0 && nv > 0) {
        hd(&h, e, iters);
    }
    CHECK(pc_algo_puppet_energy(sv, (const double (*)[2])v, (const double (*)[2])ps.dst, ps.n, ps.rot, ps.n,
                                &en, NULL) == PC_OK);
    hd(&h, &en, 1);
    CHECK(pc_algo_puppet_solve(sv, (const double (*)[2])ps.dst, ps.n > 0 ? ps.n - 1 : 0, ps.rot, ps.n,
                               MODES[atoi(a[3])], 2, (const double (*)[2])v, nv, &v2, &e2, NULL) == PC_OK);
    if (nv > 0) {
        hd(&h, &v2[0][0], nv * 2);
        hd(&h, e2, 2);
    }
    CHECK(pc_algo_puppet_draw_order(sv, ps.depth, ps.n, (const double (*)[2])ps.src, ps.n, &order, NULL) ==
          PC_OK);
    {
        size_t i, nt = pc_algo_puppet_solver_mesh(sv)->ntris;
        for (i = 0; i < nt; i++) {
            h64(&h, order[i]);
        }
    }
    free(order);
    free(v);
    free(e);
    free(v2);
    free(e2);
    pc_algo_puppet_solver_free(sv);
    pinset_fini(&ps);
    pc_surface_fini(&s);
    return hex_out(h);
}

static void grid_mesh(size_t g, double sp, bool scramble, pc_algo_puppet_mesh *m)
{
    size_t *perm = malloc(g * g * sizeof *perm), i, j, k = 0;
    for (i = 0; i < g * g; i++) {
        perm[i] = scramble ? (size_t)(((uint64_t)i * 2654435761u) % (g * g)) : i;
    }
    m->nverts = g * g;
    m->verts = calloc(g * g, sizeof *m->verts);
    m->ntris = (g - 1) * (g - 1) * 2;
    m->tris = malloc(m->ntris * sizeof *m->tris);
    m->spacing = sp;
    for (j = 0; j < g; j++) {
        for (i = 0; i < g; i++) {
            m->verts[perm[j * g + i]][0] = (double)i * sp;
            m->verts[perm[j * g + i]][1] = (double)j * sp;
        }
    }
    for (j = 0; j + 1 < g; j++) {
        for (i = 0; i + 1 < g; i++) {
            size_t a = perm[j * g + i], b = perm[j * g + i + 1], c = perm[(j + 1) * g + i + 1],
                   d = perm[(j + 1) * g + i];
            m->tris[k][0] = a, m->tris[k][1] = b, m->tris[k][2] = c, k++;
            m->tris[k][0] = a, m->tris[k][1] = c, m->tris[k][2] = d, k++;
        }
    }
    free(perm);
}

static void obtuse_mesh(pc_algo_puppet_mesh *m)
{
    static const double V[7][2] = {{0, 0}, {10, 0}, {5, 0.5}, {5, -8}, {20, 1}, {20, 1}, {30, 30}};
    static const size_t T[5][3] = {{0, 1, 2}, {0, 3, 1}, {1, 4, 2}, {1, 4, 5}, {4, 5, 5}};
    m->nverts = 7;
    m->verts = malloc(sizeof V);
    memcpy(m->verts, V, sizeof V);
    m->ntris = 5;
    m->tris = malloc(sizeof T);
    memcpy(m->tris, T, sizeof T);
    m->spacing = 1.0;
}

static char *case_custom(char **a)
{
    const char *name = a[1];
    double nan = (double)NAN, src[3][2], dst[3][2];
    pc_algo_puppet_rot rot[3] = {{false, 0}, {false, 0}, {false, 0}};
    bool fixed[3];
    size_t n = 0, k, iters = (size_t)atoi(a[3]), nv;
    pc_algo_puppet_mesh m;
    pc_algo_puppet_solver *sv = NULL;
    double (*v)[2] = NULL, *e = NULL;
    uint64_t h = H0;
    memset(&m, 0, sizeof m);
#define PIN(sx, sy, dx, dy, rs, rd)                                                                          \
    (src[n][0] = (sx), src[n][1] = (sy), dst[n][0] = (dx), dst[n][1] = (dy), rot[n].set = (rs),              \
     rot[n].deg = (rd), n++)
    if (strcmp(name, "scrambled") == 0) {
        grid_mesh(80, 3.0, true, &m);
        PIN(30, 30, 40, 20, false, 0);
        PIN(200, 210, 190, 230, true, 30);
    } else if (strcmp(name, "obtuse") == 0) {
        obtuse_mesh(&m);
        PIN(5, 0.2, 6, 3, false, 0);
        PIN(20, 1, 25, 1, false, 0);
    } else if (strcmp(name, "obtuse-rot") == 0) {
        obtuse_mesh(&m);
        PIN(5, -2, 5, -1, true, 15);
    } else if (strcmp(name, "empty") == 0) {
        PIN(10, 10, 12, 10, true, 5);
    } else if (strcmp(name, "nan-src") == 0) {
        grid_mesh(12, 5.0, false, &m);
        PIN(nan, 30, 40, 20, false, 0);
        PIN(20, 20, 25, 20, false, 0);
    } else if (strcmp(name, "nan-dst") == 0) {
        grid_mesh(12, 5.0, false, &m);
        PIN(30, 30, nan, 20, true, nan);
    } else if (strcmp(name, "coincident") == 0) {
        grid_mesh(12, 5.0, false, &m);
        PIN(30, 30, 30, 30, false, 0);
        PIN(30, 30, 40, 30, false, 0);
        PIN(30, 30, 30, 40, false, 0);
    } else {
        grid_mesh(12, 5.0, false, &m);
        PIN(10, 10, 10, 10, false, 0);
        PIN(20, 10, 20, 30, false, 0);
        PIN(30, 10, 30, 10, false, 0);
    }
#undef PIN
    for (k = 0; k < n; k++) {
        fixed[k] = rot[k].set;
    }
    CHECK(pc_algo_puppet_solver_new(&m, (const double (*)[2])src, n, fixed, n, &sv, NULL) == PC_OK);
    nv = pc_algo_puppet_solver_mesh(sv)->nverts;
    CHECK(pc_algo_puppet_solve(sv, (const double (*)[2])dst, n, rot, n, MODES[atoi(a[2])], iters, NULL, 0, &v,
                               &e, NULL) == PC_OK);
    if (nv > 0) {
        hd(&h, &v[0][0], nv * 2);
        hd(&h, e, iters);
    }
    free(v);
    free(e);
    pc_algo_puppet_solver_free(sv);
    return hex_out(h);
}

static char *case_warp(char **a)
{
    static const pc_algo_interp INTERP[3] = {PC_ALGO_INTERP_NEAREST, PC_ALGO_INTERP_BILINEAR,
                                             PC_ALGO_INTERP_BICUBIC};
    pc_surface s, o;
    pc_algo_puppet_warp w;
    yyjson_doc *d = parse(a[3]);
    pc_status st;
    char *out;
    surface_named(a[1], &s);
    CHECK(pc_algo_puppet_warp_from_json(yyjson_doc_get_root(d), &w, NULL) == PC_OK);
    yyjson_doc_free(d);
    st = pc_algo_puppet_apply(&s, pc_surface_content_bounds(&s), &w, INTERP[atoi(a[2])], &o, NULL);
    if (st == PC_OK) {
        out = hex_out(hs(&o));
        pc_surface_fini(&o);
    } else {
        out = malloc(8);
        snprintf(out, 8, st == PC_ERR_LIMIT ? "none" : "error");
    }
    pc_algo_puppet_warp_fini(&w);
    pc_surface_fini(&s);
    return out;
}

static char *case_deform(char **a)
{
    pc_surface s;
    pc_algo_puppet_warp w;
    yyjson_doc *d = parse(a[2]);
    pc_algo_puppet_solver *sv = NULL;
    double (*v)[2] = NULL;
    size_t *order = NULL, i;
    uint64_t h = H0;
    const pc_algo_puppet_mesh *m;
    surface_named(a[1], &s);
    CHECK(pc_algo_puppet_warp_from_json(yyjson_doc_get_root(d), &w, NULL) == PC_OK);
    yyjson_doc_free(d);
    CHECK(pc_algo_puppet_deform(&s, pc_surface_content_bounds(&s), &w, 5, &sv, &v, &order, NULL) == PC_OK);
    m = pc_algo_puppet_solver_mesh(sv);
    hmesh(&h, m);
    if (m->nverts > 0) {
        hd(&h, &v[0][0], m->nverts * 2);
    }
    for (i = 0; i < m->ntris; i++) {
        h64(&h, order[i]);
    }
    free(v);
    free(order);
    pc_algo_puppet_solver_free(sv);
    pc_algo_puppet_warp_fini(&w);
    pc_surface_fini(&s);
    return hex_out(h);
}

static char *case_misc(char **a, size_t nf)
{
    char *out = malloc(512);
    const char *k = a[0];
    out[0] = '\0';
    if (strcmp(k, "tool") == 0) {
        size_t i;
        for (i = 0; i < PC_ALGO_LIQUIFY_TOOL_COUNT; i++) {
            char q[64];
            const char *nm = pc_algo_enum_name(&pc_algo_liquify_tool_desc, (int)i);
            snprintf(q, sizeof q, "\"%s\"", nm);
            if (strcmp(q, a[1]) == 0) {
                snprintf(out, 512, "%s\t%d\t%d", pc_algo_liquify_tool_label((pc_algo_liquify_tool)i),
                         (int)pc_algo_liquify_tool_is_global((pc_algo_liquify_tool)i),
                         (int)pc_algo_liquify_tool_is_stationary((pc_algo_liquify_tool)i));
            }
        }
    } else if (strcmp(k, "validate") == 0) {
        pc_algo_liquify_stroke s = pc_algo_liquify_stroke_make(PC_ALGO_LIQUIFY_BLOAT, fhex(a[1]));
        char *p = a[2];
        pc_error e;
        while (*p != '\0') {
            double v[8];
            size_t n = 0;
            while (*p != '\0' && *p != ';') {
                v[n++] = fhex(p);
                p += 16;
                if (*p == ',') {
                    p++;
                }
            }
            CHECK(pc_algo_liquify_stroke_push(&s, v, n, NULL) == PC_OK);
            if (*p == ';') {
                p++;
            }
        }
        if (pc_algo_liquify_stroke_validate(&s, &e) == PC_OK) {
            snprintf(out, 512, "ok");
        } else {
            snprintf(out, 512, "%s", e.msg);
        }
        pc_algo_liquify_stroke_fini(&s);
    } else if (strcmp(k, "autocell") == 0) {
        double c = pc_algo_liquify_auto_cell(rect_of(a[1]));
        uint64_t u;
        memcpy(&u, &c, 8);
        snprintf(out, 512, "%016llx", (unsigned long long)u);
    } else if (strcmp(k, "fieldnew") == 0) {
        pc_algo_liquify_field f;
        uint64_t u;
        CHECK(pc_algo_liquify_field_init(&f, pc_rect_make(-3, 2, 50, 41), fhex(a[1]), NULL) == PC_OK);
        memcpy(&u, &f.cell, 8);
        snprintf(out, 512, "%016llx\t%zu\t%zu", (unsigned long long)u, f.w, f.h);
        pc_algo_liquify_field_fini(&f);
    } else if (strcmp(k, "mode") == 0) {
        pc_algo_puppet_mode m;
        static const char *const N[3] = {"Some(Rigid)", "Some(Normal)", "Some(Distort)"};
        snprintf(out, 512, "%s", pc_algo_puppet_mode_parse(a[1], strlen(a[1]), &m) ? N[m] : "None");
    } else if (strcmp(k, "density") == 0) {
        pc_algo_puppet_density m;
        static const char *const N[3] = {"Some(Fewer)", "Some(Normal)", "Some(More)"};
        snprintf(out, 512, "%s", pc_algo_puppet_density_parse(a[1], strlen(a[1]), &m) ? N[m] : "None");
    } else if (strcmp(k, "identity") == 0) {
        yyjson_doc *d = parse(a[1]);
        pc_algo_puppet_warp w;
        CHECK(pc_algo_puppet_warp_from_json(yyjson_doc_get_root(d), &w, NULL) == PC_OK);
        snprintf(out, 512, "%d", (int)pc_algo_puppet_warp_is_identity(&w));
        pc_algo_puppet_warp_fini(&w);
        yyjson_doc_free(d);
    }
    (void)nf;
    return out;
}

/* Splits line at tabs (in place); returns the field count. */
static size_t split(char *line, char **f, size_t cap)
{
    size_t n = 0;
    f[n++] = line;
    for (; *line != '\0'; line++) {
        if (*line == '\t' && n < cap) {
            *line = '\0';
            f[n++] = line + 1;
        }
    }
    return n;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    char *b;
    long n;
    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, fp);
    b[*len] = '\0';
    fclose(fp);
    return b;
}

static const char *ONLY = NULL;

static void test_warp_parity(void)
{
    size_t glen = 0, lines = 0, bad = 0, skipped = 0;
    char *golden = read_file(GOLDEN, &glen), *save = NULL, *line;
    CHECK(golden != NULL);
    if (golden == NULL) {
        return;
    }
    for (line = strtok_r(golden, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char *f[16], *got = NULL, want[1024];
        size_t nf = split(line, f, 16), resf = nf - 1;
        if (ONLY != NULL && strcmp(f[0], ONLY) != 0) {
            continue;
        }
        if (strcmp(f[0], "field") == 0) {
            got = case_field(f);
        } else if (strcmp(f[0], "segment") == 0) {
            got = case_segment(f);
        } else if (strcmp(f[0], "apply") == 0) {
            got = case_apply(f);
        } else if (strcmp(f[0], "proxy") == 0) {
            got = case_proxy(f);
        } else if (strcmp(f[0], "fullres") == 0) {
            got = case_fullres(f);
        } else if (strcmp(f[0], "json.strokes") == 0) {
            got = json_result(0, f[1]);
        } else if (strcmp(f[0], "json.pins") == 0) {
            got = json_result(1, f[1]);
        } else if (strcmp(f[0], "json.warp") == 0) {
            got = json_result(2, f[1]);
        } else if (strcmp(f[0], "mesh") == 0) {
            got = case_mesh(f);
        } else if (strcmp(f[0], "solve") == 0) {
            got = case_solve(f);
        } else if (strcmp(f[0], "custom") == 0) {
            got = case_custom(f);
        } else if (strcmp(f[0], "warp") == 0) {
            got = case_warp(f);
        } else if (strcmp(f[0], "deform") == 0) {
            got = case_deform(f);
        } else {
            got = case_misc(f, nf);
            /* Multi-field results: tool (3 fields), fieldnew (3). */
            if (strcmp(f[0], "tool") == 0 || strcmp(f[0], "fieldnew") == 0) {
                resf = 2;
            }
        }
        /* The expected text: every field from resf on, re-joined with tabs. */
        {
            size_t i, o = 0;
            want[0] = '\0';
            for (i = resf; i < nf; i++) {
                o += (size_t)snprintf(want + o, sizeof want - o, "%s%s", i > resf ? "\t" : "", f[i]);
                if (o >= sizeof want) {
                    break;
                }
            }
        }
        if (got == NULL) {
            skipped++;
            continue;
        }
        lines++;
        if (strcmp(got, want) != 0) {
            bad++;
            if (bad <= 25) {
                PC_T_FAIL("%s %s %s: got %s, want %s", f[0], nf > 1 ? f[1] : "", nf > 2 ? f[2] : "", got,
                          want);
            }
            if (getenv("PC_PARITY_KINDS") != NULL) {
                fprintf(stderr, "mismatch %s\n", f[0]);
            }
        }
        pc_t_checks++;
        free(got);
    }
    printf("warp parity: %zu lines, %zu mismatches, %zu skipped\n", lines, bad, skipped);
    free(golden);
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        ONLY = argv[2];
    }
    RUN(test_warp_parity);
    return pc_t_finish();
}
