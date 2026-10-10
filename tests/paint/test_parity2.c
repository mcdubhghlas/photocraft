/* Bit-exact parity with the Rust reference for chunk L3: tests/golden/paint2-vectors.txt was
 * written by `rustref paint2-vectors` (tools/rustref/src/paint2_vectors.rs, whose header documents
 * every line kind). Each line is recomputed here and compared field by field. Brushes come first
 * as serde JSON ("brush I JSON"); later lines refer to them by index. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "color/blend.h"
#include "harness.h"
#include "paint/paint.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#define GOLDEN "tests/golden/paint2-vectors.txt"
#define H0 0xcbf29ce484222325ull
#define MAXC PC_RASTER_MAX_CHANNELS
#define NBRUSH 16

/* ---- hashing and formatting (as test_parity.c) ---- */

static void fnv(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}
/* NaNs are canonicalised: which NaN an operation propagates depends on operand order. */
static uint32_t b32(float v)
{
    uint32_t u = 0x7fc00000u;
    if (!isnan(v)) {
        memcpy(&u, &v, 4);
    }
    return u;
}
static void f32b(uint64_t *h, float v)
{
    uint32_t u = b32(v);
    uint8_t b[4];
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(u >> (8 * i));
    }
    fnv(h, b, 4);
}
static float hf32(const char *s)
{
    uint32_t u = (uint32_t)strtoul(s, NULL, 16);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static double hf64(const char *s)
{
    uint64_t u = strtoull(s, NULL, 16);
    double f;
    memcpy(&f, &u, 8);
    return f;
}
static void rect_s(pc_rect r, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32, r.x0, r.y0, r.x1, r.y1);
}
static void hx(uint64_t h, char out[17])
{
    (void)snprintf(out, 17, "%016" PRIx64, h);
}
static void floats_h(const float *v, size_t n, char out[17])
{
    uint64_t h = H0;
    for (size_t i = 0; i < n; i++) {
        f32b(&h, v[i]);
    }
    hx(h, out);
}

static int g_line;
static int g_fail_lines;
#define EXPECT_STR(got, want)                                                                                \
    do {                                                                                                     \
        pc_t_checks++;                                                                                       \
        if (strcmp((got), (want)) != 0) {                                                                    \
            if (g_fail_lines++ < 40) {                                                                       \
                PC_T_FAIL("line %d: got %.300s want %.300s", g_line, (got), (want));                         \
            } else {                                                                                         \
                pc_t_failed_checks++;                                                                        \
            }                                                                                                \
        }                                                                                                    \
    } while (0)

/* ---- inputs ---- */

static size_t split(char *s, char sep, char **f, size_t max)
{
    size_t n = 0;
    f[n++] = s;
    for (char *p = s; *p != '\0'; p++) {
        if (*p == sep && n < max) {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

/* Points "x,y,p,tx,ty,rot,wheel,time;..." in hex bits, or "-". Caller frees. */
static pc_paint_point *parse_points(const char *s, size_t *n)
{
    size_t cap = 1, k = 0;
    pc_paint_point *v;
    const char *p = s;
    *n = 0;
    if (strcmp(s, "-") == 0) {
        return NULL;
    }
    for (const char *c = s; *c; c++) {
        cap += *c == ';';
    }
    v = calloc(cap, sizeof *v);
    while (v != NULL && *p != '\0' && k < cap) {
        char parts[8][17];
        for (int i = 0; i < 8; i++) {
            size_t m = 0;
            while (*p != '\0' && *p != ',' && *p != ';' && m < 16) {
                parts[i][m++] = *p++;
            }
            parts[i][m] = '\0';
            if (*p == ',' || *p == ';') {
                p++;
            }
        }
        v[k].x = hf64(parts[0]);
        v[k].y = hf64(parts[1]);
        v[k].pressure = hf32(parts[2]);
        v[k].tilt_x = hf32(parts[3]);
        v[k].tilt_y = hf32(parts[4]);
        v[k].rotation = hf32(parts[5]);
        v[k].wheel = hf32(parts[6]);
        v[k].time = hf64(parts[7]);
        k++;
    }
    *n = k;
    return v;
}

/* "a,b,c,d" f32 bits. */
static void parse_c4(const char *s, float out[4])
{
    char buf[64];
    char *f[4];
    (void)snprintf(buf, sizeof buf, "%s", s);
    if (split(buf, ',', f, 4) != 4) {
        PC_T_FAIL("line %d: bad colour %s", g_line, s);
        memset(out, 0, 4 * sizeof *out);
        return;
    }
    for (int i = 0; i < 4; i++) {
        out[i] = hf32(f[i]);
    }
}
static void c4_s(const float c[4], char *out, size_t cap)
{
    (void)snprintf(out, cap, "%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32, b32(c[0]), b32(c[1]),
                   b32(c[2]), b32(c[3]));
}

static pc_paint_brush g_brushes[NBRUSH];
static size_t g_nbrush;

static const pc_paint_brush *brush_at(const char *s)
{
    unsigned long i = strtoul(s, NULL, 10);
    if (i >= g_nbrush) {
        PC_T_FAIL("line %d: brush %s not defined", g_line, s);
        return &g_brushes[0];
    }
    return &g_brushes[i];
}

/* A stroke borrowing a table brush (never finalised) and owning parsed points. */
static pc_paint_stroke make_stroke(const char *brush, const char *pts)
{
    pc_paint_stroke st;
    memset(&st, 0, sizeof st);
    st.brush = *brush_at(brush);
    st.points = parse_points(pts, &st.count);
    return st;
}

/* ---- surfaces (paint2_vectors.rs formats / canvas / selection) ---- */

static const pc_pixel_format FORMATS[] = {
    {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true},   {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true},
    {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true},  {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, true},
    {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true},  {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false},
    {PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U16, true},  {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false},
    {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, false}, {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, true},
};
#define NFORMATS (sizeof FORMATS / sizeof FORMATS[0])

static pc_pixel_format fmt_at(const char *s)
{
    unsigned long i = strtoul(s, NULL, 10);
    return FORMATS[i < NFORMATS ? i : 0];
}

static void fill(pc_surface *s, pc_rect r, float c0, float c1, float c2, float c3)
{
    float rgba[4] = {c0, c1, c2, c3}, px[MAXC];
    pc_pixel_format f = pc_surface_format(s);
    size_t n = pc_from_rgba(f, rgba, px);
    CHECK(pc_surface_fill_rect(s, r, px, n, NULL) == PC_OK);
}

static void canvas(pc_pixel_format f, pc_surface *s)
{
    pc_rect r = pc_rect_make(120, 30, 180, 90);
    size_t n = pc_pixel_format_channels(f), k = 0;
    float *data = calloc(60 * 60 * n, sizeof *data), zero[MAXC] = {0};
    pc_surface_init(s, f);
    fill(s, pc_rect_make(-20, -20, 220, 120), 0.2f, 0.6f, 0.2f, 1.0f);
    fill(s, pc_rect_make(100, -20, 220, 120), 0.1f, 0.1f, 0.5f, 1.0f);
    fill(s, pc_rect_make(60, -20, 66, 120), 0.9f, 0.1f, 0.1f, 1.0f);
    CHECK(data != NULL);
    for (int32_t y = r.y0; data != NULL && y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            float u = (float)(x - r.x0), v = (float)(y - r.y0);
            float a = v < 40.0f ? 1.0f : 0.3f + (float)(x % 5) * 0.15f;
            float rgba[4] = {u / 59.0f, v / 59.0f, (float)((x + y) % 7) / 6.0f, a};
            k += pc_from_rgba(f, rgba, data + k);
        }
    }
    if (data != NULL) {
        CHECK(pc_surface_write_region(s, r, data, k, NULL) == PC_OK);
    }
    free(data);
    CHECK(pc_surface_fill_rect(s, pc_rect_make(20, 60, 50, 90), zero, n, NULL) == PC_OK);
}

static void selection(pc_surface *s)
{
    float one = 1.0f, half = 0.5f;
    pc_surface_init(s, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_fill_rect(s, pc_rect_make(0, 0, 100, 200), &one, 1, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(s, pc_rect_make(100, 0, 160, 200), &half, 1, NULL) == PC_OK);
}

static void surf_h(const pc_surface *s, char out[17])
{
    uint8_t *b = NULL;
    size_t n = 0;
    uint64_t h = H0;
    CHECK(pc_surface_to_interleaved(s, pc_rect_make(-40, -40, 240, 140), &b, &n, NULL) == PC_OK);
    if (pc_surface_format(s).sample == PC_SAMPLE_TYPE_F32) {
        for (size_t i = 0; i + 4 <= n; i += 4) {
            float v;
            uint32_t u;
            memcpy(&v, &b[i], 4); /* native byte order = little endian on the reference targets */
            u = b32(v);
            memcpy(&b[i], &u, 4);
        }
    }
    fnv(&h, b, n);
    free(b);
    hx(h, out);
}

/* Target canvas plus optional selection for a line. */
typedef struct setup {
    pc_surface s, sel;
    bool has_sel;
} setup;

static void setup_init(setup *u, const char *fmt, const char *sel)
{
    memset(u, 0, sizeof *u);
    canvas(fmt_at(fmt), &u->s);
    u->has_sel = strcmp(sel, "1") == 0;
    if (u->has_sel) {
        selection(&u->sel);
    }
}
static const pc_surface *setup_sel(const setup *u)
{
    return u->has_sel ? &u->sel : NULL;
}
static void setup_fini(setup *u)
{
    pc_surface_fini(&u->s);
    pc_surface_fini(&u->sel);
}

/* Compare a damage rect and the surface hash. */
static void expect_result(pc_status st, pc_rect d, const pc_surface *s, const char *want_d,
                          const char *want_h)
{
    char got[64], h[17];
    CHECK(st == PC_OK);
    rect_s(d, got, sizeof got);
    EXPECT_STR(got, want_d);
    surf_h(s, h);
    EXPECT_STR(h, want_h);
}

/* ---- footprints, bounds, coverage ---- */

static void do_fpr(char **f)
{
    char buf[128], *p[4], got[64], h[17];
    pc_paint_dab d;
    pc_paint_footprint fp;
    (void)snprintf(buf, sizeof buf, "%s", f[1]);
    if (split(buf, ',', p, 4) != 4) {
        PC_T_FAIL("line %d: bad dab", g_line);
        return;
    }
    d = pc_paint_dab_round((pc_point){hf64(p[0]), hf64(p[1])}, hf32(p[2]), hf32(p[3]));
    if (pc_paint_footprint_round(&d, 0, hf32(f[2]), &fp, NULL) != PC_OK) {
        PC_T_FAIL("line %d: footprint failed", g_line);
        return;
    }
    rect_s(fp.rect, got, sizeof got);
    EXPECT_STR(got, f[3]);
    floats_h(fp.cov, fp.len, h);
    EXPECT_STR(h, f[4]);
    pc_paint_footprint_fini(&fp);
}

static void do_sbnd(char **f)
{
    pc_paint_stroke st = make_stroke(f[1], f[2]);
    pc_paint_dab_vec ds = {0};
    char got[64];
    CHECK(pc_paint_dabs(&st.brush, st.points, st.count, &ds, NULL) == PC_OK);
    rect_s(pc_paint_stroke_bounds(ds.v, ds.len), got, sizeof got);
    EXPECT_STR(got, f[3]);
    pc_paint_dab_vec_fini(&ds);
    free(st.points);
}

static void do_cov(char **f)
{
    pc_paint_stroke st = make_stroke(f[1], f[2]);
    pc_rect r;
    float *cov = NULL;
    size_t n = 0;
    char got[64], h[17];
    if (pc_paint_stroke_coverage(&st, &r, &cov, &n, NULL) == PC_OK) {
        rect_s(r, got, sizeof got);
        EXPECT_STR(got, f[3]);
        floats_h(cov, n, h);
        EXPECT_STR(h, f[4]);
    } else {
        PC_T_FAIL("line %d: stroke_coverage failed", g_line);
    }
    free(cov);
    free(st.points);
}

static int64_t rem_euclid(int64_t a, int64_t m)
{
    int64_t r = a % m;
    return r < 0 ? r + m : r;
}

static pc_blend_mode blend_of(const char *s)
{
    for (int i = 0; i < PC_BLEND_COUNT; i++) {
        const char *nm = pc_blend_name((pc_blend_mode)i);
        if (nm != NULL && strcmp(nm, s) == 0) {
            return (pc_blend_mode)i;
        }
    }
    PC_T_FAIL("line %d: unknown blend %s", g_line, s);
    return PC_BLEND_NORMAL;
}

/* acov FMT SEL LOCK BRUSH PTS OPAC BLEND DMG HASH */
static void do_acov(char **f)
{
    setup u;
    pc_paint_stroke st = make_stroke(f[4], f[5]);
    pc_rect r, d = PC_RECT_EMPTY;
    float *cov = NULL;
    size_t n = 0;
    pc_paint_region paint = {0};
    pc_pixel_format fmt;
    pc_status s;
    setup_init(&u, f[1], f[2]);
    fmt = pc_surface_format(&u.s);
    CHECK(pc_paint_stroke_coverage(&st, &r, &cov, &n, NULL) == PC_OK);
    CHECK(pc_paint_region_new(r, pc_pixel_format_channels(fmt), &paint, NULL) == PC_OK);
    {
        size_t i = 0, ch = paint.ch;
        for (int32_t y = r.y0; y < r.y1; y++) {
            for (int32_t x = r.x0; x < r.x1; x++) {
                for (size_t c = 0; c < ch && i < paint.len; c++) {
                    paint.data[i++] =
                        fmt.alpha && c == ch - 1
                            ? 0.25f + (float)rem_euclid((int64_t)x + y, 4) * 0.25f
                            : (float)rem_euclid((int64_t)x * 7 + (int64_t)y * 13 + (int64_t)c * 5, 17) /
                                  16.0f;
                }
            }
        }
    }
    s = pc_paint_apply_coverage(&u.s, r, cov, n, hf32(f[6]), setup_sel(&u), strcmp(f[3], "1") == 0, &paint,
                                blend_of(f[7]), &d, NULL);
    expect_result(s, d, &u.s, f[8], f[9]);
    pc_paint_region_fini(&paint);
    free(cov);
    free(st.points);
    setup_fini(&u);
}

static void do_mixpm(char **f)
{
    static const float vals[9] = {0.0f, 1.0f, 0.5f, 0.25f, 1e-7f, 0.9f, NAN, -0.5f, 2.0f};
    static const float ks[5] = {0.0f, 0.3f, 1.0f, NAN, 1.5f};
    static const size_t as[6] = {0, 1, 2, 3, 4, 7};
    uint64_t h = H0;
    char got[17];
    for (int ai = 0; ai < 6; ai++) {
        for (size_t i = 0; i < 9; i++) {
            for (size_t j = 0; j < 9; j++) {
                for (int k = 0; k < 5; k++) {
                    float d[4] = {vals[i], vals[j], vals[(i + j) % 9], vals[(i * 2 + 1) % 9]};
                    float s[5] = {vals[j], vals[(j + 3) % 9], vals[i], vals[(i + 5) % 9], 0.4f};
                    pc_paint_mix_premultiplied(d, 4, s, 5, as[ai], ks[k]);
                    for (int c = 0; c < 4; c++) {
                        f32b(&h, d[c]);
                    }
                }
            }
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[1]);
}

/* ---- sequential strokes ---- */

static pc_status test_effect(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp, pc_error *err)
{
    (void)ctx;
    (void)err;
    for (int32_t y = fp->rect.y0; y < fp->rect.y1; y++) {
        for (int32_t x = fp->rect.x0; x < fp->rect.x1; x++) {
            float k = pc_paint_footprint_at(fp, x, y), *px;
            if ((px = pc_paint_region_px(work, x, y)) == NULL || k <= 0.0f) {
                continue;
            }
            for (size_t c = 0; c < work->ch; c++) {
                px[c] += ((float)((fp->index + c) % 4) / 3.0f - px[c]) * k * 0.5f;
            }
        }
    }
    return PC_OK;
}

/* dstk FMT SEL LOCK BRUSH PTS HALO DMG HASH */
static void do_dstk(char **f)
{
    setup u;
    pc_paint_stroke st = make_stroke(f[4], f[5]);
    pc_rect d = PC_RECT_EMPTY;
    pc_status s;
    setup_init(&u, f[1], f[2]);
    s = pc_paint_apply_dab_stroke(&u.s, &st, setup_sel(&u), strcmp(f[3], "1") == 0,
                                  (int32_t)strtol(f[6], NULL, 10), test_effect, NULL, &d, NULL);
    expect_result(s, d, &u.s, f[7], f[8]);
    free(st.points);
    setup_fini(&u);
}

/* smudge FMT SEL BRUSH PTS STR FINGER MAX ALPHA DMG HASH */
static void do_smudge(char **f)
{
    setup u;
    pc_paint_stroke st = make_stroke(f[3], f[4]);
    pc_rect d = PC_RECT_EMPTY;
    pc_paint_smudge sm;
    float finger[8];
    size_t nfinger = 0;
    pc_status s;
    setup_init(&u, f[1], f[2]);
    if (strcmp(f[6], "-") != 0) {
        char buf[128], *p[8];
        (void)snprintf(buf, sizeof buf, "%s", f[6]);
        nfinger = split(buf, ',', p, 8);
        for (size_t i = 0; i < nfinger; i++) {
            finger[i] = hf32(p[i]);
        }
    }
    CHECK(pc_paint_smudge_init(&sm, hf32(f[5]), strcmp(f[6], "-") != 0 ? finger : NULL, nfinger, hf32(f[7]),
                               NULL) == PC_OK);
    if (strcmp(f[8], "-") != 0) {
        pc_paint_smudge_with_alpha(&sm, true, (size_t)strtoul(f[8], NULL, 10));
    }
    s = pc_paint_apply_dab_stroke(&u.s, &st, setup_sel(&u), false, 0, pc_paint_smudge_effect, &sm, &d, NULL);
    expect_result(s, d, &u.s, f[9], f[10]);
    pc_paint_smudge_fini(&sm);
    free(st.points);
    setup_fini(&u);
}

/* ---- tools ---- */

static void state_parse(const char *s, pc_paint_mixer_state *st)
{
    char buf[256], *p[3];
    (void)snprintf(buf, sizeof buf, "%s", s);
    pc_paint_mixer_state_default(st);
    if (split(buf, '/', p, 3) != 3) {
        PC_T_FAIL("line %d: bad mixer state", g_line);
        return;
    }
    st->has_reservoir = strcmp(p[0], "-") != 0;
    if (st->has_reservoir) {
        parse_c4(p[0], st->reservoir);
    }
    st->amount = hf32(p[1]);
    st->has_pickup = strcmp(p[2], "-") != 0;
    if (st->has_pickup) {
        parse_c4(p[2], st->pickup);
    }
}

static void state_s(const pc_paint_mixer_state *st, char *out, size_t cap)
{
    char r[64] = "-", p[64] = "-";
    if (st->has_reservoir) {
        c4_s(st->reservoir, r, sizeof r);
    }
    if (st->has_pickup) {
        c4_s(st->pickup, p, sizeof p);
    }
    (void)snprintf(out, cap, "%s/%08" PRIx32 "/%s", r, b32(st->amount), p);
}

/* mixer FMT SEL LOCK SAMPLE BRUSH PTS SET STATE DMG HASH STATE' */
static void do_mixer(char **f)
{
    setup u;
    pc_paint_stroke st = make_stroke(f[5], f[6]);
    pc_rect d = PC_RECT_EMPTY;
    pc_paint_mixer_settings m;
    pc_paint_mixer_state ms;
    pc_surface sample;
    bool has_sample = strcmp(f[4], "-") != 0;
    char buf[128], *p[4], got[256];
    pc_status s;
    setup_init(&u, f[1], f[2]);
    pc_paint_mixer_default(&m);
    (void)snprintf(buf, sizeof buf, "%s", f[7]);
    if (split(buf, '/', p, 4) == 4) {
        m.wet = hf32(p[0]);
        m.load = hf32(p[1]);
        m.mix = hf32(p[2]);
        m.flow = hf32(p[3]);
    }
    state_parse(f[8], &ms);
    if (has_sample) {
        canvas(fmt_at(f[4]), &sample);
    }
    s = pc_paint_apply_mixer_stroke(&u.s, has_sample ? &sample : NULL, &st, &m, &ms, setup_sel(&u),
                                    strcmp(f[3], "1") == 0, &d, NULL);
    expect_result(s, d, &u.s, f[9], f[10]);
    state_s(&ms, got, sizeof got);
    EXPECT_STR(got, f[11]);
    if (has_sample) {
        pc_surface_fini(&sample);
    }
    free(st.points);
    setup_fini(&u);
}

/* SET = mode/sampling/limits/tol/aa/color/bg */
static void repl_parse(const char *set, pc_paint_replace_settings *rs_out)
{
    pc_paint_replace_settings rs;
    char buf[256], *p[7];
    pc_paint_replace_settings_default(&rs);
    (void)snprintf(buf, sizeof buf, "%s", set);
    if (split(buf, '/', p, 7) == 7) {
        CHECK(pc_paint_replace_mode_parse(p[0], strlen(p[0]), &rs.mode));
        CHECK(pc_paint_sampling_parse(p[1], strlen(p[1]), &rs.sampling));
        CHECK(pc_paint_limits_parse(p[2], strlen(p[2]), &rs.limits));
        rs.tolerance = hf32(p[3]);
        rs.anti_alias = strcmp(p[4], "1") == 0;
        parse_c4(p[5], rs.color);
        parse_c4(p[6], rs.background);
    } else {
        PC_T_FAIL("line %d: bad replace settings", g_line);
    }
    *rs_out = rs;
}

/* repl FMT SEL LOCK BRUSH PTS SET DMG HASH */
static void do_repl(char **f)
{
    setup u;
    pc_paint_stroke st = make_stroke(f[4], f[5]);
    pc_rect d = PC_RECT_EMPTY;
    pc_paint_replace_settings rs;
    pc_status s;
    setup_init(&u, f[1], f[2]);
    repl_parse(f[6], &rs);
    s = pc_paint_apply_color_replacement(&u.s, &st, &rs, setup_sel(&u), strcmp(f[3], "1") == 0, &d, NULL);
    expect_result(s, d, &u.s, f[7], f[8]);
    free(st.points);
    setup_fini(&u);
}

/* SET = sampling/limits/tol/pf/fg/bg */
static void bg_parse(const char *set, pc_paint_bg_erase_settings *bs_out)
{
    pc_paint_bg_erase_settings bs;
    char buf[256], *p[6];
    pc_paint_bg_erase_settings_default(&bs);
    (void)snprintf(buf, sizeof buf, "%s", set);
    if (split(buf, '/', p, 6) == 6) {
        CHECK(pc_paint_sampling_parse(p[0], strlen(p[0]), &bs.sampling));
        CHECK(pc_paint_limits_parse(p[1], strlen(p[1]), &bs.limits));
        bs.tolerance = hf32(p[2]);
        bs.protect_foreground = strcmp(p[3], "1") == 0;
        parse_c4(p[4], bs.foreground);
        parse_c4(p[5], bs.background);
    } else {
        PC_T_FAIL("line %d: bad eraser settings", g_line);
    }
    *bs_out = bs;
}

/* bger FMT SEL BRUSH PTS SET DMG HASH */
static void do_bger(char **f)
{
    setup u;
    pc_paint_stroke st = make_stroke(f[3], f[4]);
    pc_rect d = PC_RECT_EMPTY;
    pc_paint_bg_erase_settings bs;
    pc_status s;
    setup_init(&u, f[1], f[2]);
    bg_parse(f[5], &bs);
    s = pc_paint_apply_background_eraser(&u.s, &st, &bs, setup_sel(&u), &d, NULL);
    expect_result(s, d, &u.s, f[6], f[7]);
    free(st.points);
    setup_fini(&u);
}

/* ---- serde ---- */

static pc_paint_json_type type_of(const char *s)
{
    static const struct {
        const char *name;
        pc_paint_json_type t;
    } T[] = {
        {"ReplaceMode", PC_PAINT_JSON_REPLACE_MODE},
        {"Sampling", PC_PAINT_JSON_SAMPLING},
        {"Limits", PC_PAINT_JSON_LIMITS},
        {"ReplaceSettings", PC_PAINT_JSON_REPLACE_SETTINGS},
        {"BgEraseSettings", PC_PAINT_JSON_BG_ERASE_SETTINGS},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (strcmp(T[i].name, s) == 0) {
            return T[i].t;
        }
    }
    PC_T_FAIL("line %d: unknown type %s", g_line, s);
    return PC_PAINT_JSON_TYPE_COUNT;
}

static void do_serde(char **f)
{
    pc_paint_json_type t = type_of(f[1]);
    void *v = t == PC_PAINT_JSON_TYPE_COUNT ? NULL : calloc(1, pc_paint_json_size(t) + 1);
    pc_buf js = PC_BUF_INIT;
    pc_error err;
    bool de = strcmp(f[0], "de") == 0;
    if (v == NULL) {
        CHECK(0);
        return;
    }
    if (pc_paint_json_from_string(t, f[2], strlen(f[2]), v, &err) == PC_OK) {
        CHECK(pc_paint_json_to_string(t, v, 0, &js, NULL) == PC_OK);
        EXPECT_STR(pc_buf_cstr(&js), de ? f[3] : f[2]);
        pc_paint_json_fini(t, v);
    } else {
        char msg[300];
        (void)snprintf(msg, sizeof msg, "ERR %s", err.msg);
        EXPECT_STR(msg, de ? f[3] : "(ok)");
    }
    pc_buf_fini(&js);
    free(v);
}

/* serr / serb SET JSON: serialise settings that can't round-trip through JSON (NaN). */
static void do_ser_set(char **f)
{
    pc_buf js = PC_BUF_INIT;
    if (strcmp(f[0], "serr") == 0) {
        pc_paint_replace_settings rs;
        repl_parse(f[1], &rs);
        CHECK(pc_paint_json_to_string(PC_PAINT_JSON_REPLACE_SETTINGS, &rs, 0, &js, NULL) == PC_OK);
    } else {
        pc_paint_bg_erase_settings bs;
        bg_parse(f[1], &bs);
        CHECK(pc_paint_json_to_string(PC_PAINT_JSON_BG_ERASE_SETTINGS, &bs, 0, &js, NULL) == PC_OK);
    }
    EXPECT_STR(pc_buf_cstr(&js), f[2]);
    pc_buf_fini(&js);
}

/* ---- driver ---- */

static void test_parity2_lines(void)
{
    FILE *fp = fopen(GOLDEN, "rb");
    char *buf, *line, *save = NULL;
    long size;
    int lines = 0;
    if (fp == NULL) {
        PC_T_FAIL("cannot open %s (run the tests from the repository root)", GOLDEN);
        return;
    }
    (void)fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    buf = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (buf == NULL || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        PC_T_FAIL("cannot read %s", GOLDEN);
        free(buf);
        (void)fclose(fp);
        return;
    }
    (void)fclose(fp);
    buf[size] = '\0';
    for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char *f[16];
        size_t nf = split(line, '\t', f, 16);
        lines++;
        g_line = lines;
        if (strcmp(f[0], "brush") == 0 && nf == 3) {
            pc_error err;
            if (g_nbrush >= NBRUSH || pc_paint_json_from_string(PC_PAINT_JSON_BRUSH, f[2], strlen(f[2]),
                                                                &g_brushes[g_nbrush], &err) != PC_OK) {
                PC_T_FAIL("line %d: brush", lines);
            } else {
                g_nbrush++;
            }
        } else if (strcmp(f[0], "fpr") == 0 && nf == 5) {
            do_fpr(f);
        } else if (strcmp(f[0], "sbnd") == 0 && nf == 4) {
            do_sbnd(f);
        } else if (strcmp(f[0], "cov") == 0 && nf == 5) {
            do_cov(f);
        } else if (strcmp(f[0], "acov") == 0 && nf == 10) {
            do_acov(f);
        } else if (strcmp(f[0], "mixpm") == 0 && nf == 2) {
            do_mixpm(f);
        } else if (strcmp(f[0], "dstk") == 0 && nf == 9) {
            do_dstk(f);
        } else if (strcmp(f[0], "smudge") == 0 && nf == 11) {
            do_smudge(f);
        } else if (strcmp(f[0], "mixer") == 0 && nf == 12) {
            do_mixer(f);
        } else if (strcmp(f[0], "repl") == 0 && nf == 9) {
            do_repl(f);
        } else if (strcmp(f[0], "bger") == 0 && nf == 8) {
            do_bger(f);
        } else if ((strcmp(f[0], "serr") == 0 || strcmp(f[0], "serb") == 0) && nf == 3) {
            do_ser_set(f);
        } else if ((strcmp(f[0], "ser") == 0 && nf == 3) || (strcmp(f[0], "de") == 0 && nf == 4)) {
            do_serde(f);
        } else {
            PC_T_FAIL("line %d: unknown kind %s (%zu fields)", lines, f[0], nf);
        }
    }
    for (size_t i = 0; i < g_nbrush; i++) {
        pc_paint_brush_fini(&g_brushes[i]);
    }
    free(buf);
    CHECK(lines > 1500);
    (void)printf("paint2 parity: %d lines, %d mismatching checks\n", lines, g_fail_lines);
}

int main(int argc, char **argv)
{
    RUN(test_parity2_lines);
    return pc_t_finish();
}
