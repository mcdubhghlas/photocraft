/* Edit > Define Brush Preset: edit.defineBrushPreset (define_brush_cmds.c).
 *
 * Defining a brush from a selection adds a preset; a default name is generated; no selection and
 * no document are graceful errors (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdio.h>
#include <string.h>

#include "base/json.h"
#include "base/str.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/define_brush_cmds.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status run_json(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(16, 16), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(0.5f, 0.5f, 0.5f, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_define_brush_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("edit.defineBrushPreset") != NULL);
}

static void test_defines_from_selection(void)
{
    pc_engine_session *s = mk();
    CHECK(run_json(s, "select.rect", "{\"x\":2,\"y\":2,\"width\":6,\"height\":6}") == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "edit.defineBrushPreset"));
    size_t before = s->tools.npresets;
    CHECK(run_json(s, "edit.defineBrushPreset", "{}") == PC_OK);
    CHECK(s->tools.npresets == before + 1);
    /* A named brush. */
    CHECK(run_json(s, "select.rect", "{\"x\":2,\"y\":2,\"width\":6,\"height\":6}") == PC_OK);
    CHECK(run_json(s, "edit.defineBrushPreset", "{\"name\":\"My Brush\"}") == PC_OK);
    bool found = false;
    for (size_t i = 0; i < s->tools.npresets; i++) {
        if (strcmp(pc_str_c(&s->tools.presets[i].name), "My Brush") == 0) {
            found = true;
        }
    }
    CHECK(found);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "edit.defineBrushPreset"));
    CHECK(run_json(s, "edit.defineBrushPreset", "{}") != PC_OK);
    pc_engine_session_free(s);

    /* A document but no selection. */
    s = mk();
    CHECK(!pc_engine_session_is_enabled(s, "edit.defineBrushPreset"));
    CHECK(run_json(s, "edit.defineBrushPreset", "{}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_defines_from_selection);
    RUN(test_graceful);
    return pc_t_finish();
}
