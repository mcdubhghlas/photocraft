/* Drives the PhotocraftApp control handler (src/ui/shell/control.c) headlessly: it never initialises
 * SDL video, so there is no GL context — the non-screenshot methods run against the app + UiState,
 * and ui.screenshot takes the CPU capture path and still yields a valid PNG. Asserts the request /
 * response shapes of docs/control-protocol.md and that bad input is an error, never a crash. */
#include <stdlib.h>
#include <string.h>

#include "base/base64.h"
#include "base/buf.h"
#include "base/json.h"
#include "harness.h"
#include "ui/shell/app.h"
#include "ui/shell/control.h"

/* Call a control method with JSON params text (NULL = none). Fills *out_text with the serialised
 * result on success; returns the pc_status. */
static pc_status call(pc_ui_shell_app *a, const char *method, const char *params_json, pc_buf *out_text)
{
    yyjson_doc *pdoc = NULL;
    yyjson_val *params = NULL;
    if (params_json != NULL) {
        pc_error pe;
        if (pc_json_parse(params_json, strlen(params_json), &pdoc, &pe) != PC_OK) {
            PC_T_FAIL("params parse: %s", pe.msg);
            return PC_ERR_INVALID;
        }
        params = yyjson_doc_get_root(pdoc);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err;
    pc_status rc = pc_ui_control_handle(a, method, params, &out, &err);
    if (rc == PC_OK && out_text != NULL) {
        yyjson_mut_val *root = pc_json_value_root(&out);
        (void)pc_json_write(root, 0, out_text, &err);
    }
    pc_json_value_fini(&out);
    yyjson_doc_free(pdoc);
    return rc;
}

static void test_inspect_shape(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_buf t = PC_BUF_INIT;
    CHECK(call(a, "ui.inspect", NULL, &t) == PC_OK);
    const char *s = pc_buf_cstr(&t);
    CHECK_CONTAINS(s, "\"window\"");
    CHECK_CONTAINS(s, "\"tool\"");
    CHECK_CONTAINS(s, "\"panels\"");
    CHECK_CONTAINS(s, "\"theme\"");
    CHECK_CONTAINS(s, "\"session\"");
    CHECK_CONTAINS(s, "\"themeTokens\"");
    pc_buf_fini(&t);
    pc_ui_shell_app_free(a);
}

static void test_ui_set_tool_and_theme(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    /* A good tool + theme change applies. */
    CHECK(call(a, "ui.set", "{\"tool\":\"Move\",\"theme\":\"studio\"}", NULL) == PC_OK);
    pc_buf t = PC_BUF_INIT;
    CHECK(call(a, "ui.inspect", NULL, &t) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&t), "\"tool\":\"Move\"");
    CHECK_CONTAINS(pc_buf_cstr(&t), "\"theme\":\"Studio\"");
    pc_buf_fini(&t);
    CHECK(pc_ui_shell_app_tokens(a)->kind == PC_UI_THEME_STUDIO);

    /* selectionMode 0..3; out of range is an error that changes nothing. */
    CHECK(call(a, "ui.set", "{\"selectionMode\":2}", NULL) == PC_OK);
    CHECK(call(a, "ui.set", "{\"selectionMode\":9}", NULL) != PC_OK);
    CHECK(pc_ui_shell_app_state(a)->selection_mode == 2);

    /* An unknown tool / theme is an error, nothing applied. */
    CHECK(call(a, "ui.set", "{\"tool\":\"nope\"}", NULL) != PC_OK);
    CHECK(call(a, "ui.set", "{\"theme\":\"nope\"}", NULL) != PC_OK);
    pc_ui_shell_app_free(a);
}

static void test_ui_set_unknown_field(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_error e;
    yyjson_doc *pdoc = NULL;
    const char *pj = "{\"bogus\":1}";
    CHECK(pc_json_parse(pj, strlen(pj), &pdoc, &e) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_control_handle(a, "ui.set", yyjson_doc_get_root(pdoc), &out, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "unknown field `bogus`");
    pc_json_value_fini(&out);
    yyjson_doc_free(pdoc);
    pc_ui_shell_app_free(a);
}

static void test_menu_list_and_invoke(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_buf t = PC_BUF_INIT;
    CHECK(call(a, "ui.menu.list", NULL, &t) == PC_OK);
    CHECK(t.len > 100); /* a large array of menu items */
    CHECK_CONTAINS(pc_buf_cstr(&t), "\"id\":\"file.new\"");
    pc_buf_fini(&t);

    /* Invoking an engine command opens a document. */
    CHECK(call(a, "ui.menu.invoke",
               "{\"id\":\"file.new\",\"params\":{\"width\":64,\"height\":48}}", NULL) == PC_OK);
    pc_buf ins = PC_BUF_INIT;
    CHECK(call(a, "ui.inspect", NULL, &ins) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&ins), "\"document\":{");
    pc_buf_fini(&ins);

    /* An unknown command is an error. */
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"no.such.command\"}", NULL) != PC_OK);
    pc_ui_shell_app_free(a);
}

static void test_engine_execute_and_commands(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_buf t = PC_BUF_INIT;
    CHECK(call(a, "engine.commands", NULL, &t) == PC_OK);
    CHECK(t.len > 100);
    pc_buf_fini(&t);

    /* engine.execute runs a command by id directly. */
    CHECK(call(a, "engine.execute",
               "{\"command\":\"file.new\",\"params\":{\"width\":10,\"height\":10}}", NULL) == PC_OK);
    pc_ui_shell_app_free(a);
}

static void test_pointer_validation(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    /* Missing events, bad coordinates, bad kind, out-of-range pressure: all errors, no crash. */
    CHECK(call(a, "ui.pointer", "{}", NULL) != PC_OK);
    CHECK(call(a, "ui.pointer", "{\"events\":[{\"x\":\"abc\",\"y\":1}]}", NULL) != PC_OK);
    CHECK(call(a, "ui.pointer", "{\"events\":[{\"x\":1,\"y\":1,\"kind\":\"spin\"}]}", NULL) != PC_OK);
    CHECK(call(a, "ui.pointer", "{\"events\":[{\"x\":1,\"y\":1,\"pressure\":4}]}", NULL) != PC_OK);
    /* A valid gesture reports how many events were queued. */
    pc_buf t = PC_BUF_INIT;
    CHECK(call(a, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":5,\"y\":5,\"pressure\":0.5},"
               "{\"kind\":\"up\",\"x\":6,\"y\":6}]}",
               &t) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&t), "\"queued\":2");
    pc_buf_fini(&t);
    pc_ui_shell_app_free(a);
}

static void test_screenshot_png(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    /* No window: the CPU capture path must still produce a valid PNG (base64, no path). */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_control_handle(a, "ui.screenshot", NULL, &out, &err) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    yyjson_mut_val *b64 = yyjson_mut_obj_get(root, "base64");
    const char *b = yyjson_mut_get_str(b64);
    CHECK(b != NULL);
    if (b != NULL) {
        pc_buf raw = PC_BUF_INIT;
        CHECK(pc_base64_decode(b, strlen(b), &raw, &err) == PC_OK);
        /* PNG signature. */
        CHECK(raw.len > 8);
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        CHECK(raw.len >= 8 && memcmp(raw.data, sig, 8) == 0);
        pc_buf_fini(&raw);
    }
    pc_json_value_fini(&out);

    /* With a document open, the screenshot composites its thumbnail; still a valid PNG. */
    CHECK(call(a, "engine.execute",
               "{\"command\":\"file.new\",\"params\":{\"width\":32,\"height\":24}}", NULL) == PC_OK);
    pc_json_value out2 = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_control_handle(a, "ui.screenshot", NULL, &out2, &err) == PC_OK);
    pc_json_value_fini(&out2);
    pc_ui_shell_app_free(a);
}

static void test_unknown_method(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_control_handle(a, "ui.nonexistent", NULL, &out, &err) != PC_OK);
    CHECK(pc_ui_control_handle(a, NULL, NULL, &out, &err) != PC_OK);
    pc_json_value_fini(&out);
    pc_ui_shell_app_free(a);
}

static void test_resize(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    CHECK(call(a, "ui.resize", "{\"width\":640,\"height\":480}", NULL) == PC_OK);
    float w = 0, h = 0, ppp = 0;
    pc_ui_shell_app_window_size(a, &w, &h, &ppp);
    CHECK(w == 640.0f && h == 480.0f);
    CHECK(call(a, "ui.resize", "{\"width\":-1,\"height\":480}", NULL) != PC_OK);
    pc_ui_shell_app_free(a);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_inspect_shape);
    RUN(test_ui_set_tool_and_theme);
    RUN(test_ui_set_unknown_field);
    RUN(test_menu_list_and_invoke);
    RUN(test_engine_execute_and_commands);
    RUN(test_pointer_validation);
    RUN(test_screenshot_png);
    RUN(test_unknown_method);
    RUN(test_resize);
    return pc_t_finish();
}
