/* Drives the PhotocraftApp control handler (src/ui/shell/control.c) headlessly: it never initialises
 * SDL video, so there is no GL context — the non-screenshot methods run against the app + UiState,
 * and ui.screenshot takes the CPU capture path and still yields a valid PNG. Asserts the request /
 * response shapes of docs/control-protocol.md and that bad input is an error, never a crash. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/base64.h"
#include "base/buf.h"
#include "base/json.h"
#include "harness.h"
#include "ui/shell/app.h"
#include "ui/shell/control.h"
#include "ui/shell/shell_internal.h"

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
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"file.new\",\"params\":{\"width\":64,\"height\":48}}", NULL) ==
          PC_OK);
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
    CHECK(call(a, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":10,\"height\":10}}",
               NULL) == PC_OK);
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
    CHECK(call(a, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":32,\"height\":24}}",
               NULL) == PC_OK);
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

static void test_document_cameras(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    CHECK(call(a, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":80,\"height\":60}}",
               NULL) == PC_OK);
    CHECK(call(a, "ui.set", "{\"zoom\":8,\"center\":[12,23],\"rotation\":45}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    CHECK(ui->n_views == 1);
    CHECK_NEAR(ui->views[0].zoom, 8, 0);
    CHECK(call(a, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":40,\"height\":30}}",
               NULL) == PC_OK);
    CHECK(call(a, "ui.set", "{\"zoom\":2,\"center\":[7,9]}", NULL) == PC_OK);
    CHECK(ui->n_views == 2);
    CHECK_NEAR(ui->views[0].zoom, 8, 0);
    CHECK_NEAR(ui->views[1].zoom, 2, 0);
    pc_buf b = PC_BUF_INIT;
    CHECK(call(a, "ui.inspect", NULL, &b) == PC_OK);
    yyjson_doc *inspected = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&b), b.len, &inspected, &err) == PC_OK);
    double current_zoom = 0;
    CHECK(pc_json_as_f64(pc_json_get(pc_json_get(yyjson_doc_get_root(inspected), "activeView"), "zoom"),
                         &current_zoom));
    CHECK_NEAR(current_zoom, 2, 0);
    yyjson_doc_free(inspected);
    pc_buf_fini(&b);
    size_t moved;
    CHECK(pc_engine_session_move_document(pc_ui_shell_app_session(a), 0, 1, &moved));
    CHECK(pc_ui_shell_app_sync_views(a, &err) == PC_OK);
    CHECK_NEAR(ui->views[0].zoom, 2, 0);
    CHECK_NEAR(ui->views[1].zoom, 8, 0);
    CHECK(pc_engine_session_close(pc_ui_shell_app_session(a), 0));
    CHECK(pc_ui_shell_app_sync_views(a, &err) == PC_OK);
    CHECK(ui->n_views == 1);
    CHECK_NEAR(ui->views[0].zoom, 8, 0);
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.zoomOut\"}", NULL) == PC_OK);
    CHECK_NEAR(ui->views[0].zoom, 7, 0);
    pc_buf reset = PC_BUF_INIT;
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.resetView\"}", &reset) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&reset), "\"rotation\":0.0");
    CHECK_CONTAINS(pc_buf_cstr(&reset), "\"zoom\":7.0");
    pc_buf_fini(&reset);
    CHECK_NEAR(ui->views[0].rotation, 0, 0);
    pc_ui_shell_app_free(a);
}

static void test_camera_rejects_atomically(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    CHECK(call(a, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":80,\"height\":60}}",
               NULL) == PC_OK);
    CHECK(pc_ui_shell_app_state(a)->n_views == 0);
    CHECK(call(a, "ui.set", "{\"zoom\":1e100}", NULL) != PC_OK);
    CHECK(pc_ui_shell_app_state(a)->n_views == 0);
    CHECK(call(a, "ui.set", "{\"zoom\":2,\"center\":[10,20]}", NULL) == PC_OK);
    const char *bad[] = {"{\"zoom\":1e100,\"tool\":\"Brush\"}", "{\"center\":[1e100,0],\"zoom\":3}",
                         "{\"center\":[1],\"zoom\":3}", "{\"rotation\":1e100,\"zoom\":3}",
                         "{\"fit\":7,\"zoom\":3}"};
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    pc_ui_tool original = ui->tool;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(call(a, "ui.set", bad[i], NULL) != PC_OK);
        CHECK_NEAR(ui->views[0].zoom, 2, 0);
        CHECK_NEAR(ui->views[0].center[0], 10, 0);
        CHECK(ui->tool == original);
    }
    CHECK(call(a, "ui.set", "{\"zoom\":1000,\"rotation\":450}", NULL) == PC_OK);
    CHECK_NEAR(ui->views[0].zoom, 128, 0);
    CHECK_NEAR(ui->views[0].rotation, 90, 0);
    pc_ui_shell_app_free(a);
}

/* Opt-in real GL integration: run PC_TEST_GL=1 SDL_VIDEO_DRIVER=offscreen. The ordinary test
 * suite remains display-independent; the opt-in run also works under ASan with Mesa leaks off. */
static void test_gl_canvas(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "red", pc_size_make(128, 96), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(1, 0, 0), &err) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, &err) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
    for (int rotation = 0; rotation < 4; rotation++) {
        char params[80];
        (void)snprintf(params, sizeof params, "{\"zoom\":2,\"center\":[64,48],\"rotation\":%d}",
                       rotation * 90);
        CHECK(call(a, "ui.set", params, NULL) == PC_OK);
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        pc_status rc = pc_ui_shell_app_capture(a, &rgba, &w, &h, &err);
        if (rc != PC_OK) {
            PC_T_FAIL("GL capture: %s", err.msg);
        }
        CHECK(rc == PC_OK);
        if (rgba && w > 500 && h > 450) {
            const uint8_t *center = rgba + ((size_t)429 * (size_t)w + 490) * 4;
            CHECK(center[0] > 250 && center[1] < 3 && center[2] < 3);
        }
        free(rgba);
    }
    CHECK(call(a, "ui.set", "{\"tool\":\"Hand\",\"rotation\":0,\"center\":[64,48],\"zoom\":2}", NULL) ==
          PC_OK);
    SDL_Event event;
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.x = 490;
    event.button.y = 429;
    pc_ui_shell_input_event(a, &event);
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = 510;
    event.motion.y = 429;
    pc_ui_shell_input_event(a, &event);
    event.motion.x = 530;
    pc_ui_shell_input_event(a, &event);
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    CHECK_NEAR(ui->views[0].center[0], 44, 1e-3);
    CHECK_NEAR(ui->views[0].center[1], 48, 1e-3);
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = false;
    event.button.x = 530;
    event.button.y = 429;
    pc_ui_shell_input_event(a, &event);
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = INFINITY;
    event.motion.y = NAN;
    pc_ui_shell_input_event(a, &event);
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = NULL;
    pc_ui_shell_input_event(a, &event);
    CHECK(call(a, "ui.set", "{\"rotation\":0,\"center\":[96,48],\"zoom\":2}", NULL) == PC_OK);
    for (size_t variant = 0; variant < 3; variant++) {
        if (variant == 1) {
            CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.flipHorizontal\"}", NULL) == PC_OK);
        } else if (variant == 2) {
            CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.flipHorizontal\"}", NULL) == PC_OK);
            CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.pixelAspectRatio.anamorphic2To1\"}", NULL) ==
                  PC_OK);
        }
        uint8_t *pixels = NULL;
        int width = 0, height = 0;
        CHECK(pc_ui_shell_app_capture(a, &pixels, &width, &height, &err) == PC_OK);
        if (pixels && width > 600 && height > 429) {
            size_t sample_x = variant == 2 ? 600 : 350;
            const uint8_t *sample = pixels + ((size_t)429 * (size_t)width + sample_x) * 4;
            bool red = sample[0] > 250 && sample[1] < 3 && sample[2] < 3;
            CHECK(red == (variant != 1));
        }
        free(pixels);
    }
    /* Scrollbar capture must override Hand, keep a fixed drag scale, and end on release. */
    ui->views[0].zoom = 16;
    ui->views[0].center[0] = 64;
    ui->views[0].center[1] = 48;
    uint8_t *pixels = NULL;
    int width = 0, height = 0;
    CHECK(pc_ui_shell_app_capture(a, &pixels, &width, &height, &err) == PC_OK);
    free(pixels);
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.x = 490;
    event.button.y = 794;
    pc_ui_shell_input_event(a, &event);
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = 540;
    event.motion.y = 794;
    pc_ui_shell_input_event(a, &event);
    CHECK(ui->views[0].center[0] > 64);
    CHECK_NEAR(ui->views[0].center[1], 48, 0);
    float drag_center = ui->views[0].center[0];
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 540;
    event.button.y = 794;
    pc_ui_shell_input_event(a, &event);
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = 560;
    event.motion.y = 794;
    pc_ui_shell_input_event(a, &event);
    CHECK_NEAR(ui->views[0].center[0], drag_center, 0);
    CHECK(!pc_engine_doc_state_is_dirty(pc_engine_session_active(pc_ui_shell_app_session(a))));
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.rulers\"}", NULL) == PC_OK);
    CHECK(ui->extras.rulers);
    uint8_t *ruled = NULL;
    int rw = 0, rh = 0;
    CHECK(pc_ui_shell_app_capture(a, &ruled, &rw, &rh, &err) == PC_OK);
    free(ruled);
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

static void test_display_commands(void)
{
    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, NULL) == PC_OK);
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.flipHorizontal\"}", NULL) == PC_OK);
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.pixelAspectRatio.anamorphic2To1\"}", NULL) == PC_OK);
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.screenMode.cycle\"}", NULL) == PC_OK);
    pc_buf text = PC_BUF_INIT;
    CHECK(call(a, "ui.inspect", NULL, &text) == PC_OK);
    CHECK_CONTAINS((const char *)text.data, "\"flip_horizontal\":true");
    CHECK_CONTAINS((const char *)text.data, "\"pixel_aspect\":\"anamorphic2To1\"");
    CHECK_CONTAINS((const char *)text.data, "\"screen_mode\":\"fullScreenWithMenuBar\"");
    pc_buf_fini(&text);
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
    RUN(test_gl_canvas);
    RUN(test_display_commands);
    RUN(test_document_cameras);
    RUN(test_camera_rejects_atomically);
    return pc_t_finish();
}
