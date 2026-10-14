/* Magic-number detection, extensions, and the capability table, including the symmetric
 * read/write guarantee (port of crates/codecs/tests/detect_caps.rs).
 *
 * The reference's cfg!(feature = "heif") / always-on OpenEXR become cx_heif_available() /
 * cx_exr_available(): this build may compile either out (PC_NO_HEIF / PC_NO_EXR). */
#include <ctype.h>
#include <stdio.h>

#include "codecs/internal.h"
#include "common.h"

/* The detected format, or -1 for None. */
static int det(const void *b, size_t n)
{
    pc_cformat f;
    return pc_cformat_detect((const uint8_t *)b, n, &f) ? (int)f : -1;
}
#define DET(lit) det((lit), sizeof(lit) - 1)

static int ext(const char *e)
{
    pc_cformat f;
    return pc_cformat_from_extension(e, &f) ? (int)f : -1;
}

static const pc_casymmetric *find_exception(pc_cformat f)
{
    size_t i;
    for (i = 0; i < PC_ASYMMETRIC_EXCEPTIONS_LEN; i++) {
        if (PC_ASYMMETRIC_EXCEPTIONS[i].format == f) {
            return &PC_ASYMMETRIC_EXCEPTIONS[i];
        }
    }
    return NULL;
}

/* ---- symmetric guarantee ---- */

static void test_every_enabled_format_is_symmetric_or_whitelisted(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_ccaps c = pc_cformat_caps((pc_cformat)f);
        const pc_casymmetric *exc;
        if (!(c.read || c.write) || (c.read && c.write)) {
            continue;
        }
        exc = find_exception((pc_cformat)f);
        if (exc == NULL) {
            PC_T_FAIL("%s is enabled but not symmetric (read=%d, write=%d)", pc_cformat_debug((pc_cformat)f), c.read,
                      c.write);
            continue;
        }
        CHECK(strlen(exc->reason) > 20);
    }
}

static void test_exceptions_are_documented_and_unique(void)
{
    size_t i;
    size_t j;
    for (i = 0; i < PC_ASYMMETRIC_EXCEPTIONS_LEN; i++) {
        CHECK(PC_ASYMMETRIC_EXCEPTIONS[i].reason != NULL && PC_ASYMMETRIC_EXCEPTIONS[i].reason[0] != '\0');
        for (j = i + 1; j < PC_ASYMMETRIC_EXCEPTIONS_LEN; j++) {
            CHECK(PC_ASYMMETRIC_EXCEPTIONS[j].format != PC_ASYMMETRIC_EXCEPTIONS[i].format);
        }
    }
}

static void test_never_write_only_without_exception(void)
{
    /* Stronger form: no format may be writable-but-unreadable unless listed. */
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        if (writable((pc_cformat)f) && !pc_cformat_caps((pc_cformat)f).read) {
            pc_t_name = pc_cformat_debug((pc_cformat)f);
            CHECK(find_exception((pc_cformat)f) != NULL);
        }
    }
}

static void test_default_build_formats_all_symmetric(void)
{
    static const pc_cformat EXPECTED[] = {PC_CF_PNG, PC_CF_TGA,  PC_CF_JPEG, PC_CF_ICO,  PC_CF_TIFF,
                                          PC_CF_PNM, PC_CF_WEBP, PC_CF_QOI,  PC_CF_GIF,  PC_CF_EXR,
                                          PC_CF_BMP, PC_CF_HDR,  PC_CF_FITS, PC_CF_DPX};
    size_t i;
    for (i = 0; i < sizeof EXPECTED / sizeof EXPECTED[0]; i++) {
        pc_ccaps c = pc_cformat_caps(EXPECTED[i]);
        if (EXPECTED[i] == PC_CF_EXR && !cx_exr_available()) {
            /* Built without OpenEXR (PC_NO_EXR): neither read nor written, still not asymmetric. */
            CHECK(!c.read && !c.write);
            continue;
        }
        if (!(c.read && c.write)) {
            PC_T_FAIL("%s", pc_cformat_debug(EXPECTED[i]));
        }
        pc_t_checks++;
    }
}

static void test_avif_never_readable(void)
{
    static const char HDR[] = "\0\0\0\x1c" "ftypavif";
    pc_cimage img;
    pc_error e;
    CHECK(!pc_cformat_caps(PC_CF_AVIF).read);
    CHECK(!pc_cformat_caps(PC_CF_AVIF).write); /* this build has no AVIF encoder (no "avif" feature) */
    CHECK(pc_cdecode_as(PC_CF_AVIF, (const uint8_t *)HDR, sizeof HDR - 1, &img, &e) == PC_ERR_UNSUPPORTED);
}

static void test_heif_is_detected_always_and_readable_only_with_the_feature(void)
{
    static const char HDR[] = "\0\0\0\x18" "ftypheic\0\0\0\0mif1heic";
    pc_cimage img;
    pc_error e;
    CHECK(DET(HDR) == PC_CF_HEIF);
    CHECK(ext("IMG_0001.HEIC") == PC_CF_HEIF);
    CHECK(pc_cformat_caps(PC_CF_HEIF).read == cx_heif_available());
    CHECK(!pc_cformat_caps(PC_CF_HEIF).write);
    CHECK(find_exception(PC_CF_HEIF) != NULL);
    /* Either way a bare header is an error, never a crash. */
    CHECK(pc_cdecode_as(PC_CF_HEIF, (const uint8_t *)HDR, sizeof HDR - 1, &img, &e) != PC_OK);
}

static void test_detect_heif_brands(void)
{
    CHECK(DET("\0\0\0\x18" "ftypheic\0\0\0\0mif1heic") == PC_CF_HEIF);
    CHECK(DET("\0\0\0\x18" "ftypmif1\0\0\0\0mif1heic") == PC_CF_HEIF); /* generic major brand */
    CHECK(DET("\0\0\0\x18" "ftypheix\0\0\0\0mif1heix") == PC_CF_HEIF); /* 10-bit */
    CHECK(DET("\0\0\0\x18" "ftypmsf1\0\0\0\0msf1hevc") == PC_CF_HEIF); /* image sequence */
    CHECK(DET("\0\0\0\x18" "ftypmif1\0\0\0\0mif1avif") == PC_CF_AVIF); /* AVIF wins over the generic brand */
    CHECK(DET("\0\0\0\x18" "ftypisom\0\0\0\0isommp41") == -1);         /* mp4 is not HEIF */
    CHECK(DET("\0\0\0\x14" "ftypqt  \0\0\0\0qt  ") == -1);             /* QuickTime is not HEIF */
}

static void test_caps_are_internally_consistent(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_ccaps c = pc_cformat_caps((pc_cformat)f);
        bool any_alpha = false;
        size_t i;
        const char *const *exts = pc_cformat_extensions((pc_cformat)f);
        pc_t_name = pc_cformat_debug((pc_cformat)f);
        CHECK(c.ndepths > 0 && c.nlayouts > 0);
        for (i = 0; i < c.nlayouts; i++) {
            any_alpha = any_alpha || pc_clayout_has_alpha(c.layouts[i]);
        }
        CHECK(c.alpha == any_alpha); /* alpha flag vs layouts */
        CHECK(exts != NULL && exts[0] != NULL && pc_cformat_name((pc_cformat)f)[0] != '\0' &&
              strncmp(pc_cformat_mime((pc_cformat)f), "image/", 6) == 0);
    }
}

static void test_caps_function_matches_method(void)
{
    /* The reference has caps(f) and f.caps(); C has only pc_cformat_caps, so check it is a pure
     * function of f instead. */
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_ccaps a = pc_cformat_caps((pc_cformat)f);
        pc_ccaps b = pc_cformat_caps((pc_cformat)f);
        CHECK(memcmp(&a, &b, sizeof a) == 0);
    }
}

/* ---- detection ---- */

static void test_detect_encoded_output_of_every_format(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_cimage img;
        pc_buf b;
        if (!writable((pc_cformat)f)) {
            continue;
        }
        pc_t_name = pc_cformat_debug((pc_cformat)f);
        img = test_image(PC_CL_RGBA, PC_CS_U8);
        b = enc(&img, (pc_cformat)f, NULL);
        CHECK_EQ_INT(det(b.data, b.len), f);
        pc_buf_fini(&b);
        pc_cimage_fini(&img);
    }
}

static void test_detect_magic_png(void) { CHECK(DET("\x89PNG\r\n\x1a\n\0\0") == PC_CF_PNG); }

static void test_detect_magic_jpeg(void)
{
    static const uint8_t A[] = {0xFF, 0xD8, 0xFF, 0xE0};
    static const uint8_t B[] = {0xFF, 0xD8};
    CHECK(det(A, sizeof A) == PC_CF_JPEG);
    CHECK(det(B, sizeof B) == -1);
}

static void test_detect_magic_tiff_both_endians_and_bigtiff(void)
{
    CHECK(DET("II*\0\x08\0\0\0") == PC_CF_TIFF);
    CHECK(DET("MM\0*\0\0\0\x08") == PC_CF_TIFF);
    CHECK(DET("II+\0\x08\0\0\0") == PC_CF_TIFF);
    CHECK(DET("MM\0+\0\x08\0\0") == PC_CF_TIFF);
}

static void test_detect_magic_webp(void)
{
    CHECK(DET("RIFF\x10\0\0\0WEBPVP8L") == PC_CF_WEBP);
    CHECK(DET("RIFF\x10\0\0\0WAVEfmt ") == -1);
}

static void test_detect_magic_gif(void)
{
    CHECK(DET("GIF87a\x01\0") == PC_CF_GIF);
    CHECK(DET("GIF89a\x01\0") == PC_CF_GIF);
    CHECK(DET("GIF90a\x01\0") == -1);
}

static void test_detect_magic_bmp(void)
{
    uint8_t b[54] = {'B', 'M'};
    CHECK(det(b, sizeof b) == PC_CF_BMP);
    CHECK(DET("BM") == -1);
}

static void test_detect_magic_ico(void)
{
    static const uint8_t A[] = {0, 0, 1, 0, 1, 0, 16, 16};
    static const uint8_t B[] = {0, 0, 1, 0, 0, 0};
    CHECK(det(A, sizeof A) == PC_CF_ICO);
    CHECK(det(B, sizeof B) == -1); /* zero images */
}

static void test_detect_magic_pnm_family(void)
{
    static const char *const M[] = {"P1\n", "P2 ", "P3\t", "P4\n", "P5\r", "P6\n", "P7\n", "PF\n", "Pf\n"};
    size_t i;
    for (i = 0; i < sizeof M / sizeof M[0]; i++) {
        if (det(M[i], 3) != PC_CF_PNM) {
            PC_T_FAIL("%.2s", M[i]);
        }
        pc_t_checks++;
    }
    CHECK(DET("P8\n") == -1);
    CHECK(DET("P6x") == -1);
}

static void test_detect_magic_qoi(void) { CHECK(DET("qoif\0\0\0\x01\0\0\0\x01\x03\0") == PC_CF_QOI); }

static void test_detect_magic_exr(void)
{
    static const uint8_t A[] = {0x76, 0x2F, 0x31, 0x01, 2, 0, 0, 0};
    CHECK(det(A, sizeof A) == PC_CF_EXR);
}

static void test_detect_magic_hdr(void)
{
    CHECK(DET("#?RADIANCE\n") == PC_CF_HDR);
    CHECK(DET("#?RGBE\n") == PC_CF_HDR);
}

static void test_detect_magic_avif(void)
{
    CHECK(DET("\0\0\0\x1c" "ftypavif\0\0\0\0avifmif1miaf") == PC_CF_AVIF);
    CHECK(DET("\0\0\0\x1c" "ftypavis\0\0\0\0avismsf1miaf") == PC_CF_AVIF);
    CHECK(DET("\0\0\0\x18" "ftypmif1\0\0\0\0mif1avif") == PC_CF_AVIF); /* compatible brand */
    CHECK(DET("\0\0\0\x18" "ftypisom\0\0\0\0isommp41") == -1);         /* mp4 is not avif */
}

static void test_detect_tga_heuristic(void)
{
    /* Uncompressed true-colour 2x2, 24 bpp. */
    uint8_t b[30] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 24, 0};
    uint8_t bad[30];
    CHECK(det(b, sizeof b) == PC_CF_TGA);
    memcpy(bad, b, sizeof b);
    bad[2] = 7; /* invalid image type */
    CHECK(det(bad, sizeof bad) == -1);
    memcpy(bad, b, sizeof b);
    bad[16] = 13; /* invalid depth */
    CHECK(det(bad, sizeof bad) == -1);
}

static void test_detect_negatives(void)
{
    static const uint8_t ZERO[64] = {0};
    CHECK(det("", 0) == -1);
    CHECK(DET("x") == -1);
    CHECK(DET("hello world, this is not an image") == -1);
    CHECK(DET("%PDF-1.7\n") == -1);
    CHECK(DET("PK\x03\x04") == -1);
    CHECK(det(ZERO, sizeof ZERO) == -1);
}

static void test_detect_random_bytes_rarely_matches_tga(void)
{
    rng r = {5};
    int hits = 0;
    int i;
    for (i = 0; i < 2000; i++) {
        uint8_t *b = rng_bytes(&r, 64);
        hits += det(b, 64) == PC_CF_TGA;
        free(b);
    }
    if (hits >= 10) {
        PC_T_FAIL("%d false TGA positives", hits);
    }
    pc_t_checks++;
}

static void test_decode_unknown_is_error(void)
{
    static const char G[] = "garbage garbage";
    pc_cimage img;
    pc_error e;
    CHECK(pc_cdecode((const uint8_t *)G, sizeof G - 1, &img, &e) == PC_ERR_NOT_FOUND);
    CHECK_CONTAINS(e.msg, "unrecognized image format");
}

/* ---- extensions ---- */

static void test_extensions_map_back(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        const char *const *e;
        for (e = pc_cformat_extensions((pc_cformat)f); *e != NULL; e++) {
            char buf[64];
            size_t i;
            pc_t_name = *e;
            CHECK_EQ_INT(ext(*e), f);
            for (i = 0; (*e)[i] != '\0' && i + 1 < sizeof buf; i++) {
                buf[i] = (char)toupper((unsigned char)(*e)[i]);
            }
            buf[i] = '\0';
            CHECK_EQ_INT(ext(buf), f);
            (void)snprintf(buf, sizeof buf, ".%s", *e);
            CHECK_EQ_INT(ext(buf), f);
            (void)snprintf(buf, sizeof buf, "dir/file.name.%s", *e);
            CHECK_EQ_INT(ext(buf), f);
        }
    }
}

static void test_extension_specific_cases(void)
{
    CHECK(ext("JPG") == PC_CF_JPEG);
    CHECK(ext("tif") == PC_CF_TIFF);
    CHECK(ext("pfm") == PC_CF_PNM);
    CHECK(ext("pam") == PC_CF_PNM);
    CHECK(ext("exr") == PC_CF_EXR);
    CHECK(ext("FIT") == PC_CF_FITS);
    CHECK(ext("m31.fits") == PC_CF_FITS);
    CHECK(ext("psd") == -1);
    CHECK(ext("") == -1);
    CHECK(ext("png.bak") == -1);
}

static void test_extensions_unique_across_formats(void)
{
    const char *all[128];
    size_t n = 0;
    size_t i;
    size_t j;
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        const char *const *e;
        for (e = pc_cformat_extensions((pc_cformat)f); *e != NULL && n < 128; e++) {
            all[n++] = *e;
        }
    }
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (strcmp(all[i], all[j]) == 0) {
                PC_T_FAIL("duplicate extension %s", all[i]);
            }
        }
    }
    pc_t_checks++;
}

int main(int argc, char **argv)
{
    RUN(test_every_enabled_format_is_symmetric_or_whitelisted);
    RUN(test_exceptions_are_documented_and_unique);
    RUN(test_never_write_only_without_exception);
    RUN(test_default_build_formats_all_symmetric);
    RUN(test_avif_never_readable);
    RUN(test_heif_is_detected_always_and_readable_only_with_the_feature);
    RUN(test_detect_heif_brands);
    RUN(test_caps_are_internally_consistent);
    RUN(test_caps_function_matches_method);
    RUN(test_detect_encoded_output_of_every_format);
    RUN(test_detect_magic_png);
    RUN(test_detect_magic_jpeg);
    RUN(test_detect_magic_tiff_both_endians_and_bigtiff);
    RUN(test_detect_magic_webp);
    RUN(test_detect_magic_gif);
    RUN(test_detect_magic_bmp);
    RUN(test_detect_magic_ico);
    RUN(test_detect_magic_pnm_family);
    RUN(test_detect_magic_qoi);
    RUN(test_detect_magic_exr);
    RUN(test_detect_magic_hdr);
    RUN(test_detect_magic_avif);
    RUN(test_detect_tga_heuristic);
    RUN(test_detect_negatives);
    RUN(test_detect_random_bytes_rarely_matches_tga);
    RUN(test_decode_unknown_is_error);
    RUN(test_extensions_map_back);
    RUN(test_extension_specific_cases);
    RUN(test_extensions_unique_across_formats);
    return pc_t_finish();
}
