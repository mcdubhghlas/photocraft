/* Preset Manager model (U10 B8): the kind table, parsing the list reply into owned names, and the
 * selection/reorder rules. Pure logic, no window. */
#include "harness.h"
#include "ui/preset_manager.h"
#include "base/json.h"
#include <string.h>

static void test_kinds(void)
{
    const pc_ui_preset_kind *kinds = NULL;
    size_t n = pc_ui_preset_kinds(&kinds);
    CHECK_EQ_UINT(n, 3u);
    CHECK(kinds && !strcmp(kinds[0].id, "brushes"));
    CHECK(kinds && !strcmp(kinds[1].id, "customShapes"));
    CHECK(kinds && !strcmp(kinds[2].id, "patterns"));
    CHECK(pc_ui_preset_kind_valid("brushes"));
    CHECK(pc_ui_preset_kind_valid("customShapes"));
    CHECK(pc_ui_preset_kind_valid("patterns"));
    CHECK(!pc_ui_preset_kind_valid("gradients"));
    CHECK(!pc_ui_preset_kind_valid(NULL));
}

/* Parse a reply JSON string for a kind. */
static void parse_reply(const char *text, const char *kind, char ***names, size_t *n)
{
    yyjson_doc *doc = NULL;
    pc_error err;
    CHECK(pc_json_parse(text, strlen(text), &doc, &err) == PC_OK);
    /* Re-root as a mutable value (the model takes yyjson_mut_val). */
    yyjson_mut_doc *m = yyjson_doc_mut_copy(doc, NULL);
    CHECK(m != NULL);
    CHECK(pc_ui_preset_list_parse(yyjson_mut_doc_get_root(m), kind, names, n, &err) == PC_OK);
    yyjson_mut_doc_free(m);
    yyjson_doc_free(doc);
}

static void test_list_parse(void)
{
    char **names = NULL;
    size_t n = 0;
    parse_reply("{\"brushes\":[\"Soft Round\",\"Hard Round\",\"Chalk\"]}", "brushes", &names, &n);
    CHECK_EQ_UINT(n, 3u);
    CHECK(names && !strcmp(names[0], "Soft Round"));
    CHECK(names && !strcmp(names[1], "Hard Round"));
    CHECK(names && !strcmp(names[2], "Chalk"));
    pc_ui_preset_names_free(names, n);

    /* A missing field is an empty library, not an error. */
    names = NULL;
    n = 99;
    parse_reply("{\"brushes\":[]}", "customShapes", &names, &n);
    CHECK(names == NULL);
    CHECK_EQ_UINT(n, 0u);

    /* An odd (non-array) field is also an empty library. */
    names = NULL;
    n = 99;
    parse_reply("{\"patterns\":42}", "patterns", &names, &n);
    CHECK(names == NULL);
    CHECK_EQ_UINT(n, 0u);

    /* Non-string entries become empty names (never NULL, never a crash). */
    names = NULL;
    n = 0;
    parse_reply("{\"brushes\":[\"ok\",7,null]}", "brushes", &names, &n);
    CHECK_EQ_UINT(n, 3u);
    CHECK(names && !strcmp(names[0], "ok"));
    CHECK(names && names[1][0] == '\0');
    CHECK(names && names[2][0] == '\0');
    pc_ui_preset_names_free(names, n);
}

static void test_bad_input(void)
{
    char **names = (char **)0x1;
    size_t n = 7;
    pc_error err;
    /* NULL reply -> empty, OK. */
    CHECK(pc_ui_preset_list_parse(NULL, "brushes", &names, &n, &err) == PC_OK);
    CHECK(names == NULL);
    CHECK_EQ_UINT(n, 0u);
    /* Missing outputs -> invalid, not a crash. */
    CHECK(pc_ui_preset_list_parse(NULL, "brushes", NULL, &n, &err) == PC_ERR_INVALID);
    pc_ui_preset_names_free(NULL, 0);
}

static void test_selection_and_move(void)
{
    /* Clamp: past-the-end snaps to last, empty -> 0. */
    CHECK_EQ_UINT(pc_ui_preset_clamp(0, 0), 0u);
    CHECK_EQ_UINT(pc_ui_preset_clamp(5, 0), 0u);
    CHECK_EQ_UINT(pc_ui_preset_clamp(5, 3), 2u);
    CHECK_EQ_UINT(pc_ui_preset_clamp(1, 3), 1u);

    /* Move rules over a 3-item list. */
    CHECK(!pc_ui_preset_can_move_up(0, 3));
    CHECK(pc_ui_preset_can_move_up(1, 3));
    CHECK(pc_ui_preset_can_move_up(2, 3));
    CHECK(pc_ui_preset_can_move_down(0, 3));
    CHECK(pc_ui_preset_can_move_down(1, 3));
    CHECK(!pc_ui_preset_can_move_down(2, 3));
    /* Degenerate lists. */
    CHECK(!pc_ui_preset_can_move_up(0, 0));
    CHECK(!pc_ui_preset_can_move_down(0, 0));
    CHECK(!pc_ui_preset_can_move_up(0, 1));
    CHECK(!pc_ui_preset_can_move_down(0, 1));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_kinds);
    RUN(test_list_parse);
    RUN(test_bad_input);
    RUN(test_selection_and_move);
    return pc_t_finish();
}
