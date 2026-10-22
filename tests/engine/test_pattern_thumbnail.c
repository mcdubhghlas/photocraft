/* Pattern thumbnail accessor (primitive for the Pattern Overlay / pattern picker):
 * pc_engine_pattern_thumbnail resolves a pattern and renders a bounded RGBA8 swatch. */
#include <stdlib.h>
#include <string.h>

#include "engine/engine_internal.h"
#include "harness.h"
#include "engine/pattern_cmds.h"
#include "raster/pixel.h"

static void test_default_selection_renders(void)
{
    pc_engine_session *s = NULL;
    pc_rgba8_image img;
    bool found = false;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    /* An empty key picks the Patterns-panel selection, else the first library pattern. */
    CHECK(pc_engine_pattern_thumbnail(s, "", 0, 64, &img, &found, NULL) == PC_OK);
    CHECK(found);
    if (found) {
        bool any_opaque = false;
        CHECK_EQ_UINT(img.width, 64);
        CHECK_EQ_UINT(img.height, 64);
        for (size_t i = 0; i < (size_t)64 * 64; i++) {
            if (img.pixels[i * 4 + 3] > 0) {
                any_opaque = true;
                break;
            }
        }
        CHECK(any_opaque); /* the tile fills the swatch */
        pc_rgba8_image_fini(&img);
    }
    pc_engine_session_free(s);
}

static void test_unknown_pattern_is_not_found(void)
{
    pc_engine_session *s = NULL;
    pc_rgba8_image img;
    bool found = true;
    const char *key = "no-such-pattern-xyzzy";
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(pc_engine_pattern_thumbnail(s, key, strlen(key), 64, &img, &found, NULL) == PC_OK);
    CHECK(!found); /* nothing written */
    pc_engine_session_free(s);
}

static void test_size_is_clamped(void)
{
    pc_engine_session *s = NULL;
    pc_rgba8_image img;
    bool found = false;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(pc_engine_pattern_thumbnail(s, "", 0, 3, &img, &found, NULL) == PC_OK);
    if (found) {
        CHECK_EQ_UINT(img.width, 8); /* clamped up to 8 */
        CHECK_EQ_UINT(img.height, 8);
        pc_rgba8_image_fini(&img);
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_default_selection_renders);
    RUN(test_unknown_pattern_is_not_found);
    RUN(test_size_is_clamped);
    return pc_t_finish();
}
