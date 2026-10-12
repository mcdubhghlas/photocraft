/* Affinity documents through io (port of crates/io/tests/affinity.rs and affinity_corpus.rs):
 * native import (artboards, shapes, paints) and the embedded preview only as a warned fallback
 * that keeps its pixels and alpha; never written back. The proptests run as seeded loops; the
 * corpus test (public documents against the thumbnails Affinity saved in them) skips cleanly
 * without corpus/affinity. */
#include "harness.h"
#include "io/common.h"

#include "affinity/affinity.h"
#include "../affinity/synth.h"

/* ---------------------------------------------------------------- the preview envelope */

static pc_buf png_2x1(void)
{
    static const uint8_t PX[8] = {220, 40, 60, 255, 0, 0, 0, 0};
    pc_cimage img;
    pc_buf out = PC_BUF_INIT;
    pc_cencode_opts o = pc_cencode_opts_default();
    must(pc_cimage_from_copy(2, 1, PC_CL_RGBA, PC_CS_U8, PX, 8, &img, NULL));
    must(pc_cencode(&img, PC_CF_PNG, &o, &out, NULL));
    pc_cimage_fini(&img);
    return out;
}

static void be32(pc_buf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    must(pc_buf_append(b, x, 4, NULL));
}

static pc_buf chunk(const char *kind, const uint8_t *data, size_t n)
{
    pc_buf tagged = PC_BUF_INIT;
    pc_buf r = PC_BUF_INIT;
    must(pc_buf_append(&tagged, kind, 4, NULL));
    if (n > 0) {
        must(pc_buf_append(&tagged, data, n, NULL));
    }
    be32(&r, (uint32_t)n);
    must(pc_buf_append(&r, tagged.data, tagged.len, NULL));
    be32(&r, pc_crc32(0, tagged.data, tagged.len));
    pc_buf_fini(&tagged);
    return r;
}

/* A synthetic record envelope (not an Affinity writer: there is no object graph). */
static pc_buf file_of(const uint8_t *png, size_t n)
{
    pc_buf b = PC_BUF_INIT;
    uint8_t head[72];
    memset(head, 0, sizeof head);
    memcpy(head, "\x00\xffKA", 4);
    head[4] = 12;
    memcpy(head + 8, "nsrP", 4);
    memcpy(head + 12, "#Inf", 4);
    head[24] = 72;
    memcpy(head + 64, "Prot", 4);
    must(pc_buf_append(&b, head, 72, NULL));
    must(pc_buf_append(&b, "\xff\xff\xff\xffThmb", 8, NULL));
    sy_le(&b, 1, 4);
    sy_le(&b, n + 13, 4);
    sy_le(&b, 29, 4);
    sy_le(&b, 0, 4);
    sy_le(&b, n, 4);
    sy_byte(&b, 1);
    must(pc_buf_append(&b, png, n, NULL));
    return b;
}

static pc_buf file_png(void)
{
    pc_buf p = png_2x1();
    pc_buf f = file_of(p.data, p.len);
    pc_buf_fini(&p);
    return f;
}

static pc_status imp(const char *name, const uint8_t *b, size_t n, pc_io_import_result *r, pc_error *e)
{
    return pc_io_import(name, b, n, r, e);
}

/* Inserts ins before the last 12 bytes (IEND) of png. */
static pc_buf splice_before_iend(const pc_buf *png, const uint8_t *ins, size_t n)
{
    pc_buf o = PC_BUF_INIT;
    must(pc_buf_append(&o, png->data, png->len - 12, NULL));
    must(pc_buf_append(&o, ins, n, NULL));
    must(pc_buf_append(&o, png->data + png->len - 12, 12, NULL));
    return o;
}

static void test_preview_is_named_and_warns_at_its_actual_size(void)
{
    pc_buf f = file_png();
    pc_io_import_result r;
    pc_io_export_result out;
    pc_io_import_result back;
    pc_cimage img;
    pc_error e;
    must(imp("art.af", f.data, f.len, &r, NULL));
    CHECK_EQ_UINT(r.document.size.width, 2u);
    CHECK_EQ_UINT(r.document.size.height, 1u);
    CHECK_EQ_UINT(r.document.nlayers, 1u);
    CHECK_EQ_STR(pc_str_c(&r.document.layers[0].name), "Affinity preview");
    CHECK(r.source_read_only && r.preview_only);
    CHECK(strstr(r.warnings.v[0], "2\xc3\x97"
                                  "1") != NULL);
    CHECK(strstr(r.warnings.v[0], "native Affinity document could not be read") != NULL &&
          strstr(r.warnings.v[0], "no layers") != NULL);
    must(pc_io_export(&r.document, "copy.png", NULL, &out, NULL));
    must(pc_cdecode(out.bytes, out.len, &img, NULL));
    CHECK(pc_cimage_sample_count(&img) == 8 && memcmp(img.data, "\xdc\x28\x3c\xff\x00\x00\x00\x00", 8) == 0);
    pc_cimage_fini(&img);
    pc_io_export_result_fini(&out);
    must(pc_io_export(&r.document, "copy.pcraft", NULL, &out, NULL));
    must(imp("copy.pcraft", out.bytes, out.len, &back, NULL));
    CHECK(back.document.size.width == 2 && back.document.size.height == 1);
    CHECK(!back.source_read_only && !back.preview_only);
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&out);
    for (size_t i = 0; i < PC_IO_AFFINITY_EXTENSION_COUNT; i++) {
        CHECK(pc_io_export(&r.document, PC_IO_AFFINITY_EXTENSIONS[i], NULL, &out, &e) != PC_OK);
        CHECK(strstr(e.msg, "Affinity export") != NULL);
    }
    pc_io_import_result_fini(&r);
    pc_buf_fini(&f);
}

static void test_magic_wins_over_the_name(void)
{
    pc_buf f = file_png();
    pc_buf p = png_2x1();
    pc_io_import_result r;
    pc_error e;
    must(imp("renamed.png", f.data, f.len, &r, NULL));
    CHECK(r.source_read_only);
    pc_io_import_result_fini(&r);
    must(imp("unknown", f.data, f.len, &r, NULL));
    CHECK(r.source_read_only);
    pc_io_import_result_fini(&r);
    CHECK(imp("fake.af", p.data, p.len, &r, &e) != PC_OK);
    f.data[4] = 10;
    must(imp("legacy.afphoto", f.data, f.len, &r, NULL));
    CHECK(r.preview_only);
    pc_io_import_result_fini(&r);
    f.data[4] = 13;
    CHECK(imp("future.af", f.data, f.len, &r, &e) != PC_OK);
    CHECK(strstr(e.msg, "unknown container version") != NULL);
    pc_buf_fini(&f);
    pc_buf_fini(&p);
}

static void test_preview_decoder_preserves_16_bit_gray_and_alpha(void)
{
    static const uint16_t PX[4] = {12345, 65535, 0, 0};
    pc_cimage img;
    pc_buf enc = PC_BUF_INIT;
    pc_buf f;
    pc_io_import_result r;
    pc_io_export_result out;
    pc_cimage dec;
    pc_cencode_opts o = pc_cencode_opts_default();
    must(pc_cimage_from_copy(2, 1, PC_CL_GRAYA, PC_CS_U16, PX, sizeof PX, &img, NULL));
    must(pc_cencode(&img, PC_CF_PNG, &o, &enc, NULL));
    f = file_of(enc.data, enc.len);
    must(imp("gray.af", f.data, f.len, &r, NULL));
    CHECK(r.document.depth == PC_SAMPLE_TYPE_U16);
    CHECK(r.document.mode == PC_COLOR_MODE_GRAYSCALE);
    must(pc_io_export(&r.document, "copy.png", NULL, &out, NULL));
    must(pc_cdecode(out.bytes, out.len, &dec, NULL));
    CHECK(dec.sample == PC_CS_U16);
    CHECK(pc_cimage_eq(&dec, &img) || memcmp(dec.data, img.data, sizeof PX) == 0);
    pc_cimage_fini(&dec);
    pc_io_export_result_fini(&out);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&f);
    pc_buf_fini(&enc);
    pc_cimage_fini(&img);
}

static void test_previewless_files_return_an_export_fallback(void)
{
    pc_buf f = file_png();
    pc_io_import_result r;
    pc_error e;
    memset(f.data + 24, 0, 8);
    CHECK(imp("no-preview.af", f.data, f.len, &r, &e) != PC_OK);
    CHECK(strstr(e.msg, "no embedded preview") != NULL);
    pc_buf_fini(&f);
}

static void test_corruption_and_every_truncation_fail(void)
{
    pc_buf f = file_png();
    pc_io_import_result r;
    pc_error e;
    for (size_t end = 0; end < f.len; end++) {
        if (imp("bad.af", f.data, end, &r, &e) == PC_OK) {
            PC_T_FAIL("truncation at %zu opened", end);
            pc_io_import_result_fini(&r);
        }
    }
    f.data[130] ^= 1;
    CHECK(imp("bad.af", f.data, f.len, &r, &e) != PC_OK);
    pc_buf_fini(&f);
}

static void test_crc_failures_after_idat(void)
{
    pc_buf f = file_png();
    pc_buf enc = png_2x1();
    pc_buf ch = chunk("tEXt", (const uint8_t *)"note\0preview", 12);
    pc_buf spliced;
    pc_buf g;
    pc_io_import_result r;
    pc_error e;
    f.data[f.len - 1] ^= 1;
    CHECK(imp("bad-iend.af", f.data, f.len, &r, &e) != PC_OK);
    CHECK(strstr(e.msg, "PNG chunk checksum") != NULL);
    spliced = splice_before_iend(&enc, ch.data, ch.len);
    g = file_of(spliced.data, spliced.len);
    must(imp("valid-ancillary.af", g.data, g.len, &r, NULL));
    pc_io_import_result_fini(&r);
    pc_buf_fini(&g);
    spliced.data[spliced.len - 12 - 1] ^= 1;
    g = file_of(spliced.data, spliced.len);
    CHECK(imp("bad-ancillary.af", g.data, g.len, &r, &e) != PC_OK);
    CHECK(strstr(e.msg, "PNG chunk checksum") != NULL);
    pc_buf_fini(&g);
    pc_buf_fini(&spliced);
    pc_buf_fini(&ch);
    pc_buf_fini(&enc);
    pc_buf_fini(&f);
}

static void test_compressed_metadata_and_apng_fail_early(void)
{
    static const char *const KINDS[] = {"zTXt", "iTXt", "iCCP", "acTL", "fcTL", "fdAT"};
    for (size_t k = 0; k < 6; k++) {
        pc_buf enc = png_2x1();
        pc_buf ch = chunk(KINDS[k], (const uint8_t *)"preview\0\0\x78\x9c", 11);
        pc_buf spliced = splice_before_iend(&enc, ch.data, ch.len);
        pc_buf g = file_of(spliced.data, spliced.len);
        pc_io_import_result r;
        pc_error e;
        CHECK(imp("unsupported.af", g.data, g.len, &r, &e) != PC_OK);
        CHECK(strstr(e.msg, "unsupported Affinity file") != NULL);
        CHECK(strstr(e.msg, "PNG metadata") != NULL || strstr(e.msg, "animated PNG") != NULL);
        pc_buf_fini(&g);
        pc_buf_fini(&spliced);
        pc_buf_fini(&ch);
        pc_buf_fini(&enc);
    }
}

static void test_text_chunks_cannot_bypass_the_memory_budget(void)
{
    uint8_t *x = xmalloc((size_t)1024 * 1024);
    pc_buf comp = PC_BUF_INIT;
    pc_buf tagged = PC_BUF_INIT;
    pc_buf ch;
    pc_buf rep = PC_BUF_INIT;
    pc_buf enc = png_2x1();
    pc_buf spliced;
    pc_buf g;
    pc_io_import_result r;
    pc_error e;
    memset(x, 'x', (size_t)1024 * 1024);
    must(pc_deflate(PC_Z_ZLIB, PC_Z_BEST, x, (size_t)1024 * 1024, &comp, NULL));
    CHECK(comp.len < 2048);
    must(pc_buf_append(&tagged, "note\0\0", 6, NULL));
    must(pc_buf_append(&tagged, comp.data, comp.len, NULL));
    ch = chunk("zTXt", tagged.data, tagged.len);
    for (int i = 0; i < 1024; i++) {
        must(pc_buf_append(&rep, ch.data, ch.len, NULL));
    }
    spliced = splice_before_iend(&enc, rep.data, rep.len);
    g = file_of(spliced.data, spliced.len);
    CHECK(imp("text-bomb.af", g.data, g.len, &r, &e) != PC_OK);
    CHECK(strstr(e.msg, "compressed PNG metadata") != NULL);
    pc_buf_fini(&g);
    pc_buf_fini(&spliced);
    pc_buf_fini(&enc);
    pc_buf_fini(&rep);
    pc_buf_fini(&ch);
    pc_buf_fini(&tagged);
    pc_buf_fini(&comp);
    free(x);
}

static void test_invalid_critical_chunks_after_idat(void)
{
    pc_buf orig = png_2x1();
    pc_buf trailing[3];
    pc_io_import_result r;
    pc_error e;
    trailing[0] = chunk("IHDR", orig.data + 16, 13);
    trailing[1] = chunk("PLTE", (const uint8_t *)"\0\0\0", 3);
    trailing[2] = chunk("ABCD", NULL, 0);
    for (int i = 0; i < 3; i++) {
        pc_buf s = splice_before_iend(&orig, trailing[i].data, trailing[i].len);
        pc_buf g = file_of(s.data, s.len);
        CHECK(imp("bad-order.af", g.data, g.len, &r, &e) != PC_OK);
        CHECK(strstr(e.msg, "Affinity file") != NULL);
        pc_buf_fini(&g);
        pc_buf_fini(&s);
        pc_buf_fini(&trailing[i]);
    }
    {
        pc_buf a = chunk("tEXt", (const uint8_t *)"note\0preview", 12);
        pc_buf b = chunk("IDAT", NULL, 0);
        pc_buf both = PC_BUF_INIT;
        pc_buf s;
        pc_buf g;
        must(pc_buf_append(&both, a.data, a.len, NULL));
        must(pc_buf_append(&both, b.data, b.len, NULL));
        s = splice_before_iend(&orig, both.data, both.len);
        g = file_of(s.data, s.len);
        CHECK(imp("split-idat.af", g.data, g.len, &r, &e) != PC_OK);
        CHECK(strstr(e.msg, "non-contiguous PNG image data") != NULL);
        pc_buf_fini(&g);
        pc_buf_fini(&s);
        pc_buf_fini(&both);
        pc_buf_fini(&a);
        pc_buf_fini(&b);
    }
    pc_buf_fini(&orig);
}

static uint64_t rng(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void test_preview_survives_mutations(void)
{
    uint64_t seed = 0x5eed;
    for (int c = 0; c < 128; c++) {
        pc_buf f = file_png();
        int n = (int)(rng(&seed) % 16);
        pc_io_import_result r;
        pc_error e;
        for (int i = 0; i < n; i++) {
            size_t at = (size_t)(rng(&seed) % 300);
            if (at < f.len) {
                f.data[at] = (uint8_t)rng(&seed);
            }
        }
        if (imp("mutated.af", f.data, f.len, &r, &e) == PC_OK) {
            pc_io_import_result_fini(&r);
        }
        pc_buf_fini(&f);
    }
    CHECK(true);
}

/* ---------------------------------------------------------------- the native document */

static const char *const FDSC[1] = {"FDsc"};
static const char *const FILS[1] = {"FilS"};
static const char *const RGBA_[1] = {"RGBA"};

static void solid(pc_buf *b, uint32_t id, float r, float g, float bl, float a)
{
    float c[4] = {r, g, bl, a};
    uint8_t s[16];
    for (int i = 0; i < 4; i++) {
        memcpy(s + 4 * i, &c[i], 4); /* little-endian hosts (as the synth) */
    }
    sy_def_payload(b, id, FDSC, 1);
    sy_def(b, "FDeF", id + 1, FILS, 1);
    sy_def(b, "Colr", id + 2, RGBA_, 1);
    sy_struct(b, "_col", s, 16);
    sy_end(b);
    sy_end(b);
    sy_end(b);
}

static void square(uint8_t *out, double s, double o)
{
    double p[5][2] = {{o, o}, {o + s, o}, {o + s, o + s}, {o, o + s}, {o, o}};
    for (int i = 0; i < 5; i++) {
        memcpy(out + i * 18, &p[i][0], 8);
        memcpy(out + i * 18 + 8, &p[i][1], 8);
        out[i * 18 + 16] = 1;
        out[i * 18 + 17] = 0;
    }
}

/* A version-12 document at 144 ppi, 200x100: an artboard "Board" moved by (20, 10) holding a red
 * ring (two subpaths, filled even-odd) with a 4 px blue stroke. */
static pc_buf native_doc(sy_method method)
{
    static const char *const PCRV[1] = {"PCrv"};
    static const char *const SHPN[1] = {"ShpN"};
    static const char *const SHNR[1] = {"ShNR"};
    static const char *const SPRD[1] = {"Sprd"};
    static const char *const DOCN[1] = {"DocN"};
    static const char *const LDSC[1] = {"LDsc"};
    static const char *const LSTY[1] = {"LSty"};
    static const double SPRB[4] = {0, 0, 200, 100};
    static const double SHPB[4] = {0, 0, 120, 80};
    static const double XFRM[6] = {1, 0, 20, 0, 1, 10};
    pc_buf d = PC_BUF_INIT;
    uint8_t sq1[90];
    uint8_t sq2[90];
    pc_buf png = png_2x1();
    sy_entry_t ent;
    pc_buf out;
    square(sq1, 60.0, 0.0);
    square(sq2, 20.0, 20.0);
    sy_stream_begin(&d);
    sy_obj(&d, "UVCn", "UVCn");
    sy_f64(&d, "UPPI", 144.0);
    sy_end(&d);
    sy_def(&d, "DocR", 1, DOCN, 1);
    sy_shared(&d, "Chld", 1);
    sy_def_payload(&d, 2, SPRD, 1);
    sy_f64s(&d, "SprB", SPRB, 4);
    sy_bool(&d, "SprT", true);
    sy_shared(&d, "Chld", 1);
    sy_def_payload(&d, 10, SHPN, 1);
    sy_str(&d, "Desc", "Board");
    sy_bool(&d, "ABEn", true);
    sy_def(&d, "Shpe", 11, SHNR, 1);
    sy_end(&d);
    sy_f64s(&d, "ShpB", SHPB, 4);
    sy_f64s(&d, "Xfrm", XFRM, 6);
    sy_shared(&d, "BFFl", 1);
    solid(&d, 12, 1, 1, 1, 1);
    sy_shared(&d, "Chld", 1);
    sy_def_payload(&d, 20, PCRV, 1);
    sy_str(&d, "Desc", "Ring");
    sy_obj(&d, "Crvs", "PCvD");
    sy_pos(&d, "Data");
    sy_u8(&d, NULL, 0);
    sy_u32(&d, NULL, 2);
    sy_bool(&d, NULL, true);
    sy_records(&d, NULL, 18, sq1, 5);
    sy_bool(&d, NULL, true);
    sy_records(&d, NULL, 18, sq2, 5);
    sy_end(&d); /* pos */
    sy_end(&d); /* Crvs */
    sy_shared(&d, "BFFl", 1);
    solid(&d, 21, 1, 0, 0, 1);
    sy_shared(&d, "LIFl", 1);
    solid(&d, 24, 0, 0, 1, 1);
    sy_shared(&d, "LILn", 1);
    sy_def_payload(&d, 27, LDSC, 1);
    sy_def(&d, "LDeL", 28, LSTY, 1);
    sy_f64(&d, "Wght", 4.0);
    sy_end(&d);
    sy_end(&d);
    sy_end(&d); /* ring */
    sy_end(&d); /* board */
    sy_end(&d); /* spread */
    sy_end(&d); /* DocR */
    sy_end(&d); /* root */
    ent.name = "doc.dat";
    ent.data = d.data;
    ent.len = d.len;
    ent.method = method;
    out = sy_container(&ent, 1, png.data, png.len);
    pc_buf_fini(&d);
    pc_buf_fini(&png);
    return out;
}

static void test_native_artboards_shapes_and_paints(void)
{
    for (int m = 0; m < 3; m++) {
        pc_buf b = native_doc((sy_method)m);
        pc_io_import_result r;
        pc_error e;
        const pc_document *d;
        if (imp("native.af", b.data, b.len, &r, &e) != PC_OK) {
            PC_T_FAIL("method %d: %s", m, e.msg);
            pc_buf_fini(&b);
            continue;
        }
        d = &r.document;
        CHECK(r.source_read_only && !r.preview_only);
        CHECK(d->size.width == 200 && d->size.height == 100 && d->resolution_dpi == 144.0f);
        CHECK(d->nlayers >= 1);
        if (d->nlayers >= 1 && d->layers[0].content.kind == PC_LAYER_GROUP) {
            const pc_layer *board = &d->layers[0];
            const pc_group *g = &board->content.u.group;
            CHECK_EQ_STR(pc_str_c(&board->name), "Board");
            CHECK(g->has_artboard && g->artboard.rect.x0 == 20 && g->artboard.rect.y0 == 10 &&
                  g->artboard.rect.x1 == 140 && g->artboard.rect.y1 == 90);
            if (g->nchildren > 0 && g->children[0].content.kind == PC_LAYER_SHAPE) {
                const pc_shape_layer *sh = &g->children[0].content.u.shape;
                uint8_t px[4];
                CHECK_EQ_STR(pc_str_c(&g->children[0].name), "Ring");
                CHECK_EQ_UINT(sh->path.nsubpaths, 2u);
                CHECK_EQ_UINT(sh->path.subpaths[0].nknots, 4u);
                CHECK(sh->path.subpaths[0].knots[0].anchor.x == 20.0 &&
                      sh->path.subpaths[0].knots[0].anchor.y == 10.0);
                CHECK(sh->has_stroke && sh->stroke.width == 4.0f);
                CHECK(sh->has_cache);
                if (sh->has_cache) {
                    must(pc_surface_read_rgba8_into(&sh->cache, pc_rect_make(30, 20, 31, 21),
                                                    (uint8_t (*)[4])px, 1, NULL));
                    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);
                    must(pc_surface_read_rgba8_into(&sh->cache, pc_rect_make(50, 40, 51, 41),
                                                    (uint8_t (*)[4])px, 1, NULL));
                    CHECK(px[3] == 0);
                }
            } else {
                CHECK(false);
            }
        } else {
            CHECK(false);
        }
        {
            pc_io_export_result out;
            pc_io_import_result back;
            must(pc_io_export(d, "copy.pcraft", NULL, &out, NULL));
            must(imp("copy.pcraft", out.bytes, out.len, &back, NULL));
            CHECK_EQ_UINT(back.document.nlayers, d->nlayers);
            pc_io_import_result_fini(&back);
            pc_io_export_result_fini(&out);
            CHECK(pc_io_export(d, "copy.psd", NULL, &out, NULL) == PC_OK);
            pc_io_export_result_fini(&out);
            CHECK(pc_io_export(d, "copy.af", NULL, &out, &e) != PC_OK);
        }
        pc_io_import_result_fini(&r);
        pc_buf_fini(&b);
    }
}

static void test_damaged_native_falls_back_to_preview(void)
{
    pc_buf b = native_doc(SY_ZLIB);
    pc_io_import_result r;
    b.data[80] ^= 0xFF;
    must(imp("broken.af", b.data, b.len, &r, NULL));
    CHECK(r.preview_only);
    CHECK(strstr(r.warnings.v[0], "could not be read (damaged Affinity file") != NULL);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&b);
}

static void test_mutated_native_documents_never_crash(void)
{
    uint64_t seed = 0xa1f1;
    pc_buf stored = native_doc(SY_STORED);
    pc_buf zstd = native_doc(SY_ZSTD);
    for (int c = 0; c < 256; c++) {
        const pc_buf *src = (rng(&seed) & 1) != 0 ? &stored : &zstd;
        uint8_t *b = xmalloc(src->len);
        int n = 1 + (int)(rng(&seed) % 15);
        pc_io_import_result r;
        pc_error e;
        memcpy(b, src->data, src->len);
        for (int i = 0; i < n; i++) {
            b[(rng(&seed) % 2048) % src->len] = (uint8_t)rng(&seed);
        }
        if (imp("mutated.af", b, src->len, &r, &e) == PC_OK) {
            pc_io_import_result_fini(&r);
        }
        free(b);
    }
    pc_buf_fini(&stored);
    pc_buf_fini(&zstd);
    CHECK(true);
}

/* ---------------------------------------------------------------- the public corpus */

static const struct {
    const char *name;
    double ceiling;
} FILES[] = {
    {"vector-art/mexican-guy.af", 3.0},
    {"vector-art/mexican-man.af", 4.0},
    {"vector-art/mexican-woman.af", 4.0},
    {"vector-art/playing-cards.af", 4.0},
    {"vector-art/beer.afdesign", 4.0},
    {"vector-art/cactus.afdesign", 2.0},
    {"vector-art/car.afdesign", 2.0},
    {"vector-art/flowers.afdesign", 3.0},
    {"vector-art/lion-track.afdesign", 6.0},
    {"afdesignload/color.afdesign", 1.0},
    {"afdesignload/layer_mode.afdesign", 1.0},
    {"afdesignload/layer_test.afdesign", 1.0},
    {"afdesignload/margins.afdesign", 1.0},
    {"afdesignload/raster_test.afdesign", 2.0},
    {"afdesignload/revision_test.afdesign", 1.0},
    {"afdesignload/shape_test.afdesign", 6.0},
    {"afdesignload/slice_test.afdesign", 1.0},
    {"afdesignload/test_path.afdesign", 2.0},
    {"jac21/SimpleLogo.afdesign", 2.0},
    {"jac21/SimpleLogoBanner.afdesign", 4.0},
    {"asset-store-template/AssetStore.aftemplate", 1.0},
};

static double *on_white(const uint8_t *px, size_t n)
{
    double *o = xmalloc(n * 3 * sizeof(double));
    for (size_t i = 0; i < n; i++) {
        double a = px[i * 4 + 3] / 255.0;
        for (int c = 0; c < 3; c++) {
            o[i * 3 + (size_t)c] = px[i * 4 + (size_t)c] * a + 255.0 * (1.0 - a);
        }
    }
    return o;
}

static void span(size_t d, size_t s, size_t n, size_t *a, size_t *b)
{
    size_t x = d * s / n;
    size_t y = ((d + 1) * s + n - 1) / n;
    x = x < s - 1 ? x : s - 1;
    y = y < x + 1 ? x + 1 : y > s ? s : y;
    *a = x;
    *b = y;
}

static double *shrink(const double *src, size_t sw, size_t sh, size_t dw, size_t dh)
{
    double *out = xmalloc(dw * dh * 3 * sizeof(double));
    for (size_t ty = 0; ty < dh; ty++) {
        size_t y0;
        size_t y1;
        span(ty, sh, dh, &y0, &y1);
        for (size_t tx = 0; tx < dw; tx++) {
            size_t x0;
            size_t x1;
            double sum[3] = {0, 0, 0};
            span(tx, sw, dw, &x0, &x1);
            for (size_t y = y0; y < y1; y++) {
                for (size_t x = x0; x < x1; x++) {
                    for (int c = 0; c < 3; c++) {
                        sum[c] += src[(y * sw + x) * 3 + (size_t)c];
                    }
                }
            }
            for (int c = 0; c < 3; c++) {
                out[(ty * dw + tx) * 3 + (size_t)c] = sum[c] / (double)((y1 - y0) * (x1 - x0));
            }
        }
    }
    return out;
}

static void test_public_documents_look_like_their_thumbnails(void)
{
    char path[512];
    (void)snprintf(path, sizeof path, "corpus/affinity/%s", FILES[0].name);
    if (access(path, R_OK) != 0) {
        (void)printf("  corpus/affinity absent: skipped (run `make corpus`)\n");
        return;
    }
    for (size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
        pc_buf bytes = PC_BUF_INIT;
        pc_io_import_result r;
        pc_af_preview p;
        pc_cimage thumb;
        pc_io_export_result png;
        pc_cimage flat;
        uint8_t *trgb;
        uint8_t *frgb;
        size_t tn;
        size_t fn;
        uint32_t tw;
        uint32_t th;
        uint32_t fw;
        uint32_t fh;
        uint32_t fw_first;
        uint32_t cw;
        uint32_t ch;
        uint8_t *cropped;
        double *a;
        double *bb;
        double *got;
        double *want;
        double diff = 0.0;
        (void)snprintf(path, sizeof path, "corpus/affinity/%s", FILES[i].name);
        must(pc_file_read(path, (size_t)1 << 30, &bytes, NULL));
        must(pc_io_import(FILES[i].name, bytes.data, bytes.len, &r, NULL));
        CHECK(!r.preview_only);
        must(pc_af_preview_read(bytes.data, bytes.len, &p, NULL));
        must(pc_cdecode(p.png, p.len, &thumb, NULL));
        tw = thumb.width;
        th = thumb.height;
        must(pc_io_export(&r.document, "x.png", NULL, &png, NULL));
        must(pc_cdecode(png.bytes, png.len, &flat, NULL));
        fw = flat.width;
        fh = flat.height;
        {
            double v = round((double)fh * tw / th);
            fw_first = v < fw ? (uint32_t)v : fw;
        }
        must(pc_cimage_to_rgba8(&flat, &frgb, &fn, NULL));
        must(pc_cimage_to_rgba8(&thumb, &trgb, &tn, NULL));
        cropped = xmalloc((size_t)fw_first * fh * 4);
        for (uint32_t y = 0; y < fh; y++) {
            memcpy(cropped + (size_t)y * fw_first * 4, frgb + (size_t)y * fw * 4, (size_t)fw_first * 4);
        }
        if (fh < th) {
            cw = fw_first;
            ch = fh;
        } else {
            cw = tw;
            ch = th;
        }
        a = on_white(cropped, (size_t)fw_first * fh);
        bb = on_white(trgb, (size_t)tw * th);
        got = shrink(a, fw_first, fh, cw, ch);
        want = shrink(bb, tw, th, cw, ch);
        for (size_t k = 0; k < (size_t)cw * ch * 3; k++) {
            diff += fabs(got[k] - want[k]);
        }
        diff /= (double)cw * ch * 3;
        (void)printf("  %s: %.2f (ceiling %g)\n", FILES[i].name, diff, FILES[i].ceiling);
        if (diff > FILES[i].ceiling) {
            PC_T_FAIL("%s: %.2f > %g", FILES[i].name, diff, FILES[i].ceiling);
        }
        free(a);
        free(bb);
        free(got);
        free(want);
        free(cropped);
        free(frgb);
        free(trgb);
        pc_cimage_fini(&flat);
        pc_io_export_result_fini(&png);
        pc_cimage_fini(&thumb);
        pc_io_import_result_fini(&r);
        pc_buf_fini(&bytes);
    }
}

int main(int argc, char **argv)
{
    RUN(test_preview_is_named_and_warns_at_its_actual_size);
    RUN(test_magic_wins_over_the_name);
    RUN(test_preview_decoder_preserves_16_bit_gray_and_alpha);
    RUN(test_previewless_files_return_an_export_fallback);
    RUN(test_corruption_and_every_truncation_fail);
    RUN(test_crc_failures_after_idat);
    RUN(test_compressed_metadata_and_apng_fail_early);
    RUN(test_text_chunks_cannot_bypass_the_memory_budget);
    RUN(test_invalid_critical_chunks_after_idat);
    RUN(test_preview_survives_mutations);
    RUN(test_native_artboards_shapes_and_paints);
    RUN(test_damaged_native_falls_back_to_preview);
    RUN(test_mutated_native_documents_never_crash);
    RUN(test_public_documents_look_like_their_thumbnails);
    return pc_t_finish();
}
