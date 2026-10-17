/* UI state (port of crates/ui-egui/src/state.rs tests). */
#include "harness.h"
#include "base/json.h"
#include "ui/state.h"

#include <stdlib.h>
#include <string.h>

static pc_ui_tool parse(const char *s)
{
    pc_ui_tool t;
    return pc_ui_tool_from_name(s, &t) ? t : PC_TOOL_N;
}

static void test_tool_names_parse(void)
{
    CHECK(parse("brush") == PC_TOOL_BRUSH);
    CHECK(parse("Rect") == PC_TOOL_RECT_MARQUEE);
    CHECK(parse("RectMarquee") == PC_TOOL_RECT_MARQUEE);
    CHECK(parse("Eraser Tool") == PC_TOOL_ERASER);
    CHECK(parse("mixerBrush") == PC_TOOL_MIXER_BRUSH);
    CHECK(parse("Mixer Brush Tool") == PC_TOOL_MIXER_BRUSH);
    CHECK(parse("colorReplacement") == PC_TOOL_COLOR_REPLACEMENT);
    CHECK(parse("Color Replacement Tool") == PC_TOOL_COLOR_REPLACEMENT);
    CHECK(parse("redEye") == PC_TOOL_RED_EYE);
    CHECK(parse("Red Eye Tool") == PC_TOOL_RED_EYE);
    CHECK(pc_ui_tool_key(PC_TOOL_RED_EYE) == 'J');
    CHECK(!pc_ui_tool_is_brushlike(PC_TOOL_RED_EYE));
    CHECK(parse("patternStamp") == PC_TOOL_PATTERN_STAMP);
    CHECK(parse("Pattern Stamp Tool") == PC_TOOL_PATTERN_STAMP);
    CHECK_EQ_INT((int)(sizeof PC_UI_TOOL_ALL / sizeof PC_UI_TOOL_ALL[0]), 67);
    CHECK(parse("Vertical Type Mask Tool") == PC_TOOL_VERTICAL_TYPE_MASK);
    CHECK(parse("addAnchorPoint") == PC_TOOL_ADD_ANCHOR_POINT);
    CHECK(parse("Delete Anchor Point Tool") == PC_TOOL_DELETE_ANCHOR_POINT);
    CHECK(pc_ui_tool_key(PC_TOOL_DELETE_ANCHOR_POINT) == 'P');
    CHECK(parse("convertPoint") == PC_TOOL_CONVERT_POINT);
    CHECK(parse("Convert Point Tool") == PC_TOOL_CONVERT_POINT);
    CHECK(pc_ui_tool_key(PC_TOOL_CONVERT_POINT) == 'P'); /* shares the Pen's key */
    CHECK(!pc_ui_tool_is_brushlike(PC_TOOL_CONVERT_POINT));
    CHECK(parse("Remove Tool") == PC_TOOL_REMOVE);
    CHECK(pc_ui_tool_key(PC_TOOL_REMOVE) == 'J');
    CHECK(pc_ui_tool_is_brushlike(PC_TOOL_REMOVE));
    CHECK(parse("RotateView") == PC_TOOL_ROTATE_VIEW);
    CHECK(parse("Rotate View Tool") == PC_TOOL_ROTATE_VIEW);
    CHECK(pc_ui_tool_key(PC_TOOL_ROTATE_VIEW) == 'R');
    CHECK(parse("nope") == PC_TOOL_N); /* no match */
}

static void test_tool_serde_roundtrip(void)
{
    for (int i = 0; i < PC_TOOL_N; i++) {
        pc_ui_tool t = PC_UI_TOOL_ALL[i], back;
        CHECK(pc_ui_tool_from_serde(pc_ui_tool_serde(t), &back) && back == t);
        CHECK(pc_ui_tool_label(t)[0] != '\0');
        CHECK(pc_ui_tool_glyph(t)[0] != '\0');
    }
    CHECK_EQ_STR(pc_ui_tool_serde(PC_TOOL_BRUSH), "Brush");
    CHECK_EQ_STR(pc_ui_tool_serde(PC_TOOL_RECT_MARQUEE), "RectMarquee");
    CHECK_EQ_STR(pc_ui_tool_label(PC_TOOL_TYPE), "Horizontal Type Tool");
    CHECK(pc_ui_tool_is_type(PC_TOOL_VERTICAL_TYPE_MASK));
    CHECK(pc_ui_tool_is_type_mask(PC_TOOL_HORIZONTAL_TYPE_MASK));
    CHECK(!pc_ui_tool_is_type_mask(PC_TOOL_TYPE));
    CHECK(pc_ui_tool_is_vertical_type(PC_TOOL_VERTICAL_TYPE));
    CHECK(pc_ui_tool_key(PC_TOOL_BLUR) == '\0');
}

static void test_enums(void)
{
    pc_ui_dialog_kind k;
    CHECK(pc_ui_dialog_kind_from_serde("NewDocument", &k) && k == PC_DIALOG_NEW_DOCUMENT);
    CHECK(pc_ui_dialog_kind_from_serde("LayerStyle", &k) && k == PC_DIALOG_LAYER_STYLE);
    CHECK_EQ_STR(pc_ui_dialog_kind_serde(PC_DIALOG_COMMAND), "Command");
    CHECK_EQ_STR(pc_ui_transform_mode_serde(PC_TRANSFORM_FREE), "free");
    CHECK_EQ_STR(pc_ui_transform_mode_serde(PC_TRANSFORM_PERSPECTIVE), "perspective");
    CHECK(pc_ui_transform_mode_for_command("edit.transform.rotate") == PC_TRANSFORM_ROTATE);
    CHECK(pc_ui_transform_mode_for_command("edit.transform.skew") == PC_TRANSFORM_SKEW);
    CHECK(pc_ui_transform_mode_for_command("edit.freeTransform") == PC_TRANSFORM_FREE);
    CHECK(pc_ui_transform_mode_for_command("edit.transform.again") == PC_TRANSFORM_FREE);
}

static void test_defaults(void)
{
    pc_ui_state s;
    pc_ui_state_init(&s);
    CHECK(s.tool == PC_TOOL_BRUSH);
    CHECK(s.theme == PC_UI_THEME_PRO_MEDIUM);
    CHECK_EQ_UINT(s.next_id, 1);
    CHECK_EQ_STR(s.workspace, "Essentials");
    CHECK(s.panels.layers && s.panels.properties && s.panels.color && s.panels.toolbar);
    CHECK(s.panels.options_bar && s.panels.status_bar && s.panels.dock && s.panels.rail && s.panels.menu_bar);
    CHECK(!s.panels.history && !s.panels.navigator && !s.panels.brush_settings && !s.panels.gradient_editor);
    CHECK(s.extras.guides && s.extras.snap && !s.extras.rulers && !s.extras.grid && !s.extras.lock_guides);
    CHECK(!s.camera_raw_preview.has_zoom && s.camera_raw_preview.center[0] == 0.5f);
    pc_ui_view v;
    pc_ui_view_default(&v);
    CHECK(v.zoom == 1.0f && v.fit_pending && !v.fill_pending);
    pc_ui_state_fini(&s);
}

static void test_dialogs(void)
{
    pc_ui_state s;
    pc_ui_state_init(&s);
    uint64_t a = pc_ui_state_open_dialog(&s, PC_DIALOG_ABOUT, NULL);
    /* new_document_fields: build, serialize, parse to an immutable value, open the dialog. */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_doc_set_root(d, pc_ui_state_new_document_fields(d));
    pc_buf buf = PC_BUF_INIT;
    CHECK(pc_json_write(yyjson_mut_doc_get_root(d), 0, &buf, NULL) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&buf), "\"width\":1920");
    yyjson_doc *pf = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&buf), buf.len, &pf, NULL) == PC_OK);
    uint64_t b = pc_ui_state_open_dialog(&s, PC_DIALOG_NEW_DOCUMENT, yyjson_doc_get_root(pf));
    yyjson_doc_free(pf);
    pc_buf_fini(&buf);
    yyjson_mut_doc_free(d);
    CHECK(a != b);
    pc_ui_dialog *db = pc_ui_state_dialog_mut(&s, b);
    CHECK(db != NULL);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(db->fields);
    yyjson_mut_val *wv = yyjson_mut_obj_get(root, "width");
    CHECK(wv && yyjson_mut_get_uint(wv) == 1920);
    CHECK(pc_ui_state_close_dialog(&s, a));
    CHECK_EQ_UINT(s.n_dialogs, 1);
    CHECK(!pc_ui_state_close_dialog(&s, a)); /* already gone */
    pc_ui_state_fini(&s);
}

static void test_group_tools(void)
{
    pc_ui_state s;
    pc_ui_state_init(&s);
    s.tool = PC_TOOL_ELLIPSE_MARQUEE; /* key 'M' */
    pc_ui_state_remember_group_tool(&s);
    pc_ui_tool g;
    CHECK(pc_ui_state_group_tool(&s, 'M', &g) && g == PC_TOOL_ELLIPSE_MARQUEE);
    /* another tool in the same group replaces it. */
    s.tool = PC_TOOL_RECT_MARQUEE;
    pc_ui_state_remember_group_tool(&s);
    CHECK(pc_ui_state_group_tool(&s, 'M', &g) && g == PC_TOOL_RECT_MARQUEE);
    CHECK_EQ_UINT(s.n_group_tools, 1);
    /* a different group is kept separately. */
    s.tool = PC_TOOL_LASSO; /* 'L' */
    pc_ui_state_remember_group_tool(&s);
    CHECK_EQ_UINT(s.n_group_tools, 2);
    CHECK(!pc_ui_state_group_tool(&s, 'Z', &g));
    pc_ui_state_fini(&s);
}

static char *serialize(const pc_ui_state *s)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    if (pc_ui_state_to_json(s, doc, &root, NULL) != PC_OK) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    pc_buf out = PC_BUF_INIT;
    pc_status st = pc_json_write(root, 0, &out, NULL);
    yyjson_mut_doc_free(doc);
    if (st != PC_OK) {
        pc_buf_fini(&out);
        return NULL;
    }
    char *s2 = strdup(pc_buf_cstr(&out));
    pc_buf_fini(&out);
    return s2;
}

static void test_state_roundtrip(void)
{
    pc_ui_state s;
    pc_ui_state_init(&s);
    /* Serialize the default: tool must be "Brush". */
    char *json = serialize(&s);
    CHECK(json != NULL);
    CHECK_CONTAINS(json, "\"tool\":\"Brush\"");
    CHECK_CONTAINS(json, "\"theme\":\"ProMedium\"");

    /* Mutate drivable fields, serialize, parse back, re-serialize: byte-exact round-trip. */
    s.tool = PC_TOOL_ZOOM;
    s.theme = PC_UI_THEME_SOLARIZED_DARK;
    s.palette_open = true;
    s.panels.navigator = true;
    s.extras.rulers = true;
    s.view.flip_horizontal = true;
    CHECK(pc_str_set_c(&s.view.pixel_aspect, "anamorphic2To1", NULL) == PC_OK);
    s.selection_mode = 2;
    s.crop_angle = 12.5;
    free(s.status);
    s.status = strdup("Opened file");
    s.status_error = true;
    s.mask_target = true;
    /* one view and one dialog */
    s.views = calloc(1, sizeof(pc_ui_view));
    pc_ui_view_default(&s.views[0]);
    s.views[0].zoom = 2.0f;
    s.views[0].doc_size[0] = 800;
    s.views[0].doc_size[1] = 600;
    s.n_views = 1;
    pc_ui_state_open_dialog(&s, PC_DIALOG_ABOUT, NULL);
    char *a = serialize(&s);
    CHECK(a != NULL);

    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(a, strlen(a), &parsed, NULL) == PC_OK);
    pc_ui_state s2;
    pc_ui_state_init(&s2);
    CHECK(pc_ui_state_from_json(&s2, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    yyjson_doc_free(parsed);
    char *b = serialize(&s2);
    CHECK(b != NULL);
    CHECK_EQ_STR(a, b); /* round-trip is byte-exact over the U1 subset */
    /* and the drivable fields survived. */
    CHECK(s2.tool == PC_TOOL_ZOOM);
    CHECK(s2.theme == PC_UI_THEME_SOLARIZED_DARK);
    CHECK(s2.palette_open && s2.panels.navigator && s2.extras.rulers && s2.mask_target && s2.status_error);
    CHECK_EQ_UINT(s2.selection_mode, 2);
    CHECK(s2.crop_angle == 12.5);
    CHECK_EQ_STR(s2.status, "Opened file");
    CHECK(s2.view.flip_horizontal);
    CHECK_NEAR(pc_ui_view_display_aspect(&s2.view), 2, 0);
    CHECK_EQ_UINT(s2.n_views, 1);
    CHECK(s2.views[0].zoom == 2.0f && s2.views[0].doc_size[0] == 800);
    CHECK_EQ_UINT(s2.n_dialogs, 1);

    free(json);
    free(a);
    free(b);
    pc_ui_state_fini(&s);
    pc_ui_state_fini(&s2);
}

int main(int argc, char **argv)
{
    RUN(test_tool_names_parse);
    RUN(test_tool_serde_roundtrip);
    RUN(test_enums);
    RUN(test_defaults);
    RUN(test_dialogs);
    RUN(test_group_tools);
    RUN(test_state_roundtrip);
    return pc_t_finish();
}
