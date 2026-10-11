/* Bit-exact parity with the L9 part of crates/algo (Camera Raw, HDR, lens, perspective warp,
 * vanishing point, adaptive wide angle incl. apply, Relight): replays `rustref algo-photo-vectors`
 * (tools/rustref/src/algo_photo_vectors.rs) and compares every line of
 * tests/golden/algo-photo-vectors.txt. Settings with serde (Camera Raw, WideAngle, FilterParams)
 * are read back from the golden lines themselves; the other tables are mirrored here. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "base/rmath.h"
#include "base/buf.h"
#include "harness.h"
#include "raster/pixel.h"

#define GOLDEN "tests/golden/algo-photo-vectors.txt"
#define H0 0xcbf29ce484222325ull

static pc_buf OUT;
static char **G;
static size_t NG;

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
static uint64_t hs(const pc_surface *s)
{
    uint64_t h = H0;
    pc_rect cb = pc_surface_content_bounds(s);
    float *v = NULL, def[PC_RASTER_MAX_CHANNELS];
    size_t len = 0;
    uint8_t tail[2];
    hi32(&h, cb.x0), hi32(&h, cb.y0), hi32(&h, cb.x1), hi32(&h, cb.y1);
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
static uint64_t hpx(const float (*v)[4], size_t n)
{
    uint64_t h = H0;
    size_t i;
    for (i = 0; i < n; i++) {
        hf(&h, v[i], 4);
    }
    return h;
}

static float pat(int64_t x, int64_t y, int64_t c, int64_t seed)
{
    int64_t v = (x * 7 + y * 13 + c * 29 + seed * 31) % 97;
    return (float)(v < 0 ? v + 97 : v) / 96.0f;
}
static float (*photo(size_t w, size_t h, int64_t seed))[4]
{
    float (*px)[4] = malloc(w * h * sizeof *px + 16);
    size_t i;
    int c;
    for (i = 0; i < w * h; i++) {
        int64_t x = (int64_t)(i % w), y = (int64_t)(i / w);
        for (c = 0; c < 3; c++) {
            px[i][c] = 0.15f + 0.6f * ((float)x / (float)w) * (0.5f + 0.5f * ((float)c / 2.0f)) +
                       0.25f * pat(x, y, c, seed) * ((float)y / (float)h);
        }
        px[i][3] = 1.0f;
    }
    return px;
}
static void surf(pc_surface *s, pc_pixel_format fmt, pc_rect r, int64_t seed, bool opaque)
{
    size_t n = pc_pixel_format_channels(fmt), k = 0, c;
    size_t len = (size_t)pc_rect_width(r) * pc_rect_height(r) * n;
    float *v = malloc(len * sizeof(float) + 1);
    int32_t x, y;
    pc_surface_init(s, fmt);
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            for (c = 0; c < n; c++) {
                int64_t a = ((int64_t)x * 3 + (int64_t)y * 5 + seed) % 7;
                float av = (float)(a < 0 ? a + 7 : a) / 6.0f;
                v[k++] = fmt.alpha && c == n - 1 ? (opaque ? 1.0f : av) : pat(x, y, (int64_t)c, seed);
            }
        }
    }
    CHECK(pc_surface_write_region(s, r, v, len, NULL) == PC_OK);
    free(v);
}

static const struct {
    const char *name;
    pc_pixel_format f;
} FORMATS[6] = {
    {"rgba8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true}},
    {"rgb8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false}},
    {"rgba32", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}},
    {"gray16", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false}},
    {"cmyka8", {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true}},
    {"laba16", {PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U16, true}},
};

static pc_pixel_format format_named(const char *name)
{
    size_t i;
    for (i = 0; i < 6; i++) {
        if (strcmp(FORMATS[i].name, name) == 0) {
            return FORMATS[i].f;
        }
    }
    return PC_PIXEL_FORMAT_RGBA8;
}

/* Splits a copy of golden line g into space-separated tokens (at most cap). */
static size_t tokens(const char *g, char *buf, size_t bufcap, char **tok, size_t cap)
{
    size_t n = 0;
    char *save = NULL, *t;
    (void)snprintf(buf, bufcap, "%s", g);
    for (t = strtok_r(buf, " ", &save); t != NULL && n < cap; t = strtok_r(NULL, " ", &save)) {
        tok[n++] = t;
    }
    return n;
}

static bool is_key(const char *g, const char *key)
{
    size_t n = strlen(key);
    return strncmp(g, key, n) == 0 && g[n] == ' ';
}

static yyjson_doc *jparse(const char *s)
{
    return yyjson_read(s, strlen(s), 0);
}

/* ---------------------------------------------------------------------------------------------- */

static void camera_raw_vectors(void)
{
    size_t gi;
    uint64_t h = H0;
    float k = -400.0f, out[8], wb[3];
    for (gi = 0; gi < NG; gi++) {
        char buf[8192], *t[8];
        yyjson_doc *d;
        yyjson_val *a, *e;
        yyjson_arr_iter it;
        float (*pts)[2], lut[1024], lut2[2];
        size_t n = 0, cnt;
        pc_error err;
        char msg[300];
        if (!is_key(G[gi], "crlut")) {
            continue;
        }
        cnt = tokens(G[gi], buf, sizeof buf, t, 8);
        CHECK(cnt == 4);
        d = jparse(t[1]);
        a = yyjson_doc_get_root(d);
        pts = malloc(sizeof *pts * (yyjson_arr_size(a) + 1));
        yyjson_arr_iter_init(a, &it);
        while ((e = yyjson_arr_iter_next(&it)) != NULL) {
            pts[n][0] = (float)yyjson_get_num(yyjson_arr_get(e, 0));
            pts[n][1] = (float)yyjson_get_num(yyjson_arr_get(e, 1));
            n++;
        }
        h = H0;
        CHECK(pc_algo_cr_curve_lut((const float (*)[2])pts, n, lut, 1024, NULL) == PC_OK);
        CHECK(pc_algo_cr_curve_lut((const float (*)[2])pts, n, lut2, 2, NULL) == PC_OK);
        hf(&h, lut, 1024);
        hf(&h, lut2, 2);
        if (pc_algo_cr_validate_curve((const float (*)[2])pts, n, &err) == PC_OK) {
            strcpy(msg, "ok");
        } else {
            char *p;
            (void)snprintf(msg, sizeof msg, "%s", err.msg);
            for (p = msg; *p; p++) {
                *p = *p == ' ' ? '_' : *p;
            }
        }
        line("crlut %s %s %016llx", t[1], msg, (unsigned long long)h);
        free(pts);
        yyjson_doc_free(d);
    }
    h = H0;
    while (k < 800.0f) {
        pc_algo_cr_band_weights(k, out);
        hf(&h, out, 8);
        pc_algo_cr_white_balance_gains(k / 4.0f, -k / 7.0f, wb);
        hf(&h, wb, 3);
        k += 0.37f;
    }
    pc_algo_cr_band_weights(NAN, out);
    hf(&h, out, 8);
    line("crband %016llx", (unsigned long long)h);
    for (gi = 0; gi < NG; gi++) {
        char buf[8192], *t[8];
        size_t w, hh, i;
        int fl;
        yyjson_doc *d;
        pc_algo_camera_raw p;
        float (*px)[4];
        if (!is_key(G[gi], "cr")) {
            continue;
        }
        CHECK(tokens(G[gi], buf, sizeof buf, t, 8) == 6);
        w = (size_t)atol(t[1]);
        hh = (size_t)atol(t[2]);
        fl = atoi(t[3]);
        d = jparse(t[4]);
        CHECK(pc_algo_camera_raw_from_json(yyjson_doc_get_root(d), &p, NULL) == PC_OK);
        px = photo(w, hh, 3);
        if (fl) {
            for (i = 0; i < w * hh; i++) {
                px[i][0] *= 1.0f + (float)(i % 5) * 0.3f;
            }
        }
        CHECK(pc_algo_camera_raw_develop(px, w * hh, w, hh, &p, fl != 0, NULL) == PC_OK);
        line("cr %zu %zu %d %s %016llx", w, hh, fl, t[4],
             (unsigned long long)hpx((const float (*)[4])px, w * hh));
        free(px);
        pc_algo_camera_raw_fini(&p);
        yyjson_doc_free(d);
    }
    for (gi = 0; gi < NG; gi++) {
        yyjson_doc *d;
        yyjson_mut_doc *md;
        pc_algo_camera_raw p;
        char *text;
        if (!is_key(G[gi], "crjson")) {
            continue;
        }
        d = jparse(G[gi] + 7);
        CHECK(pc_algo_camera_raw_from_json(yyjson_doc_get_root(d), &p, NULL) == PC_OK);
        md = yyjson_mut_doc_new(NULL);
        yyjson_mut_doc_set_root(md, pc_algo_camera_raw_to_json(md, &p));
        text = yyjson_mut_write(md, 0, NULL);
        line("crjson %s", text != NULL ? text : "(null)");
        free(text);
        yyjson_mut_doc_free(md);
        pc_algo_camera_raw_fini(&p);
        yyjson_doc_free(d);
    }
}

/* ---------------------------------------------------------------------------------------------- */

static float (*scene(size_t w, size_t h))[3]
{
    float (*s)[3] = malloc(w * h * sizeof *s);
    size_t i;
    for (i = 0; i < w * h; i++) {
        float x = (float)(i % w) / (float)w, y = (float)(i / w) / (float)h;
        float base = 0.002f * exp2f(x * 12.0f), tex = 1.0f + 0.3f * sinf(y * 40.0f);
        s[i][0] = base * tex;
        s[i][1] = base * tex * 0.8f;
        s[i][2] = base * tex * 0.6f;
    }
    return s;
}
static float cam(float e)
{
    float v = powf(pc_rs_maxf_k(e, 0.0f), 0.45f);
    return pc_rs_minf_k(v, 1.0f);
}
static float (*shoot(const float (*sc)[3], size_t n, float dt))[4]
{
    float (*px)[4] = malloc(n * sizeof *px);
    size_t i;
    int c;
    for (i = 0; i < n; i++) {
        for (c = 0; c < 3; c++) {
            px[i][c] = roundf(cam(sc[i][c] * dt) * 255.0f) / 255.0f;
        }
        px[i][3] = roundf(1.0f * 255.0f) / 255.0f;
    }
    return px;
}

static uint64_t hidx(const size_t *v, size_t n)
{
    uint64_t h = H0;
    size_t i;
    for (i = 0; i < n; i++) {
        h64(&h, (uint64_t)v[i]);
    }
    return h;
}

static void hdr_vectors(void)
{
    const size_t w = 96, h = 40, n = w * h;
    float (*sc)[3] = scene(w, h), (*shots[4])[4];
    const float dts[4] = {0.25f, 1.0f, 4.0f, 16.0f};
    double est[4], ex[4];
    pc_algo_hdr_img refs[4];
    float *grey = malloc(128 * 96 * sizeof *grey), *sh = malloc(128 * 96 * sizeof *sh);
    const int32_t shifts[4][2] = {{5, -3}, {0, 0}, {-7, 6}, {12, 1}};
    const size_t lvs[4] = {0, 2, 4, 6};
    size_t i, k, x, y, *samples = NULL, ns = 0;
    uint64_t hh;
    uint8_t z[3 * 40];
    for (k = 0; k < 4; k++) {
        shots[k] = shoot((const float (*)[3])sc, n, dts[k]);
        ex[k] = (double)dts[k];
    }
    for (y = 10; y < 20; y++) {
        for (x = 30; x < 45; x++) {
            shots[3][y * w + x][0] = 0.3f, shots[3][y * w + x][1] = 0.2f, shots[3][y * w + x][2] = 0.1f;
            shots[3][y * w + x][3] = 1.0f;
        }
    }
    shots[1][5][0] = -0.5f, shots[1][5][1] = 1.5f, shots[1][5][2] = 0.5f, shots[1][5][3] = 0.5f;
    for (k = 0; k < 4; k++) {
        refs[k].px = (const float (*)[4])shots[k];
        refs[k].n = n;
    }
    CHECK(pc_algo_hdr_estimate_exposures(refs, 4, est, NULL) == PC_OK);
    hh = H0;
    hd(&hh, est, 4);
    line("hdr_est %016llx", (unsigned long long)hh);
    for (i = 0; i < 128 * 96; i++) {
        grey[i] = 0.5f + 0.4f * (sinf((float)(i % 128) * 0.21f) * cosf((float)(i / 128) * 0.17f));
    }
    for (k = 0; k < 4; k++) {
        int32_t dx = shifts[k][0], dy = shifts[k][1];
        size_t l;
        for (i = 0; i < 128 * 96; i++) {
            int32_t xx = pc_clamp_i32((int32_t)(i % 128) - dx, 0, 127),
                    yy = pc_clamp_i32((int32_t)(i / 128) - dy, 0, 95);
            sh[i] = grey[(size_t)yy * 128 + (size_t)xx] * 0.6f;
        }
        for (l = 0; l < 4; l++) {
            int32_t ox = 0, oy = 0;
            CHECK(pc_algo_hdr_mtb_offset(128, 96, sh, grey, lvs[l], &ox, &oy, NULL) == PC_OK);
            line("hdr_mtb %d %d %zu %d %d", dx, dy, lvs[l], ox, oy);
        }
    }
    CHECK(pc_algo_hdr_sample_pixels(w, h, (const float (*)[4])shots[1], 160, &samples, &ns, NULL) == PC_OK);
    line("hdr_samples %zu %016llx", ns, (unsigned long long)hidx(samples, ns));
    free(samples);
    CHECK(pc_algo_hdr_sample_pixels(w, h, (const float (*)[4])shots[1], 7, &samples, &ns, NULL) == PC_OK);
    line("hdr_samples7 %zu %016llx", ns, (unsigned long long)hidx(samples, ns));
    free(samples);
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 40; i++) {
            z[k * 40 + i] = (uint8_t)((i * 37 + k * 61) % 256);
        }
    }
    {
        const double lams[3] = {40.0, 1.0, 0.0};
        const char *lam_s[3] = {"40", "1", "0"};
        const double lds[3][3] = {{-1.0, 0.0, 1.0}, {0.0, 0.0, 0.0}, {-2.0, 0.5, 3.0}};
        for (k = 0; k < 3; k++) {
            double g[256];
            CHECK(pc_algo_hdr_response_curve(z, 3, 40, lds[k], lams[k], g, NULL) == PC_OK);
            hh = H0;
            hd(&hh, g, 256);
            line("hdr_resp %s %016llx", lam_s[k], (unsigned long long)hh);
        }
    }
    {
        const bool ghost[5] = {false, true, true, true, false}, given[5] = {false, false, false, true, true};
        const long base[5] = {-1, 1, -1, 9, 0};
        double resp[3][256];
        for (i = 0; i < 256; i++) {
            resp[0][i] = resp[1][i] = resp[2][i] = log(((double)i + 1.0) / 129.0);
        }
        for (k = 0; k < 5; k++) {
            pc_algo_hdr_merge_opts o;
            pc_algo_hdr_merged m;
            int c;
            memset(&o, 0, sizeof o);
            o.exposures = ex;
            o.n_exposures = 4;
            o.remove_ghosts = ghost[k];
            o.has_ghost_base = base[k] >= 0;
            o.ghost_base = base[k] >= 0 ? (size_t)base[k] : 0;
            o.response = given[k] ? (const double (*)[256])resp : NULL;
            CHECK(pc_algo_hdr_merge(w, h, refs, 4, &o, &m, NULL) == PC_OK);
            hh = H0;
            for (c = 0; c < 3; c++) {
                hd(&hh, m.response[c], 256);
            }
            hd(&hh, &m.ghost_fraction, 1);
            hd(&hh, &m.stops, 1);
            line("hdr_merge %d %ld %d %zu %016llx %016llx", ghost[k], base[k], given[k], m.ghost_base,
                 (unsigned long long)hpx((const float (*)[4])m.px, n), (unsigned long long)hh);
            if (!ghost[k] && !given[k]) {
                int t;
                for (t = 0; t < 4; t++) {
                    pc_algo_hdr_tone_method tm;
                    float (*px)[4] = malloc(n * sizeof *px);
                    memcpy(px, m.px, n * sizeof *px);
                    px[7][3] = 0.0f;
                    memset(&tm, 0, sizeof tm);
                    tm.kind = (pc_algo_hdr_tone_kind)t;
                    tm.local = pc_algo_hdr_toning_default();
                    tm.local.radius = 7.0f;
                    tm.local.strength = 0.52f;
                    tm.exposure = 0.5f;
                    tm.gamma = 1.4f;
                    CHECK(pc_algo_hdr_tone_map(px, n, w, h, &tm, NULL) == PC_OK);
                    line("hdr_tone %d %016llx", t, (unsigned long long)hpx((const float (*)[4])px, n));
                    free(px);
                }
            }
            pc_algo_hdr_merged_fini(&m);
        }
    }
    for (k = 0; k < 4; k++) {
        free(shots[k]);
    }
    free(sc), free(grey), free(sh);
}

/* ---------------------------------------------------------------------------------------------- */

#define LENS_CASES 12

static pc_algo_lens_correction lens_case(int i)
{
    pc_algo_lens_correction d;
    pc_algo_lens_correction_default(&d);
    switch (i) {
    case 0:
        break;
    case 1:
        d.distortion = 50.0;
        break;
    case 2:
        d.distortion = -60.0;
        break;
    case 3:
        d.distortion = -60.0, d.edge.kind = PC_ALGO_LENS_EDGE_EXTENSION;
        break;
    case 4:
        d.distortion = -40.0, d.edge.kind = PC_ALGO_LENS_EDGE_COLOR;
        d.edge.color[0] = 1.0f, d.edge.color[1] = 0.0f, d.edge.color[2] = 0.0f, d.edge.color[3] = 1.0f;
        break;
    case 5:
        d.vignette_amount = 100.0, d.vignette_midpoint = 30.0;
        break;
    case 6:
        d.vignette_amount = -100.0, d.edge.kind = PC_ALGO_LENS_EDGE_EXTENSION;
        break;
    case 7:
        d.red_cyan = -100.0, d.blue_yellow = 80.0;
        break;
    case 8:
        d.vertical = -50.0, d.horizontal = 30.0;
        break;
    case 9:
        d.angle = 5.0, d.scale = 110.0, d.edge.kind = PC_ALGO_LENS_EDGE_COLOR;
        d.edge.color[0] = 0.2f, d.edge.color[1] = 0.4f, d.edge.color[2] = 0.6f, d.edge.color[3] = 0.5f;
        break;
    case 10:
        d.has_profile = true, pc_algo_lens_generic_profile(16.0, &d.profile);
        break;
    default:
        d.has_profile = true;
        pc_algo_lens_generic_profile(135.0, &d.profile);
        d.profile.ca_red = 0.002, d.profile.ca_blue = -0.001, d.profile.p1 = 0.01, d.profile.p2 = -0.02;
        d.profile.vignette[2] = 0.05;
        d.correct_vignette = false, d.vertical = 80.0;
        break;
    }
    return d;
}

static void lens_vectors(void)
{
    const double focals[11] = {5.0, 12.0, 16.0, 30.0, 50.0, 85.0, 135.0, 200.0, 999.5, 2.5, -0.4};
    const char *fs[11] = {"5", "12", "16", "30", "50", "85", "135", "200", "999.5", "2.5", "-0.4"};
    pc_rect frame = pc_rect_make(-3, 2, 157, 92);
    uint64_t h;
    int i, k;
    size_t f;
    for (i = 0; i < 11; i++) {
        pc_algo_lens_profile p;
        double v[9];
        char *c;
        pc_algo_lens_generic_profile(focals[i], &p);
        v[0] = p.k1, v[1] = p.k2, v[2] = p.p1, v[3] = p.p2, v[4] = p.vignette[0], v[5] = p.vignette[1];
        v[6] = p.vignette[2], v[7] = p.ca_red, v[8] = p.ca_blue;
        h = H0;
        hd(&h, v, 9);
        for (c = p.name; *c; c++) {
            *c = *c == ' ' ? '_' : *c;
        }
        line("lens_prof %s %s %016llx", fs[i], p.name, (unsigned long long)h);
    }
    for (i = 0; i < LENS_CASES; i++) {
        pc_algo_lens_correction lc = lens_case(i);
        pc_algo_lens_map m;
        double a;
        uint64_t bits;
        int32_t x, y;
        pc_algo_lens_map_init(&m, &lc, frame);
        h = H0;
        for (y = -10; y < 110; y += 7) {
            for (x = -10; x < 170; x += 9) {
                pc_algo_lens_sample s;
                if (pc_algo_lens_map_sample(&m, (double)x + 0.25, (double)y + 0.5, &s)) {
                    double v[6] = {s.p[0], s.p[1], s.pr[0], s.pr[1], s.pb[0], s.pb[1]};
                    hd(&h, v, 6);
                    hf(&h, &s.gain, 1);
                } else {
                    uint8_t e = 0xee;
                    fnv(&h, &e, 1);
                }
            }
        }
        a = pc_algo_lens_auto_scale(&lc, frame);
        memcpy(&bits, &a, 8);
        line("lens_map %d %016llx %016llx", i, (unsigned long long)h, (unsigned long long)bits);
    }
    h = H0;
    for (k = 0; k < 40; k++) {
        double a[2] = {(double)k * 1.5, -(double)k},
               b[2] = {100.0 - (double)k * 3.0, (double)k * (double)k * 0.1};
        double v = pc_algo_lens_straighten_angle(a, b);
        hd(&h, &v, 1);
    }
    line("lens_straight %016llx", (unsigned long long)h);
    for (f = 0; f < 6; f++) {
        bool big = f == 0;
        pc_rect r = pc_rect_make(0, 0, big ? 150 : 61, big ? 90 : 43);
        pc_surface s;
        surf(&s, FORMATS[f].f, r, 2, false);
        for (i = 0; i < LENS_CASES; i++) {
            pc_algo_lens_correction lc = lens_case(i);
            pc_surface out;
            uint64_t hv = 0xbad;
            if (pc_algo_lens_correct(&s, r, &lc, &out, NULL) == PC_OK) {
                hv = hs(&out);
                pc_surface_fini(&out);
            }
            line("lens %s %d %016llx", FORMATS[f].name, i, (unsigned long long)hv);
        }
        pc_surface_fini(&s);
    }
}

/* ---------------------------------------------------------------------------------------------- */

static void persp_vectors(void)
{
    static const pc_algo_persp_plane s0[1] = {
        {{{10, 10}, {90, 12}, {85, 70}, {12, 75}}, {{10, 10}, {90, 12}, {85, 70}, {12, 75}}}};
    static const pc_algo_persp_plane s1[1] = {
        {{{0, 0}, {100, 0}, {100, 100}, {0, 100}}, {{20, 0}, {80, 0}, {100, 100}, {0, 100}}}};
    static const pc_algo_persp_plane s2[2] = {
        {{{0, 0}, {50, 0}, {50, 100}, {0, 100}}, {{0, 10}, {50, 0}, {55, 100}, {0, 90}}},
        {{{50.2, 0}, {100, 0}, {100, 100}, {50, 100.2}}, {{50, 0}, {110, 20}, {100, 80}, {55, 100}}}};
    static const pc_algo_persp_plane s3[3] = {
        {{{0, 0}, {40, 0}, {40, 40}, {0, 40}}, {{3, 2}, {41, -1}, {44, 43}, {-2, 38}}},
        {{{40, 0}, {80, 0}, {80, 40}, {40, 40}}, {{41, -1}, {85, 5}, {79, 45}, {44, 43}}},
        {{{0, 40}, {40, 40}, {40, 80}, {0, 80}}, {{-2, 38}, {44, 43}, {42, 86}, {1, 79}}}};
    const pc_algo_persp_plane *sets[4] = {s0, s1, s2, s3};
    const size_t ns[4] = {1, 1, 2, 3};
    const pc_algo_persp_straighten modes[3] = {PC_ALGO_PERSP_HORIZONTAL, PC_ALGO_PERSP_VERTICAL,
                                               PC_ALGO_PERSP_AUTO};
    const char *mn[3] = {"h", "v", "auto"};
    int k, md;
    for (k = 0; k < 4; k++) {
        size_t gof[12], ng = 0, g, q, n = ns[k];
        pc_algo_persp_map *m = NULL;
        uint64_t h = H0;
        int32_t x, y;
        pc_algo_persp_linked_corners(sets[k], n, gof, &ng);
        for (g = 0; g < ng; g++) {
            uint8_t ff = 0xff;
            for (q = 0; q < n * 4; q++) {
                if (gof[q] == g) {
                    h64(&h, q / 4);
                    h64(&h, q % 4);
                }
            }
            fnv(&h, &ff, 1);
        }
        line("persp_groups %d %zu %016llx", k, ng, (unsigned long long)h);
        CHECK(pc_algo_persp_map_new(sets[k], n, &m, NULL) == PC_OK);
        h = H0;
        for (y = -20; y < 130; y += 6) {
            for (x = -20; x < 130; x += 5) {
                double v[2];
                pc_algo_persp_map_apply(m, (double)x + 0.3, (double)y - 0.2, &v[0], &v[1]);
                hd(&h, v, 2);
            }
        }
        pc_algo_persp_map_free(m);
        line("persp %d %d %016llx", k, pc_algo_persp_is_identity(sets[k], n), (unsigned long long)h);
        for (md = 0; md < 3; md++) {
            pc_algo_persp_plane s[3];
            memcpy(s, sets[k], n * sizeof *s);
            CHECK(pc_algo_persp_straighten_planes(s, n, modes[md], NULL) == PC_OK);
            h = H0;
            for (q = 0; q < n; q++) {
                double pt[2];
                int c;
                for (c = 0; c < 4; c++) {
                    hd(&h, s[q].src[c], 2);
                }
                for (c = 0; c < 4; c++) {
                    hd(&h, s[q].dst[c], 2);
                }
                pc_algo_persp_plane_point(&s[q], 0.3, 0.7, true, pt);
                hd(&h, pt, 2);
                pc_algo_persp_plane_point(&s[q], 0.6, 0.1, false, pt);
                hd(&h, pt, 2);
            }
            line("persp_straight %d %s %016llx", k, mn[md], (unsigned long long)h);
        }
    }
}

/* ---------------------------------------------------------------------------------------------- */

static pc_algo_vp_plane floor_quad(double f, const double c[2], double rx, double ry)
{
    static const double pts[4][3] = {{-1.0, -0.5, 0.0}, {1.0, -0.5, 0.0}, {1.0, 0.5, 0.0}, {-1.0, 0.5, 0.0}};
    pc_algo_vp_plane pl;
    int k;
    for (k = 0; k < 4; k++) {
        const double *p = pts[k];
        double s = sin(rx), co = cos(rx), q[3] = {p[0], co * p[1] - s * p[2], s * p[1] + co * p[2]};
        double s2 = sin(ry), c2 = cos(ry), r[3] = {c2 * q[0] + s2 * q[2], q[1], -s2 * q[0] + c2 * q[2]};
        double z = r[2] + 4.0;
        pl.corners[k][0] = c[0] + f * r[0] / z;
        pl.corners[k][1] = c[1] + f * (r[1] + 0.0) / z;
    }
    return pl;
}

static void vp_vectors(void)
{
    const double c[2] = {400.0, 300.0}, rots[4][2] = {{0.6, 0.4}, {0.3, -0.5}, {0.0, 0.0}, {1.1, 0.2}};
    const pc_algo_vp_edge edges[4] = {PC_ALGO_VP_TOP, PC_ALGO_VP_RIGHT, PC_ALGO_VP_BOTTOM, PC_ALGO_VP_LEFT};
    const double tears[4][2] = {{90.0, 1.0}, {70.0, 0.5}, {120.0, 2.0}, {90.0, 0.01}};
    const double at[3][2] = {{400.0, 300.0}, {10.0, 10.0}, {700.0, 500.0}};
    pc_algo_vp_scene sc;
    pc_algo_vp_plane first;
    pc_algo_homography hm;
    pc_surface img;
    int k, e;
    for (k = 0; k < 4; k++) {
        pc_algo_vp_plane pl = floor_quad(500.0, c, rots[k][0], rots[k][1]);
        pc_algo_vp_lifted lf;
        double f = -1.0, lu, lv, lens2[2];
        bool has = pc_algo_vp_plane_focal(&pl, c, &f);
        uint64_t h = H0, bits;
        int q;
        if (!has) {
            f = -1.0;
        }
        pc_algo_vp_plane_lift(&pl, has ? f : 800.0, c, &lf);
        pc_algo_vp_lifted_lengths(&lf, &lu, &lv);
        for (q = 0; q < 4; q++) {
            hd(&h, lf.pts[q], 3);
        }
        lens2[0] = lu, lens2[1] = lv;
        hd(&h, lens2, 2);
        memcpy(&bits, &f, 8);
        line("vp_plane %d %016llx %016llx", k, (unsigned long long)bits, (unsigned long long)h);
        CHECK(pc_algo_vp_scene_init(&sc, &pl, c, false, 0.0, 800.0, NULL) == PC_OK);
        h = H0;
        for (e = 0; e < 4; e++) {
            size_t idx = 0;
            if (pc_algo_vp_scene_tear_off(&sc, 0, edges[e], tears[e][0], tears[e][1], &idx, NULL) == PC_OK) {
                for (q = 0; q < 4; q++) {
                    hd(&h, sc.planes[idx].corners[q], 2);
                }
            } else {
                uint8_t ee = 0xee;
                fnv(&h, &ee, 1);
            }
        }
        for (q = 0; q < 3; q++) {
            size_t idx = 0;
            h64(&h, pc_algo_vp_scene_plane_at(&sc, at[q], &idx) ? (uint64_t)idx : UINT64_MAX);
        }
        line("vp_tear %d %zu %016llx", k, sc.n, (unsigned long long)h);
        pc_algo_vp_scene_fini(&sc);
    }
    first = floor_quad(500.0, c, 0.6, 0.4);
    CHECK(pc_algo_vp_scene_init(&sc, &first, c, false, 0.0, 800.0, NULL) == PC_OK);
    surf(&img, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 100, 50), 5, false);
    {
        const double ats[3][2] = {{0.25, 0.25}, {0.0, 0.0}, {0.6, 0.1}}, widths[3] = {0.5, 1.0, 0.3};
        const char *txt[3] = {"0.25 0.25 0.5", "0 0 1", "0.6 0.1 0.3"};
        for (k = 0; k < 3; k++) {
            pc_surface out;
            uint64_t hv = 0xbad;
            if (pc_algo_vp_paste(&sc, 0, &img, pc_rect_make(0, 0, 100, 50), ats[k], widths[k], &out, NULL) ==
                PC_OK) {
                hv = hs(&out);
                pc_surface_fini(&out);
            }
            line("vp_paste %s %016llx", txt[k], (unsigned long long)hv);
        }
    }
    CHECK(pc_algo_vp_plane_homography(&sc.planes[0], &hm));
    for (k = 0; k < 2; k++) {
        pc_pixel_format fmt = k == 0 ? PC_PIXEL_FORMAT_RGBA8 : PC_PIXEL_FORMAT_RGBA32F;
        pc_surface s;
        double src[2], pts[5][2];
        size_t n = 0;
        int q;
        surf(&s, fmt, pc_rect_make(150, 150, 650, 500), 1, true);
        pc_algo_homography_apply(&hm, 0.5, 0.8, &src[0], &src[1]);
        for (q = 0; q < 5; q++) {
            pc_algo_homography_apply(&hm, 0.3 + (double)q * 0.08, 0.3, &pts[q][0], &pts[q][1]);
        }
        CHECK(pc_algo_vp_clone_stroke(&sc, &s, src, (const double (*)[2])pts, 5, 14.0, 0.6, 0.8f, &n, NULL) ==
              PC_OK);
        line("vp_clone %s %zu %016llx", k == 0 ? "rgba8" : "rgba32", n, (unsigned long long)hs(&s));
        pc_surface_fini(&s);
    }
    pc_surface_fini(&img);
    pc_algo_vp_scene_fini(&sc);
}

/* ---------------------------------------------------------------------------------------------- */

static void wide_vectors(void)
{
    const pc_rect frame = pc_rect_make(0, 0, 480, 320);
    uint64_t h = H0;
    size_t gi;
    for (gi = 0; gi < NG; gi++) {
        char buf[4096], *t[4];
        const pc_rect frames[2] = {frame, pc_rect_make(-10, 5, 790, 405)};
        const double probes[4][2] = {{10, 20}, {300, 200}, {470, 10}, {-50, 900}};
        const double a[2] = {50, 60}, b[2] = {450, 70};
        yyjson_doc *d;
        pc_algo_wide_angle p;
        int f, q;
        if (!is_key(G[gi], "wide_cam")) {
            continue;
        }
        CHECK(tokens(G[gi], buf, sizeof buf, t, 4) == 3);
        d = jparse(t[1]);
        CHECK(pc_algo_wide_angle_from_json(yyjson_doc_get_root(d), &p, NULL) == PC_OK);
        for (f = 0; f < 2; f++) {
            pc_algo_wide_camera cam;
            double v[5], r[3], px[2], (*arc)[2] = NULL;
            size_t na = 0, i;
            pc_algo_wide_camera_init(&cam, &p, frames[f]);
            v[0] = cam.f, v[1] = cam.c[0], v[2] = cam.c[1], v[3] = cam.size[0], v[4] = cam.size[1];
            hd(&h, v, 5);
            for (q = 0; q < 4; q++) {
                pc_algo_wide_camera_ray(&cam, probes[q][0], probes[q][1], r);
                hd(&h, r, 3);
                if (pc_algo_wide_camera_pixel(&cam, r, px)) {
                    hd(&h, px, 2);
                } else {
                    uint8_t e = 0xee;
                    fnv(&h, &e, 1);
                }
            }
            CHECK(pc_algo_wide_camera_arc(&cam, a, b, 24, &arc, &na, NULL) == PC_OK);
            for (i = 0; i < na; i++) {
                hd(&h, arc[i], 2);
            }
            free(arc);
        }
        line("wide_cam %s %016llx", t[1], (unsigned long long)h);
        pc_algo_wide_angle_fini(&p);
        yyjson_doc_free(d);
    }
    for (gi = 0; gi < NG; gi++) {
        char buf[4096], *t[8];
        yyjson_doc *d;
        pc_algo_wide_angle p;
        pc_algo_wide_mesh m;
        pc_rect fr;
        size_t i, q, nv;
        if (is_key(G[gi], "widejson")) {
            yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
            char *text;
            d = jparse(G[gi] + 9);
            CHECK(pc_algo_wide_angle_from_json(yyjson_doc_get_root(d), &p, NULL) == PC_OK);
            yyjson_mut_doc_set_root(md, pc_algo_wide_angle_to_json(md, &p));
            text = yyjson_mut_write(md, 0, NULL);
            line("widejson %s", text != NULL ? text : "(null)");
            free(text);
            yyjson_mut_doc_free(md);
            pc_algo_wide_angle_fini(&p);
            yyjson_doc_free(d);
            continue;
        }
        if (!is_key(G[gi], "wide")) {
            continue;
        }
        CHECK(tokens(G[gi], buf, sizeof buf, t, 8) == 7);
        fr = strcmp(t[1], "480") == 0 ? frame : pc_rect_make(3, 4, 103, 37);
        d = jparse(t[3]);
        CHECK(pc_algo_wide_angle_from_json(yyjson_doc_get_root(d), &p, NULL) == PC_OK);
        CHECK(pc_algo_wide_solve(&p, fr, &m, NULL) == PC_OK);
        h = H0;
        nv = (m.nx + 1) * (m.ny + 1);
        for (i = 0; i < nv; i++) {
            hd(&h, m.out[i], 2);
            hd(&h, m.src[i], 2);
        }
        for (i = 0; i < m.n_curves; i++) {
            uint8_t ff = 0xff;
            for (q = 0; q < m.curve_len[i]; q++) {
                hd(&h, m.curves[i][q], 2);
            }
            fnv(&h, &ff, 1);
        }
        hd(&h, &m.residual, 1);
        {
            const double pr[2][2] = {{300.0, 250.0}, {-5.0, 1000.0}};
            for (q = 0; q < 2; q++) {
                double o[2];
                pc_algo_wide_mesh_map(&m, fr, pr[q], o);
                hd(&h, o, 2);
            }
        }
        line("wide %s %s %s %zu %zu %016llx", t[1], t[2], t[3], m.nx, m.ny, (unsigned long long)h);
        pc_algo_wide_mesh_fini(&m);
        pc_algo_wide_angle_fini(&p);
        yyjson_doc_free(d);
    }
}

/* ---------------------------------------------------------------------------------------------- */

static void relight_vectors(void)
{
    const pc_rect r = pc_rect_make(0, 0, 40, 30);
    const float softs[10] = {-5.0f, 0.0f, 1.0f, 12.5f, 25.0f, 50.0f, 99.0f, 100.0f, 500.0f, NAN};
    const pc_rect bs[3] = {{0, 0, 1600, 1000}, {0, 0, 10, 4000}, {0, 0, 0, 0}};
    uint64_t h = H0;
    size_t gi;
    int i, k;
    for (gi = 0; gi < NG; gi++) {
        char buf[4096], *t[8];
        pc_algo_filter f;
        pc_surface s, out;
        pc_rect bounds, area;
        uint64_t hv = 0xbad;
        if (!is_key(G[gi], "relight")) {
            continue;
        }
        CHECK(tokens(G[gi], buf, sizeof buf, t, 8) == 5);
        CHECK(pc_algo_filter_from_json_str(t[3], strlen(t[3]), &f, NULL) == PC_OK);
        surf(&s, format_named(t[1]), r, 4, strcmp(t[1], "rgba8") == 0);
        bounds = strcmp(t[2], "40") == 0 ? r : pc_rect_make(0, 0, 300, 200);
        area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), bounds, NULL);
        if (pc_algo_apply(&s, &f, area, bounds, NULL, &out, NULL) == PC_OK) {
            hv = hs(&out);
            pc_surface_fini(&out);
        }
        line("relight %s %s %s %016llx", t[1], t[2], t[3], (unsigned long long)hv);
        pc_surface_fini(&s);
        pc_algo_filter_fini(&f);
    }
    for (i = 0; i < 10; i++) {
        for (k = 0; k < 3; k++) {
            pc_algo_relight p = {0.0f, 10.0f, 5.0f, 0.0f, 0.0f, softs[i]};
            pc_algo_filter f;
            pc_algo_halo hl;
            CHECK(pc_algo_filter_make(&PC_ALGO_FILTER_RELIGHT, &p, &f, NULL) == PC_OK);
            hl = pc_algo_filter_halo_for(&f, bs[k]);
            if (hl.bounds) {
                uint8_t bb = 0xbb;
                fnv(&h, &bb, 1);
            } else {
                hi32(&h, hl.radius);
            }
            pc_algo_filter_fini(&f);
        }
    }
    line("relight_halo %016llx", (unsigned long long)h);
}

/* ---------------------------------------------------------------------------------------------- */

/* Adaptive Wide Angle apply: `wide_apply <fmt> <interp> <src> <frame> <json> <hash>`. */
static void wide_apply_vectors(void)
{
    static const struct {
        const char *name;
        pc_pixel_format f;
    } extra[5] = {
        {"rgba16", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}},
        {"rgb32", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, false}},
        {"gray8", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false}},
        {"graya32", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, true}},
        {"cmyk16", {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U16, false}},
    };
    size_t gi;
    for (gi = 0; gi < NG; gi++) {
        char buf[4096], *t[8];
        yyjson_doc *d;
        pc_algo_wide_angle p;
        pc_pixel_format fmt;
        pc_algo_interp interp;
        pc_rect fr;
        pc_surface s, out;
        uint64_t hv = 0xbad;
        size_t i;
        if (!is_key(G[gi], "wide_apply")) {
            continue;
        }
        CHECK(tokens(G[gi], buf, sizeof buf, t, 8) == 7);
        fmt = format_named(t[1]);
        for (i = 0; i < 5; i++) {
            if (strcmp(extra[i].name, t[1]) == 0) {
                fmt = extra[i].f;
            }
        }
        interp = strcmp(t[2], "nearest") == 0    ? PC_ALGO_INTERP_NEAREST
                 : strcmp(t[2], "bilinear") == 0 ? PC_ALGO_INTERP_BILINEAR
                                                 : PC_ALGO_INTERP_BICUBIC;
        if (strcmp(t[3], "a") == 0 || strcmp(t[3], "o") == 0) {
            surf(&s, fmt, pc_rect_make(0, 0, 480, 320), 5, strcmp(t[3], "o") == 0);
        } else if (strcmp(t[3], "s") == 0) {
            surf(&s, fmt, pc_rect_make(100, 50, 380, 300), 6, false);
        } else {
            pc_surface_init(&s, fmt);
        }
        fr = strcmp(t[4], "f0") == 0   ? pc_rect_make(0, 0, 480, 320)
             : strcmp(t[4], "f1") == 0 ? pc_rect_make(40, 30, 440, 290)
             : strcmp(t[4], "f2") == 0 ? pc_rect_make(-10, 5, 790, 405)
             : strcmp(t[4], "f3") == 0 ? pc_rect_make(3, 4, 103, 37)
             : strcmp(t[4], "f4") == 0 ? pc_rect_make(300, 200, 900, 640)
                                       : pc_rect_make(5, 5, 5, 9);
        d = jparse(t[5]);
        CHECK(pc_algo_wide_angle_from_json(yyjson_doc_get_root(d), &p, NULL) == PC_OK);
        if (pc_algo_wide_apply(&s, fr, &p, interp, &out, NULL) == PC_OK) {
            hv = hs(&out);
            pc_surface_fini(&out);
        }
        line("wide_apply %s %s %s %s %s %016llx", t[1], t[2], t[3], t[4], t[5], (unsigned long long)hv);
        pc_algo_wide_angle_fini(&p);
        yyjson_doc_free(d);
        pc_surface_fini(&s);
    }
}

/* ---------------------------------------------------------------------------------------------- */

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

static void test_photo_parity(void)
{
    size_t glen = 0, lines = 0, i;
    char *golden = read_file(GOLDEN, &glen), *copy, *save = NULL, *t, *got, *gsave = NULL;
    CHECK(golden != NULL);
    if (golden == NULL) {
        return;
    }
    copy = malloc(glen + 1);
    memcpy(copy, golden, glen + 1);
    G = malloc(sizeof *G * (glen / 2 + 1));
    for (t = strtok_r(copy, "\n", &save); t != NULL; t = strtok_r(NULL, "\n", &save)) {
        G[NG++] = t;
    }
    pc_buf_init(&OUT);
    camera_raw_vectors();
    hdr_vectors();
    lens_vectors();
    persp_vectors();
    vp_vectors();
    wide_vectors();
    relight_vectors();
    wide_apply_vectors();
    (void)pc_buf_append_byte(&OUT, '\0', NULL);
    got = strtok_r((char *)OUT.data, "\n", &gsave);
    for (i = 0; i < NG || got != NULL; i++) {
        const char *want = i < NG ? G[i] : NULL;
        lines++;
        if (want == NULL || got == NULL || strcmp(want, got) != 0) {
            PC_T_FAIL("line %zu:\n  want %.300s\n  got  %.300s", lines, want != NULL ? want : "(end)",
                      got != NULL ? got : "(end)");
        } else {
            pc_t_checks++;
        }
        got = got != NULL ? strtok_r(NULL, "\n", &gsave) : NULL;
    }
    pc_buf_fini(&OUT);
    free(G);
    free(copy);
    free(golden);
}

int main(int argc, char **argv)
{
    RUN(test_photo_parity);
    return pc_t_finish();
}
