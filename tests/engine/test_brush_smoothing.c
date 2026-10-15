/* Smoothing is a tool option: the Brush starts at Photoshop's 10%, picking a preset keeps the
 * current smoothing, and an explicit stroke smoothing overrides the tool switch without changing
 * it (port of crates/engine/tests/brush_smoothing.rs). */
#include <string.h>

#include "base/str.h"
#include "engine/brush_cmds.h"
#include "engine/engine.h"
#include "harness.h"

static pc_engine_session *new_session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":64,\"height\":64}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    return s;
}

static void set_brush(pc_engine_session *s, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    if (pc_engine_session_execute_json(s, "tools.setBrush", json, &out, &err) != PC_OK) {
        PC_T_FAIL("tools.setBrush %s: %s", json, err.msg);
    } else {
        pc_json_value_fini(&out);
    }
}

static const pc_paint_smoothing *brush_smoothing(const pc_engine_session *s)
{
    return &pc_engine_session_tools(s)->brush.smoothing;
}

/* Name of the first preset whose smoothing amount satisfies the predicate (>, <, == vs `v`). */
static const char *preset_with_amount(const pc_engine_session *s, char op, float v)
{
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    for (size_t i = 0; i < t->npresets; i++) {
        float a = t->presets[i].brush.smoothing.amount;
        bool hit = (op == '>' ? a > v : op == '<' ? a < v : a == v);
        if (hit) {
            return pc_str_c(&t->presets[i].name);
        }
    }
    return NULL;
}

static void test_presets_keep_the_tool_smoothing(void)
{
    pc_engine_session *s = new_session();
    CHECK(brush_smoothing(s)->amount == 0.1f); /* Photoshop's default */
    const char *preset = preset_with_amount(s, '=', 0.0f);
    const char *smoothed = preset_with_amount(s, '>', 0.2f);
    CHECK(preset != NULL && smoothed != NULL);
    if (preset == NULL || smoothed == NULL) {
        pc_engine_session_free(s);
        return;
    }
    char p[128];
    (void)snprintf(p, sizeof p, "{\"preset\":\"%s\"}", preset);
    set_brush(s, p);
    CHECK(brush_smoothing(s)->amount == 0.1f);
    set_brush(s, "{\"brush\":{\"smoothing\":{\"amount\":0.6,\"pulledString\":true}}}");
    (void)snprintf(p, sizeof p, "{\"preset\":\"%s\"}", smoothed);
    set_brush(s, p);
    CHECK(brush_smoothing(s)->amount == 0.6f);
    CHECK(brush_smoothing(s)->pulled_string == true);
    (void)snprintf(p, sizeof p, "{\"preset\":\"%s\",\"brush\":{\"smoothing\":{\"amount\":0.0}}}", preset);
    set_brush(s, p);
    CHECK(brush_smoothing(s)->amount == 0.0f);
    pc_engine_session_free(s);
}

static void test_switch_is_drivable_and_presets_keep_disabled(void)
{
    const char *params =
        "{\"brush\":{\"smoothing\":{\"enabled\":false,\"amount\":0.6,\"pulledString\":true,"
        "\"catchUpOnEnd\":false}}}";
    pc_engine_session *s = new_session();
    set_brush(s, params);
    pc_paint_smoothing off = *brush_smoothing(s);
    CHECK(!off.enabled && off.amount == 0.6f && off.pulled_string && !off.catch_up_on_end);
    const char *preset0 = pc_str_c(&pc_engine_session_tools(s)->presets[0].name);
    char p[96];
    (void)snprintf(p, sizeof p, "{\"preset\":\"%s\"}", preset0);
    set_brush(s, p);
    CHECK(pc_paint_smoothing_eq(brush_smoothing(s), &off));
    set_brush(s, "{\"brush\":{\"smoothing\":{\"enabled\":true}}}");
    CHECK(brush_smoothing(s)->enabled);
    CHECK(brush_smoothing(s)->amount == off.amount);
    CHECK(brush_smoothing(s)->pulled_string == off.pulled_string);

    /* The same patch reproduces the off state in a fresh session (CLI/MCP path). */
    pc_engine_session *other = NULL;
    CHECK(pc_engine_session_new(&other, NULL) == PC_OK);
    set_brush(other, params);
    CHECK(pc_paint_smoothing_eq(brush_smoothing(other), &off));
    pc_paint_brush before;
    pc_paint_brush_copy(&before, &pc_engine_session_tools(other)->brush);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(other, "tools.setBrush",
                                         "{\"brush\":{\"smoothing\":{\"enabled\":\"no\"}}}", &out,
                                         NULL) != PC_OK);
    CHECK(pc_paint_brush_eq(&pc_engine_session_tools(other)->brush, &before));
    pc_paint_brush_fini(&before);
    pc_engine_session_free(other);
    pc_engine_session_free(s);
}

static void test_explicit_stroke_smoothing_overrides(void)
{
    pc_engine_session *s = new_session();
    set_brush(s, "{\"brush\":{\"smoothing\":{\"enabled\":false,\"amount\":0.6}}}");

    yyjson_doc *empty = NULL;
    CHECK(pc_json_parse("{}", 2, &empty, NULL) == PC_OK);
    pc_paint_brush inherited;
    CHECK(pc_engine_resolve_brush(s, yyjson_doc_get_root(empty), "paint.stroke", &inherited, NULL) == PC_OK);
    CHECK(!inherited.smoothing.enabled && inherited.smoothing.amount == 0.6f);
    pc_paint_brush_fini(&inherited);
    yyjson_doc_free(empty);

    yyjson_doc *sm = NULL;
    CHECK(pc_json_parse("{\"smoothing\":0.4}", 17, &sm, NULL) == PC_OK);
    pc_paint_brush explicitb;
    CHECK(pc_engine_resolve_brush(s, yyjson_doc_get_root(sm), "paint.stroke", &explicitb, NULL) == PC_OK);
    CHECK(explicitb.smoothing.enabled && explicitb.smoothing.amount == 0.4f);
    pc_paint_brush_fini(&explicitb);
    yyjson_doc_free(sm);

    CHECK(!brush_smoothing(s)->enabled); /* a stroke override does not change the tool */
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_presets_keep_the_tool_smoothing);
    RUN(test_switch_is_drivable_and_presets_keep_disabled);
    RUN(test_explicit_stroke_smoothing_overrides);
    return pc_t_finish();
}
