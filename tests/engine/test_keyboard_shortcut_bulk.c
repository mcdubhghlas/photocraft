/* A bulk edit.keyboardShortcuts that assigns one key to two commands must keep both bindings and
 * report the in-call clash in `conflicts`, not silently unbind both (#719). Port of
 * crates/engine/tests/keyboard_shortcut_bulk.rs. */
#include <string.h>

#include "engine/engine.h"
#include "harness.h"

/* result["commands"][*]["shortcut"] for the given id, or NULL. */
static const char *shortcut_of(yyjson_mut_val *root, const char *id)
{
    if (root == NULL) {
        return NULL;
    }
    yyjson_mut_val *cmds = yyjson_mut_obj_get(root, "commands");
    if (cmds == NULL || !yyjson_mut_is_arr(cmds)) {
        return NULL;
    }
    size_t i, n;
    yyjson_mut_val *c;
    yyjson_mut_arr_foreach (cmds, i, n, c) {
        const char *cid = yyjson_mut_get_str(yyjson_mut_obj_get(c, "id"));
        if (cid != NULL && strcmp(cid, id) == 0) {
            return yyjson_mut_get_str(yyjson_mut_obj_get(c, "shortcut"));
        }
    }
    return NULL;
}

static void test_bulk_same_key_reports_conflict_and_keeps_bindings(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "edit.keyboardShortcuts",
              "{\"set\":{\"edit.undo\":\"Cmd+Z\",\"edit.fill\":\"Cmd+Z\"},\"list\":true}", &out,
              NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    const char *fill = shortcut_of(root, "edit.fill");
    const char *undo = shortcut_of(root, "edit.undo");
    CHECK_EQ_STR(fill ? fill : "", "Cmd+Z");
    CHECK_EQ_STR(undo ? undo : "", "Cmd+Z");
    /* The in-call clash appears in conflicts: a Cmd+Z entry listing two commands. */
    bool clash = false;
    yyjson_mut_val *conflicts = yyjson_mut_obj_get(root, "conflicts");
    if (conflicts != NULL && yyjson_mut_is_arr(conflicts)) {
        size_t i, n;
        yyjson_mut_val *c;
        yyjson_mut_arr_foreach (conflicts, i, n, c) {
            const char *sc = yyjson_mut_get_str(yyjson_mut_obj_get(c, "shortcut"));
            yyjson_mut_val *ids = yyjson_mut_obj_get(c, "commands");
            if (sc != NULL && strcmp(sc, "Cmd+Z") == 0 && ids != NULL && yyjson_mut_is_arr(ids) &&
                yyjson_mut_arr_size(ids) == 2) {
                clash = true;
            }
        }
    }
    CHECK(clash);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_single_set_steals_from_old_owner(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "edit.keyboardShortcuts", "{\"set\":{\"edit.fill\":\"Cmd+Z\"},\"list\":true}", &out,
              NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    const char *fill = shortcut_of(root, "edit.fill");
    const char *undo = shortcut_of(root, "edit.undo");
    CHECK_EQ_STR(fill ? fill : "", "Cmd+Z");
    CHECK(undo == NULL || strcmp(undo, "Cmd+Z") != 0);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_bulk_same_key_reports_conflict_and_keeps_bindings);
    RUN(test_single_set_steals_from_old_owner);
    return pc_t_finish();
}
