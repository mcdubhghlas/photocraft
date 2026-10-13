/* ICC / EXIF / XMP / DPI / text preservation, per format, per caps (port of
 * crates/codecs/tests/metadata.rs). The Rust oracles (the `image` crate) are replaced by
 * libjpeg-turbo and libpng read directly, and by our own decoder for TIFF (no libtiff here). */
#include <setjmp.h>
#include <stdio.h>

#include <jpeglib.h>
#include <png.h>
#include <zlib.h>

#include "common.h"

#define MAX_FW 32

static bool fw_has(const pc_cimage *img, pc_cformat f, const pc_cencode_opts *o, pc_cfidelity_kind k)
{
    pc_cfidelity w[MAX_FW];
    pc_cencode_opts d = pc_cencode_opts_default();
    size_t n = pc_cfidelity_warnings_with(img, f, o != NULL ? o : &d, w, MAX_FW);
    size_t i;
    for (i = 0; i < n && i < MAX_FW; i++) {
        if (w[i].kind == k) {
            return true;
        }
    }
    return false;
}

static bool fw_too_large(const pc_cimage *img, pc_cformat f, const char *what, size_t bytes)
{
    pc_cfidelity w[MAX_FW];
    size_t n = pc_cfidelity_warnings(img, f, w, MAX_FW);
    size_t i;
    for (i = 0; i < n && i < MAX_FW; i++) {
        if (w[i].kind == PC_FW_METADATA_TOO_LARGE && (what == NULL || (w[i].what != NULL && strcmp(w[i].what, what) == 0 &&
                                                                       w[i].bytes == bytes))) {
            return true;
        }
    }
    return false;
}

static void set_buf(pc_meta_blob *b, pc_buf *v)
{
    set_blob(b, v->data, v->len);
    pc_buf_fini(v);
}

static bool has_text(const pc_cmeta *m, const char *k, size_t kn, const char *v, size_t vn)
{
    size_t i;
    for (i = 0; i < m->ntext; i++) {
        if (m->text[i].key_len == kn && m->text[i].value_len == vn && memcmp(m->text[i].key, k, kn) == 0 &&
            memcmp(m->text[i].value, v, vn) == 0) {
            return true;
        }
    }
    return false;
}

static void add_text(pc_cmeta *m, const char *k, const char *v)
{
    (void)pc_cmeta_add_text(m, k, strlen(k), v, strlen(v), NULL);
}

static bool blob_is(const pc_meta_blob *b, const char *s)
{
    return b->has && b->len == strlen(s) && memcmp(b->data, s, b->len) == 0;
}

static pc_cimage rich_image(pc_clayout l)
{
    pc_cimage img = test_image(l, PC_CS_U8);
    pc_buf icc = sample_icc(3000);
    pc_buf exif = sample_exif();
    set_buf(&img.icc, &icc);
    set_buf(&img.meta.exif, &exif);
    set_blob(&img.meta.xmp, SAMPLE_XMP, strlen(SAMPLE_XMP));
    img.meta.has_dpi = true;
    img.meta.dpi_x = 300.0f;
    img.meta.dpi_y = 150.0f;
    add_text(&img.meta, "Description", "synthetic test image");
    add_text(&img.meta, "Software", "photocraft");
    return img;
}

static void check_format(pc_cformat f)
{
    pc_ccaps c = pc_cformat_caps(f);
    pc_clayout layout;
    pc_cimage img;
    pc_buf bytes;
    pc_cimage back;
    size_t i;
    if (!(c.read && c.write)) {
        return;
    }
    layout = c.nlayouts > 0 ? c.layouts[0] : PC_CL_RGB;
    for (i = 0; i < c.nlayouts; i++) {
        if (c.layouts[i] == PC_CL_RGB) {
            layout = PC_CL_RGB;
        }
    }
    img = rich_image(layout);
    bytes = enc(&img, f, NULL);
    back = dec(bytes.data, bytes.len);

    CHECK(back.icc.has == c.icc);
    if (c.icc) {
        CHECK(pc_meta_blob_eq(&back.icc, &img.icc));
    } else {
        CHECK(fw_has(&img, f, NULL, PC_FW_ICC_DROPPED));
    }
    if (c.exif) {
        CHECK(pc_meta_blob_eq(&back.meta.exif, &img.meta.exif));
    } else {
        CHECK(fw_has(&img, f, NULL, PC_FW_EXIF_DROPPED));
    }
    if (c.xmp) {
        CHECK(pc_meta_blob_eq(&back.meta.xmp, &img.meta.xmp));
    } else {
        CHECK(!back.meta.xmp.has);
        CHECK(fw_has(&img, f, NULL, PC_FW_XMP_DROPPED));
    }
    if (c.dpi) {
        CHECK(back.meta.has_dpi);
        if (!(fabsf(back.meta.dpi_x - 300.0f) < 0.05f && fabsf(back.meta.dpi_y - 150.0f) < 0.05f)) {
            PC_T_FAIL("dpi %g,%g", (double)back.meta.dpi_x, (double)back.meta.dpi_y);
        }
    } else {
        CHECK(fw_has(&img, f, NULL, PC_FW_DPI_DROPPED));
    }
    if (c.text) {
        for (i = 0; i < img.meta.ntext; i++) {
            const pc_ctext *t = &img.meta.text[i];
            if (!has_text(&back.meta, t->key, t->key_len, t->value, t->value_len)) {
                PC_T_FAIL("lost text %s", t->key);
            }
        }
    } else {
        CHECK_EQ_UINT(back.meta.ntext, 0);
        CHECK(fw_has(&img, f, NULL, PC_FW_TEXT_DROPPED));
    }
    pc_cimage_fini(&back);
    pc_buf_fini(&bytes);
    pc_cimage_fini(&img);
}

static void test_png_metadata(void) { check_format(PC_CF_PNG); }
static void test_jpeg_metadata(void) { check_format(PC_CF_JPEG); }
static void test_tiff_metadata(void) { check_format(PC_CF_TIFF); }
static void test_webp_metadata(void) { check_format(PC_CF_WEBP); }
static void test_gif_metadata(void) { check_format(PC_CF_GIF); }
static void test_bmp_metadata(void) { check_format(PC_CF_BMP); }
static void test_tga_metadata(void) { check_format(PC_CF_TGA); }
static void test_ico_metadata(void) { check_format(PC_CF_ICO); }
static void test_pnm_metadata(void) { check_format(PC_CF_PNM); }
static void test_qoi_metadata(void) { check_format(PC_CF_QOI); }
static void test_exr_metadata(void) { check_format(PC_CF_EXR); }
static void test_hdr_metadata(void) { check_format(PC_CF_HDR); }
static void test_fits_metadata(void)
{
    check_format(PC_CF_FITS);
}

static void test_metadata_on_16bit_png_and_tiff(void)
{
    static const pc_cformat FS[2] = {PC_CF_PNG, PC_CF_TIFF};
    int i;
    for (i = 0; i < 2; i++) {
        pc_cimage img = test_image(PC_CL_RGBA, PC_CS_U16);
        pc_buf icc = sample_icc(512);
        pc_buf b;
        pc_cimage back;
        set_buf(&img.icc, &icc);
        img.meta.has_dpi = true;
        img.meta.dpi_x = 72.0f;
        img.meta.dpi_y = 72.0f;
        b = enc(&img, FS[i], NULL);
        back = dec(b.data, b.len);
        CHECK(pc_meta_blob_eq(&back.icc, &img.icc));
        CHECK(back.sample == PC_CS_U16);
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
        pc_cimage_fini(&img);
    }
}

static void test_icc_on_cmyk_tiff_and_jpeg(void)
{
    static const pc_cformat FS[2] = {PC_CF_TIFF, PC_CF_JPEG};
    int i;
    for (i = 0; i < 2; i++) {
        pc_cimage img = test_image(PC_CL_CMYK, PC_CS_U8);
        pc_buf icc = sample_icc(900);
        pc_buf b;
        pc_cimage back;
        set_buf(&img.icc, &icc);
        b = enc(&img, FS[i], NULL);
        back = dec(b.data, b.len);
        CHECK(back.layout == PC_CL_CMYK);
        CHECK(pc_meta_blob_eq(&back.icc, &img.icc));
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
        pc_cimage_fini(&img);
    }
}

static void test_cmyk_icc_not_written_to_rgb_format(void)
{
    pc_cimage img = test_image(PC_CL_CMYK, PC_CS_U8);
    pc_buf icc = sample_icc(900);
    pc_buf b;
    pc_cimage back;
    set_buf(&img.icc, &icc);
    b = enc(&img, PC_CF_PNG, NULL);
    back = dec(b.data, b.len);
    CHECK(!back.icc.has); /* a CMYK profile must not be attached to converted RGB data */
    CHECK(fw_has(&img, PC_CF_PNG, NULL, PC_FW_ICC_DROPPED));
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

/* ---- libjpeg-turbo as the oracle (replaces the `image` crate's JpegDecoder) ---- */

typedef struct jerr {
    struct jpeg_error_mgr mgr;
    jmp_buf jb;
} jerr;

static void jerr_exit(j_common_ptr c)
{
    jerr *e = (jerr *)(void *)c->err;
    longjmp(e->jb, 1);
}

static void jerr_silent(j_common_ptr c, int level)
{
    (void)c;
    (void)level;
}

/* The ICC profile (jpeg_read_icc_profile) and the APP1 Exif payload after "Exif\0\0". */
static bool jpeg_oracle_meta(const uint8_t *b, size_t n, pc_buf *icc, pc_buf *exif)
{
    struct jpeg_decompress_struct d;
    jerr e;
    JOCTET *prof = NULL;
    unsigned int plen = 0;
    jpeg_saved_marker_ptr m;
    d.err = jpeg_std_error(&e.mgr);
    e.mgr.error_exit = jerr_exit;
    e.mgr.emit_message = jerr_silent;
    if (setjmp(e.jb) != 0) {
        jpeg_destroy_decompress(&d);
        return false;
    }
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, b, (unsigned long)n);
    jpeg_save_markers(&d, JPEG_APP0 + 1, 0xFFFF);
    jpeg_save_markers(&d, JPEG_APP0 + 2, 0xFFFF);
    (void)jpeg_read_header(&d, TRUE);
    if (jpeg_read_icc_profile(&d, &prof, &plen)) {
        (void)pc_buf_append(icc, prof, plen, NULL);
        free(prof);
    }
    for (m = d.marker_list; m != NULL; m = m->next) {
        if (m->marker == JPEG_APP0 + 1 && m->data_length >= 6 && memcmp(m->data, "Exif\0\0", 6) == 0) {
            (void)pc_buf_append(exif, m->data + 6, m->data_length - 6, NULL);
        }
    }
    jpeg_destroy_decompress(&d);
    return true;
}

static void test_jpeg_multi_segment_icc(void)
{
    /* > 65519 bytes forces several APP2 segments. */
    static const size_t LENS[4] = {65519, 65520, 150000, 300001};
    int k;
    for (k = 0; k < 4; k++) {
        size_t len = LENS[k];
        pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
        pc_buf icc = sample_icc(len);
        pc_buf b;
        pc_cimage back;
        size_t app2 = 0;
        size_t i;
        pc_buf oicc = PC_BUF_INIT;
        pc_buf oexif = PC_BUF_INIT;
        set_buf(&img.icc, &icc);
        b = enc(&img, PC_CF_JPEG, NULL);
        for (i = 0; i + 14 <= b.len; i++) {
            app2 += b.data[i] == 0xFF && b.data[i + 1] == 0xE2 && memcmp(b.data + i + 4, "ICC_PROFIL", 10) == 0;
        }
        CHECK_EQ_UINT(app2, (len + 65518) / 65519);
        back = dec(b.data, b.len);
        CHECK(back.icc.has && back.icc.len == len);
        /* Oracle: libjpeg-turbo reads the same profile. */
        CHECK(jpeg_oracle_meta(b.data, b.len, &oicc, &oexif));
        CHECK(oicc.len == img.icc.len && memcmp(oicc.data, img.icc.data, oicc.len) == 0);
        pc_buf_fini(&oicc);
        pc_buf_fini(&oexif);
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
        pc_cimage_fini(&img);
    }
}

static bool ends_with(const pc_meta_blob *b, const char *s)
{
    size_t n = strlen(s);
    return b->has && b->len >= n && memcmp(b->data + b->len - n, s, n) == 0;
}

static bool blob_contains(const pc_meta_blob *b, const char *s)
{
    return b->has && find_bytes(b->data, b->len, s, strlen(s));
}

/* A layered document's XMP can list every document ever placed in it and the text of every type
 * layer: far more than the 64 KB one JPEG segment holds. The JPEG is still written, with that
 * bookkeeping left out of the XMP; metadata too large even then is dropped with a warning. */
static void test_jpeg_with_oversized_metadata_still_encodes(void)
{
    static const pc_csample SS[3] = {PC_CS_U8, PC_CS_U16, PC_CS_F32};
    pc_buf xmp = PC_BUF_INIT;
    pc_buf big = PC_BUF_INIT;
    pc_buf exif;
    pc_cimage img;
    pc_cimage back;
    pc_buf b;
    int i;
    (void)pc_buf_append_str(&xmp,
                            "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?><x:xmpmeta "
                            "xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
                            "<rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
                            "xmlns:ps=\"http://example.com/ps/1.0/\" xmp:CreatorTool=\"synthetic\"><ps:DocumentAncestors>\n"
                            "<rdf:Bag>",
                            NULL);
    for (i = 0; i < 3000; i++) {
        (void)pc_buf_appendf(&xmp, NULL, "<rdf:li>xmp.did:%032x</rdf:li>", (unsigned)i);
    }
    (void)pc_buf_append_str(&xmp, "</rdf:Bag></ps:DocumentAncestors><ps:TextLayers><rdf:Bag>", NULL);
    for (i = 0; i < 400; i++) {
        (void)pc_buf_appendf(&xmp, NULL,
                             "<rdf:li rdf:parseType=\"Resource\"><ps:LayerName>Layer %d</ps:LayerName><ps:LayerText>Words on "
                             "layer %d</ps:LayerText></rdf:li>",
                             i, i);
    }
    (void)pc_buf_append_str(&xmp,
                            "</rdf:Bag></ps:TextLayers><ps:DocumentAncestorsNote/></rdf:Description></rdf:RDF></x:xmpmeta>"
                            "<?xpacket end=\"w\"?>",
                            NULL);
    CHECK(xmp.len > 120000);
    for (i = 0; i < 3; i++) {
        pc_buf pb;
        pc_cimage pback;
        img = test_image(PC_CL_RGB, SS[i]);
        pc_t_name = pc_csample_name(SS[i]);
        set_blob(&img.meta.xmp, xmp.data, xmp.len);
        b = enc(&img, PC_CF_JPEG, NULL);
        back = dec(b.data, b.len);
        CHECK(blob_contains(&back.meta.xmp, "xmp:CreatorTool=\"synthetic\"") &&
              ends_with(&back.meta.xmp, "<?xpacket end=\"w\"?>"));
        CHECK(back.meta.xmp.has && !blob_contains(&back.meta.xmp, "DocumentAncestors>") &&
              !blob_contains(&back.meta.xmp, "TextLayers"));
        /* a longer name is not the element */
        CHECK(blob_contains(&back.meta.xmp, "<ps:DocumentAncestorsNote/>"));
        CHECK(!fw_too_large(&img, PC_CF_JPEG, NULL, 0));
        /* Formats without a segment limit keep the packet whole. */
        pb = enc(&img, PC_CF_PNG, NULL);
        pback = dec(pb.data, pb.len);
        CHECK(pc_meta_blob_eq(&pback.meta.xmp, &img.meta.xmp));
        pc_cimage_fini(&pback);
        pc_buf_fini(&pb);
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
        pc_cimage_fini(&img);
    }
    pc_t_name = "test_jpeg_with_oversized_metadata_still_encodes";
    /* Still too large: the JPEG is written without it. */
    img = test_image(PC_CL_RGB, PC_CS_U8);
    (void)pc_buf_append_str(&big, "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><!-- ", NULL);
    for (i = 0; i < 70000; i++) {
        (void)pc_buf_append_byte(&big, 'x', NULL);
    }
    (void)pc_buf_append_str(&big, " --></x:xmpmeta>", NULL);
    exif = sample_exif();
    while (exif.len < 70000) {
        (void)pc_buf_append_byte(&exif, 0, NULL);
    }
    set_blob(&img.meta.xmp, big.data, big.len);
    set_buf(&img.meta.exif, &exif);
    b = enc(&img, PC_CF_JPEG, NULL);
    back = dec(b.data, b.len);
    CHECK(!back.meta.xmp.has && !back.meta.exif.has);
    CHECK(fw_too_large(&img, PC_CF_JPEG, "XMP", big.len));
    CHECK(fw_too_large(&img, PC_CF_JPEG, "EXIF", 70000));
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
    pc_buf_fini(&big);
    pc_buf_fini(&xmp);
}

static void test_embed_flags_disable_metadata(void)
{
    static const pc_cformat FS[4] = {PC_CF_PNG, PC_CF_JPEG, PC_CF_TIFF, PC_CF_WEBP};
    pc_cimage img = rich_image(PC_CL_RGB);
    pc_cencode_opts o = pc_cencode_opts_default();
    int i;
    o.embed_icc = false;
    o.embed_metadata = false;
    for (i = 0; i < 4; i++) {
        pc_buf b = enc(&img, FS[i], &o);
        pc_cimage back = dec(b.data, b.len);
        pc_t_name = pc_cformat_debug(FS[i]);
        CHECK(!back.icc.has);
        CHECK(!back.meta.exif.has && !back.meta.xmp.has);
        CHECK_EQ_UINT(back.meta.ntext, 0);
        CHECK(!fw_has(&img, PC_CF_BMP, &o, PC_FW_ICC_DROPPED) && !fw_has(&img, PC_CF_BMP, &o, PC_FW_EXIF_DROPPED));
        pc_cimage_fini(&back);
        pc_buf_fini(&b);
    }
    pc_cimage_fini(&img);
}

static void test_png_unicode_text_uses_itxt(void)
{
    static const char *K[2] = {"Title", "Author"};
    static const char *V[2] = {"\xc3\x9c" "n" "\xc3\xaf" "c" "\xc3\xb6" "d" "\xc3\xa9 \xe2\x98\x83 \xe9\x9b\xaa", "latin1 ok \xc3\xa9"};
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    pc_buf b;
    pc_cimage back;
    int i;
    add_text(&img.meta, K[0], V[0]);
    add_text(&img.meta, K[1], V[1]);
    b = enc(&img, PC_CF_PNG, NULL);
    CHECK(find_bytes(b.data, b.len, "iTXt", 4));
    CHECK(find_bytes(b.data, b.len, "tEXt", 4));
    back = dec(b.data, b.len);
    /* sorted equality == same size and every entry present (keys are distinct) */
    CHECK_EQ_UINT(back.meta.ntext, 2);
    for (i = 0; i < 2; i++) {
        CHECK(has_text(&back.meta, K[i], strlen(K[i]), V[i], strlen(V[i])));
    }
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_png_xmp_in_itxt_keyword(void)
{
    pc_cimage img = test_image(PC_CL_GRAY, PC_CS_U8);
    pc_buf b;
    pc_cimage back;
    set_blob(&img.meta.xmp, SAMPLE_XMP, strlen(SAMPLE_XMP));
    b = enc(&img, PC_CF_PNG, NULL);
    CHECK(find_bytes(b.data, b.len, "XML:com.adobe.xmp", 17));
    back = dec(b.data, b.len);
    CHECK(blob_is(&back.meta.xmp, SAMPLE_XMP));
    CHECK_EQ_UINT(back.meta.ntext, 0); /* XMP must not leak into text */
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_png_exif_chunk_written(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    pc_buf exif = sample_exif();
    pc_buf b;
    set_buf(&img.meta.exif, &exif);
    b = enc(&img, PC_CF_PNG, NULL);
    CHECK(find_bytes(b.data, b.len, "eXIf", 4));
    CHECK(!find_bytes(b.data, b.len, "iCCP", 4));
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_jpeg_exif_readable_by_oracle(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U8);
    pc_buf exif = sample_exif();
    pc_buf want = sample_exif();
    pc_buf b;
    pc_buf oicc = PC_BUF_INIT;
    pc_buf oexif = PC_BUF_INIT;
    set_buf(&img.meta.exif, &exif);
    b = enc(&img, PC_CF_JPEG, NULL);
    /* Oracle: libjpeg-turbo's APP1 marker (replaces the image crate's exif_metadata). */
    CHECK(jpeg_oracle_meta(b.data, b.len, &oicc, &oexif));
    CHECK(oexif.len >= want.len && memcmp(oexif.data + oexif.len - want.len, want.data, want.len) == 0);
    pc_buf_fini(&oicc);
    pc_buf_fini(&oexif);
    pc_buf_fini(&want);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_jpeg_dpi_in_jfif(void)
{
    pc_cimage img = test_image(PC_CL_GRAY, PC_CS_U8);
    pc_buf b;
    pc_cimage back;
    img.meta.has_dpi = true;
    img.meta.dpi_x = 96.0f;
    img.meta.dpi_y = 200.0f;
    b = enc(&img, PC_CF_JPEG, NULL);
    back = dec(b.data, b.len);
    CHECK(back.meta.has_dpi && back.meta.dpi_x == 96.0f && back.meta.dpi_y == 200.0f);
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_tiff_icc_readable_by_oracle(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_U16);
    pc_buf icc = sample_icc(1234);
    pc_buf b;
    pc_cimage back;
    set_buf(&img.icc, &icc);
    b = enc(&img, PC_CF_TIFF, NULL);
    /* No libtiff here: our own decoder stands in for the image crate's TiffDecoder. */
    back = dec(b.data, b.len);
    CHECK(pc_meta_blob_eq(&back.icc, &img.icc));
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

/* ---- libpng as the oracle (replaces the image crate's PngDecoder) ---- */

typedef struct png_src {
    const uint8_t *b;
    size_t n;
    size_t at;
} png_src;

static void png_src_read(png_structp p, png_bytep out, size_t len)
{
    png_src *s = png_get_io_ptr(p);
    if (len > s->n - s->at) {
        png_error(p, "unexpected end of data");
    }
    memcpy(out, s->b + s->at, len);
    s->at += len;
}

static void png_quiet(png_structp p, png_const_charp msg)
{
    (void)p;
    (void)msg;
}

static void png_quiet_err(png_structp p, png_const_charp msg)
{
    (void)msg;
    png_longjmp(p, 1);
}

/* libpng validates iCCP and drops a profile whose header fields don't fit (our sample profile is
 * noise), so the chunk is read raw through libpng's unknown-chunk path and inflated with zlib. */
static bool png_oracle_icc(const uint8_t *b, size_t n, pc_buf *icc)
{
    png_src src = {b, n, 0};
    png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_quiet_err, png_quiet);
    png_infop info;
    png_unknown_chunkp unk;
    int nunk;
    int i;
    if (p == NULL) {
        return false;
    }
    info = png_create_info_struct(p);
    if (info == NULL || setjmp(png_jmpbuf(p)) != 0) {
        png_destroy_read_struct(&p, &info, NULL);
        return false;
    }
    png_set_read_fn(p, &src, png_src_read);
    png_set_keep_unknown_chunks(p, PNG_HANDLE_CHUNK_ALWAYS, (png_const_bytep) "iCCP", 1);
    png_read_info(p, info);
    nunk = png_get_unknown_chunks(p, info, &unk);
    for (i = 0; i < nunk; i++) {
        const uint8_t *d = unk[i].data;
        const uint8_t *z = memchr(d, 0, unk[i].size);
        if (memcmp(unk[i].name, "iCCP", 4) == 0 && z != NULL && (size_t)(z - d) + 2 <= unk[i].size) {
            uLongf cap = 1u << 22;
            uint8_t *out = malloc(cap);
            size_t off = (size_t)(z - d) + 2;
            if (out != NULL && uncompress(out, &cap, d + off, (uLong)(unk[i].size - off)) == Z_OK) {
                (void)pc_buf_append(icc, out, cap, NULL);
            }
            free(out);
        }
    }
    png_destroy_read_struct(&p, &info, NULL);
    return true;
}

static void test_png_icc_readable_by_oracle(void)
{
    pc_cimage img = test_image(PC_CL_RGBA, PC_CS_U8);
    pc_buf icc = sample_icc(777);
    pc_buf b;
    pc_buf oicc = PC_BUF_INIT;
    set_buf(&img.icc, &icc);
    b = enc(&img, PC_CF_PNG, NULL);
    CHECK(png_oracle_icc(b.data, b.len, &oicc));
    CHECK(oicc.len == img.icc.len && memcmp(oicc.data, img.icc.data, oicc.len) == 0);
    pc_buf_fini(&oicc);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

static void test_metadata_survives_depth_conversion(void)
{
    pc_cimage img = test_image(PC_CL_RGB, PC_CS_F32);
    pc_buf icc = sample_icc(300);
    pc_buf b;
    pc_cimage back;
    set_buf(&img.icc, &icc);
    set_blob(&img.meta.xmp, SAMPLE_XMP, strlen(SAMPLE_XMP));
    b = enc(&img, PC_CF_PNG, NULL);
    back = dec(b.data, b.len);
    CHECK(pc_meta_blob_eq(&back.icc, &img.icc));
    CHECK(pc_meta_blob_eq(&back.meta.xmp, &img.meta.xmp));
    pc_cimage_fini(&back);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
}

int main(int argc, char **argv)
{
    RUN(test_png_metadata);
    RUN(test_jpeg_metadata);
    RUN(test_tiff_metadata);
    RUN(test_webp_metadata);
    RUN(test_gif_metadata);
    RUN(test_bmp_metadata);
    RUN(test_tga_metadata);
    RUN(test_ico_metadata);
    RUN(test_pnm_metadata);
    RUN(test_qoi_metadata);
    RUN(test_exr_metadata);
    RUN(test_hdr_metadata);
    RUN(test_fits_metadata);
    RUN(test_metadata_on_16bit_png_and_tiff);
    RUN(test_icc_on_cmyk_tiff_and_jpeg);
    RUN(test_cmyk_icc_not_written_to_rgb_format);
    RUN(test_jpeg_multi_segment_icc);
    RUN(test_jpeg_with_oversized_metadata_still_encodes);
    RUN(test_embed_flags_disable_metadata);
    RUN(test_png_unicode_text_uses_itxt);
    RUN(test_png_xmp_in_itxt_keyword);
    RUN(test_png_exif_chunk_written);
    RUN(test_jpeg_exif_readable_by_oracle);
    RUN(test_jpeg_dpi_in_jfif);
    RUN(test_tiff_icc_readable_by_oracle);
    RUN(test_png_icc_readable_by_oracle);
    RUN(test_metadata_survives_depth_conversion);
    return pc_t_finish();
}
