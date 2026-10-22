#include "harness.h"
#include "ui/autoform.h"
#include "ui/parameter_schema.h"
#include <string.h>

static void test_command_predicate(void)
{
    CHECK(pc_ui_autoform_command("file.automate.contactSheetII"));
    CHECK(pc_ui_autoform_command("file.scripts.statistics"));
    CHECK(pc_ui_autoform_command("file.scripts.scriptEventsManager"));
    CHECK(pc_ui_autoform_command("file.package"));
    CHECK(pc_ui_autoform_command("file.export.pathsToIllustrator"));
    CHECK(pc_ui_autoform_command("file.scripts.browse"));
    CHECK(!pc_ui_autoform_command("file.automate.createDroplet")); /* deferred (needs an action) */
    CHECK(!pc_ui_autoform_command("file.open"));
    CHECK(!pc_ui_autoform_command("image.imageSize"));
    CHECK(!pc_ui_autoform_command(NULL));
}

static void test_labels(void)
{
    CHECK(!strcmp(pc_ui_autoform_label("file.automate.contactSheetII"), "Contact Sheet II"));
    CHECK(!strcmp(pc_ui_autoform_label("file.scripts.statistics"), "Image Statistics"));
    CHECK(!strcmp(pc_ui_autoform_label("file.export.pathsToIllustrator"), "Export Paths to File"));
    CHECK(pc_ui_autoform_label("file.open") == NULL);
}

/* Every static spec parses into a non-empty schema with the fields the engine reads. */
static void check_spec_parses(const char *spec, const char *must_have)
{
    pc_ui_parameter_schema s = {0};
    CHECK(pc_ui_parameter_schema_parse(&s, spec, NULL) == PC_OK);
    CHECK(s.count > 0);
    if (must_have) {
        bool found = false;
        for (size_t i = 0; i < s.count; i++) {
            if (!strcmp(s.items[i].key, must_have)) {
                found = true;
            }
        }
        CHECK(found);
    }
    pc_ui_parameter_schema_fini(&s);
}

static void test_static_specs(void)
{
    check_spec_parses(pc_ui_autoform_spec("file.automate.contactSheetII"), "input");
    check_spec_parses(pc_ui_autoform_spec("file.automate.contactSheetII"), "depth");
    check_spec_parses(pc_ui_autoform_spec("file.scripts.statistics"), "mode");
    check_spec_parses(pc_ui_autoform_spec("file.scripts.scriptEventsManager"), "event");
    check_spec_parses(pc_ui_autoform_spec("file.package"), "format");
    check_spec_parses(pc_ui_autoform_spec("file.scripts.browse"), "path");
    /* depth must be an integer field (the engine reads it with pc_json_as_i64). */
    pc_ui_parameter_schema s = {0};
    CHECK(pc_ui_parameter_schema_parse(&s, pc_ui_autoform_spec("file.automate.contactSheetII"), NULL) ==
          PC_OK);
    for (size_t i = 0; i < s.count; i++) {
        if (!strcmp(s.items[i].key, "depth")) {
            CHECK(s.items[i].kind == PC_UI_PARAM_INTEGER);
        }
    }
    pc_ui_parameter_schema_fini(&s);
    /* pathsToIllustrator is built dynamically. */
    CHECK(pc_ui_autoform_spec("file.export.pathsToIllustrator") == NULL);
}

static void test_browse_for(void)
{
    pc_ui_autoform_browse k = PC_UI_AUTOFORM_BROWSE_NONE;
    const char *f = pc_ui_autoform_browse_for("file.automate.contactSheetII", &k);
    CHECK(f && !strcmp(f, "input") && k == PC_UI_AUTOFORM_BROWSE_OPEN_FOLDER);
    f = pc_ui_autoform_browse_for("file.package", &k);
    CHECK(f && !strcmp(f, "dir") && k == PC_UI_AUTOFORM_BROWSE_OPEN_FOLDER);
    f = pc_ui_autoform_browse_for("file.export.pathsToIllustrator", &k);
    CHECK(f && !strcmp(f, "path") && k == PC_UI_AUTOFORM_BROWSE_SAVE_FILE);
    f = pc_ui_autoform_browse_for("file.scripts.browse", &k);
    CHECK(f && !strcmp(f, "path") && k == PC_UI_AUTOFORM_BROWSE_OPEN_FILE);
    f = pc_ui_autoform_browse_for("file.scripts.scriptEventsManager", &k);
    CHECK(f && !strcmp(f, "script") && k == PC_UI_AUTOFORM_BROWSE_OPEN_FILE);
    f = pc_ui_autoform_browse_for("file.open", &k);
    CHECK(f == NULL && k == PC_UI_AUTOFORM_BROWSE_NONE);
}

static void test_paths_spec(void)
{
    char out[4608];
    const char *names[] = {"Path 1", "Outline"};
    /* With a work path and two named paths: all | work | names, default "all". */
    CHECK(pc_ui_autoform_paths_spec(names, 2, true, out, sizeof out));
    CHECK(strstr(out, "\"all") != NULL);
    CHECK(strstr(out, "work") != NULL);
    CHECK(strstr(out, "Path 1") != NULL);
    CHECK(strstr(out, "Outline") != NULL);
    CHECK(strstr(out, "=\"all\"") != NULL);
    pc_ui_parameter_schema s = {0};
    CHECK(pc_ui_parameter_schema_parse(&s, out, NULL) == PC_OK);
    pc_ui_parameter_schema_fini(&s);
    /* No work path, no names: just "all". */
    CHECK(pc_ui_autoform_paths_spec(NULL, 0, false, out, sizeof out));
    CHECK(strstr(out, "work") == NULL);
    CHECK(pc_ui_parameter_schema_parse(&s, out, NULL) == PC_OK);
    pc_ui_parameter_schema_fini(&s);
    /* A hostile name with the choice delimiter / quotes is sanitised so the schema still parses. */
    const char *nasty[] = {"a|b\"c'd"};
    CHECK(pc_ui_autoform_paths_spec(nasty, 1, false, out, sizeof out));
    CHECK(strstr(out, "a|b") == NULL); /* the '|' was replaced */
    CHECK(pc_ui_parameter_schema_parse(&s, out, NULL) == PC_OK);
    pc_ui_parameter_schema_fini(&s);
    /* Bounds: a tiny buffer is rejected, never overrun. */
    char tiny[8];
    CHECK(!pc_ui_autoform_paths_spec(names, 2, true, tiny, sizeof tiny));
    CHECK(!pc_ui_autoform_paths_spec(names, 2, true, NULL, 0));
    /* Over 64 names is capped and still parses. */
    const char *many[128];
    for (size_t i = 0; i < 128; i++) {
        many[i] = "p";
    }
    CHECK(pc_ui_autoform_paths_spec(many, 128, false, out, sizeof out));
    CHECK(pc_ui_parameter_schema_parse(&s, out, NULL) == PC_OK);
    pc_ui_parameter_schema_fini(&s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_command_predicate);
    RUN(test_labels);
    RUN(test_static_specs);
    RUN(test_browse_for);
    RUN(test_paths_spec);
    return pc_t_finish();
}
