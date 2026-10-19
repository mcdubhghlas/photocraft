/* links / sizing / proxy / parity (ports of the self-contained logic in those modules). */
#include "harness.h"
#include "ui/brand.h"
#include "ui/icons.h"
#include "ui/links.h"
#include "ui/parity.h"
#include "ui/proxy.h"
#include "ui/sizing.h"
#include "ui/state.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void test_links(void)
{
    CHECK_EQ_STR(pc_ui_link_url_for("help.discord"), "https://discord.gg/artcraft");
    CHECK_EQ_STR(pc_ui_link_url_for("help.website"), "https://getartcraft.com/apps/photocraft");
    CHECK_EQ_STR(pc_ui_link_url_for("help.github"), "https://github.com/storytold/photocraft");
    CHECK_EQ_STR(pc_ui_link_url_for("help.reportIssue"), "https://github.com/storytold/photocraft/issues");
    CHECK_EQ_STR(pc_ui_link_url_for("help.artcraftWebsite"), "https://getartcraft.com");
    CHECK(pc_ui_link_url_for("help.nope") == NULL);
    CHECK(pc_ui_link_url_for(NULL) == NULL);
}

static void test_sizing(void)
{
    /* unit conversions round-trip. */
    const char *units[] = {"px", "percent", "in", "cm", "mm", "pt", "pica"};
    for (size_t i = 0; i < 7; i++) {
        double v = pc_ui_to_unit(1500.0, units[i], 3000.0, 300.0);
        CHECK(fabs(pc_ui_from_unit(v, units[i], 3000.0, 300.0) - 1500.0) < 1e-9);
    }
    CHECK(pc_ui_to_unit(1500.0, "percent", 3000.0, 300.0) == 50.0);
    CHECK(pc_ui_to_unit(600.0, "in", 3000.0, 300.0) == 2.0);
    char buf[16];
    pc_ui_human_bytes(6016.0 * 6016.0 * 4.0, buf);
    CHECK_EQ_STR(buf, "138.1M");
    pc_ui_human_bytes(2048.0, buf);
    CHECK_EQ_STR(buf, "2K");
    CHECK(pc_ui_is_sizing("image.imageSize") && pc_ui_is_sizing("image.canvasSize"));
    CHECK(!pc_ui_is_sizing("file.open"));
    CHECK_EQ_STR(PC_UI_SIZING_UNITS[0][0], "px");
    CHECK_EQ_STR(PC_UI_SIZING_ANCHORS[4], "center");
    const char *preferences[] = {"bicubicAutomatic", "nearestNeighbor", "bilinear",       "bicubic",
                                 "bicubicSmoother",  "bicubicSharper",  "preserveDetails"};
    const char *commands[] = {"automatic",       "nearest",        "bilinear",       "bicubic",
                              "bicubicSmoother", "bicubicSharper", "preserveDetails"};
    for (size_t i = 0; i < 7; i++)
        CHECK_EQ_STR(pc_ui_resample_preference(preferences[i]), commands[i]);
    CHECK_EQ_STR(pc_ui_resample_preference(NULL), "automatic");
}

static void test_proxy(void)
{
    CHECK_EQ_UINT(pc_ui_proxy_factor(1000ULL * 1000ULL), 1); /* small */
    uint32_t k = pc_ui_proxy_factor(6016ULL * 6016ULL);
    CHECK(k >= 3 && k <= 5);
    CHECK(6016ULL * 6016ULL / ((uint64_t)k * k) <= PC_UI_PREVIEW_PIXELS);
    CHECK_EQ_UINT(pc_ui_proxy_preview_factor(6016ULL * 6016ULL, true), k);
    CHECK_EQ_UINT(pc_ui_proxy_preview_factor(6016ULL * 6016ULL, false), 1);
    CHECK_EQ_UINT(pc_ui_proxy_preview_factor(1000ULL * 1000ULL, true), 1);
}

static void test_parity(void)
{
    CHECK_EQ_INT(PC_UI_PARITY_FLOOR, 631);
    char *missing[] = {"Sky Replacement (`edit.skyReplacement`)"};
    pc_ui_menu_parity menus[2] = {
        {"File", 10, 12, missing, 1},
        {"Edit", 4, 4, NULL, 0},
    };
    pc_ui_parity p = {14, 16, menus, 2};
    CHECK(fabs(pc_ui_parity_percent(&p) - 87.5) < 1e-9);
    pc_buf md = PC_BUF_INIT;
    CHECK(pc_ui_parity_to_markdown(&p, &md, NULL) == PC_OK);
    const char *s = pc_buf_cstr(&md);
    CHECK_CONTAINS(s, "**14 of 16 menu items live (87.5%).**");
    CHECK_CONTAINS(s, "| File | 10 | 12 | 83% |");
    CHECK_CONTAINS(s, "### File (1)");
    CHECK_CONTAINS(s, "- Sky Replacement (`edit.skyReplacement`)");
    /* Edit is fully live: no "Not yet implemented" section for it. */
    CHECK(strstr(s, "### Edit") == NULL);
    pc_buf_fini(&md);
}

static void test_icons(void)
{
    for (int i = 0; i < PC_TOOL_N; i++) {
        const char *name = pc_ui_tool_icon(PC_UI_TOOL_ALL[i]);
        CHECK(name != NULL && name[0] != '\0');
    }
    CHECK_EQ_STR(pc_ui_tool_icon(PC_TOOL_GRADIENT), "gradient");
    CHECK(strcmp(pc_ui_tool_icon(PC_TOOL_GRADIENT), pc_ui_tool_icon(PC_TOOL_PAINT_BUCKET)) != 0);
    CHECK_EQ_STR(pc_ui_tool_icon(PC_TOOL_MOVE), "move");
    CHECK_EQ_STR(pc_ui_tool_icon(PC_TOOL_TYPE), pc_ui_tool_icon(PC_TOOL_VERTICAL_TYPE));
}

static void test_brand(void)
{
    /* The icon decodes to a rounded 128px tile: corner transparent, centre opaque. */
    uint8_t *rgba = NULL;
    uint32_t w = 0, h = 0;
    CHECK(pc_ui_brand_icon_rgba(&rgba, &w, &h, NULL) == PC_OK);
    CHECK(w == 128 && h == 128);
    if (rgba) {
        CHECK_EQ_UINT(rgba[3], 0);                         /* top-left corner alpha */
        CHECK_EQ_UINT(rgba[(64 * 128 + 64) * 4 + 3], 255); /* centre alpha */
        free(rgba);
    }
}

int main(int argc, char **argv)
{
    RUN(test_links);
    RUN(test_sizing);
    RUN(test_proxy);
    RUN(test_parity);
    RUN(test_icons);
    RUN(test_brand);
    return pc_t_finish();
}
