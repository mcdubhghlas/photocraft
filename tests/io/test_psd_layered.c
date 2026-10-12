/* PSD export through the other io paths (crates/io/tests): tiff_layers.rs (the layered-TIFF
 * tests; I1 ported the four flat ones), color.rs psd_icc_roundtrip_byte_exact, resolution_axes.rs
 * (the PSD and layered-TIFF tests), flat.rs (effects_are_written_to_psd_and_read_back,
 * psd_to_png_to_psd_chain), banded.rs (psd_merged_image_matches_composite_and_drops_near_opaque_
 * alpha), orientation.rs (the PSD half of stale_orientation_in_document_metadata_is_never_
 * exported) and xmp_export.rs (layered_saves_keep_the_xmp_whatever_the_option, PSD half). */
#include "harness.h"

#include "psd_rt.h"
#include "../psd/testgen.h"
#include "psd/tiff.h"

/* ---- tiff_layers.rs ---- */

static pc_io_export_result tiff(const pc_document *d, bool layers)
{
    pc_io_export_opts o = pc_io_export_opts_default();
    pc_io_export_result r;
    o.tiff_layers = layers;
    must(pc_io_export(d, "x.tif", &o, &r, NULL));
    return r;
}

/* A layered-TIFF round trip, with the reference's checks of the written tags. */
static pc_document tiff_roundtrip(const pc_document *d)
{
    pc_io_export_result r = tiff(d, true);
    const uint8_t *res = NULL;
    const uint8_t *lay = NULL;
    size_t nres = 0;
    size_t nlay = 0;
    pc_psd_isd isd;
    pc_psd_warnings w = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    pc_cimage flat;
    pc_buf b = PC_BUF_INIT;
    pc_document back;
    size_t extra = d->nchannels + (d->has_quick_mask ? 1 : 0);
    pc_ctiff_photoshop_tags(r.bytes, r.len, &res, &nres, &lay, &nlay);
    CHECK(lay != NULL && res != NULL && nres > 0);
    if (lay != NULL) {
        pc_psd_byte_order order = pc_ctiff_writes_little_endian() ? PC_PSD_LITTLE : PC_PSD_BIG;
        must(pc_psd_isd_parse(lay, nlay, &isd, &w, NULL));
        CHECK_EQ_UINT(w.n, 0);
        pc_psd_warnings_fini(&w);
        CHECK(isd.byte_order == order);
        must(pc_psd_isd_to_bytes(&isd, order, &again, NULL, NULL));
        CHECK(again.len == nlay && memcmp(again.data, lay, nlay) == 0);
        pc_psd_bytes_fini(&again);
        pc_psd_isd_fini(&isd);
    }
    /* The TIFF is a plain image to any other reader: the composite at the document's size. */
    must(pc_cdecode(r.bytes, r.len, &flat, NULL));
    CHECK(flat.width == d->size.width && flat.height == d->size.height);
    pc_cimage_fini(&flat);
    CHECK(pc_io_warnings_any(&r.warnings, "alpha channel(s) are not written") == (extra > 0));
    must(pc_buf_append(&b, r.bytes, r.len, NULL));
    back = rt_import("x.tif", &b);
    pc_buf_fini(&b);
    pc_io_export_result_fini(&r);
    return back;
}

/* doc without its extra alpha channels and Quick Mask (a TIFF can't keep them). */
static pc_document expected(const pc_document *d)
{
    pc_document e;
    must(pc_document_clone(d, &e, NULL));
    for (size_t i = 0; i < e.nchannels; i++) {
        pc_alpha_channel_fini(&e.channels[i]);
    }
    free(e.channels);
    e.channels = NULL;
    e.nchannels = 0;
    if (e.has_quick_mask) {
        pc_alpha_channel_fini(&e.quick_mask);
        e.has_quick_mask = false;
    }
    return e;
}

static void tiff_rt_case(pc_color_mode mode, pc_sample_type depth, bool all)
{
    pc_document d = all ? t_gen_doc_all(mode, depth) : t_gen_doc(mode, depth);
    pc_document back = tiff_roundtrip(&d);
    pc_document e = expected(&d);
    rt_docs_eq(&e, &back);
    pc_document_fini(&e);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_tiff_rt_cases(void)
{
    static const pc_sample_type D[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t k = 0; k < 3; k++) {
        tiff_rt_case(PC_COLOR_MODE_RGB, D[k], true);
        tiff_rt_case(PC_COLOR_MODE_GRAYSCALE, D[k], true);
    }
    tiff_rt_case(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true);
    tiff_rt_case(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U16, true);
    tiff_rt_case(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    tiff_rt_case(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, false);
    tiff_rt_case(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, false);
}

static void test_tiff_and_psd_open_as_the_same_document(void)
{
    static const pc_color_mode M[3] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_GRAYSCALE};
    static const pc_sample_type D[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t i = 0; i < 3; i++) {
        pc_document d = t_gen_doc_all(M[i], D[i]);
        pc_buf psd = rt_export(&d, "x.psd", NULL);
        pc_document from_psd = rt_import("x.psd", &psd);
        pc_document from_tif = tiff_roundtrip(&d);
        pc_document e = expected(&from_psd);
        pc_io_export_result t = tiff(&d, true);
        pc_cimage img;
        pc_buf flat = PC_BUF_INIT;
        pc_document fd;
        pc_compose_buffer a;
        pc_compose_buffer b;
        rt_docs_eq(&e, &from_tif);
        /* The composite the TIFF carries is what the document looks like. */
        must(pc_cdecode(t.bytes, t.len, &img, NULL));
        must(pc_cencode(&img, PC_CF_TIFF, NULL, &flat, NULL));
        fd = rt_import("flat.tif", &flat);
        must(pc_compose_flatten(&d, &a, NULL));
        must(pc_compose_flatten(&fd, &b, NULL));
        CHECK(rt_max_diff(&a, &b) <= 2.0f / 255.0f);
        pc_compose_buffer_fini(&a);
        pc_compose_buffer_fini(&b);
        pc_document_fini(&fd);
        pc_buf_fini(&flat);
        pc_cimage_fini(&img);
        pc_io_export_result_fini(&t);
        pc_document_fini(&e);
        pc_document_fini(&from_tif);
        pc_document_fini(&from_psd);
        pc_buf_fini(&psd);
        pc_document_fini(&d);
    }
}

static void test_every_compression_keeps_the_layers(void)
{
    static const pc_tiff_compression C[4] = {PC_TIFF_NONE, PC_TIFF_LZW, PC_TIFF_DEFLATE, PC_TIFF_PACKBITS};
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16);
    for (size_t i = 0; i < 4; i++) {
        pc_io_export_opts o = pc_io_export_opts_default();
        pc_buf b;
        pc_document back;
        o.tiff_layers = true;
        o.encode.tiff_compression = C[i];
        b = rt_export(&d, "x.tiff", &o);
        back = rt_import("x.tiff", &b);
        rt_docs_eq(&d, &back);
        pc_document_fini(&back);
        pc_buf_fini(&b);
    }
    pc_document_fini(&d);
}

/* #1545: a layered TIFF carries EXIF as resource 1058 and reads it back, so it must not report a
 * loss; a flat TIFF really loses the tags and says so. */
static void test_layered_tiff_keeps_exif_without_claiming_a_loss(void)
{
    static const char ARTIST[] = "fixture-artist";
    uint8_t ex[64];
    size_t n = 0;
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_io_export_result r;
    pc_buf b = PC_BUF_INIT;
    pc_document back;
    memcpy(ex, "II*\0\x08\0\0\0", 8);
    n = 8;
    pc_store_le16(ex + n, 1);
    pc_store_le16(ex + n + 2, 0x013b);
    pc_store_le16(ex + n + 4, 2);
    pc_store_le32(ex + n + 6, (uint32_t)sizeof ARTIST);
    pc_store_le32(ex + n + 10, 26);
    pc_store_le32(ex + n + 14, 0);
    n += 18;
    memcpy(ex + n, ARTIST, sizeof ARTIST);
    n += sizeof ARTIST;
    must(pc_io_set_blob(&d.metadata.exif, ex, n, NULL));
    r = tiff(&d, true);
    CHECK(!pc_io_warnings_any(&r.warnings, "EXIF"));
    must(pc_buf_append(&b, r.bytes, r.len, NULL));
    back = rt_import("x.tif", &b);
    CHECK(back.metadata.exif != NULL);
    {
        const uint8_t *k = pc_blob_data(back.metadata.exif);
        size_t kl = pc_blob_len(back.metadata.exif);
        bool found = false;
        for (size_t i = 0; i + strlen(ARTIST) <= kl && !found; i++) {
            found = memcmp(k + i, ARTIST, strlen(ARTIST)) == 0;
        }
        CHECK(found);
    }
    pc_document_fini(&back);
    pc_buf_fini(&b);
    pc_io_export_result_fini(&r);
    r = tiff(&d, false);
    CHECK(pc_io_warnings_any(&r.warnings, "EXIF"));
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

/* A translucent layer over nothing: the TIFF's composite keeps its colour under the alpha
 * instead of being matted against white like a PSD's merged image. */
static void test_transparency_is_straight_in_the_composite(void)
{
    pc_document d = t_doc("t", 4, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer l = t_raster("Red", pc_document_pixel_format(&d), pc_rect_make(0, 0, 4, 4), 0, false);
    float px[64];
    pc_io_export_result r;
    pc_cimage img;
    pc_buf b = PC_BUF_INIT;
    pc_document back;
    for (int i = 0; i < 16; i++) {
        px[4 * i] = 1.0f;
        px[4 * i + 1] = 0.0f;
        px[4 * i + 2] = 0.0f;
        px[4 * i + 3] = 0.5f;
    }
    must(pc_surface_write_region(&l.content.u.raster, pc_rect_make(0, 0, 4, 4), px, 64, NULL));
    t_push(&d, &l);
    r = tiff(&d, true);
    must(pc_cdecode(r.bytes, r.len, &img, NULL));
    CHECK(pc_clayout_has_alpha(img.layout));
    CHECK(img.len >= 4 && img.data[0] == 255 && img.data[1] == 0 && img.data[2] == 0 && img.data[3] == 128);
    pc_cimage_fini(&img);
    must(pc_buf_append(&b, r.bytes, r.len, NULL));
    back = rt_import("x.tif", &b);
    rt_docs_eq(&d, &back);
    pc_document_fini(&back);
    pc_buf_fini(&b);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static void test_big_documents_use_the_psb_signature(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_io_export_opts o = pc_io_export_opts_default();
    pc_buf b;
    const uint8_t *res = NULL;
    const uint8_t *lay = NULL;
    size_t nres = 0;
    size_t nlay = 0;
    pc_document back;
    o.force_psb = true;
    o.tiff_layers = true;
    b = rt_export(&d, "x.tif", &o);
    pc_ctiff_photoshop_tags(b.data, b.len, &res, &nres, &lay, &nlay);
    CHECK(lay != NULL && nlay >= 36 && memcmp(lay, PC_PSD_ISD_SIGNATURE_PSB, 36) == 0);
    back = rt_import("x.tif", &b);
    rt_docs_eq(&d, &back);
    pc_document_fini(&back);
    pc_buf_fini(&b);
    pc_document_fini(&d);
}

static void test_psdtags_fixtures_open_with_their_layers(void)
{
    static const char *const NAMES[4] = {"layered-le-8bit.tif", "layered-be-8bit.tif", "layered-le-16bit.tif",
                                         "layered-be-16bit.tif"};
    for (size_t i = 0; i < 4; i++) {
        char path[256];
        pc_buf b = PC_BUF_INIT;
        pc_document d;
        pc_document back;
        pc_document e;
        pc_io_export_result out;
        pc_buf ob = PC_BUF_INIT;
        (void)snprintf(path, sizeof path, "corpus/layered-tiff/%s", NAMES[i]);
        if (pc_file_read(path, (size_t)1 << 28, &b, NULL) != PC_OK) {
            (void)printf("skipped %s (corpus/layered-tiff/ absent)\n", NAMES[i]);
            continue;
        }
        d = rt_import(NAMES[i], &b);
        CHECK(d.size.width == 6 && d.size.height == 4 && d.nlayers == 2);
        CHECK(d.nlayers == 2 && d.layers[1].blend == PC_BLEND_MULTIPLY);
        out = tiff(&d, true);
        must(pc_buf_append(&ob, out.bytes, out.len, NULL));
        back = rt_import(NAMES[i], &ob);
        e = expected(&d);
        rt_docs_eq(&e, &back);
        pc_document_fini(&e);
        pc_document_fini(&back);
        pc_buf_fini(&ob);
        pc_io_export_result_fini(&out);
        pc_document_fini(&d);
        pc_buf_fini(&b);
    }
}

static size_t find_bytes(const uint8_t *hay, size_t n, const uint8_t *needle, size_t m)
{
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(hay + i, needle, m) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Advanced Blending, fill opacity, channel restrictions and Blend If survive a layered TIFF
 * with its layer data in either byte order. */
static void test_advanced_blending_survives_layered_tiff_in_both_byte_orders(void)
{
    static const pc_sample_type D[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t k = 0; k < 3; k++) {
        pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, D[k]);
        pc_advanced_blending adv = {PC_KNOCKOUT_DEEP, true, false, false, true, true};
        const pc_blend_range pair[2] = {{{10, 40}, {200, 250}}, {{0, 0}, {128, 128}}};
        pc_io_export_result r;
        const uint8_t *res = NULL;
        const uint8_t *lay = NULL;
        size_t nres = 0;
        size_t nlay = 0;
        pc_psd_isd isd;
        pc_psd_bytes swapped = {NULL, 0};
        pc_psd_warnings w = {NULL, 0};
        pc_buf native = PC_BUF_INIT;
        pc_buf flipped = PC_BUF_INIT;
        pc_document e;
        d.layers[1].advanced = adv;
        d.layers[1].fill_opacity = 64.0f / 255.0f;
        d.layers[1].excluded_channels = 2;
        must(pc_blend_if_set(&d.layers[1].blend_if, 0, pair, NULL));
        d.layers[2].advanced.knockout = PC_KNOCKOUT_SHALLOW;
        r = tiff(&d, true);
        pc_ctiff_photoshop_tags(r.bytes, r.len, &res, &nres, &lay, &nlay);
        CHECK(lay != NULL);
        if (lay == NULL) {
            pc_io_export_result_fini(&r);
            pc_document_fini(&d);
            continue;
        }
        must(pc_psd_isd_parse(lay, nlay, &isd, NULL, NULL));
        must(pc_psd_isd_to_bytes(&isd, isd.byte_order == PC_PSD_LITTLE ? PC_PSD_BIG : PC_PSD_LITTLE, &swapped,
                                 &w, NULL));
        CHECK_EQ_UINT(w.n, 0);
        CHECK_EQ_UINT(swapped.len, nlay);
        must(pc_buf_append(&native, r.bytes, r.len, NULL));
        must(pc_buf_append(&flipped, r.bytes, r.len, NULL));
        {
            size_t at = find_bytes(r.bytes, r.len, lay, nlay);
            CHECK(at != SIZE_MAX);
            if (at != SIZE_MAX && swapped.len == nlay) {
                memcpy(flipped.data + at, swapped.data, nlay);
            }
        }
        e = expected(&d);
        for (int o = 0; o < 2; o++) {
            pc_document back = rt_import("x.tif", o == 0 ? &native : &flipped);
            const pc_layer *l = &back.layers[1];
            CHECK(pc_advanced_blending_eq(&l->advanced, &adv));
            CHECK(l->fill_opacity == 64.0f / 255.0f && l->excluded_channels == 2);
            CHECK(pc_blend_if_eq(&l->blend_if, &d.layers[1].blend_if));
            CHECK(back.layers[2].advanced.knockout == PC_KNOCKOUT_SHALLOW);
            rt_docs_eq(&e, &back);
            pc_document_fini(&back);
        }
        pc_document_fini(&e);
        pc_buf_fini(&native);
        pc_buf_fini(&flipped);
        pc_psd_warnings_fini(&w);
        pc_psd_bytes_fini(&swapped);
        pc_psd_isd_fini(&isd);
        pc_io_export_result_fini(&r);
        pc_document_fini(&d);
    }
}

static void try_import(const uint8_t *b, size_t n)
{
    pc_io_import_result r;
    if (pc_io_import("x.tif", b, n, &r, NULL) == PC_OK) {
        pc_io_import_result_fini(&r);
    }
}

static void test_damaged_layer_data_opens_flattened(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_io_export_result r = tiff(&d, true);
    const uint8_t *res = NULL;
    const uint8_t *lay = NULL;
    size_t nres = 0;
    size_t nlay = 0;
    uint8_t *bad = xmalloc(r.len);
    size_t at;
    pc_io_import_result r2;
    pc_ctiff_photoshop_tags(r.bytes, r.len, &res, &nres, &lay, &nlay);
    at = lay != NULL ? find_bytes(r.bytes, r.len, lay, nlay) : SIZE_MAX;
    CHECK(at != SIZE_MAX && nlay > 60);
    memcpy(bad, r.bytes, r.len);
    for (size_t i = 0; at != SIZE_MAX && i < 24; i++) {
        bad[at + 36 + i] ^= 0xa5;
    }
    must(pc_io_import("x.tif", bad, r.len, &r2, NULL));
    CHECK_EQ_UINT(r2.document.nlayers, 1);
    CHECK(pc_io_warnings_any(&r2.warnings, "could not be read"));
    pc_io_import_result_fini(&r2);
    /* Every truncation and a sweep of bit flips through the whole file: never a crash. */
    for (size_t cut = 0; cut < r.len; cut += 11) {
        try_import(r.bytes, cut);
    }
    for (size_t i = 0; i < r.len; i += 29) {
        memcpy(bad, r.bytes, r.len);
        bad[i] ^= 0x5a;
        try_import(bad, r.len);
    }
    free(bad);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

/* ---- color.rs, resolution_axes.rs ---- */

static void test_psd_icc_roundtrip_byte_exact(void)
{
    for (int b = 0; b < PC_BUILTIN_COUNT; b++) {
        pc_buf icc = PC_BUF_INIT;
        pc_color_mode mode = b == PC_BUILTIN_COATED_CMYK                             ? PC_COLOR_MODE_CMYK
                             : b == PC_BUILTIN_GRAY_GAMMA22 || b == PC_BUILTIN_SGRAY ? PC_COLOR_MODE_GRAYSCALE
                             : b == PC_BUILTIN_LAB_D50                               ? PC_COLOR_MODE_LAB
                                                                                     : PC_COLOR_MODE_RGB;
        pc_document d = t_gen_doc(mode, PC_SAMPLE_TYPE_U8);
        pc_buf out;
        pc_document back;
        must(pc_profile_to_bytes(pc_builtin_profile((pc_builtin)b), &icc, NULL));
        must(pc_io_set_blob(&d.icc_profile, icc.data, icc.len, NULL));
        out = rt_export(&d, "x.psd", NULL);
        back = rt_import("x.psd", &out);
        CHECK(back.icc_profile != NULL && pc_blob_len(back.icc_profile) == icc.len &&
              memcmp(pc_blob_data(back.icc_profile), icc.data, icc.len) == 0);
        pc_document_fini(&back);
        pc_buf_fini(&out);
        pc_document_fini(&d);
        pc_buf_fini(&icc);
    }
}

static size_t resolution_notes(const pc_io_import_result *r, const char *a, const char *b)
{
    size_t n = 0;
    for (size_t i = 0; i < r->warnings.n; i++) {
        if (strstr(r->warnings.v[i], "resolution") != NULL) {
            n++;
            CHECK(a == NULL || (strstr(r->warnings.v[i], a) != NULL && strstr(r->warnings.v[i], b) != NULL));
        }
    }
    return n;
}

static pc_buf psd_with_resolution(pc_psd_resolution ri)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    uint8_t d[16];
    size_t k = 0;
    pc_psd_bytes b = {NULL, 0};
    pc_buf out = PC_BUF_INIT;
    for (size_t i = 0; i < f.nresources; i++) {
        if (f.resources[i].id == PC_PSD_RES_RESOLUTION_INFO) {
            pc_psd_resource_fini(&f.resources[i]);
        } else {
            f.resources[k++] = f.resources[i];
        }
    }
    f.nresources = k;
    pc_psd_resolution_write(&ri, d);
    tg_res(&f, PC_PSD_RES_RESOLUTION_INFO, d, 16);
    must(pc_psd_file_to_bytes(&f, &b, NULL));
    must(pc_buf_append(&out, b.data, b.len, NULL));
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
    return out;
}

static void test_resolution_axes(void)
{
    pc_psd_resolution ri = pc_psd_resolution_from_dpi(300.0);
    pc_buf b;
    pc_io_import_result r;
    ri.v_res_fixed = pc_psd_resolution_from_dpi(150.0).v_res_fixed;
    b = psd_with_resolution(ri);
    must(pc_io_import("x.psd", b.data, b.len, &r, NULL));
    CHECK(fabsf(r.document.resolution_dpi - 300.0f) < 1e-3f);
    CHECK_EQ_UINT(resolution_notes(&r, "150.00", "300.00"), 1);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&b);
    b = psd_with_resolution(pc_psd_resolution_from_dpi(300.0));
    must(pc_io_import("x.psd", b.data, b.len, &r, NULL));
    CHECK(fabsf(r.document.resolution_dpi - 300.0f) < 1e-3f);
    CHECK_EQ_UINT(resolution_notes(&r, NULL, NULL), 0);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&b);
    /* Each axis is read in its own unit: 100 px/cm on both is 254 ppi on both. */
    ri = pc_psd_resolution_from_dpi(100.0);
    ri.h_res_unit = 2;
    ri.v_res_unit = 2;
    b = psd_with_resolution(ri);
    must(pc_io_import("x.psd", b.data, b.len, &r, NULL));
    CHECK(fabsf(r.document.resolution_dpi - 254.0f) < 1e-3f);
    CHECK_EQ_UINT(resolution_notes(&r, NULL, NULL), 0);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&b);
    /* A layered TIFF opens through the PSD importer with the TIFF's own resolution tags. */
    for (int eq = 0; eq < 2; eq++) {
        pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        pc_io_export_result t = tiff(&d, true);
        pc_cimage img;
        pc_buf enc = PC_BUF_INIT;
        must(pc_cdecode(t.bytes, t.len, &img, NULL));
        CHECK(img.meta.photoshop_layers.has);
        img.meta.has_dpi = true;
        img.meta.dpi_x = 300.0f;
        img.meta.dpi_y = eq ? 300.0f : 150.0f;
        must(pc_cencode(&img, PC_CF_TIFF, NULL, &enc, NULL));
        must(pc_io_import("x.tif", enc.data, enc.len, &r, NULL));
        CHECK(r.document.nlayers > 1);
        CHECK(fabsf(r.document.resolution_dpi - 300.0f) < 1e-3f);
        CHECK_EQ_UINT(resolution_notes(&r, eq ? NULL : "150.00", "300.00"), eq ? 0 : 1);
        pc_io_import_result_fini(&r);
        pc_buf_fini(&enc);
        pc_cimage_fini(&img);
        pc_io_export_result_fini(&t);
        pc_document_fini(&d);
    }
}

/* ---- flat.rs, banded.rs, orientation.rs, xmp_export.rs ---- */

static void test_effects_are_written_to_psd_and_read_back(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer *l = &d.layers[1];
    pc_io_export_result r;
    pc_buf b = PC_BUF_INIT;
    pc_document back;
    l->effects.items = xmalloc(2 * sizeof *l->effects.items);
    memset(&l->effects.items[0], 0, sizeof l->effects.items[0]);
    l->effects.items[0].kind = PC_FX_COLOR_OVERLAY;
    l->effects.items[0].u.color_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    l->effects.items[0].u.color_overlay.color = PC_COLOR_BLACK;
    pc_effect_default_drop_shadow(&l->effects.items[1]);
    l->effects.nitems = 2;
    must(pc_io_export(&d, "a.psd", NULL, &r, NULL));
    CHECK(!pc_io_warnings_any(&r.warnings, "effects"));
    must(pc_buf_append(&b, r.bytes, r.len, NULL));
    back = rt_import("a.psd", &b);
    CHECK_EQ_UINT(back.layers[1].effects.nitems, 2);
    CHECK(back.layers[1].effects.psd_raw != NULL);
    pc_document_fini(&back);
    pc_buf_fini(&b);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

static void test_psd_to_png_to_psd_chain(void)
{
    pc_document d = t_gen_doc_all(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16);
    pc_buf psd = rt_export(&d, "a.psd", NULL);
    pc_document d2 = rt_import("a.psd", &psd);
    pc_buf png = rt_export(&d2, "a.png", NULL);
    pc_document d3 = rt_import("a.png", &png);
    pc_buf psd2;
    pc_psd_file f;
    CHECK(d3.depth == PC_SAMPLE_TYPE_U16);
    psd2 = rt_export(&d3, "b.psd", NULL);
    CHECK(pc_psd_file_parse(psd2.data, psd2.len, &f, NULL) == PC_OK);
    pc_psd_file_fini(&f);
    pc_buf_fini(&psd2);
    pc_document_fini(&d3);
    pc_buf_fini(&png);
    pc_document_fini(&d2);
    pc_buf_fini(&psd);
    pc_document_fini(&d);
}

/* banded.rs layered(): a gradient background (alpha in its top-right quarter) under a
 * half-opacity red bar. */
static pc_document banded_doc(uint32_t w, uint32_t h, float alpha)
{
    pc_document d = t_doc("b", w, h, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_surface bg;
    float *row = xmalloc((size_t)w * 4 * sizeof *row);
    pc_layer l;
    static const float RED[4] = {1, 0, 0, 1};
    pc_surface_init(&bg, fmt);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            float a = (int32_t)x > (int32_t)w / 2 && (int32_t)y < (int32_t)h / 2 ? alpha : 1.0f;
            row[4 * x] = (float)x / (float)w;
            row[4 * x + 1] = (float)y / (float)h;
            row[4 * x + 2] = 0.5f;
            row[4 * x + 3] = a;
        }
        must(pc_surface_write_region(&bg, pc_rect_make(0, (int32_t)y, (int32_t)w, (int32_t)y + 1), row,
                                     (size_t)w * 4, NULL));
    }
    free(row);
    must(pc_io_raster_layer("bg", &bg, &l, NULL));
    pc_surface_fini(&bg);
    t_push(&d, &l);
    must(pc_layer_raster(&l, "top", fmt, NULL));
    must(pc_surface_fill_rect(&l.content.u.raster, pc_rect_make(10, 10, (int32_t)w - 10, 40), RED, 4, NULL));
    l.opacity = 0.5f;
    t_push(&d, &l);
    return d;
}

static void test_psd_merged_image_matches_composite_and_drops_near_opaque_alpha(void)
{
    static const float ALPHAS[3] = {1.0f, 0.999f, 0.5f};
    for (size_t i = 0; i < 3; i++) {
        float alpha = ALPHAS[i];
        pc_document d = banded_doc(400, 300, alpha);
        pc_psd_file f;
        float (*merged)[4] = NULL;
        size_t n = 0;
        pc_compose_buffer want;
        float m = 0.0f;
        must(pc_io_document_to_psd(&d, &f, NULL));
        CHECK(pc_psd_file_merged_has_alpha(&f) == (alpha < 0.99f));
        must(pc_io_merged_composite(&f, &merged, &n, NULL));
        must(pc_compose_flatten(&d, &want, NULL));
        CHECK_EQ_UINT(n, want.len);
        for (size_t p = 0; p < n && p < want.len; p++) {
            for (int c = 0; c < 4; c++) {
                m = fmaxf(m, fabsf(merged[p][c] - want.px[p][c]));
            }
        }
        /* Un-matting divides the 8-bit rounding error by alpha. */
        CHECK(m <= 1.0f / (255.0f * alpha) + 1e-4f);
        pc_compose_buffer_fini(&want);
        free(merged);
        pc_psd_file_fini(&f);
        pc_document_fini(&d);
    }
}

/* The PSD half of stale_orientation_in_document_metadata_is_never_exported: EXIF and XMP that
 * still say 6 are written upright. */
static void test_stale_orientation_is_never_exported_to_psd(void)
{
    pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    uint8_t ex[26];
    size_t n = t_exif(6, ex);
    pc_buf psd;
    pc_psd_file f;
    const pc_psd_resource *e;
    const pc_psd_resource *x;
    must(pc_io_set_blob(&d.metadata.exif, ex, n, NULL));
    t_set_xmp(&d, "<rdf:Description tiff:Orientation=\"6\"/>");
    psd = rt_export(&d, "psd", NULL);
    must(pc_psd_file_parse(psd.data, psd.len, &f, NULL));
    e = pc_psd_file_resource(&f, 1058);
    x = pc_psd_file_resource(&f, 1060);
    CHECK(e != NULL && pc_exif_orientation(e->data.data, e->data.len) == 1);
    CHECK(x != NULL &&
          find_bytes(x->data.data, x->data.len, (const uint8_t *)"tiff:Orientation=\"1\"", 20) != SIZE_MAX);
    pc_psd_file_fini(&f);
    pc_buf_fini(&psd);
    pc_document_fini(&d);
}

static void test_layered_saves_keep_the_xmp_whatever_the_option(void)
{
    static const char XMP[] =
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF>Secret campaign copy</rdf:RDF></x:xmpmeta>";
    for (int none = 0; none < 2; none++) {
        pc_document d = t_doc("x", 8, 6, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        pc_layer l = t_raster("flat", pc_document_pixel_format(&d), pc_rect_make(0, 0, 8, 6), 1, false);
        pc_io_export_opts o = pc_io_export_opts_default();
        pc_buf b;
        t_push(&d, &l);
        t_set_xmp(&d, XMP);
        o.xmp = none ? PC_IO_XMP_NONE : PC_IO_XMP_ALL;
        b = rt_export(&d, "out.psd", &o);
        CHECK(find_bytes(b.data, b.len, (const uint8_t *)"Secret campaign copy", 20) != SIZE_MAX);
        pc_buf_fini(&b);
        pc_document_fini(&d);
    }
}

int main(int argc, char **argv)
{
    RUN(test_tiff_rt_cases);
    RUN(test_tiff_and_psd_open_as_the_same_document);
    RUN(test_every_compression_keeps_the_layers);
    RUN(test_layered_tiff_keeps_exif_without_claiming_a_loss);
    RUN(test_transparency_is_straight_in_the_composite);
    RUN(test_big_documents_use_the_psb_signature);
    RUN(test_psdtags_fixtures_open_with_their_layers);
    RUN(test_advanced_blending_survives_layered_tiff_in_both_byte_orders);
    RUN(test_damaged_layer_data_opens_flattened);
    RUN(test_psd_icc_roundtrip_byte_exact);
    RUN(test_resolution_axes);
    RUN(test_effects_are_written_to_psd_and_read_back);
    RUN(test_psd_to_png_to_psd_chain);
    RUN(test_psd_merged_image_matches_composite_and_drops_near_opaque_alpha);
    RUN(test_stale_orientation_is_never_exported_to_psd);
    RUN(test_layered_saves_keep_the_xmp_whatever_the_option);
    return pc_t_finish();
}
