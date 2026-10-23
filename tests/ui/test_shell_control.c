/* Drives the PhotocraftApp control handler (src/ui/shell/control.c) headlessly: it never initialises
 * SDL video, so there is no GL context — the non-screenshot methods run against the app + UiState,
 * and ui.screenshot takes the CPU capture path and still yields a valid PNG. Asserts the request /
 * response shapes of docs/control-protocol.md and that bad input is an error, never a crash. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/base64.h"
#include "base/buf.h"
#include "base/json.h"
#include "harness.h"
#include "engine/prefs.h"
#include "ui/dock.h"
#include "ui/shell/app.h"
#include "ui/artboard.h"
#include "engine/floating.h"
#include "ui/shell/sampling_ui.h"
#include "ui/shell/context_ui.h"
#include "ui/shell/crop_ui.h"
#include "ui/shell/control.h"
#include "ui/shell/shell_internal.h"
#include "ui/shell/preset_manager_ui.h"
#include "ui/shell/profile_mismatch_ui.h"
#include "ui/shell/warp_ui.h"
#include "ui/preset_ui.h"
#include "base/file.h"
#include "codecs/codecs.h"
#include "codecs/format.h"
#include "codecs/image.h"

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

/* P1: forcing software compositing (--safe-gpu / a detected GPU crash) is reflected by the app
 * accessor and by ui.inspect.gpuInfo reporting the CPU canvas. */
static void test_force_cpu_rendering(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    CHECK(pc_ui_shell_app_force_cpu(a) == false);
    pc_ui_shell_app_set_force_cpu(a, true);
    CHECK(pc_ui_shell_app_force_cpu(a) == true);
    pc_json_value info = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_gpu_info(a, &info, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&info);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "canvas")), "cpu");
    pc_json_value_fini(&info);
    pc_ui_shell_app_set_force_cpu(a, false);
    CHECK(pc_ui_shell_app_force_cpu(a) == false);
    pc_ui_shell_app_set_force_cpu(NULL, true); /* NULL-safe */
    CHECK(pc_ui_shell_app_force_cpu(NULL) == false);
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
    CHECK(call(a, "engine.execute", "{\"id\":\"file.new\",\"params\":{\"width\":32,\"height\":32}}", NULL) ==
          PC_OK);
    CHECK(call(a, "ui.set", "{\"tool\":\"rectMarquee\"}", NULL) == PC_OK);
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
static void physical_digit(pc_ui_shell_app *app, SDL_Keycode key, SDL_Scancode scan, SDL_Keymod mods,
                           bool repeat)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.scancode = scan;
    event.key.mod = mods;
    event.key.down = true;
    event.key.repeat = repeat;
    pc_ui_shell_input_event(app, &event);
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    pc_ui_shell_input_event(app, &event);
}
static void test_brush_opacity_and_resize_input(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&app, &err) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, "file.new", "{\"width\":128,\"height\":96}", &out, &err) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"Brush\",\"zoom\":2,\"rotation\":90}", NULL) == PC_OK);
    physical_digit(app, SDLK_4, SDL_SCANCODE_4, 0, false);
    CHECK_NEAR(pc_engine_session_tools(session)->brush.opacity, .4f, 1e-6);
    physical_digit(app, SDLK_4, SDL_SCANCODE_4, 0, true);
    CHECK_NEAR(pc_engine_session_tools(session)->brush.opacity, .4f, 1e-6);
    physical_digit(app, SDLK_5, SDL_SCANCODE_5, 0, false);
    CHECK_NEAR(pc_engine_session_tools(session)->brush.opacity, .45f, 1e-6);
    physical_digit(app, SDLK_PERCENT, SDL_SCANCODE_5, SDL_KMOD_SHIFT, false);
    CHECK_NEAR(pc_engine_session_tools(session)->brush.flow, .5f, 1e-6);
    physical_digit(app, SDLK_0, SDL_SCANCODE_0, SDL_KMOD_SHIFT, false);
    CHECK_NEAR(pc_engine_session_tools(session)->brush.flow, .5f, 1e-6);
    CHECK(pc_engine_session_execute_json(session, "tools.setBrush",
                                         "{\"brush\":{\"size\":20,\"hardness\":0.5,\"opacity\":0.8}}", &out,
                                         &err) == PC_OK);
    pc_json_value_fini(&out);
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    CHECK(pc_ui_shell_app_capture(app, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float x = xf.x + xf.width / 2, y = xf.y + xf.height / 2;
    uint64_t revision = pc_engine_session_active(session)->revision;
    size_t journal = pc_engine_session_journal_len(session);
    SDL_SetModState(SDL_KMOD_ALT);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_RIGHT;
    e.button.down = true;
    e.button.x = x;
    e.button.y = y;
    pc_ui_shell_input_event(app, &e);
    CHECK(pc_ui_shell_canvas_pointer_active(app));
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.state = SDL_BUTTON_RMASK;
    e.motion.x = x + 20;
    e.motion.y = y + 40;
    pc_ui_shell_input_event(app, &e);
    CHECK_NEAR(pc_engine_session_tools(session)->brush.size, 40, 0);
    CHECK_NEAR(pc_engine_session_tools(session)->brush.hardness, .7f, 1e-6);
    e.motion.x = x + 40;
    e.motion.y = y + 60;
    pc_ui_shell_input_event(app, &e);
    CHECK_NEAR(pc_engine_session_tools(session)->brush.size, 60, 0);
    CHECK_NEAR(pc_engine_session_tools(session)->brush.hardness, .8f, 1e-6);
    CHECK(pc_engine_session_active(session)->revision == revision);
    CHECK(pc_engine_session_journal_len(session) == journal + 1);
    const char *snapshot = getenv("PC_BRUSH_SNAPSHOT");
    if (snapshot) {
        char request[512];
        (void)snprintf(request, sizeof request, "{\"path\":\"%s\"}", snapshot);
        CHECK(call(app, "ui.screenshot", request, NULL) == PC_OK);
    }
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.button = SDL_BUTTON_RIGHT;
    e.button.x = x + 40;
    e.button.y = y + 60;
    pc_ui_shell_input_event(app, &e);
    CHECK(!pc_ui_shell_canvas_pointer_active(app));
    SDL_SetModState(0);
    /* Background opacity is protected; a normal layer accepts digits and Shift Fill. */
    CHECK(call(app, "ui.set", "{\"tool\":\"Move\"}", NULL) == PC_OK);
    physical_digit(app, SDLK_3, SDL_SCANCODE_3, 0, false);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(session)->doc);
    CHECK_NEAR(doc->layers[0].opacity, 1, 0);
    CHECK(pc_engine_session_execute_json(session, "layer.new.layer", "{}", &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    SDL_zero(e);
    e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    pc_ui_shell_input_event(app, &e);
    physical_digit(app, SDLK_8, SDL_SCANCODE_8, 0, false);
    physical_digit(app, SDLK_5, SDL_SCANCODE_5, 0, false);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    const pc_layer *layer = pc_document_layer(pc_doc_arc_get(state->doc), state->active_layer);
    CHECK(layer != NULL);
    CHECK_NEAR(layer->opacity, .85f, 1e-6);
    physical_digit(app, SDLK_PERCENT, SDL_SCANCODE_5, SDL_KMOD_SHIFT, false);
    state = pc_engine_session_active(session);
    layer = pc_document_layer(pc_doc_arc_get(state->doc), state->active_layer);
    CHECK_NEAR(layer->fill_opacity, .5f, 1e-6);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static pc_status canvas_patch(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                              pc_error *err)
{
    (void)has_active;
    (void)active;
    const float green[4] = {0, 1, 0, 1};
    return pc_surface_fill_rect(pc_layer_surface_mut(&doc->layers[0]), *(pc_rect *)ctx, green, 4, err);
}
static void test_gl_canvas_damage(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    for (int depth = 0; depth < 2; depth++) {
        pc_ui_shell_app *app = NULL;
        pc_error err;
        CHECK(pc_ui_shell_app_new(&app, &err) == PC_OK);
        pc_engine_session *session = pc_ui_shell_app_session(app);
        pc_document doc;
        CHECK(pc_document_with_background(&doc, "damage", pc_size_make(128, 96), PC_COLOR_MODE_RGB,
                                          depth ? PC_SAMPLE_TYPE_F32 : PC_SAMPLE_TYPE_U16,
                                          pc_color_rgb(1, 0, 0), &err) == PC_OK);
        size_t index;
        CHECK(pc_engine_session_add_document(session, &doc, NULL, &index, &err) == PC_OK);
        CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
        CHECK(call(app, "ui.set", "{\"zoom\":2,\"center\":[64,48]}", NULL) == PC_OK);
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        CHECK(pc_ui_shell_app_capture(app, &rgba, &w, &h, &err) == PC_OK);
        free(rgba);
        pc_rect damage = pc_rect_make(60, 44, 68, 52);
        CHECK(pc_engine_session_edit(session, "patch", canvas_patch, &damage, &err) == PC_OK);
        pc_engine_doc_state *state = pc_engine_session_active_mut(session);
        state->last_damage = damage;
        state->has_last_damage = true;
        rgba = NULL;
        CHECK(pc_ui_shell_app_capture(app, &rgba, &w, &h, &err) == PC_OK);
        pc_ui_canvas_xform xf;
        CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
        float center_screen[2], outside_screen[2];
        CHECK(pc_ui_canvas_to_screen(&xf, 64, 48, center_screen) == PC_OK);
        CHECK(pc_ui_canvas_to_screen(&xf, 44, 48, outside_screen) == PC_OK);
        if (rgba && center_screen[0] >= 0 && center_screen[0] < (float)w && center_screen[1] >= 0 &&
            center_screen[1] < (float)h && outside_screen[0] >= 0) {
            const uint8_t *center =
                rgba + ((size_t)center_screen[1] * (size_t)w + (size_t)center_screen[0]) * 4;
            CHECK(center[0] < 3 && center[1] > 250 && center[2] < 3);
            const uint8_t *outside =
                rgba + ((size_t)outside_screen[1] * (size_t)w + (size_t)outside_screen[0]) * 4;
            CHECK(outside[0] > 250 && outside[1] < 3 && outside[2] < 3);
        }
        free(rgba);
        pc_ui_shell_app_free(app);
    }
    SDL_Quit();
}

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
    float thumb[4];
    CHECK(pc_ui_shell_app_panel_mapping(a, "view.scroll.horizontal.thumb", 0, thumb));
    float thumb_x = thumb[0] + thumb[2] / 2, thumb_y = thumb[1] + thumb[3] / 2;
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.x = thumb_x;
    event.button.y = thumb_y;
    pc_ui_shell_input_event(a, &event);
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = thumb_x + 50;
    event.motion.y = thumb_y;
    pc_ui_shell_input_event(a, &event);
    CHECK(ui->views[0].center[0] > 64);
    CHECK_NEAR(ui->views[0].center[1], 48, 0);
    float drag_center = ui->views[0].center[0];
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = thumb_x + 50;
    event.button.y = thumb_y;
    pc_ui_shell_input_event(a, &event);
    memset(&event, 0, sizeof event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = thumb_x + 70;
    event.motion.y = thumb_y;
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

/* U10: Copy mirrors the session clipboard onto the OS clipboard as PNG (needs the video subsystem,
 * so it is opt-in like the GL tests; no GL context is created). SDL serves our own just-set data
 * back in-process on any driver, so the roundtrip verifies the export without a native clipboard. */
static void test_gl_os_clipboard_image(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    /* A fresh app with nothing copied: export is a quiet no-op (no crash, no image offered). */
    SDL_ClearClipboardData();
    pc_ui_shell_os_clipboard_export(a);
    CHECK(!SDL_HasClipboardData("image/png"));
    /* Copy a full selection of a 128x96 document and read the image back off the OS clipboard. */
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "bg", pc_size_make(128, 96), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(0, 0.5f, 1), &err) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, &err) == PC_OK);
    CHECK(call(a, "engine.execute", "{\"command\":\"select.all\"}", NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(a, "edit.copy", NULL, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    pc_size clip;
    CHECK(pc_engine_session_clipboard_size(pc_ui_shell_app_session(a), &clip));
    CHECK(SDL_HasClipboardData("image/png"));
    size_t png_len = 0;
    void *png = SDL_GetClipboardData("image/png", &png_len);
    CHECK(png != NULL && png_len > 8);
    if (png != NULL && png_len > 8) {
        pc_cimage decoded;
        pc_cimage_init(&decoded);
        CHECK(pc_cdecode_as(PC_CF_PNG, png, png_len, &decoded, &err) == PC_OK);
        CHECK(decoded.width == 128 && decoded.height == 96);
        pc_cimage_fini(&decoded);
    }
    SDL_free(png);
    SDL_ClearClipboardData();
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

/* U5: the floating preset-panel Window items toggle their state flag (previously unimplemented) and
 * report their checked state, driving preset_panels::menu's float_flag half. */
static void test_preset_float_panels_toggle(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_ui_state *st = pc_ui_shell_app_state(a);
    const char *ids[4] = {"window.panel.styles", "window.panel.shapes", "window.panel.toolPresets",
                          "window.panel.cloneSource"};
    const bool *flags[4] = {&st->presets_ui.styles, &st->presets_ui.shapes,
                            &st->presets_ui.tool_presets, &st->presets_ui.clone_source};
    for (size_t i = 0; i < 4; i++) {
        CHECK(!*flags[i]);
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(pc_ui_shell_app_invoke(a, ids[i], NULL, &out, &err) == PC_OK);
        pc_json_value_fini(&out);
        CHECK(*flags[i]); /* shown */
        out = (pc_json_value)PC_JSON_VALUE_INIT;
        CHECK(pc_ui_shell_app_invoke(a, ids[i], NULL, &out, &err) == PC_OK);
        pc_json_value_fini(&out);
        CHECK(!*flags[i]); /* a second click hides it again */
    }
    pc_ui_shell_app_free(a);
}

/* An external PNG handed to SDL as if another app owned the clipboard (no mark_exported, so the
 * shell treats it as foreign and imports it). */
static struct {
    uint8_t *bytes;
    size_t len;
} g_ext_png;

static const void *SDLCALL ext_png_provide(void *userdata, const char *mime, size_t *size)
{
    (void)userdata;
    (void)mime;
    if (size != NULL) {
        *size = g_ext_png.len;
    }
    return g_ext_png.bytes;
}

/* U10: an image another app put on the OS clipboard is decoded and set as the session clipboard, so
 * Paste / New from Clipboard place it (the import half; opt-in like the other clipboard test). */
static void test_gl_os_clipboard_image_in(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_error err;
    /* A 50x40 solid-magenta external PNG. */
    const uint32_t ew = 50, eh = 40;
    uint8_t *px = malloc((size_t)ew * eh * 4);
    CHECK(px != NULL);
    for (size_t i = 0; i < (size_t)ew * eh; i++) {
        px[i * 4 + 0] = 255;
        px[i * 4 + 1] = 0;
        px[i * 4 + 2] = 255;
        px[i * 4 + 3] = 255;
    }
    pc_cimage src;
    pc_cimage_init(&src);
    CHECK(pc_cimage_from_copy(ew, eh, PC_CL_RGBA, PC_CS_U8, px, (size_t)ew * eh * 4, &src, &err) == PC_OK);
    free(px);
    pc_buf png = PC_BUF_INIT;
    CHECK(pc_cencode(&src, PC_CF_PNG, NULL, &png, &err) == PC_OK && png.len > 8);
    pc_cimage_fini(&src);
    g_ext_png.bytes = pc_buf_take(&png, &g_ext_png.len);
    CHECK(g_ext_png.bytes != NULL);
    pc_buf_fini(&png);

    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    SDL_ClearClipboardData();
    static const char *const mimes[] = {"image/png"};
    CHECK(SDL_SetClipboardData(ext_png_provide, NULL, NULL, mimes, 1));
    /* Nothing copied in PhotoCraft yet: the session clipboard is empty. */
    CHECK(!pc_engine_session_clipboard_size(pc_ui_shell_app_session(a), NULL));
    /* New from Clipboard goes through the shell invoke path, which imports the external image first,
     * then builds a 50x40 document from it. */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(a, "file.newFromClipboard", NULL, &out, &err) == PC_OK);
    pc_buf text = PC_BUF_INIT;
    CHECK(pc_json_write(pc_json_value_root(&out), 0, &text, &err) == PC_OK);
    CHECK_CONTAINS((const char *)text.data, "\"width\":50");
    CHECK_CONTAINS((const char *)text.data, "\"height\":40");
    pc_buf_fini(&text);
    pc_json_value_fini(&out);
    pc_size clip;
    CHECK(pc_engine_session_clipboard_size(pc_ui_shell_app_session(a), &clip));
    CHECK(clip.width == 50 && clip.height == 40);
    SDL_ClearClipboardData();
    pc_ui_shell_app_free(a);
    free(g_ext_png.bytes);
    g_ext_png.bytes = NULL;
    g_ext_png.len = 0;
    SDL_Quit();
}

/* U8: the Liquify freeze-mask overlay and mesh render offscreen without crashing (Rule 6). Set
 * PC_LIQUIFY_SNAPSHOT=out.png to eyeball the frozen region (red) and the displaced mesh. */
static void test_gl_liquify_freeze_overlay(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "bg", pc_size_make(160, 120), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(0.9f, 0.85f, 0.2f), &err) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, &err) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(a, "filter.liquify", NULL, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_warp_active(a));
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(a);
    CHECK(r->kind == PC_UI_WARP_LIQUIFY);
    /* Freeze the whole field and show both the mask and the mesh. */
    CHECK(pc_ui_liquify_global(&r->liquify, PC_ALGO_LIQUIFY_FREEZE_ALL, false, 0.0, &err) == PC_OK);
    r->liquify.opts.show_mask = true;
    r->liquify.opts.show_mesh = true;
    CHECK(call(a, "ui.set", "{\"zoom\":3,\"center\":[80,60]}", NULL) == PC_OK);
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    const char *snap = getenv("PC_LIQUIFY_SNAPSHOT");
    if (snap != NULL && rgba != NULL && w > 0 && h > 0) {
        pc_cimage img;
        if (pc_cimage_from_copy((uint32_t)w, (uint32_t)h, PC_CL_RGBA, PC_CS_U8, rgba,
                                (size_t)w * (size_t)h * 4, &img, &err) == PC_OK) {
            pc_buf out_png = PC_BUF_INIT;
            if (pc_cencode(&img, PC_CF_PNG, NULL, &out_png, &err) == PC_OK) {
                (void)pc_file_write_atomic(snap, out_png.data, out_png.len, &err);
            }
            pc_buf_fini(&out_png);
            pc_cimage_fini(&img);
        }
    }
    free(rgba);
    /* Thaw all / invert all also run cleanly. */
    CHECK(pc_ui_liquify_global(&r->liquify, PC_ALGO_LIQUIFY_THAW_ALL, false, 0.0, &err) == PC_OK);
    CHECK(pc_ui_liquify_global(&r->liquify, PC_ALGO_LIQUIFY_INVERT_FREEZE, false, 0.0, &err) == PC_OK);
    CHECK(pc_ui_shell_app_invoke(a, "ui.warp.cancel", NULL, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

/* U8: the Liquify live warped-pixel proxy preview seam (pure CPU; always runs). Entering Liquify
 * with no edit yet draws nothing; a forward-warp drag builds the proxy and reports a bounded dirty
 * rect per continuing move; cancel drops the preview. */
static void test_liquify_live_proxy(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "bg", pc_size_make(160, 120), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(0.2f, 0.5f, 0.9f), &err) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, &err) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(a, "filter.liquify", NULL, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_warp_active(a));
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(a);
    CHECK(r->kind == PC_UI_WARP_LIQUIFY);
    r->liquify.opts.size = 48;

    /* No edit yet: the overlay draws nothing (crisp canvas shows). */
    pc_ui_warp_overlay_frame f;
    CHECK(!pc_ui_shell_warp_live_overlay(a, &f));

    /* A forward-warp drag builds and renders the proxy. */
    CHECK(pc_ui_liquify_pointer(&r->liquify, PC_UI_POINTER_DOWN, 40, 60, 1.0, false, &err) == PC_OK);
    CHECK(pc_ui_shell_warp_live_overlay(a, &f));
    CHECK(f.px != NULL && f.w > 0 && f.h > 0);
    CHECK(pc_rect_width(f.doc_bounds) == 160 && pc_rect_height(f.doc_bounds) == 120);

    CHECK(pc_ui_liquify_pointer(&r->liquify, PC_UI_POINTER_MOVE, 70, 60, 1.0, false, &err) == PC_OK);
    CHECK(pc_ui_shell_warp_live_overlay(a, &f));
    /* A continuing move re-renders only a bounded sub-region, not the whole proxy. */
    CHECK(f.dirty[0] < f.dirty[2] && f.dirty[1] < f.dirty[3]);
    CHECK(f.dirty[2] <= f.w && f.dirty[3] <= f.h);
    CHECK(!(f.dirty[0] == 0 && f.dirty[1] == 0 && f.dirty[2] == f.w && f.dirty[3] == f.h));

    CHECK(pc_ui_liquify_pointer(&r->liquify, PC_UI_POINTER_MOVE, 100, 60, 1.0, false, &err) == PC_OK);
    CHECK(pc_ui_shell_warp_live_overlay(a, &f));
    CHECK(pc_ui_liquify_pointer(&r->liquify, PC_UI_POINTER_UP, 100, 60, 1.0, false, &err) == PC_OK);
    CHECK(pc_ui_shell_warp_live_overlay(a, &f));

    /* Idle frame: nothing changed, so there is no dirty region to upload. */
    pc_ui_warp_overlay_frame idle;
    CHECK(pc_ui_shell_warp_live_overlay(a, &idle));
    CHECK(idle.dirty[0] == 0 && idle.dirty[1] == 0 && idle.dirty[2] == 0 && idle.dirty[3] == 0);

    /* Cancel drops the editor and the preview. */
    CHECK(pc_ui_shell_app_invoke(a, "ui.warp.cancel", NULL, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(!pc_ui_shell_warp_live_overlay(a, &f));
    pc_ui_shell_app_free(a);
}

static pc_status stripe_patch(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                              pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)ctx;
    const float white[4] = {0.95f, 0.95f, 0.95f, 1.f};
    pc_surface *bg = pc_layer_surface_mut(&doc->layers[0]);
    pc_status rc = PC_OK;
    for (int sx = 0; bg && rc == PC_OK && sx < 160; sx += 24) {
        rc = pc_surface_fill_rect(bg, pc_rect_make(sx, 0, sx + 12, 120), white, 4, err);
    }
    return rc;
}

/* U8: the Liquify live warped-pixel proxy renders offscreen under a rotated/zoomed view without
 * crashing (Rule 6). Set PC_LIQUIFY_PROXY_SNAPSHOT=out.png to eyeball a warped preview. */
static void test_gl_liquify_live_proxy(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "bg", pc_size_make(160, 120), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(0.95f, 0.4f, 0.1f), &err) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, &err) == PC_OK);
    /* Paint vertical stripes so the warp of actual pixels (not just the mesh) is visible. */
    CHECK(pc_engine_session_edit(pc_ui_shell_app_session(a), "stripes", stripe_patch, NULL, &err) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(a, "filter.liquify", NULL, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(a);
    CHECK(r->kind == PC_UI_WARP_LIQUIFY);
    r->liquify.opts.size = 70;
    r->liquify.opts.pressure = 100;
    /* A bloat-and-push warp across the middle of the image. */
    CHECK(pc_ui_liquify_pointer(&r->liquify, PC_UI_POINTER_DOWN, 40, 60, 1.0, false, &err) == PC_OK);
    CHECK(pc_ui_liquify_pointer(&r->liquify, PC_UI_POINTER_MOVE, 80, 40, 1.0, false, &err) == PC_OK);
    CHECK(pc_ui_liquify_pointer(&r->liquify, PC_UI_POINTER_MOVE, 120, 60, 1.0, false, &err) == PC_OK);
    CHECK(pc_ui_liquify_pointer(&r->liquify, PC_UI_POINTER_UP, 120, 60, 1.0, false, &err) == PC_OK);
    r->liquify.opts.show_mesh = true;
    /* Rotated and zoomed: the overlay must line up with the mesh. */
    CHECK(call(a, "ui.set", "{\"zoom\":3,\"rotation\":30,\"center\":[80,60]}", NULL) == PC_OK);
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    const char *snap = getenv("PC_LIQUIFY_PROXY_SNAPSHOT");
    if (snap != NULL && rgba != NULL && w > 0 && h > 0) {
        pc_cimage img;
        if (pc_cimage_from_copy((uint32_t)w, (uint32_t)h, PC_CL_RGBA, PC_CS_U8, rgba,
                                (size_t)w * (size_t)h * 4, &img, &err) == PC_OK) {
            pc_buf out_png = PC_BUF_INIT;
            if (pc_cencode(&img, PC_CF_PNG, NULL, &out_png, &err) == PC_OK) {
                (void)pc_file_write_atomic(snap, out_png.data, out_png.len, &err);
            }
            pc_buf_fini(&out_png);
            pc_cimage_fini(&img);
        }
    }
    free(rgba);
    CHECK(pc_ui_shell_app_invoke(a, "ui.warp.commit", NULL, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

/* U7 render smoke: the retouch/gradient options bars, the Gradient Editor window and the Fill dialog
 * all render offscreen without crashing (Rule 6). */
static void test_gl_u7_surfaces(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "white", pc_size_make(128, 96), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(1, 1, 1), &err) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, &err) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
    const char *tools[] = {"Gradient",        "CloneStamp", "Healing", "PatternStamp", "SpotHealing",
                           "Patch",           "ContentAwareMove", "RedEye",  "Dodge",        "Burn",
                           "Sponge",          "Blur",       "Sharpen", "Smudge",       "ColorReplacement"};
    for (size_t i = 0; i < sizeof tools / sizeof *tools; i++) {
        char p[64];
        (void)snprintf(p, sizeof p, "{\"tool\":\"%s\"}", tools[i]);
        CHECK(call(a, "ui.set", p, NULL) == PC_OK);
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        pc_status rc = pc_ui_shell_app_capture(a, &rgba, &w, &h, &err);
        if (rc != PC_OK) {
            PC_T_FAIL("capture %s: %s", tools[i], err.msg);
        }
        CHECK(rc == PC_OK);
        free(rgba);
    }
    /* Gradient Editor window renders. */
    CHECK(call(a, "ui.set", "{\"tool\":\"Gradient\",\"panels\":{\"gradient_editor\":true}}", NULL) == PC_OK);
    CHECK(pc_ui_shell_app_state(a)->panels.gradient_editor);
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    /* Fill dialog opens and renders. */
    CHECK(call(a, "ui.menu.invoke", "{\"command\":\"edit.fill\"}", NULL) == PC_OK);
    CHECK(pc_ui_shell_app_state(a)->n_dialogs >= 1);
    rgba = NULL;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

/* U5 GL render smoke: the preset browser (Gradients/Patterns dock panels in grid and list mode,
 * folders, an inline rename field) and the floating preset windows (Styles, Shapes, Tool Presets,
 * Clone Source) all render offscreen without crashing (Rule 6). With PC_PRESET_SNAPSHOT=<dir> set,
 * writes PNGs there to eyeball. */
static void snapshot_to(pc_ui_shell_app *a, const char *name)
{
    const char *dir = getenv("PC_PRESET_SNAPSHOT");
    if (!dir) {
        return;
    }
    char req[1024];
    (void)snprintf(req, sizeof req, "{\"path\":\"%s/%s.png\"}", dir, name);
    CHECK(call(a, "ui.screenshot", req, NULL) == PC_OK);
}
static void test_gl_preset_browser(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "white", pc_size_make(160, 120), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(1, 1, 1), &err) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, &err) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);

    uint8_t *rgba = NULL;
    int w = 0, h = 0;

    /* Gradients dock panel (grid mode). */
    CHECK(call(a, "ui.menu.invoke", "{\"command\":\"window.panel.gradients\"}", NULL) == PC_OK);
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    snapshot_to(a, "preset_gradients_grid");

    /* List mode (slider at the minimum). */
    pc_ui_preset_ui_set_size(&pc_ui_shell_app_state(a)->presets_ui, "gradients", PC_UI_PRESET_MIN_THUMB);
    rgba = NULL;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    snapshot_to(a, "preset_gradients_list");

    /* An inline rename field over the first preset. */
    pc_ui_preset_ui_set_rename(&pc_ui_shell_app_state(a)->presets_ui, "gradients", "Custom", "Custom");
    rgba = NULL;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    snapshot_to(a, "preset_gradients_rename");
    pc_ui_preset_ui_clear_rename(&pc_ui_shell_app_state(a)->presets_ui);

    /* Patterns dock panel (pixel thumbnails). */
    CHECK(call(a, "ui.menu.invoke", "{\"command\":\"window.panel.patterns\"}", NULL) == PC_OK);
    rgba = NULL;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    snapshot_to(a, "preset_patterns");

    /* The four floating preset windows, all open at once (toggling via the Window menu is covered
     * by test_preset_float_panels_toggle; here the flags are set directly to render the bodies). */
    pc_ui_preset_ui *pu = &pc_ui_shell_app_state(a)->presets_ui;
    pu->styles = pu->shapes = pu->tool_presets = pu->clone_source = true;
    CHECK(pc_ui_shell_app_state(a)->presets_ui.styles);
    rgba = NULL;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    snapshot_to(a, "preset_float_windows");
    /* A second frame so the windows settle at their requested bounds. */
    rgba = NULL;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    snapshot_to(a, "preset_float_windows_2");

    pc_ui_shell_app_free(a);
    SDL_Quit();
}

/* GL render smoke: the bespoke Brush Settings panes (Brush Tip Shape with its tip grid and
 * angle/roundness widget, Texture, Dual Brush) all render offscreen without crashing (Rule 6). */
static void test_gl_brush_tip_panes(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "white", pc_size_make(64, 64), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(1, 1, 1), &err) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, &err) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              pc_ui_shell_app_session(a), "tools.setBrush",
              "{\"brush\":{\"texture\":{\"enabled\":true,\"eachTip\":true},\"dualBrush\":{\"enabled\":true}}}",
              &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    ui->panels.brush_settings = true;
    ui->brush_tab = 0;
    for (size_t section = 0; section <= 4; section++) {
        ui->brush_section = section;
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        /* Two frames: the first opens the window disabled, the second interacts live. */
        for (int frame = 0; frame < 2; frame++) {
            pc_status rc = pc_ui_shell_app_capture(a, &rgba, &w, &h, &err);
            if (rc != PC_OK) {
                PC_T_FAIL("capture brush section %zu: %s", section, err.msg);
            }
            CHECK(rc == PC_OK);
            free(rgba);
            rgba = NULL;
        }
    }
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

/* B8: the Keyboard Shortcuts / Menus editor opens as a modal, its draft commits an override and a
 * hidden menu item, and invoking the command with real params bypasses the modal (goes to engine). */
static void test_shortcuts_editor_opens_edits_and_confirms(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    /* Keyboard Shortcuts (tab 0). */
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"edit.keyboardShortcuts\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    uint64_t id = ui->dialogs[0].id;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "__prefsui")), "shortcuts");
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "tab")), 0u);
    char request[512];
    (void)snprintf(request, sizeof request,
                   "{\"dialog\":%llu,\"field\":\"overrides\",\"value\":{\"view.zoomIn\":\"Cmd+Alt+0\"}}",
                   (unsigned long long)id);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_OK);
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.confirm", request, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    pc_buf result = PC_BUF_INIT;
    CHECK(call(app, "engine.execute", "{\"id\":\"prefs.get\",\"params\":{\"path\":\"shortcuts\"}}", &result) ==
          PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&result), "view.zoomIn");
    pc_buf_fini(&result);
    /* Menus (tab 1): hide a menu item. */
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"edit.menus\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    id = ui->dialogs[0].id;
    root = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "tab")), 1u);
    (void)snprintf(request, sizeof request,
                   "{\"dialog\":%llu,\"field\":\"hidden\",\"value\":[\"image.imageSize\"]}",
                   (unsigned long long)id);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_OK);
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.confirm", request, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    result = (pc_buf)PC_BUF_INIT;
    CHECK(call(app, "engine.execute", "{\"id\":\"prefs.get\",\"params\":{\"path\":\"menus.hidden\"}}",
               &result) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&result), "image.imageSize");
    pc_buf_fini(&result);
    /* With real params the command runs through the engine, not the modal. */
    CHECK(call(app, "ui.menu.invoke",
               "{\"id\":\"edit.keyboardShortcuts\",\"params\":{\"reset\":true}}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    pc_ui_shell_app_free(app);
}

/* Rule 6: the shortcuts editor (all three tabs) and the Interface theme cards render offscreen. */
static void test_gl_prefs_and_shortcuts_surfaces(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"edit.keyboardShortcuts\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    uint64_t id = ui->dialogs[0].id;
    for (int tab = 0; tab < 3; tab++) {
        char request[128];
        (void)snprintf(request, sizeof request, "{\"dialog\":%llu,\"field\":\"tab\",\"value\":%d}",
                       (unsigned long long)id, tab);
        CHECK(call(a, "ui.dialog.set", request, NULL) == PC_OK);
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        for (int frame = 0; frame < 2; frame++) {
            CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
            free(rgba);
            rgba = NULL;
        }
    }
    char request[128];
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(a, "ui.dialog.cancel", request, NULL) == PC_OK);
    /* Interface theme cards. */
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"edit.preferences.interface\"}", NULL) == PC_OK);
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    for (int frame = 0; frame < 2; frame++) {
        CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
        free(rgba);
        rgba = NULL;
    }
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

/* The Preset Manager opens from its menu id, lists a kind, and OK dismisses it; with real params the
 * command still runs through the engine. */
static void test_preset_manager_opens_and_lists(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"edit.presets.presetManager\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    uint64_t id = ui->dialogs[0].id;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "__presetui")), "manager");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "kind")), "brushes");
    CHECK(pc_ui_shell_preset_manager_owns(root));
    /* The engine lists every library (what the body renders each frame). */
    pc_buf result = PC_BUF_INIT;
    CHECK(call(app, "engine.execute", "{\"id\":\"edit.presets.presetManager\"}", &result) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&result), "patterns");
    CHECK_CONTAINS(pc_buf_cstr(&result), "brushes");
    pc_buf_fini(&result);
    /* OK just dismisses the manager. */
    char request[128];
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.confirm", request, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    /* With real params the command runs through the engine, not the modal. */
    CHECK(call(app, "ui.menu.invoke",
               "{\"id\":\"edit.presets.presetManager\",\"params\":{\"kind\":\"brushes\"}}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    pc_ui_shell_app_free(app);
}

/* Export/Import Presets: the dialog's begin/write/read round-trip a preset file without a chooser. */
static void test_presets_io_round_trip(void)
{
    char path[] = "/dev/shm/pc-build/test-presets-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        close(fd);
    }
    /* Export from one session. */
    pc_ui_shell_app *app = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&app, &err) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_presets_io_open(app, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ui->n_dialogs == 1);
    pc_ui_dialog *dialog = &ui->dialogs[0];
    CHECK(pc_ui_shell_presets_io_owns(yyjson_mut_doc_get_root(dialog->fields)));
    bool saving = false;
    CHECK(pc_ui_shell_presets_io_begin(app, dialog, &saving, &err) == PC_OK);
    CHECK(saving); /* export direction by default */
    yyjson_mut_val *droot = yyjson_mut_doc_get_root(dialog->fields);
    const char *data = yyjson_mut_get_str(yyjson_mut_obj_get(droot, "__exportData"));
    CHECK(data != NULL);
    CHECK(data && strstr(data, "photocraft-presets") != NULL);
    CHECK(pc_ui_shell_presets_io_write(app, dialog, path, &err) == PC_OK);
    pc_ui_shell_app_free(app);

    /* The file holds a valid preset document. */
    pc_buf bytes = PC_BUF_INIT;
    CHECK(pc_file_read(path, 1u << 24, &bytes, &err) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&bytes), "photocraft-presets");
    pc_buf_fini(&bytes);

    /* Import into a fresh session via the dialog's read path. */
    pc_ui_shell_app *app2 = NULL;
    CHECK(pc_ui_shell_app_new(&app2, &err) == PC_OK);
    pc_ui_state *ui2 = pc_ui_shell_app_state(app2);
    out = (pc_json_value)PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_presets_io_open(app2, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_dialog *dialog2 = &ui2->dialogs[0];
    /* The Open-chooser result path: read the file and import it. */
    CHECK(pc_ui_shell_presets_io_read(app2, dialog2, path, &err) == PC_OK);
    pc_ui_shell_app_free(app2);
    remove(path);

    /* Bad path is an error, not a crash. */
    pc_ui_shell_app *app3 = NULL;
    CHECK(pc_ui_shell_app_new(&app3, &err) == PC_OK);
    pc_ui_state *ui3 = pc_ui_shell_app_state(app3);
    out = (pc_json_value)PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_presets_io_open(app3, &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_presets_io_read(app3, &ui3->dialogs[0], "/no/such/preset/file", &err) != PC_OK);
    pc_ui_shell_app_free(app3);
}

/* The profile-mismatch prompt opens from a synthetic report and OK dismisses it when the chosen
 * action matches what the open policy already applied (no engine edit, so no document needed). */
static void test_profile_mismatch_prompt(void)
{
    pc_ui_shell_app *app = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&app, &err) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    /* A "kept" report that does not ask -> no prompt. */
    yyjson_doc *q = NULL;
    const char *kept = "{\"action\":\"kept\",\"ask\":false}";
    CHECK(pc_json_parse(kept, strlen(kept), &q, &err) == PC_OK);
    yyjson_mut_doc *qm = yyjson_doc_mut_copy(q, NULL);
    CHECK(pc_ui_shell_profile_mismatch_maybe_open(app, yyjson_mut_doc_get_root(qm), &err) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    yyjson_mut_doc_free(qm);
    yyjson_doc_free(q);
    /* A real mismatch that asks -> a prompt whose initial action is what was applied. */
    const char *text = "{\"mismatch\":true,\"ask\":true,\"embedded\":\"sRGB IEC61966-2.1\","
                       "\"working\":\"Adobe RGB (1998)\",\"action\":\"kept\",\"policy\":\"preserve\"}";
    CHECK(pc_json_parse(text, strlen(text), &q, &err) == PC_OK);
    qm = yyjson_doc_mut_copy(q, NULL);
    CHECK(pc_ui_shell_profile_mismatch_maybe_open(app, yyjson_mut_doc_get_root(qm), &err) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    uint64_t id = ui->dialogs[0].id;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK(pc_ui_shell_profile_mismatch_owns(root));
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "action")), "preserve");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "applied")), "preserve");
    CHECK(yyjson_mut_get_str(yyjson_mut_obj_get(root, "message")) != NULL);
    yyjson_mut_doc_free(qm);
    yyjson_doc_free(q);
    /* OK with the already-applied action is a no-op that closes the prompt. */
    char request[128];
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    pc_buf result = PC_BUF_INIT;
    CHECK(call(app, "ui.dialog.confirm", request, &result) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&result), "preserve");
    pc_buf_fini(&result);
    CHECK(ui->n_dialogs == 0);
    pc_ui_shell_app_free(app);
}

/* Rule 6: the Preset Manager, Export/Import Presets and Embedded Profile Mismatch dialogs all render
 * offscreen without crashing. */
static void test_gl_preset_and_mismatch_surfaces(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    /* Preset Manager, cycling the kind so each list body renders. */
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"edit.presets.presetManager\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    uint64_t id = ui->dialogs[0].id;
    const char *kinds[] = {"brushes", "customShapes", "patterns"};
    for (size_t k = 0; k < 3; k++) {
        char request[128];
        (void)snprintf(request, sizeof request, "{\"dialog\":%llu,\"field\":\"kind\",\"value\":\"%s\"}",
                       (unsigned long long)id, kinds[k]);
        CHECK(call(a, "ui.dialog.set", request, NULL) == PC_OK);
        for (int frame = 0; frame < 2; frame++) {
            CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
            free(rgba);
            rgba = NULL;
        }
    }
    char request[128];
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(a, "ui.dialog.cancel", request, NULL) == PC_OK);
    /* Export/Import Presets. */
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"edit.presets.exportImportPresets\"}", NULL) == PC_OK);
    for (int frame = 0; frame < 2; frame++) {
        CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
        free(rgba);
        rgba = NULL;
    }
    id = ui->dialogs[ui->n_dialogs - 1].id;
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(a, "ui.dialog.cancel", request, NULL) == PC_OK);
    /* Embedded Profile Mismatch (both the embedded and missing layouts). */
    const char *reports[] = {
        "{\"mismatch\":true,\"ask\":true,\"embedded\":\"sRGB\",\"working\":\"Adobe RGB\",\"action\":\"kept\"}",
        "{\"missing\":true,\"ask\":true,\"working\":\"sRGB\",\"action\":\"assigned\"}"};
    for (size_t i = 0; i < 2; i++) {
        yyjson_doc *q = NULL;
        CHECK(pc_json_parse(reports[i], strlen(reports[i]), &q, &err) == PC_OK);
        yyjson_mut_doc *qm = yyjson_doc_mut_copy(q, NULL);
        CHECK(pc_ui_shell_profile_mismatch_maybe_open(a, yyjson_mut_doc_get_root(qm), &err) == PC_OK);
        CHECK(ui->n_dialogs == 1);
        for (int frame = 0; frame < 2; frame++) {
            CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
            free(rgba);
            rgba = NULL;
        }
        id = ui->dialogs[0].id;
        (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
        CHECK(call(a, "ui.dialog.cancel", request, NULL) == PC_OK);
        yyjson_mut_doc_free(qm);
        yyjson_doc_free(q);
    }
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

static void test_view_action_record_and_play(void)
{
    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, NULL) == PC_OK);
    CHECK(call(a, "engine.execute", "{\"id\":\"file.new\",\"params\":{\"width\":200,\"height\":200}}",
               NULL) == PC_OK);
    CHECK(call(a, "engine.execute", "{\"id\":\"actions.record\",\"params\":{\"name\":\"View\"}}", NULL) ==
          PC_OK);
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.actualPixels\"}", NULL) == PC_OK);
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"view.zoomIn\"}", NULL) == PC_OK);
    CHECK(call(a, "engine.execute", "{\"id\":\"actions.stop\"}", NULL) == PC_OK);
    CHECK_NEAR(pc_ui_shell_app_state(a)->views[0].zoom, 2, 0);
    pc_ui_shell_app_state(a)->views[0].zoom = 8;
    pc_buf result = PC_BUF_INIT;
    CHECK(call(a, "engine.execute", "{\"id\":\"actions.play\",\"params\":{\"action\":\"View\"}}", &result) ==
          PC_OK);
    CHECK_NEAR(pc_ui_shell_app_state(a)->views[0].zoom, 2, 0);
    CHECK(result.len && strstr((const char *)result.data, "\"ran\":2") != NULL);
    CHECK(strstr((const char *)result.data, "failed") == NULL);
    pc_buf_fini(&result);
    pc_ui_shell_app_free(a);
}

static void test_tool_options_control_and_atomic_patches(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "ui.set",
               "{\"tool\":\"Triangle\",\"shapeStroke\":{\"width\":5,\"cap\":\"round\",\"join\":\"bevel\","
               "\"dashes\":[0,2,4,1],\"dashOffset\":-0.5}}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->tool == PC_TOOL_TRIANGLE && ui->tool_options.stroke_width == 5);
    CHECK(ui->tool_options.shape_stroke.cap == PC_LINE_CAP_ROUND &&
          ui->tool_options.shape_stroke.ndashes == 4);
    CHECK(call(app, "ui.set",
               "{\"tool\":\"gradient\",\"gradientBlendMode\":\"difference\",\"gradientClassic\":true}",
               NULL) == PC_OK);
    CHECK(ui->tool_options.gradient_blend_mode == PC_BLEND_DIFFERENCE && ui->tool_options.gradient_classic);
    CHECK(
        call(
            app, "ui.set",
            "{\"eyedropperSampleSize\":11,\"eyedropperSample\":\"currentAndBelow\",\"eyedropperRing\":false}",
            NULL) == PC_OK);
    CHECK(ui->tool_options.eyedropper_size == 11 && !ui->tool_options.eyedropper_ring);
    CHECK(call(app, "ui.set",
               "{\"cropOverlay\":\"goldenSpiral\",\"cropOverlayShow\":\"always\",\"cropOverlayOrientation\":"
               "2,\"cropShield\":{\"opacity\":40,\"color\":\"custom\",\"custom_color\":[255,0,0]}}",
               NULL) == PC_OK);
    CHECK(ui->tool_options.crop_overlay == PC_UI_CROP_GOLDEN_SPIRAL &&
          ui->tool_options.crop_shield.opacity == 40);
    CHECK(call(app, "ui.set",
               "{\"cropShield\":{\"show_cropped_area\":false},\"dockTabs\":{\"layers\":1.0},\"colorPanel\":{"
               "\"background\":true},\"brushSection\":3,\"brushTab\":1,\"panels\":{\"dock\":false}}",
               NULL) == PC_OK);
    CHECK(!ui->tool_options.crop_shield.show_cropped_area && ui->tool_options.crop_shield.opacity == 40);
    CHECK(ui->dock_tabs.layers == 1 && ui->color_panel.background && ui->brush_section == 3 &&
          ui->brush_tab == 1 && !ui->panels.dock);
    pc_buf before = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", NULL, &before) == PC_OK);
    const char *bad[] = {"{\"tool\":\"Move\",\"shapeStroke\":{\"dashes\":[0,0]}}",
                         "{\"shapeStroke\":{\"colour\":\"#ff0000\"}}",
                         "{\"gradientBlendMode\":\"nonsense\",\"gradientClassic\":false}",
                         "{\"eyedropperSampleSize\":4}",
                         "{\"eyedropperSampleSize\":101,\"eyedropperSample\":\"nope\"}",
                         "{\"cropOverlay\":\"grid\",\"cropOverlayShow\":\"nope\"}",
                         "{\"cropOverlayOrientation\":4}",
                         "{\"cropShield\":{\"opacity\":101}}",
                         "{\"cropShield\":{\"shield\":true}}",
                         "{\"panels\":{\"layers\":false,\"dock\":1}}",
                         "{\"tool\":\"Move\",\"colorPanel\":{\"nope\":true}}",
                         "{\"maskTarget\":1}",
                         "{\"dockTabs\":{\"layers\":-1}}",
                         "{\"brushSection\":1e100}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        CHECK(call(app, "ui.set", bad[i], NULL) != PC_OK);
        pc_buf after = PC_BUF_INIT;
        CHECK(call(app, "ui.inspect", NULL, &after) == PC_OK);
        CHECK_EQ_STR(pc_buf_cstr(&before), pc_buf_cstr(&after));
        pc_buf_fini(&after);
    }
    pc_buf_fini(&before);
    CHECK(call(app, "ui.set", "{\"shapeStroke\":null,\"eyedropperSampleSize\":\"point\"}", NULL) == PC_OK);
    CHECK(ui->tool_options.stroke_width == 0 && ui->tool_options.eyedropper_size == 1);
    pc_ui_shell_app_free(app);
}

static void test_dialog_control_and_menu_modality(void)
{
    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, NULL) == PC_OK);
    CHECK(call(a, "ui.dialog.open",
               "{\"kind\":\"newDocument\",\"fields\":{\"width\":40,\"height\":30,\"mode\":\"lab\",\"depth\":"
               "16}}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    CHECK(ui->n_dialogs == 1);
    uint64_t id = ui->dialogs[0].id;
    char params[256];
    (void)snprintf(params, sizeof params, "{\"dialog\":%llu,\"field\":\"name\",\"value\":\"Modal document\"}",
                   (unsigned long long)id);
    CHECK(call(a, "ui.dialog.set", params, NULL) == PC_OK);
    CHECK(call(a, "ui.menu.invoke", "{\"id\":\"file.new\"}", NULL) != PC_OK);
    (void)snprintf(params, sizeof params, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(a, "ui.dialog.confirm", params, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(a))->doc);
    CHECK(doc->size.width == 40 && doc->size.height == 30);
    CHECK(call(a, "ui.dialog.open",
               "{\"kind\":\"colorPicker\",\"target\":\"background\",\"fields\":{\"color\":\"#12ab56\"}}",
               NULL) == PC_OK);
    id = ui->dialogs[0].id;
    (void)snprintf(params, sizeof params, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(a, "ui.dialog.confirm", params, NULL) == PC_OK);
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(a));
    CHECK_NEAR(tools->background[0], 18.0 / 255, 0.0001);
    CHECK_NEAR(tools->background[1], 171.0 / 255, 0.0001);
    CHECK(call(a, "ui.dialog.open", "{\"kind\":\"colorPicker\",\"fields\":{\"color\":\"bad hex\"}}", NULL) ==
          PC_OK);
    id = ui->dialogs[0].id;
    (void)snprintf(params, sizeof params, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(a, "ui.dialog.confirm", params, NULL) == PC_ERR_INVALID);
    CHECK(ui->n_dialogs == 1);
    CHECK(call(a, "ui.dialog.cancel", params, NULL) == PC_OK);
    CHECK(call(a, "ui.dialog.confirm", params, NULL) == PC_ERR_NOT_FOUND);
    CHECK(call(a, "ui.dialog.open", "{\"kind\":\"command\"}", NULL) == PC_ERR_INVALID);
    pc_ui_shell_app_free(a);
}

static void test_file_dialog_paths_and_failed_retry(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"id\":\"file.new\",\"params\":{\"width\":24,\"height\":20}}",
               NULL) == PC_OK);
    char directory[] = "/tmp/photocraft-dialog-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256];
    (void)snprintf(path, sizeof path, "%s/document.pcraft", directory);
    char request[512];
    (void)snprintf(request, sizeof request, "{\"kind\":\"fileSave\",\"fields\":{\"path\":\"%s\"}}", path);
    CHECK(call(app, "ui.dialog.open", request, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    uint64_t id = ui->dialogs[0].id;
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.confirm", request, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0 && access(path, F_OK) == 0);
    CHECK_EQ_STR(pc_engine_session_active(pc_ui_shell_app_session(app))->path, path);
    CHECK(call(app, "ui.dialog.open",
               "{\"kind\":\"fileOpen\",\"fields\":{\"path\":\"/this/does/not/exist.pcraft\"}}",
               NULL) == PC_OK);
    id = ui->dialogs[0].id;
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.confirm", request, NULL) != PC_OK);
    CHECK(ui->n_dialogs == 1);
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu,\"field\":\"path\",\"value\":\"%s\"}",
                   (unsigned long long)id, path);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_OK);
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.confirm", request, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    size_t count = 0;
    (void)pc_engine_session_documents(pc_ui_shell_app_session(app), &count);
    CHECK(count == 2);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    pc_ui_shell_app_free(app);
}

static void test_preferences_draft_apply_and_cancel(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"edit.preferences.interface\"}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->n_dialogs == 1);
    uint64_t id = ui->dialogs[0].id;
    char request[512];
    (void)snprintf(request, sizeof request,
                   "{\"dialog\":%llu,\"field\":\"values\",\"value\":{\"interface\":{\"theme\":\"studio\"}}}",
                   (unsigned long long)id);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_OK);
    CHECK(ui->theme == PC_UI_THEME_PRO_MEDIUM);
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.apply", request, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1 && ui->theme == PC_UI_THEME_STUDIO);
    (void)snprintf(
        request, sizeof request,
        "{\"dialog\":%llu,\"field\":\"values\",\"value\":{\"interface\":{\"theme\":\"no such theme\"}}}",
        (unsigned long long)id);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_OK);
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.confirm", request, NULL) != PC_OK);
    CHECK(ui->n_dialogs == 1 && ui->theme == PC_UI_THEME_STUDIO);
    CHECK(call(app, "ui.dialog.cancel", request, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0 && ui->theme == PC_UI_THEME_STUDIO);
    CHECK(call(app, "ui.dialog.open", "{\"kind\":\"preferences\",\"section\":\"performance\"}", NULL) ==
          PC_OK);
    id = ui->dialogs[0].id;
    (void)snprintf(
        request, sizeof request,
        "{\"dialog\":%llu,\"field\":\"values\",\"value\":{\"performance\":{\"historyStates\":12}}}",
        (unsigned long long)id);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_OK);
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.confirm", request, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    pc_buf result = PC_BUF_INIT;
    CHECK(call(app, "engine.execute",
               "{\"id\":\"prefs.get\",\"params\":{\"path\":\"performance.historyStates\"}}",
               &result) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&result), "12");
    pc_buf_fini(&result);
    CHECK(call(app, "ui.dialog.open", "{\"kind\":\"preferences\",\"section\":\"bogus\"}", NULL) ==
          PC_ERR_INVALID);
    CHECK(call(app, "ui.dialog.open", "{\"kind\":\"about\"}", NULL) == PC_OK);
    id = ui->dialogs[0].id;
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.apply", request, NULL) == PC_ERR_INVALID);
    pc_ui_shell_app_free(app);
}

static void dialog_field(pc_ui_shell_app *app, uint64_t id, const char *field, const char *value)
{
    char request[1024];
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu,\"field\":\"%s\",\"value\":\"%s\"}",
                   (unsigned long long)id, field, value);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_OK);
}
static pc_status answer_dialog(pc_ui_shell_app *app, uint64_t id, bool confirm)
{
    char request[128];
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)id);
    return call(app, confirm ? "ui.dialog.confirm" : "ui.dialog.cancel", request, NULL);
}
static void dirty_document(pc_ui_shell_app *app)
{
    CHECK(call(app, "engine.execute", "{\"id\":\"file.new\",\"params\":{\"width\":12,\"height\":12}}",
               NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"id\":\"layer.new.layer\"}", NULL) == PC_OK);
    CHECK(pc_engine_doc_state_is_dirty(pc_engine_session_active(pc_ui_shell_app_session(app))));
}
static void test_discard_stable_document_cancel_and_close_all(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    dirty_document(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_doc_id original = pc_doc_arc_get(pc_engine_session_active(session)->doc)->id;
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"file.close\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    uint64_t id = ui->dialogs[0].id;
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"file.close\"}", NULL) == PC_OK && ui->n_dialogs == 1 &&
          ui->dialogs[0].id == id);
    CHECK(answer_dialog(app, id, false) == PC_OK);
    CHECK(pc_engine_doc_state_is_dirty(pc_engine_session_active(session)));
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"file.close\"}", NULL) == PC_OK);
    id = ui->dialogs[0].id;
    dirty_document(app);
    pc_doc_id other = pc_doc_arc_get(pc_engine_session_active(session)->doc)->id;
    CHECK(other != original);
    dialog_field(app, id, "choice", "discard");
    CHECK(answer_dialog(app, id, true) == PC_OK);
    size_t count = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(session, &count);
    CHECK(count == 1 && pc_doc_arc_get(docs[0].doc)->id == other && ui->n_dialogs == 0);
    dirty_document(app);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"file.closeAll\"}", NULL) == PC_OK);
    id = ui->dialogs[0].id;
    dialog_field(app, id, "choice", "discard");
    CHECK(answer_dialog(app, id, true) == PC_OK);
    (void)pc_engine_session_documents(session, &count);
    CHECK(count == 1 && ui->n_dialogs == 1);
    CHECK(answer_dialog(app, id, false) == PC_OK);
    (void)pc_engine_session_documents(session, &count);
    CHECK(count == 1);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"file.close\"}", NULL) == PC_OK);
    id = ui->dialogs[0].id;
    CHECK(call(app, "app.quit", NULL, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1 && ui->dialogs[0].id != id && !pc_ui_shell_exit_requested(app));
    id = ui->dialogs[0].id;
    dialog_field(app, id, "choice", "discard");
    CHECK(answer_dialog(app, id, true) == PC_OK);
    CHECK(pc_ui_shell_exit_requested(app) && ui->n_dialogs == 0);
    pc_ui_shell_app_free(app);
}
static void test_discard_save_child_failure_and_success(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    dirty_document(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"file.close\"}", NULL) == PC_OK);
    uint64_t parent = ui->dialogs[0].id;
    CHECK(answer_dialog(app, parent, true) == PC_OK && ui->n_dialogs == 2);
    uint64_t child = ui->dialogs[1].id;
    CHECK(answer_dialog(app, parent, true) == PC_ERR_INVALID && ui->n_dialogs == 2);
    CHECK(answer_dialog(app, child, false) == PC_OK && ui->n_dialogs == 1);
    dialog_field(app, parent, "path", "/this/does/not/exist/document.pcraft");
    CHECK(answer_dialog(app, parent, true) != PC_OK && ui->n_dialogs == 1);
    char directory[] = "/tmp/photocraft-discard-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256];
    (void)snprintf(path, sizeof path, "%s/document.pcraft", directory);
    dialog_field(app, parent, "path", path);
    CHECK(answer_dialog(app, parent, true) == PC_OK && !ui->n_dialogs);
    size_t count = 0;
    (void)pc_engine_session_documents(pc_ui_shell_app_session(app), &count);
    CHECK(count == 0 && access(path, F_OK) == 0);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    pc_ui_shell_app_free(app);
}
static void test_dock_control_validation_and_failed_command_draft(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(call(app, "ui.set",
               "{\"dockWidth\":800,\"tileGrid\":true,\"dock\":{\"order\":[\"layers\",\"color\"],"
               "\"collapsed\":[\"color\"],\"tabOrder\":{\"layers\":[\"Channels\",\"Layers\"]}}}",
               NULL) == PC_OK);
    CHECK(ui->dock_width == 520 && ui->view.tile_grid &&
          pc_ui_dock_is_collapsed(&ui->dock, PC_UI_GROUP_COLOR));
    pc_buf before = PC_BUF_INIT, after = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", NULL, &before) == PC_OK);
    CHECK(call(app, "ui.set", "{\"dockWidth\":260,\"dock\":{},\"tileGrid\":1}", NULL) == PC_ERR_INVALID);
    CHECK(call(app, "ui.inspect", NULL, &after) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&before), pc_buf_cstr(&after));
    pc_buf_fini(&before);
    pc_buf_fini(&after);
    dirty_document(app);
    CHECK(call(app, "ui.dialog.open", "{\"kind\":\"command\",\"command\":\"layer.renameLayer\"}", NULL) ==
          PC_OK);
    uint64_t id = ui->dialogs[0].id;
    CHECK(answer_dialog(app, id, true) != PC_OK && ui->n_dialogs == 1);
    dialog_field(app, id, "name", "Retained draft");
    CHECK(answer_dialog(app, id, true) == PC_OK && !ui->n_dialogs);
    pc_ui_shell_app_free(app);
}

static void test_file_save_dialog_retains_document_identity(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    dirty_document(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_doc_id target = pc_doc_arc_get(pc_engine_session_active(session)->doc)->id;
    CHECK(call(app, "ui.dialog.open", "{\"kind\":\"fileSave\"}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    uint64_t id = ui->dialogs[0].id;
    dirty_document(app);
    pc_doc_id other = pc_doc_arc_get(pc_engine_session_active(session)->doc)->id;
    char directory[] = "/tmp/photocraft-save-target-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256];
    (void)snprintf(path, sizeof path, "%s/document.pcraft", directory);
    dialog_field(app, id, "path", path);
    char request[256];
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu,\"field\":\"__document\",\"value\":%llu}",
                   (unsigned long long)id, (unsigned long long)other);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_ERR_INVALID);
    CHECK(answer_dialog(app, id, true) == PC_OK);
    size_t count = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(session, &count);
    CHECK(count == 2 && pc_doc_arc_get(docs[0].doc)->id == target && docs[0].path && !docs[1].path);
    CHECK(!pc_engine_doc_state_is_dirty(&docs[0]) && pc_engine_doc_state_is_dirty(&docs[1]));
    CHECK(unlink(path) == 0);
    CHECK(rmdir(directory) == 0);
    pc_ui_shell_app_free(app);
}

static void test_type_key_control_and_atomic_validation(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"id\":\"file.new\",\"params\":{\"width\":400,\"height\":300}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"tool\":\"Type\",\"events\":[{\"kind\":\"down\",\"x\":30,\"y\":60},{\"kind\":\"up\",\"x\":"
               "30,\"y\":60}]}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->has_text_edit);
    CHECK(call(app, "ui.type", "{\"text\":\"Hello Ω\"}", NULL) == PC_OK);
    size_t caret = ui->text_edit.caret;
    CHECK(call(app, "ui.key", "{\"key\":\"ArrowLeft\",\"modifiers\":{\"shift\":true,\"ctrl\":\"bad\"}}",
               NULL) == PC_ERR_INVALID);
    CHECK(ui->text_edit.caret == caret && ui->text_edit.anchor == caret);
    CHECK(call(app, "ui.key", "{\"key\":\"ArrowLeft\",\"shift\":true}", NULL) == PC_OK);
    CHECK(call(app, "ui.type", "{\"text\":\"!\"}", NULL) == PC_OK);
    pc_layer_id id = ui->text_edit.layer;
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(app))->doc);
    CHECK_EQ_STR(pc_str_c(&pc_document_layer(doc, id)->content.u.text.text), "Hello !");
    pc_buf inspect = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", NULL, &inspect) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&inspect), "\"textEdit\":{");
    pc_buf_fini(&inspect);
    CHECK(call(app, "ui.key", "{\"key\":\"Enter\",\"command\":true}", NULL) == PC_OK);
    CHECK(!ui->has_text_edit);
    CHECK(call(app, "ui.key", "{\"key\":\"NoSuchKey\"}", NULL) == PC_ERR_INVALID);
    CHECK(call(app, "ui.type", "{\"text\":\"no target\"}", NULL) == PC_ERR_UNSUPPORTED);
    pc_ui_shell_app_free(app);
}
static void test_type_color_picker_scope_cancel_and_stale_guard(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":100,\"height\":100}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"tool\":\"Type\",\"events\":[{\"kind\":\"down\",\"x\":10,\"y\":30},{\"kind\":\"up\",\"x\":"
               "10,\"y\":30}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.type", "{\"text\":\"Color\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.key", "{\"key\":\"A\",\"command\":true}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_layer_id id = ui->text_edit.layer;
    uint64_t revision = pc_engine_session_active(session)->revision;
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.type.color\"}", NULL) == PC_OK && ui->n_dialogs == 1);
    char request[256];
    (void)snprintf(request, sizeof request,
                   "{\"dialog\":%llu,\"field\":\"__colorCommand\",\"value\":\"layer.delete\"}",
                   (unsigned long long)ui->dialogs[0].id);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_ERR_INVALID);
    CHECK(answer_dialog(app, ui->dialogs[ui->n_dialogs - 1].id, false) == PC_OK &&
          pc_engine_session_active(session)->revision == revision);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.type.color\"}", NULL) == PC_OK);
    dialog_field(app, ui->dialogs[0].id, "color", "#ff0000");
    CHECK(answer_dialog(app, ui->dialogs[ui->n_dialogs - 1].id, true) == PC_OK && ui->n_dialogs == 0);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(session)->doc);
    float rgb[3];
    pc_color_to_rgb(&pc_document_layer(doc, id)->content.u.text.color, rgb);
    CHECK(rgb[0] > .99f && rgb[1] < .01f);
    const pc_engine_tool_state *tools = pc_engine_session_tools(session);
    CHECK(tools->foreground[0] == 0 && tools->foreground[1] == 0);
    CHECK(call(app, "ui.type", "{\"text\":\"Red\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.type.color\"}", NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"layer.new.layer\"}", NULL) == PC_OK);
    CHECK(answer_dialog(app, ui->dialogs[ui->n_dialogs - 1].id, true) == PC_ERR_INVALID &&
          ui->n_dialogs == 1);
    CHECK(answer_dialog(app, ui->dialogs[ui->n_dialogs - 1].id, false) == PC_OK);
    pc_ui_shell_app_free(app);
}
static void test_character_style_color_and_foreground_text_scope(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":200,\"height\":100}}",
               NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"command\":\"type.create\",\"params\":{\"x\":10,\"y\":30,\"text\":\"Styled\"}}",
               NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"command\":\"type.characterStyle.new\",\"params\":{\"name\":\"Heading\",\"apply\":true}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.type.styleColor\",\"params\":{\"id\":1}}", NULL) ==
          PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->n_dialogs == 1);
    dialog_field(app, ui->dialogs[0].id, "color", "#0000ff");
    CHECK(answer_dialog(app, ui->dialogs[0].id, true) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    pc_layer_id id = state->active_layer;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    float rgb[3];
    pc_color_to_rgb(&pc_document_layer(doc, id)->content.u.text.color, rgb);
    CHECK(rgb[2] > .99f && rgb[0] < .01f);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.type.styleColor\",\"params\":{\"id\":9999}}", NULL) ==
          PC_ERR_NOT_FOUND);
    CHECK(pc_ui_type_begin(session, ui, id, true, NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"tools.setColors\",\"params\":{\"foreground\":\"#ff0000\"}}",
               NULL) == PC_OK);
    doc = pc_doc_arc_get(pc_engine_session_active(session)->doc);
    pc_color_to_rgb(&pc_document_layer(doc, id)->content.u.text.color, rgb);
    CHECK(rgb[0] > .99f && rgb[2] < .01f);
    CHECK(call(app, "ui.type", "{\"text\":\"Red\"}", NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(session)->revision;
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"tools.setColors\",\"params\":{\"foreground\":\"#00ff00\"}}",
               NULL) == PC_OK);
    CHECK(pc_engine_session_active(session)->revision == revision);
    CHECK(call(app, "ui.key", "{\"key\":\"A\",\"command\":true}", NULL) == PC_OK);
    CHECK(call(app, "ui.dialog.open", "{\"kind\":\"colorPicker\",\"target\":\"foreground\"}", NULL) == PC_OK);
    dialog_field(app, ui->dialogs[0].id, "color", "#0000ff");
    CHECK(answer_dialog(app, ui->dialogs[0].id, true) == PC_OK);
    doc = pc_doc_arc_get(pc_engine_session_active(session)->doc);
    pc_color_to_rgb(&pc_document_layer(doc, id)->content.u.text.color, rgb);
    CHECK(rgb[2] > .99f && rgb[0] < .01f);
    pc_ui_shell_app_free(app);
}
static void test_pen_control_preview_and_tool_change_finish(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"id\":\"file.new\",\"params\":{\"width\":200,\"height\":160}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"tool\":\"Pen\",\"events\":[{\"kind\":\"down\",\"x\":20,\"y\":20},{\"kind\":\"move\",\"x\":"
               "30,\"y\":20},{\"kind\":\"up\",\"x\":30,\"y\":20},{\"kind\":\"down\",\"x\":80,\"y\":60},{"
               "\"kind\":\"up\",\"x\":80,\"y\":60}]}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->has_pen && ui->pen.count == 2);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    CHECK(!pc_doc_arc_get(state->doc)->has_work_path);
    pc_buf inspect = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", NULL, &inspect) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&inspect), "\"pen\":{");
    pc_buf_fini(&inspect);
    CHECK(call(app, "ui.key", "{\"key\":\"Z\",\"command\":true}", NULL) == PC_OK && ui->pen.count == 1);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":80,\"y\":60},{\"kind\":\"up\",\"x\":80,\"y\":60}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"Brush\"}", NULL) == PC_OK && !ui->has_pen);
    state = pc_engine_session_active(pc_ui_shell_app_session(app));
    CHECK(pc_doc_arc_get(state->doc)->has_work_path && ui->selected_path &&
          !strcmp(ui->selected_path, "work"));
    pc_ui_shell_app_free(app);
}
static void test_nested_dialog_color_scope_cancel_and_commit(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"id\":\"file.new\",\"params\":{\"width\":12,\"height\":12}}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"image.adjustments.gradientMap\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    uint64_t parent = ui->dialogs[0].id;
    char request[512];
    (void)snprintf(request, sizeof request,
                   "{\"dialog\":%llu,\"field\":\"stops\",\"value\":[[0,\"#000000\"],[1,\"#ffffff\"]]}",
                   (unsigned long long)parent);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_OK);
    (void)snprintf(
        request, sizeof request,
        "{\"id\":\"ui.dialog.colorField\",\"params\":{\"dialog\":%llu,\"field\":\"stops\",\"stop\":1}}",
        (unsigned long long)parent);
    CHECK(call(app, "ui.menu.invoke", request, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 2);
    uint64_t child = ui->dialogs[1].id;
    dialog_field(app, child, "color", "#ff0000");
    CHECK(answer_dialog(app, child, false) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    yyjson_mut_val *stops = yyjson_mut_obj_get(yyjson_mut_doc_get_root(ui->dialogs[0].fields), "stops");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_arr_get(yyjson_mut_arr_get(stops, 1), 1)), "#ffffff");
    CHECK(call(app, "ui.menu.invoke", request, NULL) == PC_OK);
    child = ui->dialogs[1].id;
    dialog_field(app, child, "color", "#ff0000");
    CHECK(answer_dialog(app, child, true) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    stops = yyjson_mut_obj_get(yyjson_mut_doc_get_root(ui->dialogs[0].fields), "stops");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_arr_get(yyjson_mut_arr_get(stops, 1), 1)), "#ff0000");
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    CHECK(call(app, "ui.menu.invoke", request, NULL) == PC_OK);
    child = ui->dialogs[1].id;
    (void)snprintf(request, sizeof request,
                   "{\"dialog\":%llu,\"field\":\"stops\",\"value\":[[0,\"#000000\"],[1,\"#00ff00\"]]}",
                   (unsigned long long)parent);
    CHECK(call(app, "ui.dialog.set", request, NULL) == PC_OK);
    CHECK(answer_dialog(app, child, true) == PC_ERR_INVALID);
    CHECK(ui->n_dialogs == 2);
    CHECK(answer_dialog(app, child, false) == PC_OK);
    CHECK(answer_dialog(app, parent, true) == PC_OK);
    CHECK(ui->n_dialogs == 0 &&
          pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision + 1);
    pc_ui_shell_app_free(app);
}
static void test_workspaces_save_restore_lock_menu_and_dialog(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->panels.rail = false;
    ui->panels.menu_bar = false;
    ui->dock_width = 407;
    ui->panels.history = true;
    ui->dock_tabs.layers = 2;
    CHECK(call(app, "ui.menu.invoke",
               "{\"id\":\"window.workspace.newWorkspace\",\"params\":{\"name\":\" Retouch "
               "\",\"keyboardShortcuts\":true,\"menus\":true,\"toolbar\":true}}",
               NULL) == PC_OK);
    CHECK_EQ_STR(ui->workspace, "Retouch");
    pc_buf menu = PC_BUF_INIT;
    CHECK(call(app, "ui.menu.list", NULL, &menu) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&menu), "window.workspace.saved.Retouch");
    pc_buf_fini(&menu);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"window.workspace.photography\"}", NULL) == PC_OK);
    CHECK_EQ_STR(ui->workspace, "Photography");
    CHECK(ui->panels.navigator && !ui->panels.color && !ui->panels.rail && !ui->panels.menu_bar);
    ui->dock_width = 300;
    CHECK(call(app, "engine.execute",
               "{\"id\":\"prefs.set\",\"params\":{\"path\":\"shortcuts.edit.undo\",\"value\":\"Alt+U\"}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"window.workspace.saved.Retouch\"}", NULL) == PC_OK);
    pc_json_value shortcuts = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_prefs_get(pc_ui_shell_app_session(app), "shortcuts", &shortcuts, NULL) == PC_OK);
    CHECK(!yyjson_mut_obj_size(pc_json_value_root(&shortcuts)));
    pc_json_value_fini(&shortcuts);
    CHECK_EQ_STR(ui->workspace, "Retouch");
    CHECK(ui->dock_width == 407 && ui->panels.history && ui->dock_tabs.layers == 2);
    CHECK(!ui->panels.rail && !ui->panels.menu_bar);
    CHECK(call(app, "ui.menu.invoke",
               "{\"id\":\"window.workspace.deleteWorkspace\",\"params\":{\"name\":\"Retouch\"}}",
               NULL) != PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"window.workspace.lockWorkspace\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.menu.list", NULL, &menu) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&menu), "window.workspace.lockWorkspace");
    pc_buf_fini(&menu);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"window.workspace.essentials\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke",
               "{\"id\":\"window.workspace.deleteWorkspace\",\"params\":{\"name\":\"Retouch\"}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"window.workspace.newWorkspace\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    uint64_t id = ui->dialogs[0].id;
    char json[180];
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu,\"field\":\"name\",\"value\":\"My layout\"}",
                   (unsigned long long)id);
    CHECK(call(app, "ui.dialog.set", json, NULL) == PC_OK);
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu}", (unsigned long long)id);
    CHECK(call(app, "ui.dialog.confirm", json, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    CHECK_EQ_STR(ui->workspace, "My layout");
    CHECK(call(app, "ui.menu.invoke",
               "{\"id\":\"window.workspace.select\",\"params\":{\"name\":\"Not saved\"}}", NULL) != PC_OK);
    CHECK_EQ_STR(ui->workspace, "My layout");
    char *prefs = pc_engine_session_prefs_to_json(pc_ui_shell_app_session(app));
    pc_ui_shell_app *copy = NULL;
    CHECK(pc_ui_shell_app_new(&copy, NULL) == PC_OK);
    CHECK(prefs && pc_engine_session_load_prefs_json(pc_ui_shell_app_session(copy), prefs, strlen(prefs),
                                                     NULL) == PC_OK);
    free(prefs);
    CHECK(call(copy, "ui.menu.invoke",
               "{\"id\":\"window.workspace.select\",\"params\":{\"name\":\"My layout\"}}", NULL) == PC_OK);
    CHECK_EQ_STR(pc_ui_shell_app_state(copy)->workspace, "My layout");
    CHECK(pc_ui_shell_app_state(copy)->panels.rail && pc_ui_shell_app_state(copy)->panels.menu_bar);
    pc_ui_shell_app_free(copy);
    pc_ui_shell_app_free(app);
}
static void test_system_info_readonly_api_and_english_copy_under_localization(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"id\":\"prefs.set\",\"params\":{\"path\":\"interface.language\",\"value\":\"cs\"}}",
               NULL) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    size_t journal = pc_engine_session_journal_len(session);
    pc_buf reply = PC_BUF_INIT;
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"help.systemInfo\"}", &reply) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&reply), "\"info\":");
    CHECK_CONTAINS(pc_buf_cstr(&reply), "\"monitor\":");
    CHECK_CONTAINS(pc_buf_cstr(&reply), "\"gpu\":");
    CHECK_CONTAINS(pc_buf_cstr(&reply), "Platform:");
    pc_buf_fini(&reply);
    CHECK(pc_engine_session_journal_len(session) == journal);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->n_dialogs == 1);
    if (ui->n_dialogs) {
        yyjson_mut_val *fields = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
        const char *copy = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__copyText"));
        CHECK(copy && strstr(copy, "Platform:") && strstr(copy, "Monitor profile:"));
        uint64_t id = ui->dialogs[0].id;
        char json[80];
        (void)snprintf(json, sizeof json, "{\"dialog\":%llu}", (unsigned long long)id);
        CHECK(call(app, "ui.dialog.confirm", json, NULL) == PC_OK);
    }
    CHECK(ui->n_dialogs == 0 && pc_engine_session_journal_len(session) == journal);
    pc_ui_shell_app_free(app);
}
static void test_clipboard_new_document_preset(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_size size = pc_size_make(99, 99);
    CHECK(!pc_engine_session_clipboard_size(session, &size));
    CHECK(size.width == 0 && size.height == 0);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, "file.new", "{\"width\":73,\"height\":29}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(session, "edit.copy", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_clipboard_size(session, &size));
    CHECK(size.width == 73 && size.height == 29);
    size_t journal = pc_engine_session_journal_len(session);
    CHECK(pc_engine_session_clipboard_size(session, NULL));
    CHECK(pc_engine_session_journal_len(session) == journal);
    CHECK(call(app, "ui.dialog.open", "{\"kind\":\"newDocument\"}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->n_dialogs == 1);
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(fields, "width")) == 73);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(fields, "height")) == 29);
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(fields, "resolution")) == 72);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__preset")), "Clipboard");
    char request[96];
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu}", (unsigned long long)ui->dialogs[0].id);
    CHECK(call(app, "ui.dialog.confirm", request, NULL) == PC_OK);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(session)->doc);
    CHECK(doc->size.width == 73 && doc->size.height == 29);
    CHECK_NEAR(doc->resolution_dpi, 72, 0);
    pc_ui_shell_app_free(app);
}

static void test_timeline_control_validation_and_state(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"timeline\":{\"open\":true,\"playing\":true}}", NULL) == PC_ERR_INVALID);
    CHECK(!pc_ui_shell_app_state(app)->timeline.open);
    CHECK(call(app, "ui.set", "{\"timeline\":{\"open\":true}}", NULL) == PC_OK);
    CHECK(pc_ui_shell_app_state(app)->timeline.open);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(pc_ui_shell_app_session(app), "file.new",
                                         "{\"width\":8,\"height\":8}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(pc_ui_shell_app_session(app), "timeline.create", "{}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(call(app, "ui.set", "{\"timeline\":{\"playing\":true}}", NULL) == PC_OK);
    CHECK(pc_ui_shell_app_state(app)->timeline.playing);
    CHECK(call(app, "ui.set", "{\"timeline\":{\"open\":false},\"zoom\":-2}", NULL) == PC_ERR_INVALID);
    CHECK(pc_ui_shell_app_state(app)->timeline.open && pc_ui_shell_app_state(app)->timeline.playing);
    CHECK(call(app, "ui.set", "{\"timeline\":{\"open\":false}}", NULL) == PC_OK);
    CHECK(!pc_ui_shell_app_state(app)->timeline.open && !pc_ui_shell_app_state(app)->timeline.playing);
    pc_buf json = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", NULL, &json) == PC_OK);
    CHECK(strstr(pc_buf_cstr(&json), "\"timeline\":{\"open\":false,\"playing\":false}") != NULL);
    pc_buf_fini(&json);
    pc_ui_shell_app_free(app);
}

static void test_picker_control_values_and_rejected_mixed_update(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "ui.set",
               "{\"brushPicker\":[20,50],\"brushPickerName\":false,\"brushPickerStroke\":false,"
               "\"brushPickerScale\":0.3,\"brushSize\":37,\"brushesView\":\"grid\"}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->brush_picker.open && ui->brush_picker.point[1] == 50);
    CHECK(ui->brush_picker.show_tip && !ui->brush_picker.show_name && ui->brushes_grid);
    CHECK(pc_engine_session_tools(pc_ui_shell_app_session(app))->brush.size == 37);
    pc_ui_theme_kind theme = ui->theme;
    CHECK(call(app, "ui.set", "{\"theme\":\"studio\",\"brushPickerTip\":false,\"brushSize\":45}", NULL) ==
          PC_ERR_INVALID);
    CHECK(ui->theme == theme && ui->brush_picker.show_tip);
    CHECK(pc_engine_session_tools(pc_ui_shell_app_session(app))->brush.size == 37);
    CHECK(call(app, "ui.set", "{\"brushPicker\":null}", NULL) == PC_OK);
    CHECK(!ui->brush_picker.open);
    pc_ui_shell_app_free(app);
}

static void test_export_encoding_warnings_reach_native_notices(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"command\":\"file.new\",\"params\":{\"width\":8,\"height\":6,\"depth\":32}}",
               NULL) == PC_OK);
    char directory[] = "/tmp/photocraft-warning-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256], json[512];
    (void)snprintf(path, sizeof path, "%s/output.jpg", directory);
    (void)snprintf(json, sizeof json,
                   "{\"command\":\"file.export.exportAs\",\"params\":{\"path\":\"%s\",\"format\":\"jpg\"}}",
                   path);
    pc_buf reply = PC_BUF_INIT;
    CHECK(call(app, "engine.execute", json, &reply) == PC_OK);
    yyjson_doc *result = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&reply), reply.len, &result, NULL) == PC_OK);
    CHECK(yyjson_arr_size(pc_json_get(yyjson_doc_get_root(result), "warnings")) > 0);
    CHECK(pc_ui_shell_app_state(app)->status_error);
    yyjson_doc_free(result);
    pc_buf_fini(&reply);
    pc_json_value notices = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_notices_json(app, &notices, NULL) == PC_OK);
    CHECK(yyjson_mut_arr_size(pc_json_value_root(&notices)) > 0);
    pc_json_value_fini(&notices);
    CHECK(unlink(path) == 0 && rmdir(directory) == 0);
    pc_ui_shell_app_free(app);
}

static void test_transform_control_preview_undo_redo_and_commit(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    const char *commands[] = {
        "{\"command\":\"file.new\",\"params\":{\"width\":200,\"height\":160}}",
        "{\"command\":\"layer.new.layer\"}",
        "{\"command\":\"select.rect\",\"params\":{\"x\":20,\"y\":20,\"width\":100,\"height\":60}}",
        "{\"command\":\"edit.fill\",\"params\":{\"color\":\"#ff0000\"}}",
        "{\"command\":\"select.deselect\"}"};
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++)
        CHECK(call(app, "engine.execute", commands[i], NULL) == PC_OK);
    pc_engine_session *s = pc_ui_shell_app_session(app);
    uint64_t revision = pc_engine_session_active(s)->revision;
    size_t history = pc_history_past_len(pc_engine_session_active(s)->history);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"edit.freeTransform\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":120,\"y\":80},{\"kind\":\"move\",\"x\":170,\"y\":110},{"
               "\"kind\":\"up\",\"x\":170,\"y\":110}]}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->transform.active && ui->transform.can_undo);
    CHECK_NEAR(ui->transform.frame.quad[2][0], 170, 1e-8);
    CHECK(pc_engine_session_active(s)->revision == revision);
    CHECK(call(app, "engine.execute", "{\"command\":\"edit.undo\"}", NULL) == PC_OK);
    CHECK_NEAR(ui->transform.frame.quad[2][0], 120, 1e-8);
    CHECK(call(app, "engine.execute", "{\"command\":\"edit.redo\"}", NULL) == PC_OK);
    CHECK_NEAR(ui->transform.frame.quad[2][0], 170, 1e-8);
    pc_buf text = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", NULL, &text) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&text), "\"transform\":{");
    CHECK_CONTAINS(pc_buf_cstr(&text), "\"canUndo\":true");
    pc_buf_fini(&text);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.transform.commit\"}", NULL) == PC_OK);
    CHECK(!ui->transform.active && pc_history_past_len(pc_engine_session_active(s)->history) == history + 1);
    pc_ui_shell_app_free(app);
}

static void test_context_control_scope_selection_transform_pasteboard_and_erase(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":200,\"height\":160}}",
               NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"layer.new.layer\"}", NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"command\":\"select.rect\",\"params\":{\"x\":20,\"y\":20,\"width\":100,\"height\":60}}",
               NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"edit.fill\",\"params\":{\"color\":\"#ff0000\"}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"rectMarquee\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":50,\"y\":40},{\"kind\":\"up\","
               "\"x\":50,\"y\":40}]}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->context_menu.open && ui->context_menu.kind == PC_UI_CONTEXT_SELECTION);
    pc_buf text = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", NULL, &text) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&text), "\"canvasToolMenu\":{");
    CHECK_CONTAINS(pc_buf_cstr(&text), "select.modify.feather");
    pc_buf_fini(&text);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"file.new\"}", NULL) != PC_OK);
    CHECK(ui->context_menu.open);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"select.deselect\"}", NULL) == PC_OK);
    CHECK(!ui->context_menu.open);
    CHECK(call(app, "ui.pointer", "{\"button\":\"right\",\"events\":[{\"kind\":\"down\",\"x\":50,\"y\":40}]}",
               NULL) == PC_OK);
    CHECK(ui->context_menu.kind == PC_UI_CONTEXT_EMPTY_SELECTION);
    CHECK(call(app, "engine.execute", "{\"command\":\"layer.new.layer\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"select.all\"}", NULL) != PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":-5,\"y\":-5}]}",
               NULL) == PC_OK);
    CHECK(ui->context_menu.kind == PC_UI_CONTEXT_PASTEBOARD);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"pasteboard.black\"}", NULL) == PC_OK);
    pc_ui_color black = pc_ui_shell_canvas_color(app);
    CHECK(black.r == 0 && black.g == 0 && black.b == 0);
    CHECK(call(app, "ui.pointer",
               "{\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":-5,\"y\":-5}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"pasteboard.selectCustomColor\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    uint64_t dialog = ui->dialogs[0].id;
    char params[160];
    (void)snprintf(params, sizeof params, "{\"dialog\":%llu,\"field\":\"color\",\"value\":\"#123456\"}",
                   (unsigned long long)dialog);
    CHECK(call(app, "ui.dialog.set", params, NULL) == PC_OK);
    (void)snprintf(params, sizeof params, "{\"dialog\":%llu}", (unsigned long long)dialog);
    CHECK(call(app, "ui.dialog.confirm", params, NULL) == PC_OK);
    pc_ui_color custom = pc_ui_shell_canvas_color(app);
    CHECK(custom.r == 0x12 && custom.g == 0x34 && custom.b == 0x56);
    CHECK(call(app, "ui.set", "{\"tool\":\"Brush\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":50,\"y\":40}]}",
               NULL) == PC_OK);
    CHECK(ui->brush_picker.open && !ui->context_menu.open);
    ui->brush_picker.open = false;
    CHECK(call(app, "engine.execute", "{\"command\":\"select.all\"}", NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"edit.fill\",\"params\":{\"color\":\"#ff0000\"}}",
               NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"select.deselect\"}", NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"command\":\"prefs.set\",\"params\":{\"path\":\"tools.rightClickWithPaintingTools\","
               "\"value\":\"erase\"}}",
               NULL) == PC_OK);
    pc_engine_session *s = pc_ui_shell_app_session(app);
    size_t history = pc_history_past_len(pc_engine_session_active(s)->history);
    CHECK(call(app, "ui.pointer",
               "{\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":50,\"y\":40},{\"kind\":\"up\","
               "\"x\":50,\"y\":40}]}",
               NULL) == PC_OK);
    CHECK(ui->tool == PC_TOOL_BRUSH && !ui->brush_picker.open);
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == history + 1);
    const pc_layer *layer = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc),
                                              pc_engine_session_active(s)->active_layer);
    CHECK(pc_surface_sample_channel(pc_layer_surface(layer), 50, 40, 3) < .5f);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"edit.freeTransform\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":50,\"y\":40}]}",
               NULL) == PC_OK);
    CHECK(ui->context_menu.kind == PC_UI_CONTEXT_TRANSFORM);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"edit.transform.rotate90Cw\"}", NULL) == PC_OK);
    CHECK(ui->transform.active && ui->transform.can_undo);
    pc_ui_shell_app_free(app);
}

static void test_context_view_print_fill_and_text_selection_style(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":200,\"height\":160}}",
               NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"command\":\"prefs.set\",\"params\":{\"path\":\"unitsAndRulers.screenResolution\","
               "\"value\":144}}",
               NULL) == PC_OK);
    CHECK(
        call(
            app, "ui.pointer",
            "{\"tool\":\"Hand\",\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":50,\"y\":40}]}",
            NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->context_menu.kind == PC_UI_CONTEXT_VIEW);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"view.printSize\"}", NULL) == PC_OK);
    double dpi = pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(app))->doc)->resolution_dpi;
    CHECK_NEAR(ui->views[0].zoom, 144 / dpi, .0001);
    CHECK(call(app, "ui.pointer",
               "{\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":50,\"y\":40}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"view.fillScreen\"}", NULL) == PC_OK);
    CHECK(ui->views[0].fill_pending && !ui->views[0].fit_pending);
    CHECK(call(app, "ui.pointer",
               "{\"tool\":\"Type\",\"events\":[{\"kind\":\"down\",\"x\":30,\"y\":60},{\"kind\":\"up\",\"x\":"
               "30,\"y\":60}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.type", "{\"text\":\"Context text\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":30,\"y\":60}]}",
               NULL) == PC_OK);
    CHECK(ui->has_text_edit && ui->context_menu.kind == PC_UI_CONTEXT_TEXT);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"select.all\"}", NULL) == PC_OK);
    CHECK(ui->text_edit.anchor == 0 && ui->text_edit.caret == 12);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(app))->doc);
    CHECK(!doc->has_selection);
    CHECK(call(app, "ui.pointer",
               "{\"button\":\"secondary\",\"events\":[{\"kind\":\"down\",\"x\":30,\"y\":60}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.context.choose", "{\"id\":\"type.fauxBold\"}", NULL) == PC_OK);
    CHECK(ui->has_text_edit);
    doc = pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(app))->doc);
    const pc_layer *layer = pc_document_layer(doc, ui->text_edit.layer);
    CHECK(layer && layer->content.kind == PC_LAYER_TEXT && layer->content.u.text.runs[0].style.faux_bold);
    pc_ui_shell_app_free(app);
}

static void test_crop_pending_frame_local_undo_cancel_size_and_space(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":200,\"height\":160}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"Crop\",\"cropShield\":{\"classic_mode\":true}}", NULL) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->extras.snap = false; /* This case isolates Space and size editing. */
    size_t history = pc_history_past_len(pc_engine_session_active(session)->history);
    const pc_doc_arc *before = pc_engine_session_active(session)->doc;
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":20,\"y\":20},{\"kind\":\"move\",\"x\":120,\"y\":80},{"
               "\"kind\":\"up\",\"x\":120,\"y\":80}]}",
               NULL) == PC_OK);
    CHECK(ui->has_crop && ui->crop_editing && ui->crop_can_undo);
    const char *bad[] = {"{\"tool\":\"Move\",\"toolOptions\":{\"unknown\":true}}",
                         "{\"tool\":\"Move\",\"toolOptions\":{\"crop_shield\":{\"opacity\":101}}}",
                         "{\"toolOptions\":{\"eyedropper_sample\":\"bad\"}}"};
    for (size_t i = 0; i < 3; i++) {
        CHECK(call(app, "ui.set", bad[i], NULL) != PC_OK);
        CHECK(ui->tool == PC_TOOL_CROP && ui->crop_editing);
    }
    CHECK_NEAR(ui->crop_rect[0], 20, 1e-8);
    CHECK_NEAR(ui->crop_rect[2], 120, 1e-8);
    CHECK(pc_engine_session_active(session)->doc == before &&
          pc_history_past_len(pc_engine_session_active(session)->history) == history);
    CHECK(call(app, "engine.execute", "{\"command\":\"edit.undo\"}", NULL) == PC_OK);
    CHECK_NEAR(ui->crop_rect[0], 0, 1e-8);
    CHECK_NEAR(ui->crop_rect[2], 200, 1e-8);
    CHECK(ui->crop_can_redo && !ui->crop_can_undo);
    CHECK(call(app, "engine.execute", "{\"command\":\"edit.redo\"}", NULL) == PC_OK);
    CHECK_NEAR(ui->crop_rect[0], 20, 1e-8);
    CHECK(call(app, "ui.key", "{\"key\":\"ArrowRight\",\"shift\":true}", NULL) == PC_OK);
    CHECK_NEAR(ui->crop_rect[0], 30, 1e-8);
    CHECK(call(app, "ui.key", "{\"key\":\"X\"}", NULL) == PC_OK);
    CHECK_NEAR(ui->crop_rect[2] - ui->crop_rect[0], 60, 1e-8);
    CHECK_NEAR(ui->crop_rect[3] - ui->crop_rect[1], 100, 1e-8);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.crop.cancel\"}", NULL) == PC_OK);
    CHECK(!ui->crop_editing && pc_engine_session_active(session)->doc == before);
    CHECK(call(app, "ui.set", "{\"toolOptions\":{\"crop_ratio\":\"\"}}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":20,\"y\":20},{\"kind\":\"move\",\"x\":120,\"y\":80}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"space\":true,\"events\":[{\"kind\":\"move\",\"x\":130,\"y\":85},{\"kind\":\"up\",\"x\":"
               "130,\"y\":85}]}",
               NULL) == PC_OK);
    CHECK_NEAR(ui->crop_rect[0], 30, 1e-8);
    CHECK_NEAR(ui->crop_rect[1], 25, 1e-8);
    CHECK_NEAR(ui->crop_rect[2], 130, 1e-8);
    CHECK_NEAR(ui->crop_rect[3], 85, 1e-8);
    CHECK(call(app, "ui.set",
               "{\"toolOptions\":{\"crop_ratio\":\"whr\",\"crop_width\":\"1 in\",\"crop_height\":\"0.5 "
               "in\",\"crop_resolution\":\"100\",\"crop_resolution_unit\":\"px/in\"}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.crop.commit\"}", NULL) == PC_OK);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(session)->doc);
    CHECK(doc->size.width == 100 && doc->size.height == 50 && doc->resolution_dpi == 100);
    CHECK(pc_history_past_len(pc_engine_session_active(session)->history) == history + 1);
    CHECK(pc_engine_session_undo(session));
    CHECK(pc_doc_arc_get(pc_engine_session_active(session)->doc)->size.width == 200);
    const char *presets[] = {"preset:4x5in",      "preset:8.5x11in", "preset:1024x768px", "preset:1280x800px",
                             "preset:1366x768px", "preset:4x6in",    "preset:5x7in",      "preset:8x10in"};
    for (size_t i = 0; i < 8; i++) {
        char params[160];
        (void)snprintf(params, sizeof params, "{\"id\":\"ui.crop.preset\",\"params\":{\"name\":\"%s\"}}",
                       presets[i]);
        CHECK(call(app, "ui.menu.invoke", params, NULL) == PC_OK);
        CHECK(pc_str_eq_c(&ui->tool_options.crop_ratio, "whr") && ui->tool_options.crop_width.len &&
              ui->tool_options.crop_height.len && ui->tool_options.crop_resolution.len);
    }
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.crop.preset\",\"params\":{\"name\":\"bad\"}}", NULL) !=
          PC_OK);
    pc_ui_shell_app_free(app);
}

static void test_snap_control_crop_handle_grab_offset_and_override(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":200,\"height\":160}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"Crop\",\"cropShield\":{\"classic_mode\":true}}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->extras.snap = true;
    size_t history = pc_history_past_len(pc_engine_session_active(pc_ui_shell_app_session(app))->history);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":20,\"y\":20},{\"kind\":\"up\",\"x\":120,\"y\":80}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":124,\"y\":78},{\"kind\":\"move\",\"x\":201,\"y\":139},{"
               "\"kind\":\"up\",\"x\":201,\"y\":139}]}",
               NULL) == PC_OK);
    CHECK_NEAR(ui->crop_rect[2], 200, .001);
    CHECK_NEAR(ui->crop_rect[0], 20, .001);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.crop.cancel\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":20,\"y\":20},{\"kind\":\"up\",\"x\":120,\"y\":80}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.pointer", "{\"events\":[{\"kind\":\"down\",\"x\":124,\"y\":78}]}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer", "{\"ctrl\":true,\"events\":[{\"kind\":\"up\",\"x\":201,\"y\":139}]}",
               NULL) == PC_OK);
    CHECK_NEAR(ui->crop_rect[2], 197, .001);
    CHECK(pc_history_past_len(pc_engine_session_active(pc_ui_shell_app_session(app))->history) == history);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.crop.cancel\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":204,\"y\":80},{\"kind\":\"up\",\"x\":184,\"y\":80}]}",
               NULL) == PC_OK);
    CHECK_NEAR(ui->crop_rect[2], 180, .001);
    CHECK_NEAR(ui->crop_rect[0], 0, .001);
    pc_ui_shell_app_free(app);
}
static void test_crop_default_ratio_cancel_and_original_are_not_edits(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":200,\"height\":100}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"Crop\",\"toolOptions\":{\"crop_ratio\":\"1:1\"}}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(call(app, "ui.pointer", "{\"events\":[{\"kind\":\"move\",\"x\":20,\"y\":20}]}", NULL) == PC_OK);
    CHECK(ui->has_crop && !ui->crop_editing && !ui->crop_can_undo);
    CHECK_NEAR(ui->crop_rect[2] - ui->crop_rect[0], 100, .001);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.crop.cancel\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer", "{\"events\":[{\"kind\":\"move\",\"x\":20,\"y\":20}]}", NULL) == PC_OK);
    CHECK(!ui->crop_editing && !ui->crop_can_undo);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.crop.commit\"}", NULL) == PC_OK);
    CHECK(pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(app))->doc)->size.width == 100);
    CHECK(call(app, "engine.execute", "{\"command\":\"edit.undo\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"toolOptions\":{\"crop_ratio\":\"original\"}}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer", "{\"events\":[{\"kind\":\"move\",\"x\":20,\"y\":20}]}", NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    size_t history = pc_history_past_len(state->history);
    const pc_doc_arc *before = state->doc;
    CHECK(!ui->crop_editing && !ui->crop_can_undo);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.crop.commit\"}", NULL) == PC_OK);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->doc == before);
    CHECK(pc_history_past_len(pc_engine_session_active(pc_ui_shell_app_session(app))->history) == history);
    pc_ui_shell_app_free(app);
}
static void test_slice_control_options_auto_promotion_and_validation(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":100,\"height\":80}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"Slice\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":10,\"y\":10},{\"kind\":\"up\",\"x\":40,\"y\":30}]}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->slices.has_selected);
    CHECK(call(app, "ui.set", "{\"tool\":\"Brush\",\"slices\":{\"selected\":99999}}", NULL) ==
          PC_ERR_INVALID);
    CHECK(ui->tool == PC_TOOL_SLICE);
    CHECK(call(app, "ui.set", "{\"slices\":{\"unknown\":true}}", NULL) == PC_ERR_INVALID);
    CHECK(call(app, "ui.set", "{\"tool\":\"SliceSelect\",\"slices\":{\"hide_auto\":true}}", NULL) == PC_OK);
    CHECK(ui->slices.hide_auto);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":90,\"y\":75},{\"kind\":\"up\",\"x\":90,\"y\":75}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.slice.options\"}", NULL) == PC_OK);
    CHECK(ui->n_dialogs == 1);
    if (!ui->n_dialogs) {
        pc_ui_shell_app_free(app);
        return;
    }
    char patch[160];
    (void)snprintf(patch, sizeof patch, "{\"dialog\":%llu,\"field\":\"name\",\"value\":\"footer\"}",
                   (unsigned long long)ui->dialogs[0].id);
    CHECK(call(app, "ui.dialog.set", patch, NULL) == PC_OK);
    (void)snprintf(patch, sizeof patch, "{\"dialog\":%llu}", (unsigned long long)ui->dialogs[0].id);
    CHECK(call(app, "ui.dialog.confirm", patch, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(app))->doc);
    CHECK(doc->slices.len == 2);
    bool footer = false;
    for (size_t j = 0; j < doc->slices.len; j++)
        footer |= pc_str_eq_c(&doc->slices.list[j].name, "footer");
    CHECK(footer);
    pc_buf inspect = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", "{}", &inspect) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&inspect), "\"slices\":");
    pc_buf_fini(&inspect);
    pc_ui_shell_app_free(app);
}
static void test_persistent_lasso_control_routing_and_history(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":100,\"height\":80}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"PolygonLasso\"}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    size_t history = pc_history_past_len(pc_engine_session_active(pc_ui_shell_app_session(app))->history);
    const char *polygon = "{\"events\":[{\"kind\":\"down\",\"x\":10,\"y\":10},{\"kind\":\"up\",\"x\":10,"
                          "\"y\":10},{\"kind\":\"down\",\"x\":60,\"y\":10},{\"kind\":\"up\",\"x\":60,\"y\":"
                          "10},{\"kind\":\"down\",\"x\":60,\"y\":60},{\"kind\":\"up\",\"x\":60,\"y\":60}]}";
    CHECK(call(app, "ui.pointer", polygon, NULL) == PC_OK);
    CHECK(ui->lasso.active && ui->lasso.path.n == 3);
    CHECK(pc_history_past_len(pc_engine_session_active(pc_ui_shell_app_session(app))->history) == history);
    CHECK(call(app, "ui.key", "{\"key\":\"Backspace\"}", NULL) == PC_OK && ui->lasso.path.n == 2);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":60,\"y\":60},{\"kind\":\"up\",\"x\":60,\"y\":60}]}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.key", "{\"key\":\"Enter\"}", NULL) == PC_OK && !ui->lasso.active);
    CHECK(pc_history_past_len(pc_engine_session_active(pc_ui_shell_app_session(app))->history) ==
          history + 1);
    CHECK(call(app, "ui.set", "{\"tool\":\"MagneticLasso\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer", polygon, NULL) == PC_OK && ui->lasso.active);
    CHECK(call(app, "ui.key", "{\"key\":\"Enter\",\"alt\":true}", NULL) == PC_OK && !ui->lasso.active);
    CHECK(pc_history_past_len(pc_engine_session_active(pc_ui_shell_app_session(app))->history) ==
          history + 2);
    pc_ui_shell_app_free(app);
}
static void test_artboard_control_gesture_fit_orientation_and_new_form(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":400,\"height\":300}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"view.fitArtboardOnScreen\"}", NULL) == PC_ERR_INVALID);
    CHECK(call(app, "ui.set", "{\"tool\":\"Artboard\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":20,\"y\":30},{\"kind\":\"move\",\"x\":140,\"y\":110},{"
               "\"kind\":\"up\",\"x\":140,\"y\":110}]}",
               NULL) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_layer_id layer;
    pc_rect rect;
    CHECK(pc_ui_artboard_active(pc_engine_session_active(session), &layer, &rect));
    CHECK(pc_rect_eq(rect, pc_rect_make(20, 30, 140, 110)));
    size_t history = pc_history_past_len(pc_engine_session_active(session)->history);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"view.fitArtboardOnScreen\"}", NULL) == PC_OK);
    CHECK(pc_history_past_len(pc_engine_session_active(session)->history) == history);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.artboard.portrait\"}", NULL) == PC_OK);
    CHECK(pc_ui_artboard_active(pc_engine_session_active(session), &layer, &rect));
    CHECK(pc_rect_width(rect) == 80 && pc_rect_height(rect) == 120);
    CHECK(
        call(app, "ui.pointer",
             "{\"events\":[{\"kind\":\"down\",\"x\":250,\"y\":200},{\"kind\":\"move\",\"x\":300,\"y\":260}]}",
             NULL) == PC_OK);
    CHECK(pc_ui_shell_app_state(app)->artboard_tool.active);
    CHECK(call(app, "ui.key", "{\"key\":\"Escape\"}", NULL) == PC_OK);
    CHECK(!pc_ui_shell_app_state(app)->artboard_tool.active);
    history = pc_history_past_len(pc_engine_session_active(session)->history);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"layer.new.artboard\"}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->n_dialogs == 1);
    CHECK(pc_history_past_len(pc_engine_session_active(session)->history) == history);
    if (ui->n_dialogs) {
        char text[180];
        (void)snprintf(text, sizeof text, "{\"dialog\":%llu,\"field\":\"width\",\"value\":75}",
                       (unsigned long long)ui->dialogs[0].id);
        CHECK(call(app, "ui.dialog.set", text, NULL) == PC_OK);
        (void)snprintf(text, sizeof text, "{\"dialog\":%llu}", (unsigned long long)ui->dialogs[0].id);
        CHECK(call(app, "ui.dialog.confirm", text, NULL) == PC_OK);
        CHECK(ui->n_dialogs == 0);
        CHECK(pc_ui_artboard_active(pc_engine_session_active(session), &layer, &rect));
        CHECK(pc_rect_width(rect) == 75 && pc_rect_height(rect) == 300);
        CHECK(pc_history_past_len(pc_engine_session_active(session)->history) == history + 1);
    }
    pc_ui_shell_app_free(app);
}
static void test_selection_pointer_handoff_outline_pixels_and_keys(void)
{
    const char *tools[] = {"RectMarquee", "Lasso", "PolygonLasso", "MagicWand"};
    for (size_t i = 0; i < 4; i++) {
        pc_ui_shell_app *app = NULL;
        CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
        CHECK(call(app, "engine.execute",
                   "{\"command\":\"file.new\",\"params\":{\"width\":100,\"height\":80,\"background\":"
                   "\"transparent\"}}",
                   NULL) == PC_OK);
        CHECK(call(app, "engine.execute", "{\"command\":\"edit.fill\",\"params\":{\"color\":\"#ff0000\"}}",
                   NULL) == PC_OK);
        CHECK(call(app, "engine.execute",
                   "{\"command\":\"select.rect\",\"params\":{\"x\":10,\"y\":10,\"width\":20,\"height\":20}}",
                   NULL) == PC_OK);
        char text[160];
        (void)snprintf(text, sizeof text, "{\"tool\":\"%s\"}", tools[i]);
        CHECK(call(app, "ui.set", text, NULL) == PC_OK);
        pc_ui_shell_app_state(app)->extras.snap = false;
        pc_ui_shell_app_state(app)->view.show[4] = false;
        pc_engine_session *session = pc_ui_shell_app_session(app);
        const pc_engine_doc_state *state = pc_engine_session_active(session);
        size_t history = pc_history_past_len(state->history);
        if (i < 2) {
            CHECK(call(app, "ui.pointer",
                       "{\"events\":[{\"kind\":\"down\",\"x\":15,\"y\":15},{\"kind\":\"move\",\"x\":20,\"y\":"
                       "18},{\"kind\":\"up\",\"x\":20,\"y\":18}]}",
                       NULL) == PC_OK);
            CHECK(pc_history_past_len(state->history) == history + 1);
            CHECK(!pc_engine_floating_offset(session, NULL));
            CHECK(pc_surface_sample_channel(&pc_doc_arc_get(state->doc)->selection, 32, 30, 0) == 1);
            CHECK(call(app, "engine.execute", "{\"command\":\"edit.undo\"}", NULL) == PC_OK);
        }
        CHECK(call(app, "ui.pointer",
                   "{\"command\":true,\"events\":[{\"kind\":\"down\",\"x\":15,\"y\":15},{\"kind\":\"move\","
                   "\"x\":20,\"y\":18},{\"kind\":\"up\",\"x\":20,\"y\":18}]}",
                   NULL) == PC_OK);
        int32_t offset[2];
        CHECK(pc_engine_floating_offset(session, offset) && offset[0] == 5 && offset[1] == 3);
        CHECK(pc_history_past_len(state->history) == history);
        CHECK(call(app, "ui.key", "{\"key\":\"Enter\"}", NULL) == PC_OK);
        CHECK(!pc_engine_floating_offset(session, NULL) &&
              pc_history_past_len(state->history) == history + 1);
        CHECK(call(app, "ui.set", "{\"tool\":\"Move\"}", NULL) == PC_OK);
        CHECK(call(app, "ui.key", "{\"key\":\"ArrowRight\",\"shift\":true}", NULL) == PC_OK);
        CHECK(pc_engine_floating_offset(session, offset) && offset[0] == 10 && offset[1] == 0);
        CHECK(call(app, "ui.key", "{\"key\":\"Escape\"}", NULL) == PC_OK);
        CHECK(!pc_engine_floating_offset(session, NULL));
        pc_ui_shell_app_free(app);
    }
}
static pc_status interpolation_fixture(pc_document *doc, bool *has_active, pc_layer_id *active, void *context,
                                       pc_error *err)
{
    (void)has_active;
    (void)context;
    pc_layer *layer = pc_document_layer_mut(doc, *active);
    if (!layer)
        return PC_ERR_INVALID;
    pc_surface *surface = pc_layer_surface_mut(layer);
    const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    pc_status status = pc_surface_write_pixel(surface, 0, 0, red, 4, err);
    return status == PC_OK ? pc_surface_write_pixel(surface, 1, 0, blue, 4, err) : status;
}
static void test_crop_whr_and_image_size_honor_interpolation_preference(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"command\":\"file.new\",\"params\":{\"width\":2,\"height\":1,\"depth\":32,\"background\":"
               "\"transparent\"}}",
               NULL) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    CHECK(pc_engine_session_edit(session, "red blue fixture", interpolation_fixture, NULL, NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"command\":\"prefs.set\",\"params\":{\"path\":\"general.imageInterpolation\",\"value\":"
               "\"nearestNeighbor\"}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set",
               "{\"tool\":\"Crop\",\"toolOptions\":{\"crop_ratio\":\"whr\",\"crop_width\":\"4 "
               "px\",\"crop_height\":\"2 px\"}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.crop.commit\"}", NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    const pc_document *doc = pc_doc_arc_get(state->doc);
    CHECK(doc->size.width == 4 && doc->size.height == 2);
    float pixel[4];
    pc_surface_rgba(pc_layer_surface(pc_document_layer(doc, state->active_layer)), 1, 0, pixel);
    CHECK(pixel[0] == 1 && pixel[2] == 0);
    pc_surface_rgba(pc_layer_surface(pc_document_layer(doc, state->active_layer)), 2, 0, pixel);
    CHECK(pixel[0] == 0 && pixel[2] == 1);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"image.imageSize\"}", NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->n_dialogs == 1);
    if (ui->n_dialogs) {
        const char *resample = yyjson_mut_get_str(
            yyjson_mut_obj_get(yyjson_mut_doc_get_root(ui->dialogs[0].fields), "resample"));
        CHECK(resample && !strcmp(resample, "nearest"));
    }
    pc_ui_shell_app_free(app);
}
static void test_eyedropper_and_sampler_control_serving(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute",
               "{\"command\":\"file.new\",\"params\":{\"width\":2,\"height\":1,\"depth\":32,\"background\":"
               "\"transparent\"}}",
               NULL) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    CHECK(pc_engine_session_edit(session, "red blue", interpolation_fixture, NULL, NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    size_t history = pc_history_past_len(state->history);
    CHECK(call(app, "ui.set", "{\"tool\":\"Eyedropper\",\"eyedropperSampleSize\":1}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":0,\"y\":0},{\"kind\":\"up\",\"x\":0,\"y\":0}]}",
               NULL) == PC_OK);
    CHECK(pc_engine_session_tools(session)->foreground[0] == 1 &&
          pc_engine_session_tools(session)->foreground[2] == 0);
    CHECK(call(app, "ui.pointer",
               "{\"alt\":true,\"events\":[{\"kind\":\"down\",\"x\":1,\"y\":0},{\"kind\":\"up\",\"x\":1,\"y\":"
               "0}]}",
               NULL) == PC_OK);
    CHECK(pc_engine_session_tools(session)->background[2] == 1);
    CHECK(call(app, "ui.set", "{\"tool\":\"Brush\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer", "{\"alt\":true,\"events\":[{\"kind\":\"down\",\"x\":0,\"y\":0}]}", NULL) ==
          PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"move\",\"x\":1,\"y\":0},{\"kind\":\"up\",\"x\":1,\"y\":0}]}",
               NULL) == PC_OK);
    CHECK(pc_engine_session_tools(session)->foreground[2] == 1);
    CHECK(pc_history_past_len(state->history) == history);
    CHECK(call(app, "ui.set", "{\"tool\":\"ColorSampler\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":0,\"y\":0},{\"kind\":\"up\",\"x\":0,\"y\":0}]}",
               NULL) == PC_OK);
    CHECK(state->ncolor_samplers == 1 && pc_history_past_len(state->history) == history);
    CHECK(call(app, "ui.pointer", "{\"events\":[{\"kind\":\"down\",\"x\":0,\"y\":0}]}", NULL) == PC_OK);
    CHECK(pc_ui_shell_sampling_runtime(app)->active);
    CHECK(call(app, "ui.key", "{\"key\":\"Escape\"}", NULL) == PC_OK);
    CHECK(!pc_ui_shell_sampling_runtime(app)->active);
    pc_buf inspect = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", "{}", &inspect) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&inspect), "\"sampling\":");
    pc_buf_fini(&inspect);
    pc_ui_shell_app_free(app);
}
static void test_layer_picker_control_overrides_hand_tool_and_prunes(void)
{
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"file.new\",\"params\":{\"width\":20,\"height\":20}}",
               NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"edit.fill\",\"params\":{\"color\":[0.6,0.5,0.4]}}",
               NULL) == PC_OK);
    CHECK(call(app, "engine.execute", "{\"command\":\"layer.newAdjustmentLayer.levels\",\"params\":{}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"Hand\"}", NULL) == PC_OK);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.layerPicker.arm\",\"params\":{\"point\":\"white\"}}",
               NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->layer_picker.armed && ui->layer_picker.tool == PC_TOOL_HAND);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    size_t history = pc_history_past_len(pc_engine_session_active(session)->history);
    CHECK(call(app, "ui.pointer",
               "{\"events\":[{\"kind\":\"down\",\"x\":3,\"y\":3},{\"kind\":\"move\",\"x\":4,\"y\":4},{"
               "\"kind\":\"up\",\"x\":4,\"y\":4}]}",
               NULL) == PC_OK);
    CHECK(pc_history_past_len(pc_engine_session_active(session)->history) == history + 1 &&
          ui->layer_picker.armed);
    pc_buf inspect = PC_BUF_INIT;
    CHECK(call(app, "ui.inspect", "{}", &inspect) == PC_OK);
    CHECK_CONTAINS(pc_buf_cstr(&inspect), "\"layerPicker\":{");
    CHECK_CONTAINS(pc_buf_cstr(&inspect), "\"point\":\"white\"");
    pc_buf_fini(&inspect);
    CHECK(call(app, "ui.key", "{\"key\":\"Escape\"}", NULL) == PC_OK && !ui->layer_picker.armed);
    CHECK(call(app, "ui.menu.invoke", "{\"id\":\"ui.layerPicker.arm\",\"params\":{\"point\":\"black\"}}",
               NULL) == PC_OK);
    CHECK(call(app, "ui.set", "{\"tool\":\"Move\"}", NULL) == PC_OK && !ui->layer_picker.armed);
    CHECK(call(app, "ui.set", "{\"tool\":\"Hand\"}", NULL) == PC_OK && !ui->layer_picker.armed);
    pc_ui_shell_app_free(app);
}

/* ---- U10 Save for Web (Legacy) + Print dialogs (web_export_ui.c / print_ui.c) ---- */

static pc_ui_shell_app *wp_app(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    if (pc_ui_shell_app_new(&a, &err) != PC_OK) {
        return NULL;
    }
    pc_document doc;
    if (pc_document_with_background(&doc, "web", pc_size_make(64, 48), PC_COLOR_MODE_RGB,
                                    PC_SAMPLE_TYPE_F32, pc_color_rgb(0.5f, 0.6f, 0.7f), &err) != PC_OK) {
        pc_ui_shell_app_free(a);
        return NULL;
    }
    size_t index;
    if (pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, &err) != PC_OK) {
        pc_document_fini(&doc);
        pc_ui_shell_app_free(a);
        return NULL;
    }
    return a;
}

static uint64_t wp_last_dialog(pc_ui_shell_app *a)
{
    pc_ui_state *s = pc_ui_shell_app_state(a);
    return s->n_dialogs ? s->dialogs[s->n_dialogs - 1].id : 0;
}

static const char *wp_field(pc_ui_shell_app *a, uint64_t id, const char *key)
{
    pc_ui_state *s = pc_ui_shell_app_state(a);
    for (size_t i = 0; i < s->n_dialogs; i++) {
        if (s->dialogs[i].id == id) {
            return yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->dialogs[i].fields), key));
        }
    }
    return NULL;
}

static bool wp_starts_with(const char *path, const char *magic, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char buf[16] = {0};
    size_t got = fread(buf, 1, n < sizeof buf ? n : sizeof buf, f);
    fclose(f);
    return got == n && memcmp(buf, magic, n) == 0;
}

static void test_web_dialog_defaults_export_and_guard(void)
{
    pc_ui_shell_app *a = wp_app();
    CHECK(a != NULL);
    if (!a) {
        return;
    }
    CHECK(call(a, "ui.menu.invoke", "{\"command\":\"file.export.saveForWebLegacy\"}", NULL) == PC_OK);
    uint64_t id = wp_last_dialog(a);
    CHECK(id != 0);
    CHECK(wp_field(a, id, "format") && !strcmp(wp_field(a, id, "format"), "jpeg"));
    CHECK(wp_field(a, id, "palette") && !strcmp(wp_field(a, id, "palette"), "selective"));
    CHECK(wp_field(a, id, "matte") && !strcmp(wp_field(a, id, "matte"), "#ffffff"));

    char params[64];
    (void)snprintf(params, sizeof params, "{\"dialog\":%llu}", (unsigned long long)id);
    /* No destination yet: confirm reports an error, not a crash, and the dialog stays open. */
    CHECK(call(a, "ui.dialog.confirm", params, NULL) != PC_OK);
    CHECK(pc_ui_shell_app_state(a)->n_dialogs == 1);

    char path[256];
    const char *tmp = getenv("TMPDIR");
    (void)snprintf(path, sizeof path, "%s/pc-webui-%d.gif", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    char p[384];
    (void)snprintf(p, sizeof p, "{\"dialog\":%llu,\"field\":\"format\",\"value\":\"gif\"}",
                   (unsigned long long)id);
    CHECK(call(a, "ui.dialog.set", p, NULL) == PC_OK);
    (void)snprintf(p, sizeof p, "{\"dialog\":%llu,\"field\":\"path\",\"value\":\"%s\"}",
                   (unsigned long long)id, path);
    CHECK(call(a, "ui.dialog.set", p, NULL) == PC_OK);
    CHECK(call(a, "ui.dialog.confirm", params, NULL) == PC_OK);
    CHECK(wp_starts_with(path, "GIF89a", 6));
    CHECK(pc_ui_shell_app_state(a)->n_dialogs == 0);
    (void)remove(path);
    pc_ui_shell_app_free(a);
}

static void test_print_dialog_defaults_and_pdf(void)
{
    pc_ui_shell_app *a = wp_app();
    CHECK(a != NULL);
    if (!a) {
        return;
    }
    CHECK(call(a, "ui.menu.invoke", "{\"command\":\"file.print\"}", NULL) == PC_OK);
    uint64_t id = wp_last_dialog(a);
    CHECK(id != 0);
    CHECK(wp_field(a, id, "paper") && !strcmp(wp_field(a, id, "paper"), "letter"));
    CHECK(wp_field(a, id, "colorHandling") &&
          !strcmp(wp_field(a, id, "colorHandling"), "printerManages"));

    char path[256];
    const char *tmp = getenv("TMPDIR");
    (void)snprintf(path, sizeof path, "%s/pc-printui-%d.pdf", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    char p[384];
    (void)snprintf(p, sizeof p, "{\"dialog\":%llu,\"field\":\"output\",\"value\":\"%s\"}",
                   (unsigned long long)id, path);
    CHECK(call(a, "ui.dialog.set", p, NULL) == PC_OK);
    char params[64];
    (void)snprintf(params, sizeof params, "{\"dialog\":%llu}", (unsigned long long)id);
    /* Output to a file avoids any spooler dependency. */
    pc_status rc = call(a, "ui.dialog.confirm", params, NULL);
    CHECK(rc == PC_OK);
    if (rc == PC_OK) {
        CHECK(wp_starts_with(path, "%PDF", 4));
    }
    (void)remove(path);
    pc_ui_shell_app_free(a);
}

/* GL render smoke: both dialog bodies and every view render offscreen without crashing (Rule 6). */
static void test_gl_web_print_surfaces(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = wp_app();
    CHECK(a != NULL);
    if (!a) {
        SDL_Quit();
        return;
    }
    pc_error err;
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);

    CHECK(call(a, "ui.menu.invoke", "{\"command\":\"file.export.saveForWebLegacy\"}", NULL) == PC_OK);
    uint64_t id = wp_last_dialog(a);
    const char *views[] = {"original", "optimized", "2up", "4up"};
    for (size_t i = 0; i < sizeof views / sizeof *views; i++) {
        char p[128];
        (void)snprintf(p, sizeof p, "{\"dialog\":%llu,\"field\":\"__view\",\"value\":\"%s\"}",
                       (unsigned long long)id, views[i]);
        (void)call(a, "ui.dialog.set", p, NULL);
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        pc_status rc = pc_ui_shell_app_capture(a, &rgba, &w, &h, &err);
        if (rc != PC_OK) {
            PC_T_FAIL("web capture %s: %s", views[i], err.msg);
        }
        CHECK(rc == PC_OK);
        free(rgba);
    }
    char cancel[64];
    (void)snprintf(cancel, sizeof cancel, "{\"dialog\":%llu}", (unsigned long long)id);
    (void)call(a, "ui.dialog.cancel", cancel, NULL);

    CHECK(call(a, "ui.menu.invoke", "{\"command\":\"file.print\"}", NULL) == PC_OK);
    id = wp_last_dialog(a);
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    char p[128];
    (void)snprintf(p, sizeof p,
                   "{\"dialog\":%llu,\"field\":\"colorHandling\",\"value\":\"photocraftManages\"}",
                   (unsigned long long)id);
    (void)call(a, "ui.dialog.set", p, NULL);
    (void)snprintf(p, sizeof p, "{\"dialog\":%llu,\"field\":\"center\",\"value\":false}",
                   (unsigned long long)id);
    (void)call(a, "ui.dialog.set", p, NULL);
    rgba = NULL;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);

    pc_ui_shell_app_free(a);
    SDL_Quit();
}

/* Invoke a shell command with JSON params text (NULL = none). */
static pc_status invoke_json(pc_ui_shell_app *a, const char *id, const char *params_json, pc_error *err)
{
    yyjson_doc *pdoc = NULL;
    yyjson_val *params = NULL;
    if (params_json != NULL) {
        if (pc_json_parse(params_json, strlen(params_json), &pdoc, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        params = yyjson_doc_get_root(pdoc);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_ui_shell_app_invoke(a, id, params, &out, err);
    pc_json_value_fini(&out);
    yyjson_doc_free(pdoc);
    return rc;
}

/* U3: window.panel.move re-parents a panel across dock groups (cross-group drag's command path);
 * invalid requests are atomic errors, never crashes, and never silently customize the layout. */
static void test_panel_move_command(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_ui_state *st = pc_ui_shell_app_state(a);
    CHECK(invoke_json(a, "window.panel.move", NULL, &err) != PC_OK);
    CHECK(invoke_json(a, "window.panel.move", "{\"panel\":\"layers\"}", &err) != PC_OK);
    CHECK(invoke_json(a, "window.panel.move", "{\"panel\":\"bogus\",\"anchor\":\"layers\"}", &err) != PC_OK);
    CHECK(invoke_json(a, "window.panel.move", "{\"panel\":\"layers\",\"anchor\":\"layers\"}", &err) != PC_OK);
    CHECK(invoke_json(a, "window.panel.move",
                      "{\"panel\":\"layers\",\"anchor\":\"properties\",\"before\":\"nope\"}", &err) != PC_OK);
    CHECK(!st->dock.customized);
    CHECK(invoke_json(a, "window.panel.move",
                      "{\"panel\":\"swatches\",\"anchor\":\"layers\",\"before\":\"channels\"}", &err) == PC_OK);
    CHECK(st->dock.customized);
    pc_ui_group g;
    CHECK(pc_ui_dock_label_group(&st->dock, "Swatches", false, &g) && g == PC_UI_GROUP_LAYERS);
    const char *labels[PC_UI_DOCK_MAX_MEMBERS];
    size_t n = pc_ui_dock_group_labels(&st->dock, PC_UI_GROUP_LAYERS, false, labels);
    CHECK(n == 4 && strcmp(labels[1], "Swatches") == 0 && strcmp(labels[2], "Channels") == 0);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(st, doc, &root, NULL) == PC_OK);
    yyjson_mut_doc_set_root(doc, root);
    pc_buf buf = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &buf, NULL) == PC_OK);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&buf), strlen(pc_buf_cstr(&buf)), &parsed, NULL) == PC_OK);
    pc_ui_state s2;
    pc_ui_state_init(&s2);
    CHECK(pc_ui_state_from_json(&s2, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    CHECK(s2.dock.customized);
    CHECK(pc_ui_dock_label_group(&s2.dock, "Swatches", false, &g) && g == PC_UI_GROUP_LAYERS);
    pc_ui_state_fini(&s2);
    yyjson_doc_free(parsed);
    pc_buf_fini(&buf);
    yyjson_mut_doc_free(doc);
    pc_ui_shell_app_free(a);
}

/* U3: window.panel.float / window.panel.dock mutate the dock float model; bad input is a graceful
 * error, never a crash. */
static void test_panel_float_command(void)
{
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    pc_ui_state *st = pc_ui_shell_app_state(a);
    /* Bad input: missing / unknown / wrong-typed panel are errors, not crashes. */
    CHECK(invoke_json(a, "window.panel.float", NULL, &err) != PC_OK);
    CHECK(invoke_json(a, "window.panel.float", "{\"panel\":\"bogus\"}", &err) != PC_OK);
    CHECK(invoke_json(a, "window.panel.float", "{\"panel\":42}", &err) != PC_OK);
    CHECK(invoke_json(a, "window.panel.dock", "{}", &err) != PC_OK);
    /* Float the Properties group with an explicit rect. */
    CHECK(invoke_json(a, "window.panel.float",
                      "{\"panel\":\"properties\",\"x\":40,\"y\":30,\"width\":260,\"height\":300}",
                      &err) == PC_OK);
    CHECK(pc_ui_dock_is_floating(&st->dock, PC_UI_GROUP_PROPERTIES));
    CHECK(st->panels.properties); /* the group is shown */
    CHECK(st->panels.dock);
    float r[4];
    CHECK(pc_ui_dock_float_rect(&st->dock, PC_UI_GROUP_PROPERTIES, r));
    CHECK(r[0] == 40 && r[1] == 30 && r[2] == 260 && r[3] == 300);
    /* Floating a second panel floats its (different) group; same group floats once. */
    CHECK(invoke_json(a, "window.panel.float", "{\"panel\":\"navigator\"}", &err) == PC_OK);
    CHECK(invoke_json(a, "window.panel.float", "{\"panel\":\"info\"}", &err) == PC_OK); /* same group */
    CHECK(st->dock.n_floating == 2);
    /* Re-dock the Properties group via a sibling tab. */
    CHECK(invoke_json(a, "window.panel.dock", "{\"panel\":\"adjustments\"}", &err) == PC_OK);
    CHECK(!pc_ui_dock_is_floating(&st->dock, PC_UI_GROUP_PROPERTIES));
    CHECK(st->dock.n_floating == 1);
    /* The float state survives a UiState serde round-trip. */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(st, doc, &root, NULL) == PC_OK);
    yyjson_mut_doc_set_root(doc, root);
    pc_buf buf = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &buf, NULL) == PC_OK);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&buf), strlen(pc_buf_cstr(&buf)), &parsed, NULL) == PC_OK);
    pc_ui_state s2;
    pc_ui_state_init(&s2);
    CHECK(pc_ui_state_from_json(&s2, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    CHECK(pc_ui_dock_is_floating(&s2.dock, PC_UI_GROUP_NAVIGATOR));
    CHECK(!pc_ui_dock_is_floating(&s2.dock, PC_UI_GROUP_PROPERTIES));
    pc_ui_state_fini(&s2);
    yyjson_doc_free(parsed);
    pc_buf_fini(&buf);
    yyjson_mut_doc_free(doc);
    pc_ui_shell_app_free(a);
}

/* GL: rendering floating dock windows (incl. the all-floated empty-column edge) never crashes. */
static void test_gl_floating_dock(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    pc_error err;
    CHECK(pc_ui_shell_app_new(&a, &err) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, &err) == PC_OK);
    pc_ui_state *st = pc_ui_shell_app_state(a);
    CHECK(invoke_json(a, "window.panel.float", "{\"panel\":\"properties\"}", &err) == PC_OK);
    CHECK(invoke_json(a, "window.panel.float", "{\"panel\":\"navigator\"}", &err) == PC_OK);
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    CHECK(w > 0 && h > 0);
    free(rgba);
    /* Float every group: the dock column goes empty — must still render. */
    const char *all[6] = {"color", "properties", "character", "navigator", "history", "layers"};
    for (int i = 0; i < 6; i++) {
        char p[64];
        (void)snprintf(p, sizeof p, "{\"panel\":\"%s\"}", all[i]);
        CHECK(invoke_json(a, "window.panel.float", p, &err) == PC_OK);
    }
    rgba = NULL;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    /* Re-dock everything. */
    for (int i = 0; i < 6; i++) {
        char p[64];
        (void)snprintf(p, sizeof p, "{\"panel\":\"%s\"}", all[i]);
        CHECK(invoke_json(a, "window.panel.dock", p, &err) == PC_OK);
    }
    CHECK(st->dock.n_floating == 0);
    rgba = NULL;
    CHECK(pc_ui_shell_app_capture(a, &rgba, &w, &h, &err) == PC_OK);
    free(rgba);
    pc_ui_shell_app_free(a);
}

int main(int argc, char **argv)
{
    RUN(test_layer_picker_control_overrides_hand_tool_and_prunes);
    RUN(test_eyedropper_and_sampler_control_serving);
    RUN(test_crop_whr_and_image_size_honor_interpolation_preference);
    RUN(test_selection_pointer_handoff_outline_pixels_and_keys);
    RUN(test_artboard_control_gesture_fit_orientation_and_new_form);
    RUN(test_persistent_lasso_control_routing_and_history);
    RUN(test_slice_control_options_auto_promotion_and_validation);
    RUN(test_crop_default_ratio_cancel_and_original_are_not_edits);
    RUN(test_snap_control_crop_handle_grab_offset_and_override);
    RUN(test_crop_pending_frame_local_undo_cancel_size_and_space);
    RUN(test_context_view_print_fill_and_text_selection_style);
    RUN(test_context_control_scope_selection_transform_pasteboard_and_erase);
    RUN(test_transform_control_preview_undo_redo_and_commit);
    RUN(test_system_info_readonly_api_and_english_copy_under_localization);
    RUN(test_workspaces_save_restore_lock_menu_and_dialog);
    RUN(test_nested_dialog_color_scope_cancel_and_commit);
    (void)argc;
    (void)argv;
    RUN(test_pen_control_preview_and_tool_change_finish);
    RUN(test_character_style_color_and_foreground_text_scope);
    RUN(test_type_color_picker_scope_cancel_and_stale_guard);
    RUN(test_type_key_control_and_atomic_validation);
    RUN(test_file_save_dialog_retains_document_identity);
    RUN(test_discard_stable_document_cancel_and_close_all);
    RUN(test_discard_save_child_failure_and_success);
    RUN(test_dock_control_validation_and_failed_command_draft);
    RUN(test_preferences_draft_apply_and_cancel);
    RUN(test_file_dialog_paths_and_failed_retry);
    RUN(test_dialog_control_and_menu_modality);
    RUN(test_tool_options_control_and_atomic_patches);
    RUN(test_inspect_shape);
    RUN(test_view_action_record_and_play);
    RUN(test_export_encoding_warnings_reach_native_notices);
    RUN(test_picker_control_values_and_rejected_mixed_update);
    RUN(test_ui_set_tool_and_theme);
    RUN(test_ui_set_unknown_field);
    RUN(test_menu_list_and_invoke);
    RUN(test_engine_execute_and_commands);
    RUN(test_pointer_validation);
    RUN(test_screenshot_png);
    RUN(test_unknown_method);
    RUN(test_resize);
    RUN(test_timeline_control_validation_and_state);
    RUN(test_clipboard_new_document_preset);
    RUN(test_brush_opacity_and_resize_input);
    RUN(test_gl_canvas_damage);
    RUN(test_gl_canvas);
    RUN(test_preset_float_panels_toggle);
    RUN(test_panel_move_command);
    RUN(test_panel_float_command);
    RUN(test_gl_floating_dock);
    RUN(test_gl_os_clipboard_image);
    RUN(test_gl_os_clipboard_image_in);
    RUN(test_liquify_live_proxy);
    RUN(test_gl_liquify_freeze_overlay);
    RUN(test_gl_liquify_live_proxy);
    RUN(test_gl_u7_surfaces);
    RUN(test_gl_preset_browser);
    RUN(test_gl_brush_tip_panes);
    RUN(test_web_dialog_defaults_export_and_guard);
    RUN(test_print_dialog_defaults_and_pdf);
    RUN(test_gl_web_print_surfaces);
    RUN(test_shortcuts_editor_opens_edits_and_confirms);
    RUN(test_gl_prefs_and_shortcuts_surfaces);
    RUN(test_preset_manager_opens_and_lists);
    RUN(test_presets_io_round_trip);
    RUN(test_profile_mismatch_prompt);
    RUN(test_gl_preset_and_mismatch_surfaces);
    RUN(test_display_commands);
    RUN(test_document_cameras);
    RUN(test_camera_rejects_atomically);
    RUN(test_force_cpu_rendering);
    return pc_t_finish();
}
