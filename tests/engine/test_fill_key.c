/* The fill keys (port of a subset of crates/engine/src/fill_key_cmds.rs tests, E4). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "raster/surface.h"
#include "harness.h"

static const char *const IDS[] = {"edit.fillForeground", "edit.fillBackground",
                                  "edit.fillForegroundPreserve", "edit.fillBackgroundPreserve",
                                  "edit.fillHistory"};

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return rc;
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(exec(s, "file.new", "{\"width\":8,\"height\":6,\"background\":\"transparent\"}") == PC_OK);
    float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    memcpy(s->tools.foreground, red, sizeof red);
    memcpy(s->tools.background, blue, sizeof blue);
    return s;
}

static void px(pc_engine_session *s, int32_t x, int32_t y, float out[4])
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_surface_rgba(pc_layer_surface(pc_document_layer(doc, st->active_layer)), x, y, out);
}

static void test_foreground_and_background(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "edit.fillForeground", "{}") == PC_OK);
    float got[4];
    px(s, 0, 0, got);
    CHECK(got[0] == 1.0f && got[3] == 1.0f);
    px(s, 7, 5, got);
    CHECK(got[0] == 1.0f);
    /* One undo step. */
    CHECK(exec(s, "edit.undo", "{}") == PC_OK);
    px(s, 0, 0, got);
    CHECK(got[3] == 0.0f);
    /* Background fills only the selection. */
    CHECK(exec(s, "select.rect", "{\"x\":2,\"y\":1,\"width\":3,\"height\":2}") == PC_OK);
    CHECK(exec(s, "edit.fillBackground", "{}") == PC_OK);
    px(s, 3, 2, got);
    CHECK(got[2] == 1.0f && got[3] == 1.0f);
    px(s, 0, 0, got);
    CHECK(got[3] == 0.0f);
    pc_engine_session_free(s);
}

static void test_preserve_and_params_cannot_override(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":2,\"height\":2}") == PC_OK);
    CHECK(exec(s, "edit.fillBackground", "{}") == PC_OK);
    CHECK(exec(s, "select.deselect", "{}") == PC_OK);
    CHECK(exec(s, "edit.fillForegroundPreserve", "{}") == PC_OK);
    float got[4];
    px(s, 1, 1, got);
    CHECK(got[0] == 1.0f && got[3] == 1.0f); /* recoloured */
    px(s, 4, 4, got);
    CHECK(got[3] == 0.0f); /* transparent pixels stay transparent */
    /* A colour or opacity param can't change what the key means. */
    CHECK(exec(s, "edit.fillForeground", "{\"color\":\"#00ff00\",\"opacity\":10}") == PC_OK);
    px(s, 0, 0, got);
    CHECK(got[0] == 1.0f && got[1] == 0.0f);
    pc_engine_session_free(s);
}

static void test_shortcuts(void)
{
    const pc_engine_command *c = pc_engine_command_find("edit.fillForeground");
    CHECK(c != NULL && strcmp(c->shortcut, "Alt+Backspace") == 0);
    c = pc_engine_command_find("edit.fillBackground");
    CHECK(c != NULL && strcmp(c->shortcut, "Cmd+Backspace") == 0);
    c = pc_engine_command_find("edit.fillHistory");
    CHECK(c != NULL && strcmp(c->shortcut, "Cmd+Alt+Backspace") == 0);
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk();
    const char *bad[] = {"[1,2]", "\"x\"", "{\"layer\":99999}", "{\"target\":{\"channel\":42}}"};
    for (size_t k = 0; k < sizeof IDS / sizeof IDS[0]; k++) {
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            CHECK(exec(s, IDS[k], bad[i]) != PC_OK);
        }
    }
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    for (size_t k = 0; k < sizeof IDS / sizeof IDS[0]; k++) {
        CHECK(exec(empty, IDS[k], "{}") != PC_OK); /* no document */
    }
    pc_engine_session_free(empty);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_foreground_and_background);
    RUN(test_preserve_and_params_cannot_override);
    RUN(test_shortcuts);
    RUN(test_adversarial);
    return pc_t_finish();
}
