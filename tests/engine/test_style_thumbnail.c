/* Style preset thumbnail render (gap B2): pc_engine_style_preset_thumbnail. */
#include <stdlib.h>
#include <string.h>

#include "engine/engine_internal.h"
#include "harness.h"
#include "engine/presets.h"
#include "raster/pixel.h"

static void test_empty_style_is_a_grey_rounded_square(void)
{
    pc_engine_session *s = NULL;
    pc_style_preset sp;
    pc_rgba8_image img;
    memset(&sp, 0, sizeof sp);
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(pc_engine_style_preset_thumbnail(s, &sp, 64, &img, NULL) == PC_OK);
    CHECK_EQ_UINT(img.width, 64);
    CHECK_EQ_UINT(img.height, 64);
    /* Centre: the grey square (~0.86) at full alpha. */
    {
        size_t c = (32u * 64u + 32u) * 4u;
        CHECK(img.pixels[c + 0] >= 210 && img.pixels[c + 0] <= 226);
        CHECK_EQ_UINT(img.pixels[c + 3], 255);
    }
    /* Corner: transparent around the rounded square. */
    CHECK(img.pixels[3] <= 2);
    pc_rgba8_image_fini(&img);
    pc_engine_session_free(s);
}

static void test_size_is_clamped(void)
{
    pc_engine_session *s = NULL;
    pc_style_preset sp;
    pc_rgba8_image a, b;
    memset(&sp, 0, sizeof sp);
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(pc_engine_style_preset_thumbnail(s, &sp, 2, &a, NULL) == PC_OK);
    CHECK_EQ_UINT(a.width, 8); /* clamped up to 8 */
    CHECK(pc_engine_style_preset_thumbnail(s, &sp, 9999, &b, NULL) == PC_OK);
    CHECK_EQ_UINT(b.width, 256); /* clamped down to 256 */
    pc_rgba8_image_fini(&a);
    pc_rgba8_image_fini(&b);
    pc_engine_session_free(s);
}

static void test_a_builtin_style_changes_the_swatch(void)
{
    pc_engine_session *s = NULL;
    pc_preset_groups g;
    pc_style_preset empty;
    pc_rgba8_image base, styled;
    bool differs = false;
    memset(&empty, 0, sizeof empty);
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(pc_styles_builtin(&g, NULL) == PC_OK);
    CHECK(g.n > 0);
    CHECK(pc_engine_style_preset_thumbnail(s, &empty, 48, &base, NULL) == PC_OK);
    /* At least one built-in style must alter the plain grey swatch (effects applied). */
    for (size_t gi = 0; gi < g.n && !differs; gi++) {
        const pc_style_preset *items = g.v[gi].items;
        for (size_t ii = 0; ii < g.v[gi].n && !differs; ii++) {
            CHECK(pc_engine_style_preset_thumbnail(s, &items[ii], 48, &styled, NULL) == PC_OK);
            CHECK_EQ_UINT(styled.width, 48);
            for (size_t i = 0; i < (size_t)48 * 48 * 4; i++) {
                if (base.pixels[i] != styled.pixels[i]) {
                    differs = true;
                    break;
                }
            }
            pc_rgba8_image_fini(&styled);
        }
    }
    CHECK(differs);
    pc_rgba8_image_fini(&base);
    pc_preset_groups_fini(&g, pc_style_preset_ops());
    pc_engine_session_free(s);
}

static void test_null_style_is_rejected(void)
{
    pc_engine_session *s = NULL;
    pc_rgba8_image img;
    pc_error e = {0};
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(pc_engine_style_preset_thumbnail(s, NULL, 64, &img, &e) == PC_ERR_INVALID);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_empty_style_is_a_grey_rounded_square);
    RUN(test_size_is_clamped);
    RUN(test_a_builtin_style_changes_the_swatch);
    RUN(test_null_style_is_rejected);
    return pc_t_finish();
}
