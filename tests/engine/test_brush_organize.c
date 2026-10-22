/* Brush preset organiser (brush_organize_cmds.c): rename / move / moveGroup / renameGroup /
 * deleteGroup reorder the library as the Brushes panel's drag-and-drop does; behavioural checks on a
 * known library plus graceful failure on adversarial params (rule 9). */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "engine/brush_organize_cmds.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "harness.h"
#include "paint/presets.h"

/* Replace the library with a known set of (name, group) presets, no nested folders. */
static void reset_presets(pc_engine_session *s, const char *const names[], const char *const groups[],
                          size_t count)
{
    for (size_t i = 0; i < s->tools.npresets; i++) {
        pc_paint_preset_fini(&s->tools.presets[i]);
    }
    free(s->tools.presets);
    s->tools.presets = calloc(count ? count : 1, sizeof(pc_paint_preset));
    CHECK(s->tools.presets != NULL);
    for (size_t i = 0; i < count; i++) {
        pc_paint_preset_init(&s->tools.presets[i]);
        CHECK(pc_str_set_c(&s->tools.presets[i].name, names[i], NULL) == PC_OK);
        CHECK(pc_str_set_c(&s->tools.presets[i].group, groups[i], NULL) == PC_OK);
        s->tools.presets[i].builtin = false;
    }
    s->tools.npresets = count;
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    static const char *const names[] = {"A1", "A2", "A3", "B1", "B2"};
    static const char *const groups[] = {"Alpha", "Alpha", "Alpha", "Beta", "Beta"};
    reset_presets(s, names, groups, 5);
    return s;
}

static pc_status run(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

/* Index of a preset by name; SIZE_MAX if absent. */
static size_t idx(pc_engine_session *s, const char *name)
{
    for (size_t i = 0; i < s->tools.npresets; i++) {
        if (strcmp(pc_str_c(&s->tools.presets[i].name), name) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static const char *group_of(pc_engine_session *s, const char *name)
{
    size_t i = idx(s, name);
    return i == (size_t)-1 ? "" : pc_str_c(&s->tools.presets[i].group);
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_brush_organize_specs(&n);
    CHECK_EQ_UINT(n, 5);
    CHECK(pc_engine_command_find("brush.presets.rename") != NULL);
    CHECK(pc_engine_command_find("brush.presets.move") != NULL);
    CHECK(pc_engine_command_find("brush.presets.moveGroup") != NULL);
    CHECK(pc_engine_command_find("brush.presets.renameGroup") != NULL);
    CHECK(pc_engine_command_find("brush.presets.deleteGroup") != NULL);
}

static void test_rename(void)
{
    pc_engine_session *s = mk();
    CHECK(run(s, "brush.presets.rename", "{\"name\":\"A1\",\"newName\":\"Alpha One\"}") == PC_OK);
    CHECK(idx(s, "Alpha One") != (size_t)-1);
    CHECK(idx(s, "A1") == (size_t)-1);
    /* Case-insensitive match finds it; renaming to an existing name fails. */
    CHECK(run(s, "brush.presets.rename", "{\"name\":\"a2\",\"newName\":\"alpha one\"}") != PC_OK);
    CHECK(run(s, "brush.presets.rename", "{\"name\":\"nope\",\"newName\":\"x\"}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_move_within_and_between_groups(void)
{
    pc_engine_session *s = mk();
    /* Move A3 before A1: Alpha order becomes A3, A1, A2. */
    CHECK(run(s, "brush.presets.move", "{\"name\":\"A3\",\"before\":\"A1\"}") == PC_OK);
    CHECK(idx(s, "A3") < idx(s, "A1"));
    /* Move A2 into Beta. */
    CHECK(run(s, "brush.presets.move", "{\"name\":\"A2\",\"group\":\"Beta\"}") == PC_OK);
    CHECK(strcmp(group_of(s, "A2"), "Beta") == 0);
    pc_engine_session_free(s);
}

static void test_move_group(void)
{
    pc_engine_session *s = mk();
    /* Move Beta before Alpha. */
    CHECK(run(s, "brush.presets.moveGroup", "{\"group\":\"Beta\",\"before\":\"Alpha\"}") == PC_OK);
    CHECK(idx(s, "B1") < idx(s, "A1"));
    pc_engine_session_free(s);
}

static void test_rename_group(void)
{
    pc_engine_session *s = mk();
    CHECK(run(s, "brush.presets.renameGroup", "{\"group\":\"Alpha\",\"newName\":\"First\"}") == PC_OK);
    CHECK(strcmp(group_of(s, "A1"), "First") == 0);
    /* Clashing with an existing group fails. */
    CHECK(run(s, "brush.presets.renameGroup", "{\"group\":\"First\",\"newName\":\"Beta\"}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_delete_group(void)
{
    pc_engine_session *s = mk();
    size_t before = s->tools.npresets;
    CHECK(run(s, "brush.presets.deleteGroup", "{\"group\":\"Beta\"}") == PC_OK);
    CHECK(s->tools.npresets == before - 2);
    CHECK(idx(s, "B1") == (size_t)-1);
    CHECK(idx(s, "A1") != (size_t)-1);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = mk();
    const char *bad_rename[] = {"{}", "{\"name\":\"A1\"}", "{\"name\":\"A1\",\"newName\":\"  \"}",
                                "{\"name\":\"nope\",\"newName\":\"x\"}"};
    for (size_t i = 0; i < sizeof bad_rename / sizeof bad_rename[0]; i++) {
        CHECK(run(s, "brush.presets.rename", bad_rename[i]) != PC_OK);
    }
    const char *bad_move[] = {"{}", "{\"name\":\"Nope\"}", "{\"name\":\"A1\",\"before\":\"A1\"}",
                              "{\"name\":\"A1\",\"before\":\"Nope\"}", "{\"name\":\"A1\",\"index\":-1}",
                              "{\"name\":\"A1\",\"index\":\"first\"}", "{\"name\":\"A1\",\"group\":7}"};
    for (size_t i = 0; i < sizeof bad_move / sizeof bad_move[0]; i++) {
        CHECK(run(s, "brush.presets.move", bad_move[i]) != PC_OK);
    }
    const char *bad_group[] = {"{\"group\":\"Nope\"}", "{\"group\":\"Alpha\",\"before\":\"Alpha\"}"};
    for (size_t i = 0; i < sizeof bad_group / sizeof bad_group[0]; i++) {
        CHECK(run(s, "brush.presets.moveGroup", bad_group[i]) != PC_OK);
    }
    CHECK(run(s, "brush.presets.renameGroup", "{\"group\":\"Alpha\"}") != PC_OK);
    CHECK(run(s, "brush.presets.deleteGroup", "{\"group\":\"Nope\"}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_rename);
    RUN(test_move_within_and_between_groups);
    RUN(test_move_group);
    RUN(test_rename_group);
    RUN(test_delete_group);
    RUN(test_graceful);
    return pc_t_finish();
}
