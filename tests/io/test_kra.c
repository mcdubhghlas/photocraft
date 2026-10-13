/* Krita (.kra) import runtime tests (port of crates/io/src/kra.rs `mod tests`, R10c): the merged
 * PNG opens as one editable raster layer, the source is marked read-only, Krita's layer tree is
 * not imported (a warning says so), and malformed or non-Krita archives are refused without a
 * crash. Synthetic .kra archives are built here with the format/zip.h writer and a PNG encoded by
 * the codecs; the structural assertions are the values crates/io/src/kra.rs asserts on the Rust
 * side, so this is parity with the reference. */
#include "harness.h"

#include "common.h"

#include "base/compress.h"
#include "codecs/codecs.h"
#include "format/zip.h"
#include "io/kra.h"
#include "io/ora.h"

/* The 2x1 RGBA8 PNG the Rust test uses: opaque red, then half-transparent blue. */
static pc_buf kra_png(void)
{
    static const uint8_t px[8] = {255, 0, 0, 255, 0, 0, 255, 128};
    pc_cimage img;
    pc_buf out = PC_BUF_INIT;
    must(pc_cimage_from_copy(2, 1, PC_CL_RGBA, PC_CS_U8, px, sizeof px, &img, NULL));
    must(pc_cencode(&img, PC_CF_PNG, NULL, &out, NULL));
    pc_cimage_fini(&img);
    return out;
}

/* A .kra-shaped archive: mimetype, maindoc.xml, and (optionally) mergedimage.png. */
static pc_buf kra_archive(const char *mime, size_t mime_len, bool with_merged)
{
    pc_zip_writer z;
    pc_buf out = PC_BUF_INIT;
    pc_zip_writer_init(&z);
    must(pc_zip_writer_add(&z, "mimetype", 8, (const uint8_t *)mime, mime_len, NULL));
    must(pc_zip_writer_add(&z, "maindoc.xml", 11, (const uint8_t *)"<DOC/>", 6, NULL));
    if (with_merged) {
        pc_buf png = kra_png();
        must(pc_zip_writer_add(&z, "mergedimage.png", 15, png.data, png.len, NULL));
        pc_buf_fini(&png);
    }
    must(pc_zip_writer_finish(&z, &out, NULL));
    pc_zip_writer_fini(&z);
    return out;
}

static void test_imports_krita_composite_without_claiming_layers(void)
{
    static const char *const names[] = {"scene.kra", "download"};
    pc_buf bytes = kra_archive("application/x-krita", 19, true);
    CHECK(pc_io_is_kra(bytes.data, bytes.len));
    for (size_t k = 0; k < 2; k++) {
        pc_io_import_result r;
        const pc_surface *s;
        float rgba[4];
        /* The mimetype is authoritative even without a .kra extension. */
        must(pc_io_import(names[k], bytes.data, bytes.len, &r, NULL));
        CHECK_EQ_UINT(r.document.size.width, 2);
        CHECK_EQ_UINT(r.document.size.height, 1);
        CHECK(pc_str_eq_c(&r.document.name, names[k]));
        CHECK_EQ_UINT(r.document.nlayers, 1);
        CHECK(pc_str_eq_c(&r.document.layers[0].name, "Layer 0"));
        s = pc_layer_surface(&r.document.layers[0]);
        CHECK(s != NULL);
        pc_surface_rgba(s, 0, 0, rgba);
        CHECK(rgba[0] > 0.99f);
        pc_surface_rgba(s, 1, 0, rgba);
        CHECK(rgba[2] > 0.99f);
        CHECK_NEAR(rgba[3], 128.0f / 255.0f, 0.01f);
        CHECK(r.source_read_only); /* a flat save must not overwrite the .kra */
        CHECK(!r.preview_only);    /* the flattened image is editable */
        CHECK(pc_io_warnings_any(&r.warnings, "layers"));
        CHECK(pc_io_warnings_any(&r.warnings, "not imported"));
        pc_io_import_result_fini(&r);
    }
    pc_buf_fini(&bytes);
}

static pc_status try_import(const char *name, const uint8_t *b, size_t n)
{
    pc_io_import_result r;
    pc_error e;
    pc_status st = pc_io_import(name, b, n, &r, &e);
    if (st == PC_OK) {
        pc_io_import_result_fini(&r);
    } else {
        CHECK(e.msg[0] != '\0');
    }
    return st;
}

static void test_malformed_or_non_krita_archives(void)
{
    pc_buf wrong = kra_archive("image/openraster", 16, true);
    pc_buf missing = kra_archive("application/x-krita", 19, false);
    CHECK(!pc_io_is_kra(wrong.data, wrong.len));
    /* image/openraster opens as an OpenRaster archive (no stack.xml -> error), never as Krita. */
    CHECK(try_import("wrong.kra", wrong.data, wrong.len) != PC_OK);
    CHECK(try_import("missing.kra", missing.data, missing.len) != PC_OK);
    CHECK(try_import("broken.kra", (const uint8_t *)"not a zip", 9) != PC_OK);
    pc_buf_fini(&wrong);
    pc_buf_fini(&missing);
}

static void test_png_with_misleading_kra_extension_stays_png(void)
{
    pc_buf png = kra_png();
    pc_io_import_result r;
    /* A real PNG detected by content wins over the .kra suffix. */
    must(pc_io_import("renamed.kra", png.data, png.len, &r, NULL));
    CHECK_EQ_UINT(r.document.size.width, 2);
    CHECK(!r.source_read_only); /* a PNG must not be treated as a Krita source */
    pc_io_import_result_fini(&r);
    pc_buf_fini(&png);
}

/* ---- never-crash: hostile archives (truncation, byte flips, CRC damage) ---- */

static void test_hostile_kra_archives_never_crash(void)
{
    pc_buf bytes = kra_archive("application/x-krita", 19, true);
    size_t step = bytes.len / 200 > 0 ? bytes.len / 200 : 1;
    uint8_t *c = xmalloc(bytes.len);
    uint32_t state = 0x9e3779b9u;
    /* Every truncated prefix. */
    for (size_t cut = 0; cut <= bytes.len; cut += step) {
        (void)try_import("x.kra", bytes.data, cut);
    }
    /* Random byte flips (deterministic xorshift, like the Rust fuzz loop). */
    for (int iter = 0; iter < 300; iter++) {
        memcpy(c, bytes.data, bytes.len);
        for (int f = 0; f < 4; f++) {
            size_t i;
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            i = (size_t)state % bytes.len;
            c[i] ^= (uint8_t)((state >> 24) | 1u);
        }
        (void)try_import("x.kra", c, bytes.len);
    }
    free(c);
    pc_buf_fini(&bytes);
}

/* A zip bomb entry (mergedimage.png declaring a huge decoded size) must be refused by the
 * decoder's pixel budget, not allocated. */
static void test_zip_bomb_and_path_traversal_are_refused(void)
{
    pc_zip_writer z;
    pc_buf out = PC_BUF_INIT;
    pc_buf png = kra_png();
    /* Forge the IHDR width/height to 30000x30000 (bytes 16..24 after the 8-byte signature). */
    if (png.len >= 24) {
        static const uint8_t forged[8] = {0, 0, 0x75, 0x30, 0, 0, 0x75, 0x30};
        memcpy(png.data + 16, forged, 8);
    }
    pc_zip_writer_init(&z);
    must(pc_zip_writer_add(&z, "mimetype", 8, (const uint8_t *)"application/x-krita", 19, NULL));
    /* A path-traversal entry name must not escape anything (io never touches the filesystem). */
    must(pc_zip_writer_add(&z, "../../etc/passwd", 16, (const uint8_t *)"x", 1, NULL));
    must(pc_zip_writer_add(&z, "mergedimage.png", 15, png.data, png.len, NULL));
    must(pc_zip_writer_finish(&z, &out, NULL));
    pc_zip_writer_fini(&z);
    CHECK(try_import("bomb.kra", out.data, out.len) != PC_OK);
    pc_buf_fini(&png);
    pc_buf_fini(&out);
}

int main(int argc, char **argv)
{
    RUN(test_imports_krita_composite_without_claiming_layers);
    RUN(test_malformed_or_non_krita_archives);
    RUN(test_png_with_misleading_kra_extension_stays_png);
    RUN(test_hostile_kra_archives_never_crash);
    RUN(test_zip_bomb_and_path_traversal_are_refused);
    return pc_t_finish();
}
