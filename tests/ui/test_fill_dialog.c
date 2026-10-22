#include "harness.h"
#include "ui/fill_dialog.h"
#include <string.h>
#include <yyjson.h>

static void test_contents_order(void)
{
    CHECK(!strcmp(PC_UI_FILL_CONTENTS[0], "foreground"));
    CHECK(!strcmp(PC_UI_FILL_CONTENTS[1], "background"));
    CHECK(!strcmp(PC_UI_FILL_CONTENTS[2], "color"));
    CHECK(!strcmp(PC_UI_FILL_CONTENTS[3], "contentAware"));
    CHECK(!strcmp(PC_UI_FILL_CONTENTS[4], "pattern"));
    CHECK(!strcmp(PC_UI_FILL_CONTENTS[5], "history"));
    CHECK(!strcmp(PC_UI_FILL_CONTENTS[6], "black"));
    CHECK(!strcmp(PC_UI_FILL_CONTENTS[7], "gray"));
    CHECK(!strcmp(PC_UI_FILL_CONTENTS[8], "white"));
    CHECK(!strcmp(PC_UI_FILL_CONTENT_LABELS[7], "50% Gray"));
}
static void test_defaults(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK(d != NULL);
    float fg[4] = {1, 0, 0, 1};
    yyjson_mut_val *root = pc_ui_fill_default_fields(d, fg);
    CHECK(root != NULL);
    CHECK(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(root, "contents")), "foreground"));
    CHECK(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(root, "color")), "#ff0000"));
    CHECK(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(root, "mode")), "Normal"));
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(root, "colorAdaptation")) == true);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(root, "preserveTransparency")) == false);
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(root, "opacity")) == 100.0);
    yyjson_mut_doc_free(d);
}
static void test_null_foreground(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK(d != NULL);
    yyjson_mut_val *root = pc_ui_fill_default_fields(d, NULL);
    CHECK(root != NULL);
    CHECK(!strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(root, "color")), "#000000"));
    yyjson_mut_doc_free(d);
    /* NULL doc never crashes. */
    CHECK(pc_ui_fill_default_fields(NULL, NULL) == NULL);
}
int main(int argc, char **argv)
{
    RUN(test_contents_order);
    RUN(test_defaults);
    RUN(test_null_foreground);
    (void)argc;
    (void)argv;
    return pc_t_finish();
}
