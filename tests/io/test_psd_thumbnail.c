/* The PSD/PSB image preview resource (1036): the 28-byte header follows the Adobe spec and the
 * JFIF stream follows it; the top-level PSD export inserts it before the version-info resource
 * when image previews are on, and omits it when off. */
#include "harness.h"

#include "common.h"
#include "base/bytes.h"
#include "doc/doc.h"
#include "io/core.h"
#include "io/io.h"
#include "io/psd_thumbnail.h"
#include "psd/file.h"

static uint32_t be32(const uint8_t *b) { return pc_load_be32(b); }
static uint16_t be16(const uint8_t *b) { return pc_load_be16(b); }

static void test_header_follows_the_spec(void)
{
    pc_document d;
    pc_psd_resource r;
    pc_error e;
    must(pc_document_with_background(&d, "thumb", pc_size_make(40, 24), PC_COLOR_MODE_RGB,
                                     PC_SAMPLE_TYPE_U8, PC_COLOR_WHITE, NULL));
    must(pc_io_psd_thumbnail_resource(&d, &r, &e));
    CHECK_EQ_UINT(r.id, 1036);
    CHECK(r.data.len > PC_IO_THUMBNAIL_HEADER_LEN);
    {
        const uint8_t *b = r.data.data;
        uint32_t w = be32(b + 4);
        uint32_t h = be32(b + 8);
        uint32_t width_bytes = be32(b + 12);
        uint32_t total = be32(b + 16);
        uint32_t compressed = be32(b + 20);
        CHECK_EQ_UINT(be32(b), 1);              /* format 1 = kJpegRGB */
        CHECK(w == 40 && h == 24);              /* 40x24 fits in 160 px */
        CHECK_EQ_UINT(width_bytes, (40 * 24 + 31) / 32 * 4);
        CHECK_EQ_UINT(total, width_bytes * h);
        CHECK_EQ_UINT(compressed, (uint32_t)(r.data.len - PC_IO_THUMBNAIL_HEADER_LEN));
        CHECK_EQ_UINT(be16(b + 24), 24);        /* bits per pixel */
        CHECK_EQ_UINT(be16(b + 26), 1);         /* planes */
        /* The JFIF stream starts at the SOI marker. */
        CHECK(b[28] == 0xFF && b[29] == 0xD8);
    }
    pc_psd_resource_fini(&r);
    pc_document_fini(&d);
}

/* Counts the resources with a given id in the bytes a PSD export produced. */
static size_t count_resource(const uint8_t *bytes, size_t len, uint16_t id)
{
    pc_psd_file f;
    pc_error e;
    size_t n = 0;
    if (pc_psd_file_parse(bytes, len, &f, &e) != PC_OK) {
        PC_T_FAIL("re-parsing the exported PSD: %s", e.msg);
        return 0;
    }
    for (size_t i = 0; i < f.nresources; i++) {
        n += f.resources[i].id == id ? 1 : 0;
    }
    pc_psd_file_fini(&f);
    return n;
}

static void test_export_inserts_and_omits_preview(void)
{
    pc_document d;
    pc_io_export_opts o = pc_io_export_opts_default();
    pc_io_export_result r;
    pc_error e;
    must(pc_document_with_background(&d, "thumb", pc_size_make(16, 16), PC_COLOR_MODE_RGB,
                                     PC_SAMPLE_TYPE_U8, PC_COLOR_WHITE, NULL));
    /* On by default: resource 1036 is present, once, ahead of the version info (1057). */
    CHECK(o.image_previews);
    must(pc_io_export(&d, "x.psd", &o, &r, &e));
    CHECK_EQ_UINT(count_resource(r.bytes, r.len, 1036), 1);
    pc_io_export_result_fini(&r);
    /* Off: no preview resource. */
    o.image_previews = false;
    must(pc_io_export(&d, "x.psd", &o, &r, &e));
    CHECK_EQ_UINT(count_resource(r.bytes, r.len, 1036), 0);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_header_follows_the_spec);
    RUN(test_export_inserts_and_omits_preview);
    return pc_t_finish();
}
