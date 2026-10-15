/* Artboard portable primitives (port of the raster_pdf / preset parts of artboard_cmds.rs, E6).
 * The commands are deferred (E3 file I/O, E4 translate); only these primitives are tested. */
#include <string.h>

#include "base/buf.h"
#include "engine/artboard_cmds.h"
#include "harness.h"

static bool contains(const pc_buf *b, const char *needle)
{
    size_t nlen = strlen(needle);
    if (b->len < nlen) {
        return false;
    }
    for (size_t i = 0; i + nlen <= b->len; i++) {
        if (memcmp(b->data + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static void test_preset_sizes(void)
{
    uint32_t w = 0, h = 0;
    CHECK(pc_engine_artboard_preset_size("A4", &w, &h) && w == 595 && h == 842);
    CHECK(pc_engine_artboard_preset_size("web 1920", &w, &h) && w == 1920 && h == 1080); /* case-insensitive */
    CHECK(pc_engine_artboard_preset_size("iPhone 14", &w, &h) && w == 390 && h == 844);
    CHECK(!pc_engine_artboard_preset_size("No Such", &w, &h));
    CHECK(!pc_engine_artboard_preset_size(NULL, &w, &h));
}

static void test_raster_pdf_structure(void)
{
    const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0xff, 0xd9};
    pc_engine_pdf_page pages[2] = {
        {100, 200, 72.0f, jpeg, sizeof jpeg},
        {64, 64, 144.0f, jpeg, sizeof jpeg},
    };
    pc_buf out;
    CHECK(pc_engine_artboard_raster_pdf(pages, 2, &out, NULL) == PC_OK);
    CHECK(out.len > 100);
    CHECK(out.len >= 8 && memcmp(out.data, "%PDF-1.4", 8) == 0);
    CHECK(contains(&out, "/Type /Catalog"));
    CHECK(contains(&out, "/Count 2"));
    CHECK(contains(&out, "/Filter /DCTDecode"));
    CHECK(contains(&out, "/Width 100"));
    CHECK(contains(&out, "/Width 64"));
    CHECK(contains(&out, "xref"));
    CHECK(contains(&out, "/Root 1 0 R"));
    CHECK(contains(&out, "%%EOF"));
    /* The 72-dpi page prints at pixel size; MediaBox is 100 x 200 pt. */
    CHECK(contains(&out, "/MediaBox [0 0 100.000 200.000]"));
    /* The 144-dpi 64px page prints at half: 32 x 32 pt. */
    CHECK(contains(&out, "/MediaBox [0 0 32.000 32.000]"));
    pc_buf_fini(&out);

    /* Zero pages: a valid (empty) page tree. */
    CHECK(pc_engine_artboard_raster_pdf(NULL, 0, &out, NULL) == PC_OK);
    CHECK(contains(&out, "/Count 0") && contains(&out, "%%EOF"));
    pc_buf_fini(&out);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_preset_sizes);
    RUN(test_raster_pdf_structure);
    return pc_t_finish();
}
