/* Bit-exact parity with the Rust reference: tests/golden/paint-vectors.txt was written by
 * `rustref paint-vectors` (tools/rustref/src/paint_vectors.rs, whose header documents every line
 * kind). Each line is recomputed here and compared field by field. Brushes travel as their serde
 * JSON (so the reader is exercised too); non-finite values that JSON can't carry come as
 * "name=f32bits" overrides; points as hex bits. Error messages compare on pc_error's 255 bytes. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/json.h"
#include "base/rmath.h"
#include "paint/pmath.h"
#include "harness.h"
#include "paint/paint.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#define GOLDEN "tests/golden/paint-vectors.txt"
#define H0 0xcbf29ce484222325ull

/* ---- hashing and formatting ---- */

static void fnv(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}
static void u64le(uint64_t *h, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    fnv(h, b, 8);
}
/* NaNs are canonicalised (see paint_vectors.rs): which NaN an operation propagates depends on
 * operand order, which compilers choose freely. */
static uint32_t b32(float v)
{
    uint32_t u = 0x7fc00000u;
    if (!isnan(v)) {
        memcpy(&u, &v, 4);
    }
    return u;
}
static uint64_t b64(double v)
{
    uint64_t u = 0x7ff8000000000000ull;
    if (!isnan(v)) {
        memcpy(&u, &v, 8);
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
static void f64b(uint64_t *h, double v)
{
    u64le(h, b64(v));
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
static void x32(float v, char out[9])
{
    (void)snprintf(out, 9, "%08" PRIx32, b32(v));
}
static void x64(double v, char out[17])
{
    (void)snprintf(out, 17, "%016" PRIx64, b64(v));
}
static void rect_s(pc_rect r, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32, r.x0, r.y0, r.x1, r.y1);
}
static void hx(uint64_t h, char out[17])
{
    (void)snprintf(out, 17, "%016" PRIx64, h);
}

/* Line bookkeeping: on a mismatch report the golden line number. */
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

static size_t split(char *s, char **f, size_t max)
{
    size_t n = 0;
    f[n++] = s;
    for (char *p = s; *p != '\0'; p++) {
        if (*p == '\t' && n < max) {
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
        char buf[17];
        char parts[8][17];
        for (int i = 0; i < 8; i++) {
            size_t m = 0;
            while (*p != '\0' && *p != ',' && *p != ';' && m < 16) {
                buf[m++] = *p++;
            }
            buf[m] = '\0';
            memcpy(parts[i], buf, m + 1);
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

static bool parse_brush(const char *json, pc_paint_brush *b)
{
    pc_error err;
    if (pc_paint_json_from_string(PC_PAINT_JSON_BRUSH, json, strlen(json), b, &err) != PC_OK) {
        PC_T_FAIL("line %d: brush JSON: %s", g_line, err.msg);
        return false;
    }
    return true;
}

static void apply_ovr(pc_paint_brush *b, const char *s)
{
    char buf[512];
    char *save = NULL;
    if (strcmp(s, "-") == 0) {
        return;
    }
    (void)snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok_r(buf, ",", &save); t != NULL; t = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(t, '=');
        float v;
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        v = hf32(eq + 1);
        if (strcmp(t, "size") == 0) {
            b->size = v;
        } else if (strcmp(t, "dual.size") == 0) {
            b->dual_brush.size = v;
        } else if (strcmp(t, "dual.enabled") == 0) {
            b->dual_brush.enabled = v != 0.0f;
        } else if (strcmp(t, "scatter") == 0) {
            b->scattering.scatter.jitter = v;
        } else if (strcmp(t, "scatter.enabled") == 0) {
            b->scattering.enabled = v != 0.0f;
        } else if (strcmp(t, "dual.scatter") == 0) {
            b->dual_brush.scatter = v;
        } else if (strcmp(t, "spacing") == 0) {
            b->spacing = v;
        } else if (strcmp(t, "rate") == 0) {
            b->build_up_rate = v;
        } else if (strcmp(t, "buildup") == 0) {
            b->build_up = v != 0.0f;
        } else if (strcmp(t, "hardness") == 0) {
            b->hardness = v;
        } else if (strcmp(t, "roundness") == 0) {
            b->roundness = v;
        } else if (strcmp(t, "opacity") == 0) {
            b->opacity = v;
        } else if (strcmp(t, "flow") == 0) {
            b->flow = v;
        } else if (strcmp(t, "angle") == 0) {
            b->angle = v;
        } else if (strcmp(t, "smooth") == 0) {
            b->smoothing.amount = v;
        } else if (strcmp(t, "pulled") == 0) {
            b->smoothing.pulled_string = v != 0.0f;
        } else if (strcmp(t, "color0") == 0) {
            b->color[0] = v;
        } else if (strcmp(t, "color3") == 0) {
            b->color[3] = v;
        } else {
            PC_T_FAIL("line %d: unknown override %s", g_line, t);
        }
    }
}

static void gtile(uint32_t w, uint32_t h, uint64_t seed, pc_paint_gray_tile *out)
{
    size_t n = (size_t)w * h;
    uint16_t *d = calloc(n > 0 ? n : 1, sizeof *d);
    CHECK(d != NULL);
    for (size_t i = 0; d != NULL && i < n; i++) {
        d[i] = (uint16_t)(pc_paint_mix64(seed * 1000u + i) >> 48);
    }
    CHECK(pc_paint_gray_tile_from_u16(w, h, d, n, out, NULL) == PC_OK);
    free(d);
}

/* ---- scenario surfaces (paint_vectors.rs `scenario`) ---- */

typedef struct scen {
    pc_surface s;
    pc_surface sel;
    bool has_sel;
    bool lock;
} scen;

static void prefill(pc_surface *s, pc_pixel_format f, pc_rect r, const float c[4])
{
    float px[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_from_rgba(f, c, px);
    pc_surface_init(s, f);
    CHECK(pc_surface_fill_rect(s, r, px, n, NULL) == PC_OK);
}

static void scenario(int i, scen *o)
{
    static const float white[4] = {1, 1, 1, 1}, blue[4] = {0.2f, 0.6f, 0.9f, 1.0f},
                       grey[4] = {0.5f, 0.5f, 0.5f, 0.8f};
    float px[PC_RASTER_MAX_CHANNELS];
    pc_pixel_format f;
    memset(o, 0, sizeof *o);
    switch (i) {
    case 0:
        pc_surface_init(&o->s, PC_PIXEL_FORMAT_RGBA8);
        break;
    case 1:
        pc_surface_init(&o->s, PC_PIXEL_FORMAT_RGBA16);
        break;
    case 2:
        pc_surface_init(&o->s, PC_PIXEL_FORMAT_RGBA32F);
        break;
    case 3:
        pc_surface_init(&o->s, PC_PIXEL_FORMAT_GRAYA8);
        break;
    case 4:
        pc_surface_init(&o->s, PC_PIXEL_FORMAT_CMYKA8);
        break;
    case 5:
        f = (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false};
        pc_surface_init_default(&o->s, f, px, pc_from_rgba(f, white, px));
        break;
    case 6:
        pc_surface_init(&o->s, (pc_pixel_format){PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U16, true});
        break;
    case 7:
        prefill(&o->s, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 120, 60), blue);
        o->lock = true;
        break;
    case 8:
        prefill(&o->s, PC_PIXEL_FORMAT_RGBA32F, pc_rect_make(-50, -50, 350, 200), grey);
        break;
    case 9:
        pc_surface_init(&o->s, PC_PIXEL_FORMAT_RGBA8);
        o->has_sel = true;
        break;
    case 10:
        px[0] = 0.5f;
        pc_surface_init_default(&o->s, (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false},
                                px, 1);
        break;
    case 11:
        f = (pc_pixel_format){PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, false};
        pc_surface_init_default(&o->s, f, px, pc_from_rgba(f, white, px));
        break;
    default:
        prefill(&o->s, PC_PIXEL_FORMAT_RGBA16, pc_rect_make(0, 0, 120, 60), blue);
        o->has_sel = true;
        break;
    }
    if (o->has_sel) {
        float one = 1.0f, half = 0.5f;
        pc_surface_init(&o->sel, PC_PIXEL_FORMAT_GRAY8);
        CHECK(pc_surface_fill_rect(&o->sel, pc_rect_make(0, 0, 100, 200), &one, 1, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&o->sel, pc_rect_make(100, 0, 160, 200), &half, 1, NULL) == PC_OK);
    }
}

static void scen_fini(scen *o)
{
    pc_surface_fini(&o->s);
    pc_surface_fini(&o->sel);
}

static uint64_t surf_h(const pc_surface *s)
{
    uint8_t *b = NULL;
    size_t n = 0;
    uint64_t h = H0;
    CHECK(pc_surface_to_interleaved(s, pc_rect_make(-40, -40, 360, 200), &b, &n, NULL) == PC_OK);
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
    return h;
}

static void dab_h(uint64_t *h, const pc_paint_dab *d)
{
    uint8_t fl[2] = {d->flip_x, d->flip_y};
    f64b(h, d->center.x);
    f64b(h, d->center.y);
    f32b(h, d->radius);
    f32b(h, d->alpha);
    f32b(h, d->angle);
    f32b(h, d->roundness);
    fnv(h, fl, 2);
    f32b(h, d->opacity);
    for (int i = 0; i < 4; i++) {
        f32b(h, d->color[i]);
    }
    f32b(h, d->depth);
    u64le(h, d->index);
    f32b(h, d->proj_angle);
    f32b(h, d->proj_scale);
    f32b(h, d->wet);
    f32b(h, d->mix);
}

#include "parity_cases.h"

int main(int argc, char **argv)
{
    RUN(test_parity_lines);
    return pc_t_finish();
}
