/* Colour strings supplied through the commands CLI/MCP clients use: an unparseable Unicode hex
 * colour must fall back, not error, and the preference hex parser rejects it (port of
 * crates/engine/tests/unicode_colors.rs).
 *
 * filter.pixelate.pointillize is DEFERRED in the C port (the pixelate filter family is unported),
 * so that case is skipped with a note; the gallery and render colours exercise the same fallback. */
#include <string.h>

#include "engine/prefs.h"
#include "engine/engine.h"
#include "harness.h"

#define BAD_HEX "#\xe2\x82\xac\xe2\x82\xac" /* "#€€" */

/* Exercise a colour-reading command with an unparseable Unicode colour; it must still succeed via
 * its fallback. Skips cleanly when the command is DEFERRED. */
static void run_fallback(const char *id, const char *json)
{
    if (pc_engine_command_find(id) == NULL) {
        (void)printf("  (skipped %s: DEFERRED)\n", id);
        return;
    }
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":8,\"height\":8}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_error err = {0};
    if (pc_engine_session_execute_json(s, id, json, &out, &err) != PC_OK) {
        PC_T_FAIL("%s rejected a Unicode colour instead of falling back: %s", id, err.msg);
    } else {
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

static void test_unicode_pixelate_colour(void)
{
    run_fallback("filter.pixelate.pointillize", "{\"background\":\"" BAD_HEX "\"}");
}

static void test_unicode_gallery_colour(void)
{
    run_fallback("filter.gallery.photocopy", "{\"foreground\":\"" BAD_HEX "\"}");
}

static void test_unicode_render_colour(void)
{
    run_fallback("filter.render.tree", "{\"leavesColor\":\"" BAD_HEX "\"}");
}

static void test_unicode_preference_colour_is_invalid(void)
{
    uint8_t rgb[3] = {0};
    CHECK(!pc_pref_parse_hex(BAD_HEX, strlen(BAD_HEX), rgb));
    CHECK(!pc_pref_parse_hex("", 0, rgb));
    CHECK(pc_pref_parse_hex("#12aBcD", 7, rgb));
    CHECK(rgb[0] == 0x12 && rgb[1] == 0xab && rgb[2] == 0xcd);
}

int main(int argc, char **argv)
{
    RUN(test_unicode_pixelate_colour);
    RUN(test_unicode_gallery_colour);
    RUN(test_unicode_render_colour);
    RUN(test_unicode_preference_colour_is_invalid);
    return pc_t_finish();
}
