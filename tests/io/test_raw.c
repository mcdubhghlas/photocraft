/* Camera raws and EXIF orientation through io: crates/io/tests/raw.rs (5 tests) and
 * orientation.rs (the PSD half of stale_orientation_in_document_metadata_is_never_exported waits
 * for I3), with the synthetic raw writers of tests/raw/testgen.h. */
#include "harness.h"

#include "common.h"
#include "../raw/testgen.h"

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

static void test_dng_opens_as_16_bit_prophoto(void)
{
    static const uint8_t cfa[4] = {0, 1, 1, 2};
    float (*sc)[3] = tg_scene(40, 24);
    uint16_t *m = tg_mosaic((const float (*)[3])sc, 40 * 24, 40, cfa, 0, 65535);
    tg_dng spec = tg_dng_cfa(40, 24, m);
    tg_bytes b;
    pc_io_import_result r;
    pc_buf icc = PC_BUF_INIT;
    bool note = false;
    spec.has_neutral = true;
    spec.neutral[0] = 0.6;
    spec.neutral[1] = 1.0;
    spec.neutral[2] = 0.8;
    b = tg_dng_build(&spec);
    r = imp("shot.dng", b.d, b.n);
    CHECK(r.document.size.width == 40 && r.document.size.height == 24);
    CHECK(r.document.mode == PC_COLOR_MODE_RGB && r.document.depth == PC_SAMPLE_TYPE_U16);
    CHECK_EQ_UINT(r.document.nlayers, 1);
    must(pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_PROPHOTO_COMPAT), &icc, NULL));
    CHECK(pc_blob_len(r.document.icc_profile) == icc.len &&
          memcmp(pc_blob_data(r.document.icc_profile), icc.data, icc.len) == 0);
    for (size_t i = 0; i < r.warnings.n; i++) {
        note =
            note || (strstr(r.warnings.v[i], "DNG") != NULL && strstr(r.warnings.v[i], "ProPhoto") != NULL);
    }
    CHECK(note);
    pc_buf_fini(&icc);
    pc_io_import_result_fini(&r);
    tg_free(&b);
    free(m);
    free(sc);
}

static void test_cr2_opens(void)
{
    static const uint8_t cfa[4] = {0, 1, 1, 2};
    float (*sc)[3] = tg_scene(48, 16);
    uint16_t *m = tg_mosaic((const float (*)[3])sc, 48 * 16, 48, cfa, 256, 12000);
    tg_cr2 spec;
    tg_bytes b;
    pc_io_import_result r;
    memset(&spec, 0, sizeof spec);
    spec.width = 48;
    spec.height = 16;
    spec.data = m;
    spec.precision = 14;
    spec.components = 2;
    spec.slices[0] = 24;
    spec.slices[1] = 24;
    spec.nslices = 2;
    spec.orientation = 6;
    b = tg_cr2_build(&spec);
    r = imp("IMG_0001.CR2", b.d, b.n);
    /* Orientation 6 rotates the 48x16 sensor image to 16x48. */
    CHECK(r.document.size.width == 16 && r.document.size.height == 48);
    CHECK(r.document.depth == PC_SAMPLE_TYPE_U16);
    pc_io_import_result_fini(&r);
    tg_free(&b);
    free(m);
    free(sc);
}

/* A NEF-like file with Nikon's compression and a full-size baseline JPEG preview in IFD0. */
static tg_bytes nef(bool has_orientation, uint16_t orientation, const uint8_t *jpeg, size_t jlen)
{
    tg_tiff t;
    tg_bytes strip;
    tg_bytes prev;
    size_t s;
    size_t p;
    tg_entry raw[9];
    tg_entry ifd0[5];
    size_t n0 = 0;
    size_t ri;
    size_t i0;
    static const uint8_t cfa[4] = {0, 1, 1, 2};
    static const uint16_t rep[2] = {2, 2};
    memset(&t, 0, sizeof t);
    memset(&strip, 0, sizeof strip);
    memset(&prev, 0, sizeof prev);
    tg_resize(&strip, 64);
    tg_put(&prev, jpeg, jlen);
    s = tg_blob(&t, strip);
    p = tg_blob(&t, prev);
    raw[0] = (tg_entry){256, tg_v_long1(8)};
    raw[1] = (tg_entry){257, tg_v_long1(8)};
    raw[2] = (tg_entry){258, tg_v_short1(12)};
    raw[3] = (tg_entry){259, tg_v_short1(34713)};
    raw[4] = (tg_entry){262, tg_v_short1(32803)};
    raw[5] = (tg_entry){273, tg_v_ref(TG_BLOBS, s)};
    raw[6] = (tg_entry){279, tg_v_long1(64)};
    raw[7] = (tg_entry){33421, tg_v_short(rep, 2)};
    raw[8] = (tg_entry){33422, tg_v_bytes(TG_BYTE, cfa, 4)};
    ri = tg_ifd_add(&t, raw, 9);
    ifd0[n0++] = (tg_entry){271, tg_v_ascii("NIKON CORPORATION")};
    if (has_orientation) {
        ifd0[n0++] = (tg_entry){274, tg_v_short1(orientation)};
    }
    ifd0[n0++] = (tg_entry){330, tg_v_ref(TG_IFDS, ri)};
    ifd0[n0++] = (tg_entry){513, tg_v_ref(TG_BLOBS, p)};
    ifd0[n0++] = (tg_entry){514, tg_v_long1((uint32_t)jlen)};
    i0 = tg_ifd_add(&t, ifd0, n0);
    tg_set_chain(&t, &i0, 1);
    {
        tg_bytes out = tg_tiff_build(&t);
        tg_tiff_free(&t);
        return out;
    }
}

static void encode_jpeg(const pc_cimage *img, int quality, bool subsample, pc_buf *out)
{
    pc_cencode_opts o = pc_cencode_opts_default();
    o.jpeg_quality = (uint8_t)quality;
    o.jpeg_chroma_subsampling = subsample;
    must(pc_cencode(img, PC_CF_JPEG, &o, out, NULL));
}

static void test_unsupported_raw_falls_back_to_the_embedded_preview(void)
{
    pc_cimage img;
    pc_buf jpeg = PC_BUF_INIT;
    uint8_t *px = xmalloc(32 * 20 * 3);
    tg_bytes b;
    pc_io_import_result r;
    memset(px, 180, 32 * 20 * 3);
    must(pc_cimage_from_raw(32, 20, PC_CL_RGB, PC_CS_U8, px, 32 * 20 * 3, &img, NULL));
    encode_jpeg(&img, 90, true, &jpeg);
    b = nef(false, 1, jpeg.data, jpeg.len);
    r = imp("DSC_0001.NEF", b.d, b.n);
    CHECK(r.document.size.width == 32 && r.document.size.height == 20);
    CHECK(r.warnings.n > 0 && strstr(r.warnings.v[0], "Nikon compressed NEF") != NULL &&
          strstr(r.warnings.v[0], "embedded") != NULL);
    pc_io_import_result_fini(&r);
    /* truncated_raws_do_not_panic */
    for (size_t n = 0; n < b.n; n += 7) {
        pc_error e;
        if (pc_io_import("x.nef", b.d, n, &r, &e) == PC_OK) {
            pc_io_import_result_fini(&r);
        }
    }
    tg_free(&b);
    pc_buf_fini(&jpeg);
    pc_cimage_fini(&img);
}

static void test_unsupported_raw_without_preview_is_a_clear_error(void)
{
    uint8_t cr3[24] = {0, 0, 0, 24, 'f', 't', 'y', 'p', 'c', 'r', 'x', ' '};
    pc_io_import_result r;
    pc_error e;
    pc_status st = pc_io_import("IMG_0001.CR3", cr3, sizeof cr3, &r, &e);
    CHECK(st != PC_OK);
    CHECK(pc_io_error_kind(st, &e) == PC_IO_ERR_RAW);
    CHECK_CONTAINS(e.msg, "CR3");
}

/* ---- orientation.rs ---- */

/* Upright 16x32 portrait: red top-left quarter, green top-right, blue bottom half. */
static void upright_color(int x, int y, uint8_t out[3])
{
    if (y < 16) {
        out[0] = x < 8 ? 230 : 20;
        out[1] = x < 8 ? 20 : 230;
        out[2] = 20;
    } else {
        out[0] = 20;
        out[1] = 20;
        out[2] = 230;
    }
}

/* The scene as a camera stores it for Orientation = 6 (32x16 landscape). */
static pc_cimage stored_orientation_6(void)
{
    const int w = 16, h = 32;
    uint8_t *px = xmalloc(16 * 32 * 3);
    pc_cimage img;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = y, sy = w - 1 - x;
            upright_color(x, y, px + ((size_t)sy * (size_t)h + (size_t)sx) * 3);
        }
    }
    must(pc_cimage_from_raw((uint32_t)h, (uint32_t)w, PC_CL_RGB, PC_CS_U8, px, 16 * 32 * 3, &img, NULL));
    return img;
}

static void jpeg_with_exif(const uint8_t *jpeg, size_t n, const uint8_t *exif, size_t en, pc_buf *out)
{
    size_t seg = 6 + en;
    pc_buf_truncate(out, 0);
    must(pc_buf_append(out, jpeg, 2, NULL));
    must(pc_buf_append(out, "\xFF\xE1", 2, NULL));
    must(pc_buf_append_byte(out, (uint8_t)((seg + 2) >> 8), NULL));
    must(pc_buf_append_byte(out, (uint8_t)(seg + 2), NULL));
    must(pc_buf_append(out, "Exif\0\0", 6, NULL));
    if (en > 0) {
        must(pc_buf_append(out, exif, en, NULL));
    }
    must(pc_buf_append(out, jpeg + 2, n - 2, NULL));
}

static void iphone_jpeg(pc_buf *out)
{
    pc_cimage img = stored_orientation_6();
    pc_buf jpeg = PC_BUF_INIT;
    uint8_t ex[26];
    size_t en = t_exif(6, ex);
    encode_jpeg(&img, 100, false, &jpeg);
    jpeg_with_exif(jpeg.data, jpeg.len, ex, en, out);
    pc_buf_fini(&jpeg);
    pc_cimage_fini(&img);
}

static void check_upright(const pc_cimage *img0, const char *what)
{
    static const int pts[4][2] = {{3, 4}, {12, 4}, {3, 28}, {12, 28}};
    pc_cimage tmp;
    const pc_cimage *img;
    CHECK(img0->width == 16 && img0->height == 32);
    must(pc_cimage_converted(img0, PC_CL_RGB, PC_CS_U8, &tmp, &img, NULL));
    for (int k = 0; k < 4 && img0->width == 16 && img0->height == 32; k++) {
        uint8_t want[3];
        const uint8_t *got = img->data + ((size_t)pts[k][1] * 16 + (size_t)pts[k][0]) * 3;
        bool ok = true;
        upright_color(pts[k][0], pts[k][1], want);
        for (int c = 0; c < 3; c++) {
            ok = ok && abs((int)got[c] - (int)want[c]) <= 12;
        }
        if (!ok) {
            PC_T_FAIL("%s (%d,%d): %u %u %u", what, pts[k][0], pts[k][1], got[0], got[1], got[2]);
        }
    }
    pc_cimage_fini(&tmp);
}

static void test_rotated_jpeg_opens_upright_and_exports_with_orientation_1(void)
{
    static const char *exts[4] = {"jpg", "png", "tif", "webp"};
    pc_buf j = PC_BUF_INIT;
    pc_io_import_result r;
    iphone_jpeg(&j);
    r = imp("IMG_0001.JPG", j.data, j.len);
    CHECK(r.document.size.width == 16 && r.document.size.height == 32);
    CHECK(pc_exif_orientation(pc_blob_data(r.document.metadata.exif),
                              pc_blob_len(r.document.metadata.exif)) == 1);
    for (int i = 0; i < 4; i++) {
        pc_io_export_result out;
        pc_cimage back;
        pc_io_import_result again;
        char name[32];
        must(pc_io_export(&r.document, exts[i], NULL, &out, NULL));
        CHECK(pc_exif_orientation(out.bytes, out.len) == 1);
        must(pc_cdecode(out.bytes, out.len, &back, NULL));
        if (back.meta.exif.has) {
            CHECK(pc_exif_orientation(back.meta.exif.data, back.meta.exif.len) == 1);
        }
        check_upright(&back, exts[i]);
        (void)snprintf(name, sizeof name, "again.%s", exts[i]);
        again = imp(name, out.bytes, out.len);
        CHECK(again.document.size.width == 16 && again.document.size.height == 32);
        pc_io_import_result_fini(&again);
        pc_cimage_fini(&back);
        pc_io_export_result_fini(&out);
    }
    pc_io_import_result_fini(&r);
    pc_buf_fini(&j);
}

static void test_stale_orientation_in_document_metadata_is_never_exported(void)
{
    pc_buf j = PC_BUF_INIT;
    pc_io_import_result r;
    uint8_t ex[26];
    size_t en = t_exif(6, ex);
    pc_io_export_opts none = pc_io_export_opts_default();
    pc_io_export_result out;
    pc_cimage back;
    iphone_jpeg(&j);
    r = imp("IMG_0001.JPG", j.data, j.len);
    must(pc_io_set_blob(&r.document.metadata.exif, ex, en, NULL));
    t_set_xmp(&r.document, "<rdf:Description tiff:Orientation=\"6\"/>");
    none.xmp = PC_IO_XMP_NONE;
    must(pc_io_export(&r.document, "jpg", &none, &out, NULL));
    must(pc_cdecode(out.bytes, out.len, &back, NULL));
    CHECK(back.meta.exif.has && pc_exif_orientation(back.meta.exif.data, back.meta.exif.len) == 1);
    CHECK(!back.meta.xmp.has);
    pc_cimage_fini(&back);
    pc_io_export_result_fini(&out);
    must(pc_io_export(&r.document, "jpg", NULL, &out, NULL));
    must(pc_cdecode(out.bytes, out.len, &back, NULL));
    CHECK(back.meta.xmp.has && back.meta.xmp.len > 0 &&
          strstr((const char *)back.meta.xmp.data, "tiff:Orientation=\"1\"") != NULL);
    check_upright(&back, "jpg");
    pc_cimage_fini(&back);
    pc_io_export_result_fini(&out);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&j);
}

static void test_raw_preview_is_turned_upright_exactly_once(void)
{
    pc_cimage img = stored_orientation_6();
    pc_buf plain = PC_BUF_INIT;
    pc_buf with = PC_BUF_INIT;
    uint8_t ex[26];
    size_t en = t_exif(6, ex);
    tg_bytes b;
    pc_io_import_result r;
    pc_io_export_result png;
    pc_cimage back;
    encode_jpeg(&img, 100, false, &plain);
    /* The raw's IFD0 orientation applies to a preview without its own. */
    b = nef(true, 6, plain.data, plain.len);
    r = imp("DSC_0001.NEF", b.d, b.n);
    must(pc_io_export(&r.document, "png", NULL, &png, NULL));
    must(pc_cdecode(png.bytes, png.len, &back, NULL));
    check_upright(&back, "IFD0 orientation");
    pc_cimage_fini(&back);
    pc_io_export_result_fini(&png);
    pc_io_import_result_fini(&r);
    tg_free(&b);
    /* A preview with its own EXIF orientation is not turned a second time by IFD0's. */
    jpeg_with_exif(plain.data, plain.len, ex, en, &with);
    b = nef(true, 6, with.data, with.len);
    r = imp("DSC_0002.NEF", b.d, b.n);
    must(pc_io_export(&r.document, "png", NULL, &png, NULL));
    must(pc_cdecode(png.bytes, png.len, &back, NULL));
    check_upright(&back, "preview orientation");
    pc_cimage_fini(&back);
    pc_io_export_result_fini(&png);
    pc_io_import_result_fini(&r);
    tg_free(&b);
    /* Upright raws stay as they are. */
    b = nef(true, 1, plain.data, plain.len);
    r = imp("DSC_0003.NEF", b.d, b.n);
    CHECK(r.document.size.width == 32 && r.document.size.height == 16);
    pc_io_import_result_fini(&r);
    tg_free(&b);
    pc_buf_fini(&plain);
    pc_buf_fini(&with);
    pc_cimage_fini(&img);
}

static void test_malformed_exif_opens_without_panicking(void)
{
    pc_cimage img = stored_orientation_6();
    pc_buf jpeg = PC_BUF_INIT;
    pc_buf j = PC_BUF_INIT;
    uint8_t e0[26], e9[26], emax[26], huge[26];
    static const uint8_t mm[4] = {'M', 'M', 0, '*'};
    struct {
        const uint8_t *b;
        size_t n;
    } cases[6];
    encode_jpeg(&img, 90, true, &jpeg);
    (void)t_exif(0, e0);
    (void)t_exif(9, e9);
    (void)t_exif(65535, emax);
    (void)t_exif(6, huge);
    huge[8] = 0xff;
    huge[9] = 0xff;
    cases[0].b = NULL;
    cases[0].n = 0;
    cases[1].b = mm;
    cases[1].n = 4;
    cases[2].b = e0;
    cases[2].n = 26;
    cases[3].b = e9;
    cases[3].n = 26;
    cases[4].b = emax;
    cases[4].n = 26;
    cases[5].b = huge;
    cases[5].n = 14;
    for (int i = 0; i < 6; i++) {
        pc_io_import_result r;
        jpeg_with_exif(jpeg.data, jpeg.len, cases[i].b, cases[i].n, &j);
        r = imp("x.jpg", j.data, j.len);
        CHECK(r.document.size.width == 32 && r.document.size.height == 16);
        pc_io_import_result_fini(&r);
    }
    pc_buf_fini(&jpeg);
    pc_buf_fini(&j);
    pc_cimage_fini(&img);
}

static void test_raw_tuning(void)
{
    static const uint8_t cfa[4] = {0, 1, 1, 2};
    float (*sc)[3] = tg_scene(32, 16);
    uint16_t *m = tg_mosaic((const float (*)[3])sc, 32 * 16, 32, cfa, 256, 12000);
    tg_cr2 spec;
    tg_bytes b;
    pc_io_import_result r;
    pc_io_raw_tuning t = {25.0f, -10.0f, 0.5f};
    bool applied = false;
    pc_error e;
    memset(&spec, 0, sizeof spec);
    spec.width = 32;
    spec.height = 16;
    spec.data = m;
    spec.precision = 14;
    spec.components = 2;
    spec.slices[0] = 16;
    spec.slices[1] = 16;
    spec.nslices = 2;
    spec.has_wb = true;
    spec.wb_rggb[0] = 2048;
    spec.wb_rggb[1] = 1024;
    spec.wb_rggb[2] = 1024;
    spec.wb_rggb[3] = 1536;
    spec.orientation = 1;
    b = tg_cr2_build(&spec);
    must(pc_io_import_raw_tuned("t.cr2", b.d, b.n, &t, &r, &applied, NULL));
    CHECK(applied);
    CHECK(pc_io_warnings_any(&r.warnings,
                             "developed in Camera Raw (exposure +0.50 EV, as-shot white balance, "
                             "temperature +25, tint -10)"));
    pc_io_import_result_fini(&r);
    t.exposure = 10.5f;
    CHECK(pc_io_import_raw_tuned("t.cr2", b.d, b.n, &t, &r, &applied, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "malformed camera raw: exposure 10.5 EV is out of range");
    t.exposure = NAN;
    CHECK(pc_io_import_raw_tuned("t.cr2", b.d, b.n, &t, &r, &applied, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "malformed camera raw: exposure NaN EV is out of range");
    tg_free(&b);
    free(m);
    free(sc);
}

int main(int argc, char **argv)
{
    RUN(test_dng_opens_as_16_bit_prophoto);
    RUN(test_cr2_opens);
    RUN(test_unsupported_raw_falls_back_to_the_embedded_preview);
    RUN(test_unsupported_raw_without_preview_is_a_clear_error);
    RUN(test_rotated_jpeg_opens_upright_and_exports_with_orientation_1);
    RUN(test_stale_orientation_in_document_metadata_is_never_exported);
    RUN(test_raw_preview_is_turned_upright_exactly_once);
    RUN(test_malformed_exif_opens_without_panicking);
    RUN(test_raw_tuning);
    return pc_t_finish();
}
