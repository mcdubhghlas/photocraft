#include "harness.h"
#include "ui/parameter_schema.h"
#include "engine/engine.h"
#include <string.h>
#include <stdlib.h>
static void test_registry_notation(void)
{
    pc_ui_parameter_schema s = {0};
    CHECK(pc_ui_parameter_schema_parse(&s,
                                       "{\"radius\":0.1..1000=1,\"method\":\"spin|zoom\"=\"zoom\",\"mono\":"
                                       "bool=true,\"seed\":u32=2,\"path\":text,\"matrix\":int[25],\"points\":"
                                       "json,\"color\":color=#ff0088,\"layer\":layer id} -> {\"ignored\":1}",
                                       NULL) == PC_OK);
    CHECK(s.count == 8);
    CHECK(s.items[0].kind == PC_UI_PARAM_RANGE && s.items[0].min == .1 && s.items[0].number == 1);
    CHECK(s.items[1].kind == PC_UI_PARAM_CHOICE && s.items[1].nchoices == 2);
    CHECK_EQ_STR(s.items[1].text, "zoom");
    CHECK(s.items[2].boolean);
    CHECK(s.items[3].number == 2);
    CHECK(s.items[4].kind == PC_UI_PARAM_TEXT);
    CHECK(s.items[5].grid_size == 25);
    CHECK(s.items[6].kind == PC_UI_PARAM_STRUCTURED);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    CHECK(pc_ui_parameter_schema_defaults(&s, doc, o, NULL) == PC_OK);
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(o, "radius")) == 1);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(o, "color")), "#ff0088");
    CHECK(!yyjson_mut_obj_get(o, "points"));
    CHECK(yyjson_mut_is_int(yyjson_mut_obj_get(o, "seed")));
    yyjson_mut_doc_free(doc);
    pc_ui_parameter_schema_fini(&s);
}
static void test_nested_and_defaults(void)
{
    pc_ui_parameter_schema s = {0};
    CHECK(pc_ui_parameter_schema_parse(
              &s,
              "{\"reds\":json,\"opts\":{\"x\":1,\"y\":[2,3]},\"curve\":[[0,0],[255,255]],\"gamma\":0.01..9."
              "99=1} (per-range [c,m,y,k] arrays: reds, greens)",
              NULL) == PC_OK);
    CHECK(s.count == 4);
    CHECK_EQ_STR(s.items[0].key, "reds");
    CHECK(s.items[0].kind == PC_UI_PARAM_STRUCTURED);
    CHECK(s.items[3].number == 1);
    CHECK(pc_ui_parameter_schema_parse(&s, "{\"enabled\":bool=false,\"method\":\"one|two\"=\"bad\"}", NULL) ==
          PC_OK);
    CHECK(s.count == 2 && !s.items[0].boolean);
    CHECK_EQ_STR(s.items[1].text, "one");
    CHECK(pc_ui_parameter_schema_parse(&s, "{\"broken\"}", NULL) != PC_OK);
    CHECK(s.count == 2);
    char *huge = malloc(65538);
    CHECK(huge != NULL);
    if (huge) {
        memset(huge, 'x', 65537);
        huge[65537] = 0;
        CHECK(pc_ui_parameter_schema_parse(&s, huge, NULL) != PC_OK);
        free(huge);
    }
    pc_ui_parameter_schema_fini(&s);
}
static void test_all_builtin_filter_adjustment_specs(void)
{
    size_t count = 0;
    const pc_engine_command *commands = pc_engine_commands(&count);
    size_t parsed = 0;
    for (size_t i = 0; i < count; i++)
        if (!strncmp(commands[i].id, "filter.", 7) || !strncmp(commands[i].id, "image.adjustments.", 18)) {
            pc_ui_parameter_schema s = {0};
            pc_error error = {0};
            pc_status status = pc_ui_parameter_schema_parse(&s, commands[i].params, &error);
            if (status != PC_OK)
                fprintf(stderr, "%s: %s\n", commands[i].id, error.msg);
            CHECK(status == PC_OK);
            parsed++;
            pc_ui_parameter_schema_fini(&s);
        }
    CHECK(parsed > 100);
}
static void test_typed_integer_defaults_and_text_float_tags(void)
{
    pc_ui_parameter_schema schema = {0};
    CHECK(pc_ui_parameter_schema_parse(&schema,
                                       "{\"seed\":u32=4294967295,\"offset\":i32=-2,\"name\":str=\"hello "
                                       "world\",\"angle\":deg=30,\"scale\":f32=1.5}",
                                       NULL) == PC_OK);
    CHECK(schema.count == 5 && schema.items[0].min == 0 && schema.items[0].max == 4294967295.0);
    CHECK(schema.items[2].kind == PC_UI_PARAM_TEXT);
    CHECK_EQ_STR(schema.items[2].text, "hello world");
    CHECK(schema.items[3].kind == PC_UI_PARAM_RANGE && schema.items[3].max == 360);
    CHECK(schema.items[4].kind == PC_UI_PARAM_RANGE && schema.items[4].number == 1.5);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(d);
    CHECK(pc_ui_parameter_schema_defaults(&schema, d, o, NULL) == PC_OK);
    CHECK(yyjson_mut_is_int(yyjson_mut_obj_get(o, "seed")));
    CHECK(yyjson_mut_get_sint(yyjson_mut_obj_get(o, "seed")) == 4294967295LL);
    CHECK(yyjson_mut_get_sint(yyjson_mut_obj_get(o, "offset")) == -2);
    yyjson_mut_doc_free(d);
    pc_ui_parameter_schema_fini(&schema);
}
int main(int argc, char **argv)
{
    RUN(test_typed_integer_defaults_and_text_float_tags);
    RUN(test_registry_notation);
    RUN(test_nested_and_defaults);
    RUN(test_all_builtin_filter_adjustment_specs);
    return pc_t_finish();
}
