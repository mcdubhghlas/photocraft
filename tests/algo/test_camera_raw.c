/* Camera Raw develop (port of the tests in crates/algo/src/camera_raw.rs, histogram.rs's
 * exposure comb test) plus never-crash checks for hostile settings. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/rmath.h"
#include "harness.h"

static float (*img(size_t w, size_t h))[4]
{
    float (*px)[4] = malloc(w * h * sizeof *px);
    size_t i;
    for (i = 0; i < w * h; i++) {
        float x = (float)(i % w) / (float)w, y = (float)(i / w) / (float)h;
        px[i][0] = 0.2f + 0.6f * x;
        px[i][1] = 0.3f + 0.4f * y;
        px[i][2] = 0.5f - 0.3f * x * y;
        px[i][3] = 1.0f;
    }
    return px;
}

static float (*fill(size_t n, float r, float g, float b))[4]
{
    float (*px)[4] = malloc(n * sizeof *px);
    size_t i;
    for (i = 0; i < n; i++) {
        px[i][0] = r, px[i][1] = g, px[i][2] = b, px[i][3] = 1.0f;
    }
    return px;
}

static float (*dup(const float (*a)[4], size_t n))[4]
{
    float (*px)[4] = malloc(n * sizeof *px);
    memcpy(px, a, n * sizeof *px);
    return px;
}

static float mean(const float (*px)[4], size_t n, int c)
{
    float s = -0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        s += px[i][c];
    }
    return s / (float)n;
}

static pc_algo_camera_raw def(void)
{
    pc_algo_camera_raw p;
    pc_algo_camera_raw_default(&p);
    return p;
}

static pc_status from_text(const char *json, pc_algo_camera_raw *out)
{
    yyjson_doc *d = yyjson_read(json, strlen(json), 0);
    pc_status st;
    if (d == NULL) {
        return PC_ERR_FORMAT;
    }
    st = pc_algo_camera_raw_from_json(yyjson_doc_get_root(d), out, NULL);
    yyjson_doc_free(d);
    return st;
}

static void test_new_curves_are_validated_but_saved_curves_still_load_and_render(void)
{
    static const char *fields[4] = {"pointCurve", "pointCurveRed", "pointCurveGreen", "pointCurveBlue"};
    static const char *bad[] = {"[[128, 128]]",
                                "[[255, 255], [0, 0]]",
                                "[[0, 0], [0, 255]]",
                                "[[0, -1], [255, 255]]",
                                "[[0, 0], [256, 255]]",
                                "[[0, 0], [1e30, 255]]",
                                NULL};
    char json[1024], pts[512];
    pc_algo_camera_raw p, d0, edited, broken;
    float (*a)[4];
    size_t i, f, k;
    for (f = 0; f < 4; f++) {
        for (i = 0; bad[i] != NULL; i++) {
            (void)snprintf(json, sizeof json, "{\"%s\": %s}", fields[f], bad[i]);
            CHECK(from_text(json, &p) == PC_OK);
            CHECK(pc_algo_camera_raw_validate(&p, NULL) != PC_OK);
            pc_algo_camera_raw_fini(&p);
        }
        /* 17 points */
        strcpy(pts, "[");
        for (k = 0; k < 17; k++) {
            char e[32];
            (void)snprintf(e, sizeof e, "%s[%zu, %zu]", k ? "," : "", k * 15, k * 15);
            strcat(pts, e);
        }
        strcat(pts, "]");
        (void)snprintf(json, sizeof json, "{\"%s\": %s}", fields[f], pts);
        CHECK(from_text(json, &p) == PC_OK);
        CHECK(pc_algo_camera_raw_validate(&p, NULL) != PC_OK);
        pc_algo_camera_raw_fini(&p);
        /* good: empty, two points, 16 points */
        (void)snprintf(json, sizeof json, "{\"%s\": []}", fields[f]);
        CHECK(from_text(json, &p) == PC_OK && pc_algo_camera_raw_validate(&p, NULL) == PC_OK);
        pc_algo_camera_raw_fini(&p);
        (void)snprintf(json, sizeof json, "{\"%s\": [[0, 0], [255, 255]]}", fields[f]);
        CHECK(from_text(json, &p) == PC_OK && pc_algo_camera_raw_validate(&p, NULL) == PC_OK);
        pc_algo_camera_raw_fini(&p);
        strcpy(pts, "[");
        for (k = 0; k < 16; k++) {
            char e[32];
            (void)snprintf(e, sizeof e, "%s[%zu, %zu]", k ? "," : "", k * 17, k * 17);
            strcat(pts, e);
        }
        strcat(pts, "]");
        (void)snprintf(json, sizeof json, "{\"%s\": %s}", fields[f], pts);
        CHECK(from_text(json, &p) == PC_OK && pc_algo_camera_raw_validate(&p, NULL) == PC_OK);
        pc_algo_camera_raw_fini(&p);
    }
    CHECK(from_text("{}", &p) == PC_OK);
    d0 = def();
    CHECK(pc_algo_camera_raw_eq(&p, &d0));
    pc_algo_camera_raw_fini(&p);
    /* An older editor could stack two points on one input and exceed 16 points; such a saved
     * Smart Filter must still develop. */
    strcpy(pts, "[");
    for (k = 0; k < 21; k++) {
        char e[48];
        if (k == 5) {
            (void)snprintf(e, sizeof e, ",[60.0, 200.0]");
        } else {
            size_t v = k < 5 ? k : k - 1;
            (void)snprintf(e, sizeof e, "%s[%zu.0, %zu.0]", k ? "," : "", v * 12, v * 12);
        }
        strcat(pts, e);
    }
    strcat(pts, "]");
    (void)snprintf(json, sizeof json, "{\"pointCurve\": %s}", pts);
    CHECK(from_text(json, &p) == PC_OK);
    CHECK(p.point_curve.n == 21);
    CHECK(pc_algo_camera_raw_validate(&p, NULL) != PC_OK);
    CHECK(pc_algo_camera_raw_copy(&edited, &p, NULL) == PC_OK);
    edited.exposure = 1.0f;
    CHECK(pc_algo_camera_raw_validate_changes(&edited, &p, NULL) == PC_OK);
    CHECK(pc_algo_camera_raw_copy(&broken, &p, NULL) == PC_OK);
    free(broken.point_curve.v);
    broken.point_curve.v = malloc(sizeof *broken.point_curve.v);
    broken.point_curve.v[0][0] = 10.0f, broken.point_curve.v[0][1] = 10.0f;
    broken.point_curve.n = 1;
    CHECK(pc_algo_camera_raw_validate_changes(&broken, &p, NULL) != PC_OK);
    a = img(4, 4);
    CHECK(pc_algo_camera_raw_develop(a, 16, 4, 4, &p, false, NULL) == PC_OK);
    for (i = 0; i < 16; i++) {
        CHECK(isfinite(a[i][0]) && isfinite(a[i][1]) && isfinite(a[i][2]) && isfinite(a[i][3]));
    }
    free(a);
    pc_algo_camera_raw_fini(&p), pc_algo_camera_raw_fini(&edited), pc_algo_camera_raw_fini(&broken);
}

static void test_validation_messages(void)
{
    pc_error e;
    const float one[1][2] = {{1, 2}};
    pc_algo_camera_raw p = def();
    CHECK(pc_algo_cr_validate_curve(one, 1, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "a nonempty curve needs at least two points");
    p.point_curve_green.v = malloc(sizeof *p.point_curve_green.v * 2);
    p.point_curve_green.n = 2;
    p.point_curve_green.v[0][0] = 0, p.point_curve_green.v[0][1] = 0;
    p.point_curve_green.v[1][0] = 0.5f, p.point_curve_green.v[1][1] = 1;
    CHECK(pc_algo_camera_raw_validate(&p, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "pointCurveGreen: curve inputs must increase by at least one level");
    pc_algo_camera_raw_fini(&p);
}

static void test_defaults_are_identity(void)
{
    float (*a)[4] = img(40, 30), (*b)[4] = dup((const float (*)[4])a, 1200);
    pc_algo_camera_raw p = def();
    CHECK(pc_algo_camera_raw_develop(a, 1200, 40, 30, &p, false, NULL) == PC_OK);
    CHECK(memcmp(a, b, 1200 * sizeof *a) == 0);
    free(a), free(b);
}

static float (*run(const float (*base)[4], size_t w, size_t h, const pc_algo_camera_raw *p))[4]
{
    float (*a)[4] = dup(base, w * h);
    CHECK(pc_algo_camera_raw_develop(a, w * h, w, h, p, false, NULL) == PC_OK);
    return a;
}

static float spread(const float *q)
{
    return pc_rs_maxf(pc_rs_maxf(q[0], q[1]), q[2]) - pc_rs_minf(pc_rs_minf(q[0], q[1]), q[2]);
}

static void test_basic_panel_moves_the_right_way(void)
{
    const size_t w = 64, h = 48, n = w * h;
    float (*base)[4] = img(w, h), (*o)[4];
    const float (*cb)[4] = (const float (*)[4])base;
    pc_algo_camera_raw p;
    size_t i, k;
    p = def(), p.temperature = 50.0f;
    o = run(cb, w, h, &p);
    CHECK(mean((const float (*)[4])o, n, 0) > mean(cb, n, 0) &&
          mean((const float (*)[4])o, n, 2) < mean(cb, n, 2));
    free(o);
    p = def(), p.exposure = 1.0f;
    o = run(cb, w, h, &p);
    CHECK(mean((const float (*)[4])o, n, 1) > mean(cb, n, 1) + 0.1f);
    free(o);
    p = def(), p.shadows = 100.0f;
    o = run(cb, w, h, &p);
    CHECK(o[0][1] > base[0][1]);
    free(o);
    p = def(), p.highlights = -100.0f;
    o = run(cb, w, h, &p);
    CHECK(o[n - 1][0] < base[n - 1][0]);
    free(o);
    p = def(), p.contrast = 100.0f;
    o = run(cb, w, h, &p);
    CHECK(o[0][1] < base[0][1] && o[n - 1][0] > base[n - 1][0]);
    free(o);
    p = def(), p.saturation = -100.0f;
    o = run(cb, w, h, &p);
    for (i = 0; i < n; i++) {
        CHECK(fabsf(o[i][0] - o[i][1]) < 1e-4f && fabsf(o[i][1] - o[i][2]) < 1e-4f);
    }
    free(o);
    p = def(), p.vibrance = 100.0f;
    o = run(cb, w, h, &p);
    CHECK(spread(o[n / 2]) > spread(base[n / 2]));
    free(o);
    for (k = 0; k < 5; k++) {
        bool same = true;
        p = def();
        switch (k) {
        case 0:
            p.clarity = 80.0f;
            break;
        case 1:
            p.texture = 80.0f;
            break;
        case 2:
            p.dehaze = 60.0f;
            break;
        case 3:
            p.dehaze = -60.0f;
            break;
        default:
            p.whites = 50.0f, p.blacks = -50.0f;
            break;
        }
        o = run(cb, w, h, &p);
        for (i = 0; i < n; i++) {
            int c;
            for (c = 0; c < 3; c++) {
                CHECK(isfinite(o[i][c]) && o[i][c] >= 0.0f && o[i][c] <= 1.0f);
            }
        }
        same = memcmp(o, base, n * sizeof *o) == 0;
        CHECK(!same);
        free(o);
    }
    free(base);
}

static void test_curves_hsl_grading_detail_effects(void)
{
    const float pts[4][2] = {{0, 0}, {64, 40}, {192, 220}, {255, 255}};
    float lut[256], wts[8];
    const size_t w = 48, h = 32, n = w * h;
    float (*a)[4], (*noisy)[4], (*nr)[4], (*sh)[4], (*g1)[4], (*g2)[4], (*base)[4], (*v)[4];
    float g[2][4] = {{0.1f, 0.1f, 0.1f, 1.0f}, {0.95f, 0.95f, 0.95f, 1.0f}};
    pc_algo_camera_raw p;
    int hh, k;
    size_t i;
    CHECK(pc_algo_cr_curve_lut(pts, 4, lut, 256, NULL) == PC_OK);
    CHECK(fabsf(lut[64] - 40.0f / 255.0f) < 0.01f && fabsf(lut[192] - 220.0f / 255.0f) < 0.01f);
    for (k = 1; k < 256; k++) {
        CHECK(lut[k] >= lut[k - 1]);
    }
    for (hh = 0; hh < 360; hh += 7) {
        float s = -0.0f;
        pc_algo_cr_band_weights((float)hh, wts);
        for (k = 0; k < 8; k++) {
            s += wts[k];
        }
        CHECK(fabsf(s - 1.0f) < 1e-4f);
    }
    p = def();
    p.hsl_sat[0] = -100.0f;
    a = fill(n, 0.8f, 0.2f, 0.2f);
    CHECK(pc_algo_camera_raw_develop(a, n, w, h, &p, false, NULL) == PC_OK);
    CHECK(a[0][0] - a[0][1] < 0.1f);
    free(a);
    a = fill(n, 0.2f, 0.2f, 0.8f);
    CHECK(pc_algo_camera_raw_develop(a, n, w, h, &p, false, NULL) == PC_OK);
    CHECK(fabsf(a[0][2] - 0.8f) < 1e-4f);
    free(a);
    /* Grading: blue shadows tint dark pixels, leave bright ones nearly alone. */
    p = def();
    p.grade_shadows = (pc_algo_cr_wheel){220.0f, 80.0f, 0.0f};
    CHECK(pc_algo_camera_raw_develop(g, 2, 2, 1, &p, false, NULL) == PC_OK);
    CHECK(g[0][2] > g[0][0] + 0.05f && fabsf(g[1][2] - g[1][0]) < 0.02f);
    /* Point curve + parametric. */
    p = def();
    p.point_curve.v = malloc(2 * sizeof *p.point_curve.v);
    p.point_curve.n = 2;
    p.point_curve.v[0][0] = 0, p.point_curve.v[0][1] = 30, p.point_curve.v[1][0] = 255,
    p.point_curve.v[1][1] = 255;
    p.curve_shadows = 50.0f;
    a = img(w, h);
    CHECK(pc_algo_camera_raw_develop(a, n, w, h, &p, false, NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        CHECK(a[i][1] >= 30.0f / 255.0f - 1e-4f);
    }
    free(a);
    pc_algo_camera_raw_fini(&p);
    /* Noise reduction reduces variance; sharpening increases it; grain is deterministic. */
    noisy = malloc(n * sizeof *noisy);
    for (i = 0; i < n; i++) {
        float v0 = pc_algo_photo_hash01((int64_t)i, 0, 3) * 0.2f;
        noisy[i][0] = noisy[i][1] = noisy[i][2] = 0.4f + v0;
        noisy[i][3] = 1.0f;
    }
#define VAR(arr, out)                                                                                        \
    do {                                                                                                     \
        float m_ = mean((const float (*)[4])(arr), n, 1), s_ = -0.0f;                                        \
        for (i = 0; i < n; i++) {                                                                            \
            s_ += ((arr)[i][1] - m_) * ((arr)[i][1] - m_);                                                   \
        }                                                                                                    \
        (out) = s_ / (float)n;                                                                               \
    } while (0)
    {
        float vn, vr, vs;
        p = def(), p.noise_luminance = 80.0f;
        nr = run((const float (*)[4])noisy, w, h, &p);
        p = def(), p.sharpen_amount = 100.0f;
        sh = run((const float (*)[4])noisy, w, h, &p);
        VAR(noisy, vn);
        VAR(nr, vr);
        VAR(sh, vs);
        CHECK(vr < vn * 0.5f);
        CHECK(vs > vn);
        free(nr), free(sh);
    }
#undef VAR
    base = img(w, h);
    p = def(), p.grain_amount = 50.0f, p.seed = 4;
    g1 = run((const float (*)[4])base, w, h, &p);
    g2 = run((const float (*)[4])base, w, h, &p);
    CHECK(memcmp(g1, g2, n * sizeof *g1) == 0);
    CHECK(memcmp(g1, base, n * sizeof *g1) != 0);
    free(g1), free(g2), free(noisy);
    /* Post-crop vignette darkens corners, leaves the centre. */
    p = def(), p.vignette_amount = -80.0f;
    v = fill(n, 0.5f, 0.5f, 0.5f);
    CHECK(pc_algo_camera_raw_develop(v, n, w, h, &p, false, NULL) == PC_OK);
    CHECK(v[0][0] < 0.35f && fabsf(v[(h / 2) * w + w / 2][0] - 0.5f) < 0.01f);
    free(v), free(base);
}

static void test_float_documents_keep_overrange(void)
{
    float (*a)[4] = fill(16, 2.0f, 1.5f, 0.5f);
    pc_algo_camera_raw p = def();
    p.exposure = 0.5f;
    CHECK(pc_algo_camera_raw_develop(a, 16, 4, 4, &p, true, NULL) == PC_OK);
    CHECK(a[0][0] > 2.0f);
    free(a);
}

/* histogram.rs: exposure_of_an_eight_bit_gradient_reproduces_the_quantization_comb */
static void test_exposure_of_an_eight_bit_gradient_reproduces_the_quantization_comb(void)
{
    float px[256][4];
    pc_algo_rgb_histogram hist;
    pc_algo_camera_raw p = def();
    int i, zeros = 0;
    for (i = 0; i < 256; i++) {
        px[i][0] = px[i][1] = px[i][2] = (float)i / 255.0f;
        px[i][3] = 1.0f;
    }
    pc_algo_rgb_histogram_from_rgba((const float (*)[4])px, 256, &hist);
    for (i = 0; i < 256; i++) {
        CHECK(hist.channels[0][i] == 1);
    }
    p.exposure = 0.5f;
    CHECK(pc_algo_camera_raw_develop(px, 256, 256, 1, &p, true, NULL) == PC_OK);
    pc_algo_rgb_histogram_from_rgba((const float (*)[4])px, 256, &hist);
    for (i = 10; i < 230; i++) {
        zeros += hist.channels[0][i] == 0;
    }
    CHECK(zeros > 10);
    CHECK(hist.samples == 256);
    CHECK(hist.overflow[0] > 0);
}

static void test_json_roundtrip(void)
{
    pc_algo_camera_raw p = def(), q;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_doc *back;
    char *text;
    size_t len;
    p.exposure = 0.75f;
    p.grade_midtones = (pc_algo_cr_wheel){10.0f, 20.0f, -5.0f};
    p.vignette_style = malloc(13);
    memcpy(p.vignette_style, "paintOverlay", 13);
    p.seed = 77;
    yyjson_mut_doc_set_root(d, pc_algo_camera_raw_to_json(d, &p));
    text = yyjson_mut_write(d, 0, &len);
    CHECK(text != NULL && strstr(text, "\"vignetteStyle\":\"paintOverlay\"") != NULL);
    CHECK(text != NULL && strncmp(text, "{\"temperature\":", 15) == 0);
    back = yyjson_read(text, len, 0);
    CHECK(pc_algo_camera_raw_from_json(yyjson_doc_get_root(back), &q, NULL) == PC_OK);
    CHECK(pc_algo_camera_raw_eq(&p, &q));
    pc_algo_camera_raw_fini(&q);
    yyjson_doc_free(back);
    free(text);
    yyjson_mut_doc_free(d);
    pc_algo_camera_raw_fini(&p);
    /* serde errors */
    {
        pc_error e;
        yyjson_doc *bad = yyjson_read("{\"exposure\": \"x\"}", 17, 0);
        CHECK(pc_algo_camera_raw_from_json(yyjson_doc_get_root(bad), &q, &e) == PC_ERR_INVALID);
        CHECK_EQ_STR(e.msg, "invalid type: string \"x\", expected f32");
        yyjson_doc_free(bad);
    }
}

static void test_hostile_settings_never_crash(void)
{
    const size_t w = 24, h = 16, n = w * h;
    float (*base)[4] = img(w, h), (*a)[4];
    pc_algo_camera_raw p;
    float vals[4] = {NAN, INFINITY, -INFINITY, 1e30f};
    size_t i;
    int k, f;
    /* Every float control at NaN / +-inf / huge: finite statuses, no crash. */
    for (k = 0; k < 4; k++) {
        for (f = 0; f < 40; f++) {
            pc_status st;
            float *fields[40];
            p = def();
            fields[0] = &p.temperature, fields[1] = &p.tint, fields[2] = &p.exposure, fields[3] = &p.contrast;
            fields[4] = &p.highlights, fields[5] = &p.shadows, fields[6] = &p.whites, fields[7] = &p.blacks;
            fields[8] = &p.texture, fields[9] = &p.clarity, fields[10] = &p.dehaze, fields[11] = &p.vibrance;
            fields[12] = &p.saturation, fields[13] = &p.curve_highlights, fields[14] = &p.curve_lights;
            fields[15] = &p.curve_darks, fields[16] = &p.curve_shadows, fields[17] = &p.curve_splits[1];
            fields[18] = &p.hsl_hue[2], fields[19] = &p.hsl_sat[3], fields[20] = &p.hsl_lum[4];
            fields[21] = &p.grade_shadows.sat, fields[22] = &p.grade_global.lum,
            fields[23] = &p.grade_blending;
            fields[24] = &p.grade_balance, fields[25] = &p.sharpen_amount, fields[26] = &p.sharpen_radius;
            fields[27] = &p.sharpen_detail, fields[28] = &p.sharpen_masking, fields[29] = &p.noise_luminance;
            fields[30] = &p.noise_luminance_detail, fields[31] = &p.noise_color,
            fields[32] = &p.noise_color_detail;
            fields[33] = &p.grain_amount, fields[34] = &p.grain_size, fields[35] = &p.grain_roughness;
            fields[36] = &p.vignette_amount, fields[37] = &p.vignette_roundness, fields[38] = &p.pixel_scale;
            fields[39] = &p.grade_midtones.hue;
            *fields[f] = vals[k];
            /* make the neighbouring stages run too */
            if (f >= 26 && f <= 28) {
                p.sharpen_amount = 50.0f;
            }
            if (f == 30) {
                p.noise_luminance = 20.0f;
            }
            if (f == 32) {
                p.noise_color = 20.0f;
            }
            if (f >= 34 && f <= 35) {
                p.grain_amount = 20.0f;
            }
            if (f == 37 || f == 38) {
                p.vignette_amount = -40.0f;
            }
            a = dup((const float (*)[4])base, n);
            st = pc_algo_camera_raw_develop(a, n, w, h, &p, k == 3, NULL);
            CHECK(st == PC_OK || st == PC_ERR_INVALID || st == PC_ERR_LIMIT);
            free(a);
        }
    }
    /* Specific refusals. */
    p = def(), p.noise_luminance = 1e30f;
    a = dup((const float (*)[4])base, n);
    CHECK(pc_algo_camera_raw_develop(a, n, w, h, &p, false, NULL) == PC_ERR_LIMIT);
    CHECK(memcmp(a, base, n * sizeof *a) == 0);
    p = def(), p.sharpen_amount = 50.0f, p.sharpen_detail = -50.0f;
    CHECK(pc_algo_camera_raw_develop(a, n, w, h, &p, false, NULL) == PC_ERR_INVALID);
    p = def(), p.exposure = 1.0f;
    CHECK(pc_algo_camera_raw_develop(a, n - 1, w, h, &p, false, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_camera_raw_develop(NULL, 0, 0, 0, &p, false, NULL) == PC_OK);
    free(a);
    /* Tiny and degenerate shapes with every stage on. */
    for (i = 1; i <= 3; i++) {
        float (*t)[4] = img(i, 1);
        p = def();
        p.exposure = 0.3f, p.dehaze = 40.0f, p.clarity = 50.0f, p.texture = 50.0f, p.highlights = -40.0f;
        p.vibrance = 20.0f, p.curve_lights = 30.0f, p.hsl_hue[0] = 20.0f, p.grade_global.sat = 30.0f;
        p.sharpen_amount = 60.0f, p.sharpen_masking = 40.0f, p.noise_luminance = 50.0f, p.noise_color = 50.0f;
        p.grain_amount = 30.0f, p.vignette_amount = 30.0f;
        CHECK(pc_algo_camera_raw_develop(t, i, i, 1, &p, false, NULL) == PC_OK);
        CHECK(pc_algo_camera_raw_develop(t, i, 1, i, &p, false, NULL) == PC_OK);
        free(t);
    }
    free(base);
}

int main(int argc, char **argv)
{
    RUN(test_new_curves_are_validated_but_saved_curves_still_load_and_render);
    RUN(test_validation_messages);
    RUN(test_defaults_are_identity);
    RUN(test_basic_panel_moves_the_right_way);
    RUN(test_curves_hsl_grading_detail_effects);
    RUN(test_float_documents_keep_overrange);
    RUN(test_exposure_of_an_eight_bit_gradient_reproduces_the_quantization_comb);
    RUN(test_json_roundtrip);
    RUN(test_hostile_settings_never_crash);
    return pc_t_finish();
}
