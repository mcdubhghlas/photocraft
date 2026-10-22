/* Theme-card model (B7): option tables, the active-slot rule, favourite-set membership. Pure logic,
 * no window. */
#include "harness.h"
#include "ui/theme_cards.h"
#include "ui/theme.h"
#include <string.h>

static void test_options(void)
{
    const pc_ui_theme_option *light = NULL, *dark = NULL;
    size_t nl = pc_ui_theme_options(false, &light);
    size_t nd = pc_ui_theme_options(true, &dark);
    CHECK_EQ_UINT(nl, 7u);
    CHECK_EQ_UINT(nd, 7u);
    CHECK(light && !strcmp(light[0].id, "studioLight"));
    CHECK(dark && !strcmp(dark[0].id, "proMedium"));
    CHECK(light && light[0].kind == PC_UI_THEME_STUDIO_LIGHT);
    CHECK(dark && dark[0].kind == PC_UI_THEME_PRO_MEDIUM);
    /* Every option id round-trips to its kind's canonical id. */
    for (size_t i = 0; i < nl; i++) {
        CHECK(!strcmp(light[i].id, pc_ui_theme_kind_id(light[i].kind)));
    }
    for (size_t i = 0; i < nd; i++) {
        CHECK(!strcmp(dark[i].id, pc_ui_theme_kind_id(dark[i].kind)));
    }
}

static void test_light_active(void)
{
    CHECK(pc_ui_theme_light_active("light", false) == true);
    CHECK(pc_ui_theme_light_active("light", true) == true);
    CHECK(pc_ui_theme_light_active("dark", true) == false);
    CHECK(pc_ui_theme_light_active("dark", false) == false);
    CHECK(pc_ui_theme_light_active("auto", true) == true);
    CHECK(pc_ui_theme_light_active("auto", false) == false);
    CHECK(pc_ui_theme_light_active("bogus", true) == true); /* unknown -> follow system */
    CHECK(pc_ui_theme_light_active(NULL, true) == false);
}

static void test_favorites(void)
{
    const char *favs[] = {"studioLight", "proMedium"};
    CHECK(pc_ui_theme_favorite_contains(favs, 2, "studioLight") == true);
    CHECK(pc_ui_theme_favorite_contains(favs, 2, "proMedium") == true);
    CHECK(pc_ui_theme_favorite_contains(favs, 2, "classic") == false);
    CHECK(pc_ui_theme_favorite_contains(favs, 0, "studioLight") == false);
    CHECK(pc_ui_theme_favorite_contains(NULL, 0, "studioLight") == false);
    CHECK(pc_ui_theme_favorite_contains(favs, 2, NULL) == false);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_options);
    RUN(test_light_active);
    RUN(test_favorites);
    return pc_t_finish();
}
