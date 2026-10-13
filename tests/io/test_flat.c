/* Flat formats through io (crates/io/tests): flat.rs (all but the two PSD tests, which need I2
 * and I3), modes.rs, color.rs (PNG/JPEG profiles, CMYK -> sRGB; the PSD round trip waits for
 * I2/I3), orientation.rs (all but the PSD half of the stale-orientation test), resolution_axes.rs
 * (the PNG tests), banded.rs (all but the merged-image test, which needs I3), xmp_export.rs (the
 * flat and .pcraft parts), tiff_pages.rs, tiff_layers.rs (the four flat-TIFF tests),
 * heif_unsupported.rs and heif.rs (corpus/heif; skipped when absent or built without HEIF). */
#include "harness.h"

#include "docs.h"
#include "base/num.h"
#include "cms/transform.h"
#include "vector/vector.h"

static pc_io_import_result imp(const char *name, const uint8_t *b, size_t n)
{
    pc_io_import_result r;
    pc_error e;
    if (pc_io_import(name, b, n, &r, &e) != PC_OK) {
        (void)fprintf(stderr, "import %s: %s\n", name, e.msg);
        abort();
    }
    return r;
}

static pc_io_export_result exp_(const pc_document *d, const char *name, const pc_io_export_opts *o)
{
    pc_io_export_result r;
    pc_error e;
    if (pc_io_export(d, name, o, &r, &e) != PC_OK) {
        (void)fprintf(stderr, "export %s: %s\n", name, e.msg);
        abort();
    }
    return r;
}

static float *region(const pc_document *d, size_t *n)
{
    float *v = NULL;
    must(pc_surface_read_region(pc_layer_surface(&d->layers[0]), pc_document_bounds(d), &v, n, NULL));
    return v;
}

static float pixels_diff(const pc_document *a, const pc_document *b)
{
    size_t na = 0;
    size_t nb = 0;
    float *va = region(a, &na);
    float *vb = region(b, &nb);
    float m = 0;
    if (na != nb) {
        m = INFINITY;
    }
    for (size_t i = 0; i < na && i < nb; i++) {
        float d = fabsf(va[i] - vb[i]);
        m = d > m ? d : m;
    }
    free(va);
    free(vb);
    return m;
}

static void codec_rt(const char *ext, pc_color_mode mode, pc_sample_type depth, bool alpha, float tol)
{
    pc_document d = t_single(mode, depth, alpha);
    pc_io_export_result r = exp_(&d, ext, NULL);
    char name[16];
    pc_io_import_result back;
    (void)snprintf(name, sizeof name, "x.%s", ext);
    back = imp(name, r.bytes, r.len);
    CHECK(back.document.size.width == d.size.width && back.document.size.height == d.size.height);
    CHECK(back.document.mode == mode);
    CHECK(back.document.depth == depth);
    CHECK(pixels_diff(&d, &back.document) <= tol);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static void test_codec_roundtrips(void)
{
    codec_rt("png", PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true, 0);
    codec_rt("png", PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false, 0);
    codec_rt("png", PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true, 0);
    codec_rt("png", PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false, 0);
    codec_rt("png", PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, true, 0);
    /* A 32-bit Targa's alpha opens as an "Alpha 1" channel, not layer transparency, so an
     * alpha round trip no longer holds pixel-for-pixel; only the opaque cases are tested. */
    codec_rt("tga", PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false, 0);
    codec_rt("tga", PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false, 0);
    codec_rt("tiff", PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true, 0);
    codec_rt("tif", PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, false, 0);
    codec_rt("tiff", PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, false, 0);
    codec_rt("tiff", PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U16, true, 0);
    codec_rt("tiff", PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false, 0);
    codec_rt("fits", PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false, 0);
    codec_rt("fit", PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false, 0);
    codec_rt("fits", PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, false, 0);
}

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* EXR stores linear light: a linear document round-trips exactly; an sRGB one is linearised on
 * export and comes back tagged linear sRGB with the same colours. */
static void test_exr_rgba32(void)
{
    pc_document d;
    pc_io_export_result r;
    pc_io_import_result back;
    float a[4], b[4];
    if (!pc_cformat_caps(PC_CF_EXR).write) {
        (void)printf("  (skipped: built without OpenEXR)\n");
        return;
    }
    d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true);
    t_set_icc(&d, PC_BUILTIN_LINEAR_SRGB);
    r = exp_(&d, "exr", NULL);
    back = imp("x.exr", r.bytes, r.len);
    CHECK(back.document.mode == d.mode && back.document.depth == d.depth);
    CHECK(pc_blob_eq(back.document.icc_profile, d.icc_profile));
    CHECK(pixels_diff(&d, &back.document) == 0.0f);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
    d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true);
    r = exp_(&d, "exr", NULL);
    back = imp("x.exr", r.bytes, r.len);
    pc_surface_read_pixel(pc_layer_surface(&d.layers[0]), 2, 3, a);
    pc_surface_read_pixel(pc_layer_surface(&back.document.layers[0]), 2, 3, b);
    for (int c = 0; c < 3; c++) {
        CHECK(fabsf(srgb_to_linear(a[c]) - b[c]) < 1e-4f);
    }
    CHECK(fabsf(a[3] - b[3]) < 1e-6f);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static pc_document smooth(pc_color_mode mode)
{
    pc_document d = t_doc("s", 32, 16, mode, PC_SAMPLE_TYPE_U8);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    size_t n = pc_pixel_format_channels(fmt);
    float *v = xmalloc(32 * 16 * n * sizeof *v);
    size_t k = 0;
    pc_surface s;
    pc_layer l;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 32; x++) {
            for (size_t c = 0; c + 1 < n; c++) {
                float f = (float)(x * 4 + y * 2 + (int)c * 20) / 255.0f;
                v[k++] = f < 1.0f ? f : 1.0f;
            }
            v[k++] = 1.0f;
        }
    }
    pc_surface_init(&s, fmt);
    must(pc_surface_write_region(&s, pc_document_bounds(&d), v, k, NULL));
    must(pc_io_raster_layer("Background", &s, &l, NULL));
    pc_surface_fini(&s);
    t_push(&d, &l);
    free(v);
    return d;
}

static void jpeg_rt(pc_color_mode mode, const char *ext)
{
    pc_document d = smooth(mode);
    pc_io_export_result r = exp_(&d, ext, NULL);
    pc_io_import_result back;
    size_t na = 0, nb = 0;
    float *a;
    float *b;
    double sum = -0.0;
    CHECK(pc_io_warnings_any(&r.warnings, "lossy"));
    back = imp("x.jpg", r.bytes, r.len);
    CHECK(back.document.mode == mode);
    a = region(&d, &na);
    b = region(&back.document, &nb);
    CHECK(na == nb);
    for (size_t i = 0; i < na && i < nb; i++) {
        sum += fabs((double)a[i] - (double)b[i]);
    }
    CHECK(sum / (double)na < 0.02);
    free(a);
    free(b);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static void test_jpeg_rgb8_and_gray8(void)
{
    jpeg_rt(PC_COLOR_MODE_RGB, "jpg");
    jpeg_rt(PC_COLOR_MODE_GRAYSCALE, "jpeg");
}

static void test_metadata_roundtrip_png(void)
{
    pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    uint8_t icc[128] = {0};
    pc_io_export_opts none = pc_io_export_opts_default();
    pc_io_export_result r;
    pc_io_import_result back;
    /* Arbitrary bytes are fine for PNG iCCP (the codec does not validate). */
    memcpy(icc + 36, "acsp", 4);
    d.resolution_dpi = 300.0f;
    must(pc_io_set_blob(&d.icc_profile, icc, sizeof icc, NULL));
    t_set_xmp(&d, "<x:xmpmeta xmlns:x='adobe:ns:meta/'/>");
    /* DPI and ICC always travel with the file; XMP unless Export As asks for none (#647). */
    none.xmp = PC_IO_XMP_NONE;
    r = exp_(&d, "png", &none);
    back = imp("x.png", r.bytes, r.len);
    CHECK(fabsf(back.document.resolution_dpi - 300.0f) < 1.0f);
    CHECK(pc_blob_eq(back.document.icc_profile, d.icc_profile));
    CHECK(!back.document.metadata.has_xmp);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    r = exp_(&d, "png", NULL);
    back = imp("x.png", r.bytes, r.len);
    CHECK(back.document.metadata.has_xmp && pc_str_eq(&back.document.metadata.xmp, &d.metadata.xmp));
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static float max_diff(const float (*a)[4], const float (*b)[4], size_t n)
{
    float m = 0;
    for (size_t i = 0; i < n; i++) {
        if (a[i][3] <= 0 && b[i][3] <= 0) {
            continue;
        }
        for (int c = 0; c < 4; c++) {
            float x = c < 3 ? a[i][c] * a[i][3] : a[i][c];
            float y = c < 3 ? b[i][c] * b[i][3] : b[i][c];
            float dd = fabsf(x - y);
            m = dd > m ? dd : m;
        }
    }
    return m;
}

static void test_layered_to_png_warns_flatten(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_io_export_result r = exp_(&d, "out.png", NULL);
    pc_io_import_result back;
    pc_compose_buffer flat;
    float *px = NULL;
    size_t n = 0;
    CHECK(pc_io_warnings_any(&r.warnings, "flattened"));
    back = imp("out.png", r.bytes, r.len);
    must(pc_compose_flatten(&d, &flat, NULL));
    must(pc_surface_read_region(pc_layer_surface(&back.document.layers[0]),
                                pc_document_bounds(&back.document), &px, &n, NULL));
    CHECK_EQ_UINT(n, flat.len * 4);
    CHECK(max_diff((const float (*)[4])px, (const float (*)[4])flat.px, flat.len) <= 1.0f / 255.0f + 1e-4f);
    free(px);
    pc_compose_buffer_fini(&flat);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static void test_export_warnings(void)
{
    pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, false);
    pc_io_export_result r = exp_(&d, "a.jpg", NULL);
    bool bit = false;
    for (size_t i = 0; i < r.warnings.n; i++) {
        bit = bit || strstr(r.warnings.v[i], "bit") != NULL;
    }
    CHECK(bit); /* sixteen_bit_to_jpeg_warns_depth */
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
    d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true);
    r = exp_(&d, "a.jpg", NULL);
    CHECK(r.warnings.n >= 2); /* alpha_to_jpeg_warns */
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
    d = t_gen_doc(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8);
    r = exp_(&d, "a.png", NULL);
    CHECK(pc_io_warnings_any(&r.warnings, "RGB")); /* cmyk_layered_to_png_warns_conversion */
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static void test_unknown_extension_and_garbage(void)
{
    pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    pc_io_export_result r;
    pc_io_import_result i;
    pc_error e;
    pc_status st = pc_io_export(&d, "a.xyz", NULL, &r, &e);
    CHECK(st == PC_ERR_NOT_FOUND && pc_io_error_kind(st, &e) == PC_IO_ERR_UNKNOWN_FORMAT);
    CHECK_EQ_STR(e.msg, "unknown file format: a.xyz");
    st = pc_io_import("x", (const uint8_t *)"definitely not an image", 23, &i, &e);
    CHECK(st != PC_OK);
    st = pc_io_import("x.psd", (const uint8_t *)"8BPS\0\x01", 6, &i, &e);
    CHECK(st != PC_OK && pc_io_error_kind(st, &e) == PC_IO_ERR_PSD);
    pc_document_fini(&d);
}

static void test_extension_forms(void)
{
    static const char *forms[4] = {"png", ".png", "a/b/c.PNG", "C:\\x\\y.png"};
    pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    for (int i = 0; i < 4; i++) {
        pc_io_export_result r = exp_(&d, forms[i], NULL);
        CHECK(r.len >= 4 && memcmp(r.bytes, "\x89PNG", 4) == 0);
        pc_io_export_result_fini(&r);
    }
    pc_document_fini(&d);
}

static void test_background_lock_follows_alpha(void)
{
    for (int alpha = 0; alpha < 2; alpha++) {
        pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, alpha != 0);
        pc_io_export_result r = exp_(&d, "png", NULL);
        pc_io_import_result back = imp("a.png", r.bytes, r.len);
        CHECK(back.document.layers[0].locks.transparency == (alpha == 0));
        pc_io_import_result_fini(&back);
        pc_io_export_result_fini(&r);
        pc_document_fini(&d);
    }
}

/* The pixel at the centre of each 16-pixel column. */
static void column_pixel(const pc_document *d, size_t i, float out[8])
{
    pc_surface_read_pixel(pc_layer_surface(&d->layers[0]), 16 * (int32_t)i + 8, 8, out);
}

static bool colors_close(const float *got, const float *want, size_t n, float tol)
{
    for (size_t c = 0; c < n; c++) {
        if (!(fabsf(got[c] - want[c]) <= tol)) {
            return false;
        }
    }
    return true;
}

/* Formats without alpha get the document composited over white instead of its alpha dropped. */
static void test_transparency_is_composited_over_white_for_formats_without_alpha(void)
{
    static const float rgb[12] = {0, 0, 0, 0, 0, 0, 1, 0.5f, 0, 0.63f, 0, 1};
    static const float rgb_want[9] = {1, 1, 1, 0.5f, 0.5f, 1, 0, 0.63f, 0};
    static const float gray[4] = {0, 0, 0, 0.5f};
    static const float gray_want[2] = {1, 0.5f};
    static const float cmyk[10] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 0.5f};
    static const float cmyk_want[8] = {0, 0, 0, 0, 0, 0, 0, 0.5f};
    struct {
        pc_color_mode mode;
        pc_sample_type depth;
        const float *cols;
        size_t ncols;
        size_t ch;
        const float *want;
    } cases[5] = {{PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, rgb, 3, 4, rgb_want},
                  {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, rgb, 3, 4, rgb_want},
                  {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, rgb, 3, 4, rgb_want},
                  {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, gray, 2, 2, gray_want},
                  {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, cmyk, 2, 5, cmyk_want}};
    for (size_t k = 0; k < 5; k++) {
        pc_document d = t_columns(cases[k].mode, cases[k].depth, cases[k].cols, cases[k].ncols, cases[k].ch);
        pc_io_export_result r = exp_(&d, "a.jpg", NULL);
        pc_io_import_result back;
        float px[8];
        size_t cc = cases[k].ch - 1;
        CHECK(pc_io_warnings_any(&r.warnings, "composited over white"));
        CHECK(!pc_io_warnings_any(&r.warnings, "alpha will be discarded"));
        back = imp("a.jpg", r.bytes, r.len);
        CHECK(back.document.mode == cases[k].mode);
        for (size_t i = 0; i < cases[k].ncols; i++) {
            column_pixel(&back.document, i, px);
            CHECK(colors_close(px, cases[k].want + i * cc, cc, 0.03f));
        }
        pc_io_import_result_fini(&back);
        pc_io_export_result_fini(&r);
        /* PNG keeps the transparency itself. */
        if (cases[k].mode != PC_COLOR_MODE_CMYK) {
            r = exp_(&d, "a.png", NULL);
            CHECK(!pc_io_warnings_any(&r.warnings, "composited"));
            back = imp("a.png", r.bytes, r.len);
            column_pixel(&back.document, 1, px);
            CHECK(fabsf(px[cc] - 0.5f) <= 1.0f / 255.0f);
            pc_io_import_result_fini(&back);
            pc_io_export_result_fini(&r);
        } else {
            /* PNG can't hold CMYK: converted to RGB, the alpha kept. */
            r = exp_(&d, "a.png", NULL);
            CHECK(!pc_io_warnings_any(&r.warnings, "composited"));
            back = imp("a.png", r.bytes, r.len);
            column_pixel(&back.document, 1, px);
            CHECK(fabsf(px[3] - 0.5f) <= 1.0f / 255.0f);
            pc_io_import_result_fini(&back);
            pc_io_export_result_fini(&r);
        }
        pc_document_fini(&d);
    }
    {
        static const float opaque[4] = {0.2f, 0.4f, 0.6f, 1};
        pc_document d = t_columns(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, opaque, 1, 4);
        pc_io_export_result r = exp_(&d, "a.jpg", NULL);
        CHECK(!pc_io_warnings_any(&r.warnings, "composited"));
        pc_io_export_result_fini(&r);
        pc_document_fini(&d);
    }
}

/* Formats that can't embed a profile get sRGB values, converted through the colour engine. */
static void test_non_srgb_rgb_is_converted_to_srgb_for_formats_without_a_profile(void)
{
    static const float cols[12] = {0, 0.05f, 0, 1, 0.2f, 0.5f, 0.8f, 1, 1, 0.25f, 0, 1};
    static const char *exts[4] = {"bmp", "gif", "qoi", "tga"};
    struct {
        pc_builtin b;
        pc_sample_type depth;
    } cases[3] = {{PC_BUILTIN_LINEAR_SRGB, PC_SAMPLE_TYPE_F32},
                  {PC_BUILTIN_LINEAR_SRGB, PC_SAMPLE_TYPE_U16},
                  {PC_BUILTIN_DISPLAY_P3, PC_SAMPLE_TYPE_U8}};
    for (size_t k = 0; k < 3; k++) {
        pc_document d = t_columns(PC_COLOR_MODE_RGB, cases[k].depth, cols, 3, 4);
        float stored[3][8];
        float want[3][4];
        pc_transform *t = NULL;
        pc_io_export_result r;
        pc_io_import_result back;
        t_set_icc(&d, cases[k].b);
        for (size_t i = 0; i < 3; i++) {
            column_pixel(&d, i, stored[i]);
            memcpy(want[i], stored[i], sizeof want[i]);
        }
        must(pc_transform_new(pc_builtin_profile(cases[k].b), pc_builtin_profile(PC_BUILTIN_SRGB),
                              PC_INTENT_PERCEPTUAL, true, &t, NULL));
        must(pc_transform_apply(t, &want[0][0], 12, 4, NULL));
        pc_transform_release(t);
        for (size_t i = 0; i < 3; i++) {
            for (int c = 0; c < 4; c++) {
                want[i][c] = pc_clampf(want[i][c], 0, 1); /* the formats hold 8-bit sRGB */
            }
        }
        for (size_t x = 0; x < 4; x++) {
            char name[16];
            float px[8];
            r = exp_(&d, exts[x], NULL);
            CHECK(pc_io_warnings_any(&r.warnings, "converted to sRGB"));
            CHECK(!pc_io_warnings_any(&r.warnings, "ICC profile"));
            (void)snprintf(name, sizeof name, "a.%s", exts[x]);
            back = imp(name, r.bytes, r.len);
            CHECK(back.document.icc_profile == NULL);
            for (size_t i = 0; i < 3; i++) {
                column_pixel(&back.document, i, px);
                CHECK(colors_close(px, want[i], 3, 1.5f / 255.0f));
            }
            pc_io_import_result_fini(&back);
            pc_io_export_result_fini(&r);
        }
        /* Formats that embed the profile keep it and the values. */
        r = exp_(&d, "a.png", NULL);
        back = imp("a.png", r.bytes, r.len);
        CHECK(pc_blob_eq(back.document.icc_profile, d.icc_profile));
        for (size_t i = 0; i < 3; i++) {
            float px[8];
            column_pixel(&back.document, i, px);
            CHECK(colors_close(px, stored[i], 3, 1e-4f));
        }
        pc_io_import_result_fini(&back);
        pc_io_export_result_fini(&r);
        pc_document_fini(&d);
    }
    /* sRGB (tagged or not) is written unchanged. */
    for (int tagged = 0; tagged < 2; tagged++) {
        pc_document d = t_columns(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, cols, 3, 4);
        pc_io_export_result r;
        pc_io_import_result back;
        if (tagged) {
            t_set_icc(&d, PC_BUILTIN_SRGB);
        }
        r = exp_(&d, "a.bmp", NULL);
        CHECK(!pc_io_warnings_any(&r.warnings, "converted to sRGB"));
        back = imp("a.bmp", r.bytes, r.len);
        for (size_t i = 0; i < 3; i++) {
            float a[8], b[8];
            column_pixel(&d, i, a);
            column_pixel(&back.document, i, b);
            CHECK(colors_close(b, a, 3, 0));
        }
        pc_io_import_result_fini(&back);
        pc_io_export_result_fini(&r);
        pc_document_fini(&d);
    }
}

/* #518: a JPEG cut off inside its image data opens with a warning, never silently. */
static void test_truncated_jpeg_imports_with_a_warning(void)
{
    static const pc_color_mode modes[2] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE};
    for (int m = 0; m < 2; m++) {
        pc_document d = smooth(modes[m]);
        pc_io_export_result full = exp_(&d, "jpg", NULL);
        pc_io_import_result r = imp("x.jpg", full.bytes, full.len);
        CHECK_EQ_UINT(r.warnings.n, 0);
        pc_io_import_result_fini(&r);
        r = imp("x.jpg", full.bytes, full.len - 4);
        CHECK(r.document.size.width == 32 && r.document.size.height == 16);
        CHECK(r.warnings.n > 0 && strncmp(r.warnings.v[0], "JPEG data ends early", 20) == 0);
        pc_io_import_result_fini(&r);
        pc_io_export_result_fini(&full);
        pc_document_fini(&d);
    }
}

static void gif_frames(size_t frames, pc_buf *b)
{
    must(pc_buf_append(b, "GIF89a\x01\0\x01\0\x80\0\0\0\0\0\xFF\xFF\xFF", 19, NULL));
    for (size_t i = 0; i < frames; i++) {
        must(pc_buf_append(b, "\x2C\0\0\0\0\x01\0\x01\0\0\x02\x02\x44\x01\0", 15, NULL));
    }
    must(pc_buf_append_byte(b, 0x3B, NULL));
}

/* #523: an animation opens its first frame, unchanged, with a warning that the rest was left out. */
static void test_animation_imports_the_first_frame_with_a_warning(void)
{
    pc_buf one = PC_BUF_INIT;
    pc_buf three = PC_BUF_INIT;
    pc_io_import_result a;
    pc_io_import_result b;
    gif_frames(1, &one);
    gif_frames(3, &three);
    a = imp("a.gif", one.data, one.len);
    b = imp("a.gif", three.data, three.len);
    CHECK_EQ_UINT(a.warnings.n, 0);
    CHECK(b.warnings.n == 1 && strcmp(b.warnings.v[0], "only the first of 3 frames was imported") == 0);
    CHECK(pixels_diff(&a.document, &b.document) == 0.0f);
    pc_io_import_result_fini(&a);
    pc_io_import_result_fini(&b);
    pc_buf_fini(&one);
    pc_buf_fini(&three);
}

/* ---- modes.rs ---- */

static void test_indexed_png_is_palette_png(void)
{
    pc_document d;
    pc_io_export_result r;
    pc_io_import_result back;
    float px[4];
    bool plte = false;
    must(t_export_doc("indexed", &d) ? PC_OK : PC_ERR_INVALID);
    r = exp_(&d, "x.png", NULL);
    /* IHDR colour type 3 = indexed; a PLTE chunk follows. */
    CHECK(r.len > 25 && r.bytes[25] == 3);
    for (size_t i = 0; i + 4 <= r.len; i++) {
        plte = plte || memcmp(r.bytes + i, "PLTE", 4) == 0;
    }
    CHECK(plte);
    back = imp("x.png", r.bytes, r.len);
    pc_surface_rgba(pc_layer_surface(&back.document.layers[0]), 1, 1, px);
    CHECK(px[0] == 1.0f && px[1] == 0.0f && px[2] == 0.0f);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    /* Other formats keep the expanded pixels. */
    r = exp_(&d, "x.tif", NULL);
    CHECK(r.len > 0);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static void test_duotone_exports_the_inks_as_rgb(void)
{
    pc_document d;
    pc_io_export_result r;
    pc_io_import_result back;
    float px[4];
    must(t_export_doc("duotone", &d) ? PC_OK : PC_ERR_INVALID);
    r = exp_(&d, "x.png", NULL);
    CHECK(pc_io_warnings_any(&r.warnings, "Duotone"));
    back = imp("x.png", r.bytes, r.len);
    pc_surface_rgba(pc_layer_surface(&back.document.layers[0]), 0, 0, px);
    CHECK(px[0] > px[2] + 0.05f); /* warm */
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

/* ---- color.rs ---- */

static void test_png_and_jpeg_embed_and_extract_icc(void)
{
    static const pc_builtin bs[4] = {PC_BUILTIN_SRGB, PC_BUILTIN_DISPLAY_P3, PC_BUILTIN_ADOBE_RGB_COMPAT,
                                     PC_BUILTIN_PROPHOTO_COMPAT};
    for (size_t k = 0; k < 4; k++) {
        for (int j = 0; j < 2; j++) {
            const char *name = j == 0 ? "x.png" : "x.jpg";
            pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, j == 0);
            pc_io_export_result r;
            pc_io_import_result back;
            pc_profile p;
            t_set_icc(&d, bs[k]);
            r = exp_(&d, name, NULL);
            back = imp(name, r.bytes, r.len);
            CHECK(pc_blob_eq(back.document.icc_profile, d.icc_profile));
            must(pc_profile_parse(pc_blob_data(back.document.icc_profile),
                                  pc_blob_len(back.document.icc_profile), &p, NULL));
            CHECK_EQ_STR(p.description, pc_builtin_description(bs[k]));
            pc_profile_fini(&p);
            pc_io_import_result_fini(&back);
            pc_io_export_result_fini(&r);
            pc_document_fini(&d);
        }
    }
    {
        /* Gray PNG with a gray profile. */
        pc_document d = t_single(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false);
        pc_io_export_result r;
        pc_io_import_result back;
        t_set_icc(&d, PC_BUILTIN_GRAY_GAMMA22);
        r = exp_(&d, "g.png", NULL);
        back = imp("g.png", r.bytes, r.len);
        CHECK(pc_blob_eq(back.document.icc_profile, d.icc_profile));
        pc_io_import_result_fini(&back);
        pc_io_export_result_fini(&r);
        pc_document_fini(&d);
    }
}

static void test_cmyk_to_png_is_colour_managed_and_tagged_srgb(void)
{
    pc_document d = t_single(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, false);
    pc_io_export_result r;
    pc_io_import_result back;
    pc_profile p;
    t_set_icc(&d, PC_BUILTIN_COATED_CMYK);
    r = exp_(&d, "c.png", NULL);
    CHECK(pc_io_warnings_any(&r.warnings, "sRGB"));
    back = imp("c.png", r.bytes, r.len);
    CHECK(back.document.mode == PC_COLOR_MODE_RGB);
    CHECK(back.document.icc_profile != NULL);
    must(pc_profile_parse(pc_blob_data(back.document.icc_profile), pc_blob_len(back.document.icc_profile), &p,
                          NULL));
    CHECK_EQ_STR(p.description, pc_builtin_profile(PC_BUILTIN_SRGB)->description);
    pc_profile_fini(&p);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
    /* Lab documents never write Lab numbers as RGB. */
    d = t_single(PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U8, false);
    r = exp_(&d, "l.png", NULL);
    back = imp("l.png", r.bytes, r.len);
    must(pc_profile_parse(pc_blob_data(back.document.icc_profile), pc_blob_len(back.document.icc_profile), &p,
                          NULL));
    CHECK(p.space == PC_CS_RGB);
    pc_profile_fini(&p);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

/* ---- resolution_axes.rs (PNG) ---- */

static size_t resolution_notes(const pc_io_import_result *r, const char **first)
{
    size_t n = 0;
    for (size_t i = 0; i < r->warnings.n; i++) {
        if (strstr(r->warnings.v[i], "vertical resolution") != NULL) {
            if (n++ == 0) {
                *first = r->warnings.v[i];
            }
        }
    }
    return n;
}

static void png_dpi(float x, float y, pc_buf *out)
{
    pc_cimage img;
    pc_cencode_opts o = pc_cencode_opts_default();
    uint8_t px[12];
    memset(px, 10, sizeof px);
    must(pc_cimage_from_copy(2, 2, PC_CL_RGB, PC_CS_U8, px, sizeof px, &img, NULL));
    img.meta.has_dpi = true;
    img.meta.dpi_x = x;
    img.meta.dpi_y = y;
    must(pc_cencode(&img, PC_CF_PNG, &o, out, NULL));
    pc_cimage_fini(&img);
}

static void test_png_resolution_axes(void)
{
    pc_buf b = PC_BUF_INIT;
    pc_io_import_result r;
    const char *note = NULL;
    png_dpi(300, 150, &b);
    r = imp("x.png", b.data, b.len);
    CHECK(fabsf(r.document.resolution_dpi - 300.0f) < 0.01f);
    CHECK_EQ_UINT(resolution_notes(&r, &note), 1);
    CHECK(note != NULL && strstr(note, "150.01") != NULL && strstr(note, "300.00") != NULL);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&b);
    png_dpi(300, 300, &b);
    r = imp("x.png", b.data, b.len);
    CHECK(fabsf(r.document.resolution_dpi - 300.0f) < 0.01f);
    CHECK_EQ_UINT(r.warnings.n, 0);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&b);
}

/* ---- banded.rs ---- */

static pc_document layered(uint32_t w, uint32_t h, pc_sample_type depth, float alpha)
{
    pc_document d = t_doc("b", w, h, PC_COLOR_MODE_RGB, depth);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_surface bg;
    float *row = xmalloc((size_t)w * 4 * sizeof *row);
    pc_layer l;
    float red[4] = {1, 0, 0, 1};
    pc_surface_init(&bg, fmt);
    for (int32_t y = 0; y < (int32_t)h; y++) {
        for (int32_t x = 0; x < (int32_t)w; x++) {
            float a = x > (int32_t)w / 2 && y < (int32_t)h / 2 ? alpha : 1.0f;
            row[x * 4] = (float)x / (float)w;
            row[x * 4 + 1] = (float)y / (float)h;
            row[x * 4 + 2] = 0.5f;
            row[x * 4 + 3] = a;
        }
        must(pc_surface_write_region(&bg, pc_rect_make(0, y, (int32_t)w, y + 1), row, (size_t)w * 4, NULL));
    }
    must(pc_io_raster_layer("bg", &bg, &l, NULL));
    pc_surface_fini(&bg);
    t_push(&d, &l);
    must(pc_layer_raster(&l, "top", fmt, NULL));
    must(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(10, 10, (int32_t)w - 10, 40), red, 4,
                              NULL));
    l.opacity = 0.5f;
    t_push(&d, &l);
    free(row);
    return d;
}

static void test_layered_png_export_spans_bands(void)
{
    /* 3000 x 3000 = 9 MP: two bands. */
    static const pc_sample_type depths[2] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16};
    static const float scales[2] = {255.0f, 65535.0f};
    for (int k = 0; k < 2; k++) {
        for (int a = 0; a < 2; a++) {
            float alpha = a == 0 ? 1.0f : 0.5f;
            pc_document d = layered(3000, 3000, depths[k], alpha);
            pc_io_export_result r = exp_(&d, "x.png", NULL);
            pc_cimage img;
            float *got = NULL;
            size_t n = 0;
            pc_compose_buffer flat;
            bool same = true;
            must(pc_cdecode(r.bytes, r.len, &img, NULL));
            CHECK(pc_clayout_has_alpha(img.layout) == (alpha < 1.0f));
            must(pc_cimage_to_rgba_f32(&img, &got, &n, NULL));
            must(pc_compose_flatten(&d, &flat, NULL));
            CHECK_EQ_UINT(n, flat.len * 4);
            for (size_t i = 0; i < n && i < flat.len * 4 && same; i++) {
                float want = roundf(pc_clampf(flat.px[i / 4][i % 4], 0, 1) * scales[k]);
                same = roundf(got[i] * scales[k]) == want;
            }
            CHECK(same);
            free(got);
            pc_compose_buffer_fini(&flat);
            pc_cimage_fini(&img);
            pc_io_export_result_fini(&r);
            pc_document_fini(&d);
        }
    }
}

static void test_single_layer_native_export_strips_opaque_alpha(void)
{
    for (int a = 0; a < 2; a++) {
        float alpha = a == 0 ? 1.0f : 0.25f;
        pc_document d = layered(700, 300, PC_SAMPLE_TYPE_U16, alpha);
        pc_io_export_result r;
        pc_io_import_result back;
        pc_cimage img;
        pc_layer gone;
        must(pc_document_remove(&d, d.layers[1].id, &gone, NULL));
        pc_layer_fini(&gone);
        r = exp_(&d, "x.tiff", NULL);
        back = imp("x.tiff", r.bytes, r.len);
        CHECK(pixels_diff(&d, &back.document) == 0.0f);
        must(pc_cdecode(r.bytes, r.len, &img, NULL));
        CHECK(pc_clayout_has_alpha(img.layout) == (alpha < 1.0f));
        pc_cimage_fini(&img);
        pc_io_import_result_fini(&back);
        pc_io_export_result_fini(&r);
        pc_document_fini(&d);
    }
    {
        /* A fractional vector-mask edge exercises coverage rather than cropping raw samples. */
        pc_document d = layered(8, 4, PC_SAMPLE_TYPE_U16, 1.0f);
        pc_layer gone;
        pc_io_export_result r;
        pc_cimage img;
        float *got = NULL;
        size_t n = 0;
        pc_compose_buffer flat;
        bool same = true;
        must(pc_document_remove(&d, d.layers[1].id, &gone, NULL));
        pc_layer_fini(&gone);
        {
            pc_path p;
            must(pc_vec_shape_rect(0.0, 0.0, 4.5, 4.0, &p, NULL));
            pc_vector_mask_init(&d.layers[0].vector_mask, &p);
            d.layers[0].has_vector_mask = true;
        }
        r = exp_(&d, "x.png", NULL);
        must(pc_cdecode(r.bytes, r.len, &img, NULL));
        CHECK(img.sample == PC_CS_U16 && pc_clayout_has_alpha(img.layout));
        must(pc_cimage_to_rgba_f32(&img, &got, &n, NULL));
        must(pc_compose_flatten(&d, &flat, NULL));
        CHECK_EQ_UINT(n, flat.len * 4);
        for (size_t i = 0; i < n && i < flat.len * 4 && same; i++) {
            same = roundf(got[i] * 65535.0f) == roundf(pc_clampf(flat.px[i / 4][i % 4], 0, 1) * 65535.0f);
        }
        CHECK(same);
        free(got);
        pc_compose_buffer_fini(&flat);
        pc_cimage_fini(&img);
        pc_io_export_result_fini(&r);
        pc_document_fini(&d);
    }
}

static void test_flat_import_converts_rgb_in_bands(void)
{
    /* An opaque RGB file becomes an RGBA document (converted in ~32 MB bands: 36 MB here). */
    uint32_t w = 4000, h = 3000;
    size_t n = (size_t)w * h * 3;
    uint8_t *px = xmalloc(n);
    pc_cimage img;
    pc_buf png = PC_BUF_INIT;
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_io_import_result r;
    uint8_t *back = NULL;
    size_t blen = 0;
    bool same = true;
    for (size_t i = 0; i < n; i++) {
        px[i] = (uint8_t)(i % 251);
    }
    must(pc_cimage_from_copy(w, h, PC_CL_RGB, PC_CS_U8, px, n, &img, NULL));
    o.png_compression = PC_PNG_FAST;
    must(pc_cencode(&img, PC_CF_PNG, &o, &png, NULL));
    pc_cimage_fini(&img);
    r = imp("x.png", png.data, png.len);
    CHECK(pc_pixel_format_eq(pc_surface_format(pc_layer_surface(&r.document.layers[0])),
                             PC_PIXEL_FORMAT_RGBA8));
    must(pc_surface_to_interleaved(pc_layer_surface(&r.document.layers[0]), pc_document_bounds(&r.document),
                                   &back, &blen, NULL));
    CHECK_EQ_UINT(blen, (size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h && same; i++) {
        same = memcmp(back + i * 4, px + i * 3, 3) == 0 && back[i * 4 + 3] == 255;
    }
    CHECK(same);
    free(back);
    free(px);
    pc_buf_fini(&png);
    pc_io_import_result_fini(&r);
}

/* ---- xmp_export.rs ---- */

#define XMP_PACKET                                                                                           \
    "<x:xmpmeta "                                                                                            \
    "xmlns:x=\"adobe:ns:meta/\"><rdf:RDF><photoshop:DocumentAncestors><rdf:Bag><rdf:li>xmp.did:11111111-"    \
    "2222-3333-4444-555555555555</rdf:li></rdf:Bag></"                                                       \
    "photoshop:DocumentAncestors><photoshop:TextLayers><rdf:Bag>"                                            \
    "<rdf:li>Secret campaign copy</rdf:li></rdf:Bag></photoshop:TextLayers></rdf:RDF></x:xmpmeta>"

static pc_document doc_with_xmp(void)
{
    static const float px[4] = {0.5f, 0.4f, 0.3f, 1};
    pc_document d = t_columns(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, px, 1, 4);
    t_set_xmp(&d, XMP_PACKET);
    return d;
}

static bool contains_mem(const uint8_t *b, size_t n, const char *s)
{
    size_t k = strlen(s);
    for (size_t i = 0; i + k <= n; i++) {
        if (memcmp(b + i, s, k) == 0) {
            return true;
        }
    }
    return false;
}

static bool leaks_xmp(const pc_io_export_result *r)
{
    return contains_mem(r->bytes, r->len, "x:xmpmeta") ||
           contains_mem(r->bytes, r->len, "DocumentAncestors") ||
           contains_mem(r->bytes, r->len, "Secret campaign copy");
}

static void test_xmp_embedding(void)
{
    static const char *exts[4] = {"png", "jpg", "webp", "tif"};
    pc_document d = doc_with_xmp();
    pc_io_export_opts none = pc_io_export_opts_default();
    none.xmp = PC_IO_XMP_NONE;
    CHECK(pc_io_export_opts_default().xmp == PC_IO_XMP_ALL);
    for (int i = 0; i < 4; i++) {
        char name[16];
        pc_io_export_result r;
        (void)snprintf(name, sizeof name, "out.%s", exts[i]);
        r = exp_(&d, name, &none);
        CHECK(!leaks_xmp(&r)); /* metadata_none_embeds_no_xmp */
        pc_io_export_result_fini(&r);
        if (i < 3) {
            r = exp_(&d, name, NULL);
            CHECK(leaks_xmp(&r)); /* all_embeds_the_whole_packet_and_is_the_default */
            pc_io_export_result_fini(&r);
        }
    }
    /* layered_saves_keep_the_xmp_whatever_the_option: .pcraft here (PSD needs I3). */
    for (int k = 0; k < 2; k++) {
        pc_io_export_result r = exp_(&d, "out.pcraft", k == 0 ? NULL : &none);
        pc_io_import_result back = imp("out.pcraft", r.bytes, r.len);
        CHECK(back.document.metadata.has_xmp && pc_str_eq_c(&back.document.metadata.xmp, XMP_PACKET));
        pc_io_import_result_fini(&back);
        pc_io_export_result_fini(&r);
    }
    pc_document_fini(&d);
}

/* ---- tiff_pages.rs ---- */

static void tiff_pages(bool big, const uint32_t (*pages)[5], size_t n, pc_buf *f)
{
    size_t next_field = big ? 8 : 4;
    size_t width = big ? 8 : 4;
    pc_buf_truncate(f, 0);
    must(pc_buf_append(f, big ? "II\x2b\0\x08\0\0\0\0\0\0\0\0\0\0\0" : "II\x2a\0\0\0\0\0", big ? 16 : 8,
                       NULL));
    for (size_t p = 0; p < n; p++) {
        uint64_t e[11][3] = {{254, 4, pages[p][3]},
                             {256, 4, pages[p][0]},
                             {257, 4, pages[p][1]},
                             {258, 3, 8},
                             {259, 3, 1},
                             {262, 3, 1},
                             {273, 4, f->len},
                             {274, 3, pages[p][4]},
                             {277, 3, 1},
                             {278, 4, pages[p][1]},
                             {279, 4, (uint64_t)pages[p][0] * pages[p][1]}};
        for (uint32_t k = 0; k < pages[p][0] * pages[p][1]; k++) {
            must(pc_buf_append_byte(f, (uint8_t)pages[p][2], NULL));
        }
        if (f->len % 2 == 1) {
            must(pc_buf_append_byte(f, 0, NULL));
        }
        for (size_t k = 0; k < width; k++) {
            f->data[next_field + k] = (uint8_t)((uint64_t)f->len >> (8 * k));
        }
        for (size_t k = 0; k < (big ? 8u : 2u); k++) {
            must(pc_buf_append_byte(f, k == 0 ? 11 : 0, NULL));
        }
        for (size_t i = 0; i < 11; i++) {
            uint8_t t[20];
            size_t tl = 0;
            t[tl++] = (uint8_t)e[i][0];
            t[tl++] = (uint8_t)(e[i][0] >> 8);
            t[tl++] = (uint8_t)e[i][1];
            t[tl++] = 0;
            for (size_t k = 0; k < width; k++) {
                t[tl++] = k == 0 ? 1 : 0;
            }
            for (size_t k = 0; k < width; k++) {
                t[tl++] = (uint8_t)(e[i][2] >> (8 * k));
            }
            must(pc_buf_append(f, t, tl, NULL));
        }
        next_field = f->len;
        for (size_t k = 0; k < width; k++) {
            must(pc_buf_append_byte(f, 0, NULL));
        }
    }
}

static void test_the_first_full_resolution_page_opens_and_any_page_on_request(void)
{
    static const uint32_t pages[3][5] = {{2, 2, 9, 1, 1}, {6, 4, 50, 0, 1}, {3, 5, 90, 2, 1}};
    static const uint32_t sizes[3][2] = {{2, 2}, {6, 4}, {3, 5}};
    pc_buf f = PC_BUF_INIT;
    for (int big = 0; big < 2; big++) {
        pc_io_import_result r;
        pc_tiff_info info;
        size_t def = 99;
        pc_error e;
        tiff_pages(big != 0, pages, 3, &f);
        r = imp("pages.tif", f.data, f.len);
        CHECK(r.document.size.width == 6 && r.document.size.height == 4);
        CHECK(pc_io_warnings_any(&r.warnings, "only the first of 2 pages was imported"));
        pc_io_import_result_fini(&r);
        must(pc_ctiff_info(f.data, f.len, &info, NULL));
        CHECK(info.npages == 3 && pc_tiff_info_default_page(&info, &def) && def == 1 &&
              info.big_tiff == (big != 0));
        pc_tiff_info_fini(&info);
        for (size_t p = 0; p < 3; p++) {
            must(pc_io_import_tiff_page("pages.tif", f.data, f.len, true, p, &r, NULL));
            CHECK(r.document.size.width == sizes[p][0] && r.document.size.height == sizes[p][1]);
            pc_io_import_result_fini(&r);
        }
        CHECK(pc_io_import_tiff_page("pages.tif", f.data, f.len, true, 3, &r, &e) != PC_OK);
    }
    pc_buf_fini(&f);
}

static void test_bigtiff_orientation_is_applied_on_open(void)
{
    static const uint32_t turned[1][5] = {{6, 4, 50, 0, 6}};
    pc_buf f = PC_BUF_INIT;
    for (int big = 0; big < 2; big++) {
        pc_io_import_result r;
        tiff_pages(big != 0, turned, 1, &f);
        r = imp("turned.tif", f.data, f.len);
        CHECK(r.document.size.width == 4 && r.document.size.height == 6);
        pc_io_import_result_fini(&r);
        must(pc_io_import_tiff_page("turned.tif", f.data, f.len, true, 0, &r, NULL));
        CHECK(r.document.size.width == 4 && r.document.size.height == 6);
        pc_io_import_result_fini(&r);
    }
    pc_buf_fini(&f);
}

/* ---- tiff_layers.rs (flat TIFF) ---- */

static bool tiff_tags(const pc_io_export_result *r, bool *res, bool *layers)
{
    const uint8_t *a = NULL;
    const uint8_t *b = NULL;
    size_t an = 0, bn = 0;
    pc_ctiff_photoshop_tags(r->bytes, r->len, &a, &an, &b, &bn);
    *res = a != NULL;
    *layers = b != NULL;
    return true;
}

static void test_discard_layers_writes_a_flat_file(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_io_export_result r = exp_(&d, "x.tif", NULL);
    pc_io_import_result back;
    bool res, lay;
    tiff_tags(&r, &res, &lay);
    CHECK(!res && !lay);
    CHECK(pc_io_warnings_any(&r.warnings, "flattened"));
    back = imp("x.tif", r.bytes, r.len);
    CHECK_EQ_UINT(back.document.nlayers, 1);
    CHECK(!pc_io_would_write_layers(&back.document));
    CHECK(pc_io_would_write_layers(&d)); /* default_export_options_write_a_flat_tiff */
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static void test_a_plain_background_needs_no_layer_data(void)
{
    pc_document g = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_io_export_result png = exp_(&g, "x.png", NULL);
    pc_io_import_result d = imp("x.png", png.bytes, png.len);
    pc_io_export_opts o = pc_io_export_opts_default();
    pc_io_export_result r;
    pc_io_import_result back;
    bool res, lay;
    o.tiff_layers = true;
    CHECK_EQ_UINT(d.document.nlayers, 1);
    CHECK(!pc_io_would_write_layers(&d.document));
    r = exp_(&d.document, "x.tif", &o);
    tiff_tags(&r, &res, &lay);
    CHECK(!res && !lay);
    CHECK_EQ_UINT(r.warnings.n, 0);
    back = imp("x.tif", r.bytes, r.len);
    CHECK(pixels_diff(&d.document, &back.document) == 0.0f);
    CHECK(back.document.layers[0].locks.transparency == d.document.layers[0].locks.transparency);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_io_import_result_fini(&d);
    pc_io_export_result_fini(&png);
    pc_document_fini(&g);
}

static void test_lab_documents_are_saved_flat_with_a_warning(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U8);
    pc_io_export_opts o = pc_io_export_opts_default();
    pc_io_export_result r;
    pc_io_import_result back;
    bool res, lay;
    bool lab = false;
    o.tiff_layers = true;
    r = exp_(&d, "x.tif", &o);
    tiff_tags(&r, &res, &lay);
    CHECK(!res && !lay);
    for (size_t i = 0; i < r.warnings.n; i++) {
        lab =
            lab || (strstr(r.warnings.v[i], "Lab") != NULL && strstr(r.warnings.v[i], "layers are not kept"));
    }
    CHECK(lab);
    back = imp("x.tif", r.bytes, r.len);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

/* ---- heif ---- */

static void test_heic_without_heif_is_a_clear_error(void)
{
    static const char *names[3] = {"IMG_0001.HEIC", "photo.heif", "noext"};
    static const uint8_t header[24] = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c',
                                       0, 0, 0, 0,    'm', 'i', 'f', '1', 'h', 'e', 'i', 'c'};
    if (pc_cformat_caps(PC_CF_HEIF).read) {
        (void)printf("  (skipped: built with HEIF)\n");
        return;
    }
    for (int i = 0; i < 3; i++) {
        pc_io_import_result r;
        pc_error e;
        CHECK(pc_io_import(names[i], header, sizeof header, &r, &e) != PC_OK);
        CHECK_CONTAINS(e.msg, "isn't included in this build");
    }
}

static bool corpus_file(const char *rel, pc_buf *out)
{
    char path[512];
    (void)snprintf(path, sizeof path, "corpus/heif/%s", rel);
    return pc_file_read(path, (size_t)1 << 28, out, NULL) == PC_OK;
}

static void test_heif_corpus(void)
{
    pc_buf strips = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    pc_io_import_result r;
    pc_error e;
    if (!pc_cformat_caps(PC_CF_HEIF).read || !corpus_file("heic-rs/rgb-strips-96.heic", &strips)) {
        (void)printf("  (skipped: needs corpus/heif and a HEIF build)\n");
        return;
    }
    r = imp("IMG_0001.HEIC", strips.data, strips.len);
    CHECK(r.document.size.width == 96 && r.document.size.height == 32);
    CHECK(r.document.mode == PC_COLOR_MODE_RGB && r.document.depth == PC_SAMPLE_TYPE_U8);
    CHECK(r.document.nlayers == 1 && r.warnings.n == 0);
    {
        /* HEIC can't be written and says so. */
        pc_io_export_result x;
        CHECK(pc_io_export(&r.document, "x.heic", NULL, &x, &e) != PC_OK);
    }
    pc_io_import_result_fini(&r);
    if (corpus_file("pillow-heif/heif/RGBA_10__29x100.heif", &b)) {
        r = imp("hdr.heic", b.data, b.len);
        CHECK(r.document.size.width == 29 && r.document.size.height == 100);
        CHECK(r.document.depth == PC_SAMPLE_TYPE_U16 && !r.document.layers[0].locks.transparency);
        pc_io_import_result_fini(&r);
        pc_buf_fini(&b);
    }
    if (corpus_file("heic-rs/with-exif.heic", &b)) {
        pc_io_export_result out;
        pc_cimage back;
        r = imp("photo.heic", b.data, b.len);
        CHECK(r.document.size.width == 2048 && r.document.size.height == 1536);
        CHECK(r.document.metadata.exif != NULL && r.document.metadata.has_xmp &&
              strstr(pc_str_c(&r.document.metadata.xmp), "x:xmpmeta") != NULL);
        out = exp_(&r.document, "photo.jpg", NULL);
        must(pc_cdecode(out.bytes, out.len, &back, NULL));
        CHECK(back.width == 2048 && back.height == 1536);
        CHECK(back.meta.exif.has && pc_exif_orientation(back.meta.exif.data, back.meta.exif.len) == 1);
        pc_cimage_fini(&back);
        pc_io_export_result_fini(&out);
        pc_io_import_result_fini(&r);
        pc_buf_fini(&b);
    }
    {
        size_t cuts[5] = {0, 12, 40, strips.len / 2, strips.len - 1};
        for (int i = 0; i < 5; i++) {
            CHECK(pc_io_import("x.heic", strips.data, cuts[i], &r, &e) != PC_OK);
        }
    }
    pc_buf_fini(&strips);
}

int main(int argc, char **argv)
{
    RUN(test_codec_roundtrips);
    RUN(test_exr_rgba32);
    RUN(test_jpeg_rgb8_and_gray8);
    RUN(test_metadata_roundtrip_png);
    RUN(test_layered_to_png_warns_flatten);
    RUN(test_export_warnings);
    RUN(test_unknown_extension_and_garbage);
    RUN(test_extension_forms);
    RUN(test_background_lock_follows_alpha);
    RUN(test_transparency_is_composited_over_white_for_formats_without_alpha);
    RUN(test_non_srgb_rgb_is_converted_to_srgb_for_formats_without_a_profile);
    RUN(test_truncated_jpeg_imports_with_a_warning);
    RUN(test_animation_imports_the_first_frame_with_a_warning);
    RUN(test_indexed_png_is_palette_png);
    RUN(test_duotone_exports_the_inks_as_rgb);
    RUN(test_png_and_jpeg_embed_and_extract_icc);
    RUN(test_cmyk_to_png_is_colour_managed_and_tagged_srgb);
    RUN(test_png_resolution_axes);
    RUN(test_layered_png_export_spans_bands);
    RUN(test_single_layer_native_export_strips_opaque_alpha);
    RUN(test_flat_import_converts_rgb_in_bands);
    RUN(test_xmp_embedding);
    RUN(test_the_first_full_resolution_page_opens_and_any_page_on_request);
    RUN(test_bigtiff_orientation_is_applied_on_open);
    RUN(test_discard_layers_writes_a_flat_file);
    RUN(test_a_plain_background_needs_no_layer_data);
    RUN(test_lab_documents_are_saved_flat_with_a_warning);
    RUN(test_heic_without_heif_is_a_clear_error);
    RUN(test_heif_corpus);
    return pc_t_finish();
}
