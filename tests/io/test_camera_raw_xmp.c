/* Camera Raw settings files (.xmp): read / write / round-trip and graceful failure (port of
 * crates/io/src/camera_raw_xmp.rs tests). */
#include <stdlib.h>
#include <string.h>

#include "algo/camera_raw.h"
#include "base/buf.h"
#include "base/mem.h"
#include "harness.h"
#include "io/camera_raw_xmp.h"

/* A hand-written settings file laid out as Photoshop 26's Camera Raw Filter saves one, plus
 * element-form settings (the reference's FIXTURE). */
static const char FIXTURE[] =
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"Adobe XMP Core 7.0\">\n"
    " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "  <rdf:Description rdf:about=\"\"\n"
    "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n"
    "   crs:Version=\"18.1.1\" crs:ProcessVersion=\"15.4\" crs:WhiteBalance=\"Custom\"\n"
    "   crs:IncrementalTemperature=\"0\" crs:IncrementalTint=\"+10\" crs:Exposure2012=\"+0.05\"\n"
    "   crs:Contrast2012=\"-5\" crs:Highlights2012=\"-20\" crs:Shadows2012=\"+10\" crs:Whites2012=\"+35\"\n"
    "   crs:Blacks2012=\"0\" crs:Texture=\"+30\" crs:Clarity2012=\"+5\" crs:Vibrance=\"+20\" crs:Saturation=\"-5\"\n"
    "   crs:ParametricShadowSplit=\"25\" crs:ParametricMidtoneSplit=\"50\" crs:ParametricHighlightSplit=\"75\"\n"
    "   crs:Sharpness=\"15\" crs:SharpenRadius=\"+1.0\" crs:LuminanceSmoothing=\"5\"\n"
    "   crs:LuminanceNoiseReductionContrast=\"0\" crs:HueAdjustmentGreen=\"+25\" crs:SaturationAdjustmentGreen=\"+20\"\n"
    "   crs:ColorGradeBlending=\"50\" crs:LensProfileEnable=\"0\" crs:VignetteAmount=\"-20\"\n"
    "   crs:PerspectiveScale=\"100\" crs:GrainAmount=\"10\" crs:RedHue=\"-5\" crs:GrainSeed=\"2834497962\"\n"
    "   crs:ToneCurveName2012=\"Linear\" crs:CameraProfile=\"Default Color\" crs:HasSettings=\"True\" crs:HasCrop=\"False\">\n"
    "   <crs:Dehaze>+12</crs:Dehaze>\n"
    "   <crs:ToneCurvePV2012>\n    <rdf:Seq>\n     <rdf:li>0, 0</rdf:li>\n     <rdf:li>255, 255</rdf:li>\n    </rdf:Seq>\n   </crs:ToneCurvePV2012>\n"
    "   <crs:ToneCurvePV2012Red>\n    <rdf:Seq>\n     <rdf:li>0, 0</rdf:li>\n     <rdf:li>128, 140</rdf:li>\n     <rdf:li>255, 255</rdf:li>\n    </rdf:Seq>\n   </crs:ToneCurvePV2012Red>\n"
    "  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>";

static void test_reads_a_photoshop_preset(void)
{
    pc_algo_camera_raw p;
    pc_io_cr_xmp_report r;
    pc_algo_camera_raw_default(&p);
    CHECK(pc_io_camera_raw_xmp_load(&p, (const uint8_t *)FIXTURE, strlen(FIXTURE), &r, NULL) == PC_OK);
    CHECK(p.tint == 10.0f);
    CHECK(p.exposure == 0.05f);
    CHECK(p.contrast == -5.0f);
    CHECK(p.highlights == -20.0f);
    CHECK(p.shadows == 10.0f);
    CHECK(p.whites == 35.0f);
    CHECK(p.texture == 30.0f);
    CHECK(p.clarity == 5.0f);
    CHECK(p.vibrance == 20.0f);
    CHECK(p.saturation == -5.0f);
    CHECK(p.hsl_hue[3] == 25.0f);
    CHECK(p.hsl_sat[3] == 20.0f);
    CHECK(p.sharpen_amount == 15.0f);
    CHECK(p.noise_luminance == 5.0f);
    CHECK(p.grain_amount == 10.0f);
    CHECK(p.grade_blending == 50.0f);
    CHECK(p.dehaze == 12.0f);
    /* The linear tone curve is PhotoCraft's empty one; an element-form curve is read too. */
    CHECK_EQ_UINT(p.point_curve.n, 0);
    CHECK_EQ_UINT(p.point_curve_red.n, 3);
    if (p.point_curve_red.n == 3) {
        CHECK(p.point_curve_red.v[1][0] == 128.0f);
        CHECK(p.point_curve_red.v[1][1] == 140.0f);
    }
    /* Lens vignetting and calibration have no counterpart, in file order. */
    CHECK_EQ_UINT(r.nignored, 2);
    if (r.nignored == 2) {
        CHECK_EQ_STR(r.ignored[0], "VignetteAmount");
        CHECK_EQ_STR(r.ignored[1], "RedHue");
    }
    pc_io_cr_xmp_report_fini(&r);
    pc_algo_camera_raw_fini(&p);
}

static void test_save_then_load_round_trips(void)
{
    pc_algo_camera_raw p, back;
    pc_io_cr_xmp_report r;
    pc_buf text = PC_BUF_INIT;
    pc_algo_camera_raw_default(&p);
    p.temperature = -12.0f;
    p.exposure = 1.25f;
    p.contrast = 30.0f;
    p.sharpen_radius = 1.4f;
    p.vignette_amount = -40.0f;
    p.vignette_style = pc_strndup("paintOverlay", strlen("paintOverlay"));
    p.curve_splits[0] = 20.0f;
    p.curve_splits[1] = 45.0f;
    p.curve_splits[2] = 80.0f;
    p.point_curve.v = malloc(3 * sizeof *p.point_curve.v);
    p.point_curve.n = 3;
    p.point_curve.v[0][0] = 0.0f;   p.point_curve.v[0][1] = 10.0f;
    p.point_curve.v[1][0] = 128.0f; p.point_curve.v[1][1] = 150.0f;
    p.point_curve.v[2][0] = 255.0f; p.point_curve.v[2][1] = 240.0f;
    p.hsl_lum[5] = -33.0f;
    p.grade_shadows.hue = 210.0f;
    p.grade_shadows.sat = 40.0f;
    p.grade_global.lum = 7.0f;

    CHECK(pc_io_camera_raw_xmp_save(&p, &text, NULL) == PC_OK);
    CHECK(strstr(pc_buf_cstr(&text), "crs:Exposure2012=\"+1.25\"") != NULL);
    CHECK(strstr(pc_buf_cstr(&text), "crs:WhiteBalance=\"Custom\"") != NULL);

    pc_algo_camera_raw_default(&back);
    CHECK(pc_io_camera_raw_xmp_load(&back, text.data, text.len, &r, NULL) == PC_OK);
    CHECK_EQ_UINT(r.nignored, 0);
    CHECK(pc_algo_camera_raw_eq(&back, &p));
    pc_io_cr_xmp_report_fini(&r);
    pc_buf_fini(&text);
    pc_algo_camera_raw_fini(&p);
    pc_algo_camera_raw_fini(&back);
}

static void test_load_applies_only_the_settings_in_the_file(void)
{
    static const char xmp[] =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
        "<rdf:Description xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\">"
        "<crs:Exposure2012>-9</crs:Exposure2012><crs:Clarity2012>abc</crs:Clarity2012>"
        "</rdf:Description></rdf:RDF></x:xmpmeta>";
    pc_algo_camera_raw p;
    pc_io_cr_xmp_report r;
    pc_algo_camera_raw_default(&p);
    p.contrast = 40.0f;
    CHECK(pc_io_camera_raw_xmp_load(&p, (const uint8_t *)xmp, strlen(xmp), &r, NULL) == PC_OK);
    CHECK(p.exposure == -5.0f); /* clamped to the slider range */
    CHECK(p.contrast == 40.0f); /* untouched */
    CHECK(p.clarity == 0.0f);
    CHECK_EQ_UINT(r.applied, 1);
    CHECK_EQ_UINT(r.nignored, 1);
    if (r.nignored == 1) {
        CHECK_EQ_STR(r.ignored[0], "Clarity2012");
    }
    pc_io_cr_xmp_report_fini(&r);
    pc_algo_camera_raw_fini(&p);
}

static void test_malformed_input_is_an_error(void)
{
    const char *bad[] = {
        "",
        "\xff\xfe\x00garbage",
        "<x:xmpmeta><unclosed",
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>",
    };
    const size_t badlen[] = {0, 10, strlen(bad[2]), strlen(bad[3])};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pc_algo_camera_raw p;
        pc_algo_camera_raw_default(&p);
        p.exposure = 0.5f;
        CHECK(pc_io_camera_raw_xmp_load(&p, (const uint8_t *)bad[i], badlen[i], NULL, NULL) != PC_OK);
        CHECK(p.exposure == 0.5f); /* left alone */
        pc_algo_camera_raw_fini(&p);
    }
    /* A broken curve or NaN is skipped, not fatal. */
    static const char xmp[] =
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
        "xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\" crs:Contrast2012=\"NaN\">"
        "<crs:ToneCurvePV2012><rdf:Seq><rdf:li>10</rdf:li></rdf:Seq></crs:ToneCurvePV2012>"
        "</rdf:Description></rdf:RDF>";
    pc_algo_camera_raw p;
    pc_io_cr_xmp_report r;
    pc_algo_camera_raw_default(&p);
    p.exposure = 0.5f;
    CHECK(pc_io_camera_raw_xmp_load(&p, (const uint8_t *)xmp, strlen(xmp), &r, NULL) == PC_OK);
    CHECK(p.exposure == 0.5f);
    CHECK_EQ_UINT(r.applied, 0);
    CHECK_EQ_UINT(r.nignored, 2);
    if (r.nignored == 2) {
        CHECK_EQ_STR(r.ignored[0], "Contrast2012");
        CHECK_EQ_STR(r.ignored[1], "ToneCurvePV2012");
    }
    pc_io_cr_xmp_report_fini(&r);
    pc_algo_camera_raw_fini(&p);
}

static void test_too_large_is_rejected(void)
{
    pc_algo_camera_raw p;
    size_t n = PC_IO_CR_XMP_MAX_BYTES + 1;
    uint8_t *big = calloc(n, 1);
    pc_algo_camera_raw_default(&p);
    if (big != NULL) {
        memset(big, ' ', n);
        CHECK(pc_io_camera_raw_xmp_load(&p, big, n, NULL, NULL) == PC_ERR_LIMIT);
        free(big);
    }
    pc_algo_camera_raw_fini(&p);
}

int main(int argc, char **argv)
{
    RUN(test_reads_a_photoshop_preset);
    RUN(test_save_then_load_round_trips);
    RUN(test_load_applies_only_the_settings_in_the_file);
    RUN(test_malformed_input_is_an_error);
    RUN(test_too_large_is_rejected);
    return pc_t_finish();
}
