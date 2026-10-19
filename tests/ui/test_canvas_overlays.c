#include "engine/prefs.h"
#include "ui/shell/transform_ui.h"
#include "ui/shell/context_ui.h"
#include "ui/shell/crop_ui.h"
#include "ui/shell/lasso_ui.h"
#include "ui/shell/slice_ui.h"
#include "ui/artboard.h"
#include "engine/floating.h"
#include "ui/shell/sampling_ui.h"
#include "ui/layer_picker.h"
#include "ui/shell/brush_picker_ui.h"
#include "ui/i18n.h"
#include "base/utf8.h"
#include "text/fonts.h"
#include "ui/rasterize.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include "harness.h"
#include <stdatomic.h>
#include "ui/shell/jobs_ui.h"
#include "ui/shell/shell_internal.h"
#include "ui/canvas.h"
#include "ui/menu_tree.h"
#include "ui/menu_state.h"
#include "font.h"
#include "ui/shell/control.h"
#include "ui/shell/dialogs.h"
#include "ui/shell/type_controls.h"
#include "engine/live_stroke.h"
#include "engine/display_color.h"
#include "base/file.h"
#include "codecs/codecs.h"
#include "codecs/image.h"

static void invoke(pc_ui_shell_app *a, const char *id, const char *params)
{
    yyjson_doc *doc = NULL;
    CHECK(pc_json_parse(params, strlen(params), &doc, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error error;
    pc_status status = pc_ui_shell_app_invoke(a, id, yyjson_doc_get_root(doc), &out, &error);
    CHECK(status == PC_OK);
    if (status != PC_OK) {
        PC_T_FAIL("%s: %s", id, error.msg);
    }
    pc_json_value_fini(&out);
    yyjson_doc_free(doc);
}
static void capture(pc_ui_shell_app *a, uint8_t **pixels, int *w, int *h)
{
    CHECK(pc_ui_shell_app_capture(a, pixels, w, h, NULL) == PC_OK);
    const char *directory = getenv("PC_OVERLAY_SNAPSHOTS");
    if (directory && *pixels) {
        static unsigned index;
        char path[1024];
        (void)snprintf(path, sizeof path, "%s/overlay-%u.png", directory, index++);
        pc_cimage image;
        pc_buf png = PC_BUF_INIT;
        pc_cencode_opts opts = pc_cencode_opts_default();
        CHECK(pc_cimage_from_copy((uint32_t)*w, (uint32_t)*h, PC_CL_RGBA, PC_CS_U8, *pixels,
                                  (size_t)*w * (size_t)*h * 4, &image, NULL) == PC_OK);
        CHECK(pc_cencode(&image, PC_CF_PNG, &opts, &png, NULL) == PC_OK);
        CHECK(pc_file_write_atomic(path, png.data, png.len, NULL) == PC_OK);
        pc_buf_fini(&png);
        pc_cimage_fini(&image);
    }
}
static void button(pc_ui_shell_app *a, bool down, float x, float y)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = down;
    event.button.x = x;
    event.button.y = y;
    pc_ui_shell_input_event(a, &event);
}
static void motion(pc_ui_shell_app *a, float x, float y)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x;
    event.motion.y = y;
    pc_ui_shell_input_event(a, &event);
}
static const pc_document *document(pc_ui_shell_app *a)
{
    return pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(a))->doc);
}
static void test_native_overlays_and_guides(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, NULL) == PC_OK);
    invoke(a, "file.new", "{\"width\":640,\"height\":480}");
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(a, "view.rulers", "{\"on\":true}");
    invoke(a, "view.newGuide", "{\"orientation\":\"vertical\",\"position\":128,\"color\":\"cyan\"}");
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    ui->views[0].zoom = 1;
    ui->views[0].center[0] = 320;
    ui->views[0].center[1] = 240;
    ui->views[0].fit_pending = ui->views[0].fill_pending = false;
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(a, &pixels, &w, &h);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(a, &xf));
    float p[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 128, 100, p) == PC_OK);
    if (pixels && p[0] >= 0 && p[0] < (float)w && p[1] >= 0 && p[1] < (float)h) {
        const uint8_t *color = pixels + ((size_t)p[1] * (size_t)w + (size_t)p[0]) * 4;
        CHECK(color[1] > 200 && color[2] > 200 && color[0] < 180);
    }
    free(pixels);
    /* Drag a horizontal guide from the top ruler. Preview leaves the document unchanged. */
    button(a, true, xf.x + xf.width / 2, xf.y - 8);
    CHECK(pc_ui_canvas_to_screen(&xf, 300, 120, p) == PC_OK);
    motion(a, p[0], p[1]);
    CHECK(document(a)->guides.nhorizontal == 0);
    button(a, false, p[0], p[1]);
    CHECK(document(a)->guides.nhorizontal == 1);
    CHECK_NEAR(document(a)->guides.horizontal[0], 120, .001);
    ui->tool = PC_TOOL_MOVE;
    CHECK(pc_ui_canvas_to_screen(&xf, 128, 100, p) == PC_OK);
    button(a, true, p[0], p[1]);
    CHECK(pc_ui_canvas_to_screen(&xf, 200, 100, p) == PC_OK);
    motion(a, p[0], p[1]);
    button(a, false, p[0], p[1]);
    CHECK_NEAR(document(a)->guides.vertical[0], 200, .001);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(a)));
    CHECK_NEAR(document(a)->guides.vertical[0], 128, .001);
    /* Hidden/locked guides cannot steal Move-tool presses. */
    ui->extras.lock_guides = true;
    CHECK(pc_ui_canvas_to_screen(&xf, 128, 100, p) == PC_OK);
    button(a, true, p[0], p[1]);
    CHECK(pc_ui_canvas_to_screen(&xf, 250, 100, p) == PC_OK);
    motion(a, p[0], p[1]);
    button(a, false, p[0], p[1]);
    CHECK_NEAR(document(a)->guides.vertical[0], 128, .001);
    ui->extras.lock_guides = false;
    CHECK(pc_ui_canvas_to_screen(&xf, 128, 100, p) == PC_OK);
    button(a, true, p[0], p[1]);
    SDL_Event focus;
    SDL_zero(focus);
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    pc_ui_shell_input_event(a, &focus);
    CHECK(pc_ui_canvas_to_screen(&xf, 250, 100, p) == PC_OK);
    button(a, false, p[0], p[1]);
    CHECK_NEAR(document(a)->guides.vertical[0], 128, .001);
    invoke(a, "select.rect", "{\"x\":64,\"y\":80,\"width\":100,\"height\":80}");
    capture(a, &pixels, &w, &h);
    free(pixels);
    invoke(a, "prefs.set", "{\"path\":\"unitsAndRulers.pointSize\",\"value\":\"traditional\"}");
    invoke(a, "prefs.set", "{\"path\":\"guidesGridAndSlices.gridUnit\",\"value\":\"picas\"}");
    invoke(a, "prefs.set", "{\"path\":\"guidesGridAndSlices.gridStyle\",\"value\":\"dashedLines\"}");
    invoke(a, "view.show.grid", "{\"on\":true}");
    capture(a, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_free(a);
    SDL_Quit();
}
static void test_gl_colour_serving(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "colour", pc_size_make(100, 100), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(.25f, .25f, .25f), NULL) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    for (unsigned variant = 0; variant < 4; variant++) {
        if (variant == 1) {
            invoke(a, "view.thirtyTwoBitPreviewOptions", "{\"exposure\":1}");
        }
        if (variant == 2) {
            invoke(a, "edit.assignProfile", "{\"profile\":\"linear-srgb\"}");
            invoke(a, "view.thirtyTwoBitPreviewOptions", "{\"exposure\":-1}");
        }
        if (variant == 3) {
            invoke(a, "view.proofSetup.legacyMacintoshRgb", "{}");
        }
        uint8_t *pixels = NULL;
        int w = 0, h = 0;
        capture(a, &pixels, &w, &h);
        pc_ui_canvas_xform xf;
        CHECK(pc_ui_shell_app_canvas_mapping(a, &xf));
        size_t x = (size_t)(xf.x + xf.width / 2), y = (size_t)(xf.y + xf.height / 2);
        pc_engine_display display = {0};
        CHECK(pc_engine_display_build(pc_ui_shell_app_session(a), 33, &display, NULL) == PC_OK);
        float rgb[3] = {.25f, .25f, .25f}, expected[3];
        pc_engine_display_apply(&display, rgb, expected);
        if (pixels && x < (size_t)w && y < (size_t)h) {
            const uint8_t *p = pixels + (y * (size_t)w + x) * 4;
            for (size_t ch = 0; ch < 3; ch++) {
                CHECK_NEAR((float)p[ch] / 255, expected[ch], 2.f / 255);
            }
        }
        pc_engine_display_fini(&display);
        free(pixels);
    }
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

static void test_gl_channel_serving(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "channel", pc_size_make(16, 16), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(.25f, .5f, .75f), NULL) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(a, "channel.target", "{\"channel\":\"green\"}");
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(a, &pixels, &w, &h);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(a, &xf));
    size_t x = (size_t)(xf.x + xf.width / 2), y = (size_t)(xf.y + xf.height / 2);
    if (pixels && x < (size_t)w && y < (size_t)h) {
        const uint8_t *p = pixels + (y * (size_t)w + x) * 4;
        for (size_t ch = 0; ch < 3; ch++) {
            CHECK_NEAR((float)p[ch] / 255, .5, 2.f / 255);
        }
    }
    free(pixels);
    invoke(a, "prefs.set", "{\"path\":\"interface.showChannelsInColor\",\"value\":true}");
    capture(a, &pixels, &w, &h);
    if (pixels && x < (size_t)w && y < (size_t)h) {
        const uint8_t *p = pixels + (y * (size_t)w + x) * 4;
        CHECK_NEAR(p[0], 0, 2);
        CHECK_NEAR((float)p[1] / 255, .5, 2.f / 255);
        CHECK_NEAR(p[2], 0, 2);
    }
    free(pixels);
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

static void test_gl_tiled_detail(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "wide", pc_size_make(8192, 64), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(0, 0, 0), NULL) == PC_OK);
    float red[4] = {1, 0, 0, 1};
    for (int32_t y = 0; y < 64; y++) {
        CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[0]), 4096, y, red, 4, NULL) == PC_OK);
    }
    size_t index;
    CHECK(pc_engine_session_add_document(pc_ui_shell_app_session(a), &doc, NULL, &index, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(a, "view.actualPixels", "{}");
    pc_ui_shell_app_set_window_size(a, 4700, 400);
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    ui->views[0].zoom = 1;
    ui->views[0].center[0] = 4096;
    ui->views[0].center[1] = 32;
    ui->views[0].fit_pending = ui->views[0].fill_pending = false;
    for (unsigned frame = 0; frame < 3; frame++) {
        uint8_t *pixels = NULL;
        int w = 0, h = 0;
        capture(a, &pixels, &w, &h);
        pc_ui_canvas_xform xf;
        CHECK(pc_ui_shell_app_canvas_mapping(a, &xf));
        CHECK(xf.width > 4096);
        for (int32_t x = 4093; x < 4100; x++) {
            float p[2];
            CHECK(pc_ui_canvas_to_screen(&xf, (float)x + .5f, 16.5f, p) == PC_OK);
            CHECK(pixels && p[0] >= 0 && p[0] < (float)w && p[1] >= 0 && p[1] < (float)h);
            if (pixels && p[0] >= 0 && p[0] < (float)w && p[1] >= 0 && p[1] < (float)h) {
                const uint8_t *color = pixels + ((size_t)p[1] * (size_t)w + (size_t)p[0]) * 4;
                CHECK_NEAR(color[0], x == 4096 ? 255 : 0, 2);
                CHECK_NEAR(color[1], 0, 2);
                CHECK_NEAR(color[2], 0, 2);
            }
        }
        free(pixels);
        ui->views[0].center[0] += 128;
        if (frame == 1) {
            ui->view.flip_horizontal = true;
        }
    }
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

static void finger(pc_ui_shell_app *app, Uint32 type, SDL_FingerID id, float x, float y)
{
    float w, h;
    pc_ui_shell_app_window_size(app, &w, &h, NULL);
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    e.tfinger.touchID = 1;
    e.tfinger.fingerID = id;
    e.tfinger.x = x / w;
    e.tfinger.y = y / h;
    pc_ui_shell_input_event(app, &e);
}
static void key(pc_ui_shell_app *app, SDL_Keycode code, bool down, SDL_Keymod mods)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = code;
    e.key.down = down;
    e.key.mod = mods;
    pc_ui_shell_input_event(app, &e);
}
static void test_native_gestures_and_temporary_tools(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, NULL) == PC_OK);
    invoke(a, "file.new", "{\"width\":640,\"height\":480}");
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    uint8_t *pixels = NULL;
    int w, h;
    capture(a, &pixels, &w, &h);
    free(pixels);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(a, &xf));
    pc_ui_view *v = &pc_ui_shell_app_state(a)->views[0];
    float x = xf.x + xf.width / 2, y = xf.y + xf.height / 2;
    float before[2];
    CHECK(pc_ui_canvas_to_doc(&xf, x, y, before) == PC_OK);
    float zoom = v->zoom;
    finger(a, SDL_EVENT_FINGER_DOWN, 11, x - 50, y);
    finger(a, SDL_EVENT_FINGER_DOWN, 12, x + 50, y);
    finger(a, SDL_EVENT_FINGER_DOWN, 13, x, y + 20);
    finger(a, SDL_EVENT_FINGER_MOTION, 12, x + 100, y);
    CHECK_NEAR(v->zoom, zoom * 1.5f, 1e-5);
    pc_ui_canvas_xform next = xf;
    next.zoom = v->zoom;
    next.rotation = v->rotation;
    next.center[0] = v->center[0];
    next.center[1] = v->center[1];
    float after[2];
    CHECK(pc_ui_canvas_to_doc(&next, x + 25, y, after) == PC_OK);
    CHECK_NEAR(before[0], after[0], .001);
    CHECK_NEAR(before[1], after[1], .001);
    finger(a, SDL_EVENT_FINGER_MOTION, 12, x - 50, y + 150);
    CHECK_NEAR(v->rotation, 90, .001);
    finger(a, SDL_EVENT_FINGER_CANCELED, 12, x, y);
    zoom = v->zoom;
    finger(a, SDL_EVENT_FINGER_MOTION, 12, x + 200, y);
    CHECK_NEAR(v->zoom, zoom, 0);
    SDL_Event focus;
    SDL_zero(focus);
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    pc_ui_shell_input_event(a, &focus);
    invoke(a, "view.actualPixels", "{}");
    capture(a, &pixels, &w, &h);
    free(pixels);
    CHECK(pc_ui_shell_app_canvas_mapping(a, &xf));
    x = xf.x + xf.width / 2;
    y = xf.y + xf.height / 2;
    pc_ui_shell_app_state(a)->tool = PC_TOOL_BRUSH;
    key(a, SDLK_SPACE, true, SDL_KMOD_CTRL);
    button(a, true, x, y);
    key(a, SDLK_SPACE, false, 0);
    button(a, false, x, y);
    CHECK_NEAR(v->zoom, 2, 0);
    CHECK(pc_ui_shell_app_state(a)->tool == PC_TOOL_BRUSH);
    key(a, SDLK_SPACE, true, SDL_KMOD_ALT);
    button(a, true, x, y);
    key(a, SDLK_SPACE, false, 0);
    button(a, false, x, y);
    CHECK_NEAR(v->zoom, 1, 0);
    CHECK(pc_ui_shell_app_state(a)->tool == PC_TOOL_BRUSH);
    invoke(a, "prefs.set", "{\"path\":\"enhancedControls.zoomWithTrackpadPinch\",\"value\":false}");
    finger(a, SDL_EVENT_FINGER_DOWN, 21, x - 50, y);
    finger(a, SDL_EVENT_FINGER_DOWN, 22, x + 50, y);
    finger(a, SDL_EVENT_FINGER_MOTION, 22, x + 100, y);
    CHECK_NEAR(v->zoom, 1, 0);
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

static void test_served_gpu_fallback(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *a = NULL;
    CHECK(pc_ui_shell_app_new(&a, NULL) == PC_OK);
    invoke(a, "file.new", "{\"width\":128,\"height\":128}");
    CHECK(pc_ui_shell_app_open_window(a, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(a, "view.actualPixels", "{}");
    uint8_t *pixels = NULL;
    int w, h;
    capture(a, &pixels, &w, &h);
    free(pixels);
    pc_json_value info = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_gpu_info(a, &info, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&info);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "canvas")), "gpu");
    uint64_t submissions = yyjson_mut_get_uint(yyjson_mut_obj_get(root, "submissions"));
    CHECK(submissions > 0);
    pc_json_value_fini(&info);
    CHECK(pc_ui_shell_app_gpu_simulate_loss(a, false, &info, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&info), "wasActive")));
    pc_json_value_fini(&info);
    CHECK(pc_ui_shell_app_state(a)->gpu_fallback_notice != NULL);
    invoke(a, "view.newGuide", "{\"orientation\":\"vertical\",\"position\":10}");
    capture(a, &pixels, &w, &h);
    free(pixels);
    CHECK(pc_ui_shell_app_gpu_info(a, &info, NULL) == PC_OK);
    root = pc_json_value_root(&info);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "canvas")), "cpu");
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "submissions")) == submissions);
    CHECK(yyjson_mut_is_str(yyjson_mut_obj_get(root, "lost")));
    pc_json_value_fini(&info);
    /* Native shortcuts see this session's current override immediately. */
    invoke(a, "edit.keyboardShortcuts",
           "{\"set\":{\"view.actualPixels\":\"F6\"},\"removeConflicts\":true,\"allowUnknown\":true}");
    pc_ui_shell_app_state(a)->views[0].zoom = 4;
    key(a, SDLK_F6, true, 0);
    CHECK_NEAR(pc_ui_shell_app_state(a)->views[0].zoom, 1, 0);
    pc_ui_shell_app_free(a);
    SDL_Quit();
}

static void panel_click(pc_ui_shell_app *app, const char *command, pc_layer_id layer, float scale)
{
    float rect[4];
    bool found = pc_ui_shell_app_panel_mapping(app, command, layer, rect);
    CHECK(found);
    if (!found) {
        PC_T_FAIL("missing panel widget %s", command);
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float density = SDL_GetWindowPixelDensity(pc_ui_shell_app_sdl_window(app));
    float x = (rect[0] + rect[2] / 2) * scale / density;
    float y = (rect[1] + rect[3] / 2) * scale / density;
    for (unsigned frame = 0; frame < 3; frame++) {
        nk_input_begin(ctx);
        if (frame < 2) {
            motion(app, x, y);
            button(app, frame == 0, x, y);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, (int)(1280 * scale), (int)(800 * scale), NULL) == PC_OK);
    }
}
static void test_native_layer_panels(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    const float scales[] = {1, 1.5f, 2};
    for (size_t i = 0; i < 3; i++) {
        CHECK(SDL_Init(SDL_INIT_VIDEO));
        pc_ui_shell_app *app = NULL;
        CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
        invoke(app, "file.new", "{\"width\":64,\"height\":64}");
        invoke(app, "layer.new.layer", "{\"name\":\"child\"}");
        pc_layer_id child = pc_engine_session_active(pc_ui_shell_app_session(app))->active_layer;
        invoke(app, "layer.groupLayers", "{\"name\":\"group\"}");
        pc_layer_id group = pc_engine_session_active(pc_ui_shell_app_session(app))->active_layer;
        pc_ui_state *ui = pc_ui_shell_app_state(app);
        ui->panels.history = ui->panels.properties = ui->panels.color = ui->panels.navigator =
            ui->panels.character = false;
        CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", scales[i], true, NULL) ==
              PC_OK);
        uint8_t *pixels = NULL;
        int w, h;
        capture(app, &pixels, &w, &h);
        free(pixels);
        panel_click(app, "layer.select", child, scales[i]);
        CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->active_layer == child);
        uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
        panel_click(app, "layer.visible", child, scales[i]);
        CHECK(!pc_document_layer(document(app), child)->visible);
        CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision + 1);
        panel_click(app, "layer.setExpanded", group, scales[i]);
        CHECK(!pc_document_layer(document(app), group)->content.u.group.expanded);
        CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->active_layer == child);
        float rect[4];
        CHECK(!pc_ui_shell_app_panel_mapping(app, "layer.select", child, rect));
        panel_click(app, "layer.setExpanded", group, scales[i]);
        CHECK(pc_document_layer(document(app), group)->content.u.group.expanded);
        size_t layers = document(app)->nlayers;
        panel_click(app, "layer.new.layer", 0, scales[i]);
        // New layers land next to the active child inside its group.
        CHECK(document(app)->nlayers == layers);
        CHECK(pc_document_layer(document(app), group)->content.u.group.nchildren == 2);
        capture(app, &pixels, &w, &h);
        free(pixels);
        pc_ui_shell_app_free(app);
        SDL_Quit();
    }
}

static size_t history_current(pc_ui_shell_app *app)
{
    pc_json_value info = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_inspect_history(pc_engine_session_active(pc_ui_shell_app_session(app)), &info, NULL) ==
          PC_OK);
    size_t current = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&info), "current"));
    pc_json_value_fini(&info);
    return current;
}
static void test_native_history_panel(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    invoke(app, "layer.new.layer", "{\"name\":\"Paint\"}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    invoke(app, "edit.fill", "{\"color\":\"#00ff00\"}");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->panels.layers = ui->panels.properties = ui->panels.color = ui->panels.navigator =
        ui->panels.character = false;
    ui->panels.history = true;
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    CHECK(history_current(app) == 3);
    panel_click(app, "history.selectState", 1, 1);
    CHECK(history_current(app) == 1);
    panel_click(app, "history.selectState", 3, 1);
    CHECK(history_current(app) == 3);
    panel_click(app, "history.newSnapshot", 0, 1);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->nsnapshots == 1 &&
          history_current(app) == 3);
    panel_click(app, "history.selectState", 0, 1);
    CHECK(document(app)->nlayers == 1);
    panel_click(app, "history.selectSnapshot", 1, 1);
    CHECK(document(app)->nlayers == 2 && history_current(app) == 1);
    panel_click(app, "history.deleteSnapshot", 1, 1);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->nsnapshots == 0 &&
          history_current(app) == 1);
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_dialog_forms(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    const float scales[] = {1, 1.5f, 2};
    for (size_t n = 0; n < 3; n++) {
        CHECK(SDL_Init(SDL_INIT_VIDEO));
        pc_ui_shell_app *app = NULL;
        CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
        CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", scales[n], true, NULL) ==
              PC_OK);
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(pc_ui_shell_app_invoke(app, "file.new", NULL, &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        pc_ui_state *ui = pc_ui_shell_app_state(app);
        CHECK(ui->n_dialogs == 1);
        uint8_t *pixels = NULL;
        int w = 0, h = 0;
        capture(app, &pixels, &w, &h);
        free(pixels);
        float rect[4];
        CHECK(pc_ui_shell_app_dialog_mapping(app, true, rect));
        float density = SDL_GetWindowPixelDensity(pc_ui_shell_app_sdl_window(app));
        float x = (rect[0] + rect[2] / 2) * scales[n] / density;
        float y = (rect[1] + rect[3] / 2) * scales[n] / density;
        struct nk_context *ctx = pc_ui_shell_app_nk(app);
        for (unsigned f = 0; f < 3; f++) {
            nk_input_begin(ctx);
            if (f < 2) {
                motion(app, x, y);
                button(app, f == 0, x, y);
            }
            nk_input_end(ctx);
            CHECK(pc_ui_shell_app_render_frame(app, (int)(1280 * scales[n]), (int)(800 * scales[n]), NULL) ==
                  PC_OK);
        }
        CHECK(ui->n_dialogs == 0);
        CHECK(pc_engine_session_active(pc_ui_shell_app_session(app)) != NULL);
        yyjson_doc *request = NULL;
        const char *json = "{\"kind\":\"colorPicker\",\"fields\":{\"color\":\"#39a6cd\"}}";
        CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
        CHECK(pc_ui_control_handle(app, "ui.dialog.open", yyjson_doc_get_root(request), &out, NULL) == PC_OK);
        yyjson_doc_free(request);
        pc_json_value_fini(&out);
        capture(app, &pixels, &w, &h);
        free(pixels);
        nk_input_begin(ctx);
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_KEY_DOWN;
        e.key.key = SDLK_ESCAPE;
        CHECK(ui->n_dialogs == 1);
        pc_ui_shell_input_event(app, &e);
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, (int)(1280 * scales[n]), (int)(800 * scales[n]), NULL) ==
              PC_OK);
        CHECK(ui->n_dialogs == 0);
        pc_ui_shell_app_free(app);
        SDL_Quit();
    }
}

static void test_native_brush_preview_commit(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    invoke(app, "tools.setBrush", "{\"size\":8,\"pressureSize\":false,\"smoothing\":{\"enabled\":false}}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_BRUSH;
    ui->views[0].zoom = 4;
    ui->views[0].center[0] = ui->views[0].center[1] = 32;
    ui->views[0].fit_pending = ui->views[0].fill_pending = false;
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float first[2], last[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 20, 20, first) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 40, 30, last) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    nk_input_begin(ctx);
    motion(app, first[0], first[1]);
    button(app, true, first[0], first[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    motion(app, last[0], last[1]);
    nk_input_end(ctx);
    capture(app, &pixels, &w, &h);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    size_t index = ((size_t)first[1] * (size_t)w + (size_t)first[0]) * 4;
    CHECK(pixels && pixels[index] < 30 && pixels[index + 1] < 30 && pixels[index + 2] < 30);
    free(pixels);
    nk_input_begin(ctx);
    button(app, false, last[0], last[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision + 1);
    CHECK(pc_engine_session_live_stroke_reuses(pc_ui_shell_app_session(app)) == 1);
    capture(app, &pixels, &w, &h);
    CHECK(pixels && pixels[index] < 30);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_pen_pressure_and_duplicate_mouse(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    invoke(app, "tools.setBrush",
           "{\"size\":12,\"pressureSize\":false,\"pressureOpacity\":true,\"smoothing\":{\"enabled\":false}}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_BRUSH;
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float at[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 32, 32, at) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_PEN_AXIS;
    e.paxis.which = 12;
    e.paxis.axis = SDL_PEN_AXIS_PRESSURE;
    e.paxis.x = at[0];
    e.paxis.y = at[1];
    e.paxis.value = 0;
    nk_input_begin(ctx);
    pc_ui_shell_input_event(app, &e);
    SDL_zero(e);
    e.type = SDL_EVENT_PEN_DOWN;
    e.ptouch.which = 12;
    e.ptouch.x = at[0];
    e.ptouch.y = at[1];
    e.ptouch.down = true;
    pc_ui_shell_input_event(app, &e);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    SDL_zero(e);
    e.type = SDL_EVENT_PEN_AXIS;
    e.paxis.which = 12;
    e.paxis.axis = SDL_PEN_AXIS_PRESSURE;
    e.paxis.x = at[0] + 20;
    e.paxis.y = at[1];
    e.paxis.value = 1;
    nk_input_begin(ctx);
    pc_ui_shell_input_event(app, &e);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    SDL_zero(e);
    e.type = SDL_EVENT_PEN_UP;
    e.ptouch.which = 12;
    e.ptouch.x = at[0] + 20;
    e.ptouch.y = at[1];
    nk_input_begin(ctx);
    pc_ui_shell_input_event(app, &e);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision + 1);
    CHECK(pc_engine_session_live_stroke_reuses(pc_ui_shell_app_session(app)) == 1);
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.which = SDL_PEN_MOUSEID;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = true;
    e.button.x = at[0];
    e.button.y = at[1];
    nk_input_begin(ctx);
    pc_ui_shell_input_event(app, &e);
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.down = false;
    pc_ui_shell_input_event(app, &e);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision + 1);
    CHECK(ui->tool == PC_TOOL_BRUSH);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_preferences_sections(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    const char *sections[] = {"general",
                              "interface",
                              "workspace",
                              "tools",
                              "historyLog",
                              "fileHandling",
                              "export",
                              "performance",
                              "scratchDisks",
                              "cursors",
                              "transparencyAndGamut",
                              "unitsAndRulers",
                              "guidesGridAndSlices",
                              "plugIns",
                              "type",
                              "enhancedControls",
                              "rawDefaults",
                              "integrations"};
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    for (size_t i = 0; i < sizeof sections / sizeof *sections; i++) {
        char command[128];
        (void)snprintf(command, sizeof command, "edit.preferences.%s", sections[i]);
        pc_json_value result = PC_JSON_VALUE_INIT;
        CHECK(pc_ui_shell_app_invoke(app, command, NULL, &result, NULL) == PC_OK);
        pc_json_value_fini(&result);
        pc_ui_state *ui = pc_ui_shell_app_state(app);
        CHECK(ui->n_dialogs == 1);
        uint8_t *pixels = NULL;
        int w = 0, h = 0;
        capture(app, &pixels, &w, &h);
        free(pixels);
        float rect[4];
        CHECK(pc_ui_shell_app_dialog_action_mapping(app, "apply", rect));
        if (!strcmp(sections[i], "interface")) {
            yyjson_doc *request = NULL;
            char json[256];
            (void)snprintf(
                json, sizeof json,
                "{\"dialog\":%llu,\"field\":\"values\",\"value\":{\"interface\":{\"theme\":\"studio\"}}}",
                (unsigned long long)ui->dialogs[0].id);
            CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
            CHECK(pc_ui_control_handle(app, "ui.dialog.set", yyjson_doc_get_root(request), &result, NULL) ==
                  PC_OK);
            yyjson_doc_free(request);
            pc_json_value_fini(&result);
            struct nk_context *ctx = pc_ui_shell_app_nk(app);
            for (unsigned frame = 0; frame < 3; frame++) {
                nk_input_begin(ctx);
                if (frame < 2) {
                    motion(app, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2);
                    button(app, frame == 0, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2);
                }
                nk_input_end(ctx);
                CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
            }
            CHECK(ui->theme == PC_UI_THEME_STUDIO && ui->n_dialogs == 1);
        }
        CHECK(pc_ui_state_close_dialog(ui, ui->dialogs[0].id));
    }
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void click_widget(pc_ui_shell_app *app, const char *command)
{
    float r[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, command, 0, r));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    for (unsigned frame = 0; frame < 3; frame++) {
        nk_input_begin(ctx);
        if (frame < 2) {
            motion(app, r[0] + r[2] / 2, r[1] + r[3] / 2);
            button(app, frame == 0, r[0] + r[2] / 2, r[1] + r[3] / 2);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
}
static void test_native_toolbar_and_panel_routes(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    click_widget(app, "tools.select.brush");
    CHECK(ui->tool == PC_TOOL_BRUSH);
    click_widget(app, "window.toolbar.double");
    CHECK(ui->panels.toolbar_double);
    click_widget(app, "color.foreground");
    CHECK(ui->n_dialogs == 1);
    CHECK(pc_ui_state_close_dialog(ui, ui->dialogs[0].id));
    static const struct {
        const char *id;
        size_t *unused;
    } routes[] = {{"window.panel.channels", NULL},
                  {"window.panel.actions", NULL},
                  {"window.panel.swatches", NULL},
                  {"window.panel.paragraph", NULL}};
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        invoke(app, routes[i].id, "{}");
        uint8_t *pixels = NULL;
        int w = 0, h = 0;
        capture(app, &pixels, &w, &h);
        free(pixels);
    }
    CHECK(ui->dock_tabs.layers == 1 && ui->dock_tabs.history == 1 && ui->dock_tabs.color == 1 &&
          ui->dock_tabs.character == 1);
    invoke(app, "window.togglePanels", "{}");
    CHECK(!ui->panels.toolbar && !ui->panels.options_bar && !ui->panels.dock);
    invoke(app, "window.togglePanels", "{}");
    CHECK(ui->panels.toolbar && ui->panels.options_bar && ui->panels.dock);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void menu_key(pc_ui_shell_app *app, SDL_Keycode key)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.down = true;
    nk_input_begin(ctx);
    pc_ui_shell_input_event(app, &event);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
}
static void test_native_nested_menu_keyboard_and_modal(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    menu_key(app, SDLK_F10);
    CHECK(pc_ui_shell_app_nav(app)->top == 0);
    menu_key(app, SDLK_DOWN);
    CHECK(pc_ui_shell_app_nav(app)->hi[0] == 0);
    menu_key(app, SDLK_RETURN);
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 1);
    if (!pc_ui_shell_app_state(app)->n_dialogs) {
        pc_ui_shell_app_free(app);
        SDL_Quit();
        return;
    }
    CHECK(pc_ui_state_close_dialog(pc_ui_shell_app_state(app), pc_ui_shell_app_state(app)->dialogs[0].id));
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "menu.Edit");
    CHECK(pc_ui_shell_app_nav(app)->top == 1);
    pc_ui_menu_item *items = NULL;
    size_t count = 0;
    CHECK(pc_ui_menu_state_items(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app), &items, &count,
                                 NULL) == PC_OK);
    const char *path[3] = {"Edit", NULL, NULL};
    pc_ui_tree_row rows[PC_UI_MENU_ROWS];
    size_t n = 0;
    CHECK(pc_ui_menu_tree_rows(items, count, path, 1, rows, PC_UI_MENU_ROWS, &n, NULL) == PC_OK);
    size_t preferences = n;
    for (size_t i = 0; i < n; i++) {
        if (rows[i].submenu && !strcmp(rows[i].label, "Preferences")) {
            preferences = i;
        }
    }
    CHECK(preferences < n);
    /* Navigate only enabled rows, exactly as a real keyboard user does. */
    size_t steps = 0;
    for (size_t i = 0; i <= preferences && i < n; i++) {
        steps += rows[i].enabled;
    }
    for (size_t i = 0; i < steps; i++) {
        menu_key(app, SDLK_DOWN);
    }
    CHECK(pc_ui_shell_app_nav(app)->hi[0] == (int)preferences);
    menu_key(app, SDLK_RIGHT);
    CHECK(pc_ui_shell_app_nav(app)->level == 1);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    menu_key(app, SDLK_DOWN);
    menu_key(app, SDLK_RETURN);
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 1 && !pc_ui_menu_nav_is_open(pc_ui_shell_app_nav(app)));
    pc_ui_menu_items_free(items, count);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void drag_frames(pc_ui_shell_app *app, float x, float y, float dx, float dy, bool cancel)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    for (unsigned frame = 0; frame < 3; frame++) {
        nk_input_begin(ctx);
        if (frame == 0) {
            motion(app, x, y);
            button(app, true, x, y);
        } else if (frame == 1) {
            motion(app, x + dx, y + dy);
        } else if (cancel) {
            SDL_Event e;
            SDL_zero(e);
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.key = SDLK_ESCAPE;
            e.key.down = true;
            pc_ui_shell_input_event(app, &e);
            button(app, false, x + dx, y + dy);
        } else {
            button(app, false, x + dx, y + dy);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
}
static void test_native_dock_resize_reorder_and_cancel(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    float rect[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "dock.width", 0, rect));
    drag_frames(app, rect[0] + rect[2] / 2, rect[1] + 100, -60, 0, false);
    CHECK(ui->dock_width == 360);
    CHECK(pc_ui_shell_app_panel_mapping(app, "dock.width", 0, rect));
    drag_frames(app, rect[0] + rect[2] / 2, rect[1] + 100, -40, 0, true);
    CHECK(ui->dock_width == 360);
    CHECK(pc_ui_shell_app_panel_mapping(app, "dock.split.color", 0, rect));
    drag_frames(app, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2, 0, 25, false);
    CHECK(ui->dock.has_height[PC_UI_GROUP_COLOR]);
    float before = ui->dock.heights[PC_UI_GROUP_COLOR];
    CHECK(pc_ui_shell_app_panel_mapping(app, "dock.split.color", 0, rect));
    drag_frames(app, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2, 0, 15, true);
    CHECK(ui->dock.heights[PC_UI_GROUP_COLOR] == before);
    float color[4], layers[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "dock.tab.Color", 0, color));
    CHECK(pc_ui_shell_app_panel_mapping(app, "dock.tab.Layers", 0, layers));
    float x = color[0] + color[2] / 2, y = color[1] + color[3] / 2;
    drag_frames(app, x, y, layers[0] - color[0], layers[1] - color[1] + 60, false);
    pc_ui_group order[PC_UI_GROUP_N];
    pc_ui_dock_order(&ui->dock, order);
    size_t color_index = 0, properties_index = 0, layers_index = 0;
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        if (order[i] == PC_UI_GROUP_COLOR) {
            color_index = i;
        }
        if (order[i] == PC_UI_GROUP_PROPERTIES) {
            properties_index = i;
        }
        if (order[i] == PC_UI_GROUP_LAYERS) {
            layers_index = i;
        }
    }
    CHECK(properties_index < color_index && color_index < layers_index);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_ui_font_dynamic_unicode_and_bounds(void)
{
    const float scales[] = {1, 1.5f, 2};
    for (size_t i = 0; i < 3; i++) {
        ui_font font = {0};
        CHECK(ui_font_init(&font, "assets/fonts/Inter-Regular.ttf", 14, scales[i], NULL) == PC_OK);
        CHECK(ui_font_has_glyph(&font, 'A'));
        CHECK(ui_font_has_glyph(&font, 0x416));
        CHECK(ui_font_has_glyph(&font, 0x3a9));
        size_t count = font.nglyphs;
        const char *text = "PhotoCraft \xCE\xA9 \xD0\x96";
        float width = font.nk.width(font.nk.userdata, font.nk.height, text, (int)strlen(text));
        CHECK(width > 70 && width < 180);
        for (unsigned r = 0; r < 100; r++) {
            CHECK(font.nk.width(font.nk.userdata, font.nk.height, text, (int)strlen(text)) == width);
        }
        CHECK(font.nglyphs == count && font.dirty);
        struct nk_user_font_glyph glyph = {0};
        font.nk.query(font.nk.userdata, font.nk.height, &glyph, 0x416, 0);
        CHECK(glyph.width > 0 && glyph.height > 0 && glyph.uv[0].x >= 0 && glyph.uv[1].x <= 1);
        CHECK(!ui_font_has_glyph(&font, 0x110000));
        CHECK(!ui_font_has_glyph(&font, 0xd800));
        CHECK(font.atlas_w == 2048 && font.atlas_h == 2048 && font.nglyphs <= 8192);
        ui_font_fini(&font);
        ui_font_fini(&font);
    }
    ui_font font = {0};
    CHECK(ui_font_init(&font, "assets/fonts/Inter-Regular.ttf", NAN, 1, NULL) == PC_ERR_INVALID);
    ui_font_fini(&font);
}

static void click_dialog_action(pc_ui_shell_app *app, const char *action)
{
    float rect[4];
    CHECK(pc_ui_shell_app_dialog_action_mapping(app, action, rect));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    for (unsigned frame = 0; frame < 3; frame++) {
        nk_input_begin(ctx);
        if (frame < 2) {
            motion(app, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2);
            button(app, frame == 0, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
}
static void test_native_discard_buttons_and_localized_menu(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    invoke(app, "layer.new.layer", "{}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(app, "file.close", "{}");
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 1);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_dialog_action(app, "cancel");
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 0 &&
          pc_engine_session_active(pc_ui_shell_app_session(app)));
    invoke(app, "file.close", "{}");
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_dialog_action(app, "discard");
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 0 &&
          !pc_engine_session_active(pc_ui_shell_app_session(app)));
    invoke(app, "prefs.set", "{\"path\":\"interface.language\",\"value\":\"ru\"}");
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_held_move_and_erase_are_latched(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64,\"background\":\"transparent\"}");
    invoke(app, "paint.pencil",
           "{\"points\":[[10,20],[20,20]],\"size\":8,\"smoothing\":0,\"color\":\"#ff0000\"}");
    invoke(app, "tools.setBrush", "{\"size\":8,\"pressureSize\":false,\"smoothing\":{\"enabled\":false}}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_BRUSH;
    ui->extras.snap = false;
    ui->view.show[4] = false; /* Isolate held-tool deltas from smart alignment on release. */
    ui->views[0].zoom = 4;
    ui->views[0].fit_pending = false;
    ui->views[0].center[0] = ui->views[0].center[1] = 32;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float start[2], end[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 12, 20, start) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 22, 25, end) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_layer_id layer = pc_engine_session_active(session)->active_layer;
    pc_rect before = pc_surface_content_bounds(&pc_document_layer(document(app), layer)->content.u.raster);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    key(app, SDLK_LCTRL, true, SDL_KMOD_CTRL);
    motion(app, start[0], start[1]);
    button(app, true, start[0], start[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    motion(app, end[0], end[1]);
    key(app, SDLK_LCTRL, false, 0);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    button(app, false, end[0], end[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_rect after = pc_surface_content_bounds(&pc_document_layer(document(app), layer)->content.u.raster);
    CHECK(after.x0 == before.x0 + 10 && after.y0 == before.y0 + 5 && ui->tool == PC_TOOL_BRUSH);
    float old[4], now[4];
    pc_pixel_format format = pc_document_pixel_format(document(app));
    pc_surface_read_pixel(&pc_document_layer(document(app), layer)->content.u.raster, 22, 25, old);
    nk_input_begin(ctx);
    key(app, SDLK_GRAVE, true, 0);
    motion(app, end[0], end[1]);
    button(app, true, end[0], end[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    key(app, SDLK_GRAVE, false, 0);
    button(app, false, end[0], end[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_surface_read_pixel(&pc_document_layer(document(app), layer)->content.u.raster, 22, 25, now);
    CHECK(now[pc_pixel_format_channels(format) - 1] < old[pc_pixel_format_channels(format) - 1] &&
          ui->tool == PC_TOOL_BRUSH);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void native_text(pc_ui_shell_app *app, const char *text, bool composition)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = composition ? SDL_EVENT_TEXT_EDITING : SDL_EVENT_TEXT_INPUT;
    if (composition) {
        event.edit.text = text;
    } else {
        event.text.text = text;
    }
    pc_ui_shell_input_event(app, &event);
}
static void test_native_type_unicode_ime_selection_and_mask(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":400,\"height\":300,\"resolution\":72}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_TYPE;
    ui->tool_options.type_size = 24;
    CHECK(SDL_TextInputActive(pc_ui_shell_app_sdl_window(app)));
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float start[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 40, 80, start) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    motion(app, start[0], start[1]);
    button(app, true, start[0], start[1]);
    button(app, false, start[0], start[1]);
    nk_input_end(ctx);
    CHECK(ui->has_text_edit);
    pc_layer_id id = ui->text_edit.layer;
    nk_input_begin(ctx);
    native_text(app, "Hello \xce\xa9 \xd0\x96", false);
    nk_input_end(ctx);
    const pc_layer *layer = pc_document_layer(document(app), id);
    CHECK(layer && layer->content.kind == PC_LAYER_TEXT);
    CHECK_EQ_STR(pc_str_c(&layer->content.u.text.text), "Hello \xce\xa9 \xd0\x96");
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    key(app, SDLK_A, true, SDL_KMOD_CTRL);
    key(app, SDLK_A, false, SDL_KMOD_CTRL);
    native_text(app, "n", true);
    native_text(app, "\xe6\x97\xa5", true);
    native_text(app, "\xe6\x97\xa5\xe6\x9c\xac", false);
    nk_input_end(ctx);
    layer = pc_document_layer(document(app), id);
    CHECK_EQ_STR(pc_str_c(&layer->content.u.text.text), "\xe6\x97\xa5\xe6\x9c\xac");
    CHECK(ui->text_edit.caret == 2 && !ui->text_edit.has_preedit);
    nk_input_begin(ctx);
    key(app, SDLK_A, true, SDL_KMOD_CTRL);
    key(app, SDLK_A, false, SDL_KMOD_CTRL);
    nk_input_end(ctx);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    nk_input_begin(ctx);
    key(app, SDLK_RETURN, true, SDL_KMOD_CTRL);
    key(app, SDLK_RETURN, false, SDL_KMOD_CTRL);
    nk_input_end(ctx);
    CHECK(!ui->has_text_edit && pc_document_layer(document(app), id));
    ui->tool = PC_TOOL_HORIZONTAL_TYPE_MASK;
    nk_input_begin(ctx);
    button(app, true, start[0], start[1]);
    button(app, false, start[0], start[1]);
    native_text(app, "MASK", false);
    nk_input_end(ctx);
    CHECK(ui->has_text_edit && ui->text_edit.mask);
    capture(app, &pixels, &w, &h);
    free(pixels);
    nk_input_begin(ctx);
    key(app, SDLK_RETURN, true, SDL_KMOD_CTRL);
    key(app, SDLK_RETURN, false, SDL_KMOD_CTRL);
    nk_input_end(ctx);
    CHECK(!ui->has_text_edit && document(app)->has_selection);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_type_font_picker_and_character_panel(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":400,\"height\":300}");
    invoke(app, "type.create", "{\"x\":40,\"y\":80,\"text\":\"Typeface\",\"size\":24}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_TYPE;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    click_widget(app, "type.font");
    click_widget(app, "type.font.search");
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    CHECK(pc_ui_shell_keyboard_captured(app));
    nk_input_begin(ctx);
    native_text(app, "DejaVu Sans", false);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_type_controls *controls = pc_ui_shell_type_controls(app);
    CHECK_EQ_STR(controls->search, "DejaVu Sans");
    CHECK(controls->nfiltered > 0 && controls->nfiltered < 100);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    uint8_t *picker_pixels = NULL;
    int picker_w, picker_h;
    capture(app, &picker_pixels, &picker_w, &picker_h);
    free(picker_pixels);
    yyjson_mut_doc *scope_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *scope = yyjson_mut_obj(scope_doc);
    CHECK(pc_ui_type_add_scope(pc_ui_shell_app_session(app), ui, scope_doc, scope, NULL) == PC_OK);
    yyjson_mut_doc_free(scope_doc);
    char id[64];
    (void)snprintf(id, sizeof id, "type.font.family.%zu", controls->filtered[0]);
    click_widget(app, id);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_layer *layer = pc_document_layer(document(app), state->active_layer);
    CHECK_CONTAINS(pc_str_c(&layer->content.u.text.font_family), "DejaVu Sans");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(app, "type.panels.character", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ui->panels.character);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    float rect[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "type.tracking", 0, rect));
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_type_styles_glyphs_and_spelling_windows(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":400,\"height\":300}");
    invoke(app, "type.create",
           "{\"x\":40,\"y\":80,\"text\":\"Styles\",\"font\":\"DejaVu Sans\",\"size\":24}");
    invoke(app, "type.characterStyle.new", "{\"name\":\"Heading\",\"attrs\":{\"size\":30},\"apply\":true}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_TYPE;
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(app, "type.panels.characterStyles", NULL, &out, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_invoke(app, "type.panels.glyphs", NULL, &out, NULL) == PC_OK);
    CHECK(ui->type_panels.styles && ui->type_panels.glyphs);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t layers = document(app)->nlayers;
    pc_layer_id id = pc_engine_session_active(pc_ui_shell_app_session(app))->active_layer;
    click_widget(app, "type.glyph.65");
    CHECK_EQ_STR(pc_str_c(&pc_document_layer(document(app), id)->content.u.text.text), "Styles");
    CHECK(ui->type_panels.has_glyph_selected && pc_str_eq_c(&ui->type_panels.glyph_selected, "A"));
    float glyph_rect[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "type.glyph.65", 0, glyph_rect));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    for (unsigned frame = 0; frame < 3; frame++) {
        nk_input_begin(ctx);
        if (frame < 2) {
            SDL_Event event;
            SDL_zero(event);
            event.type = frame == 0 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = frame == 0;
            event.button.clicks = 2;
            event.button.x = glyph_rect[0] + glyph_rect[2] / 2;
            event.button.y = glyph_rect[1] + glyph_rect[3] / 2;
            pc_ui_shell_input_event(app, &event);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
    CHECK(document(app)->nlayers == layers && !ui->has_text_edit);
    CHECK_EQ_STR(pc_str_c(&pc_document_layer(document(app), id)->content.u.text.text), "StylesA");
    CHECK(ui->type_panels.glyph_recent.n == 1);
    CHECK_EQ_STR(ui->type_panels.glyph_recent.v[0], "A");
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    uint64_t before_options = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    click_widget(app, "type.style.options");
    CHECK(ui->type_panels.has_options);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == before_options);
    CHECK(!pc_document_layer(document(app), id)->content.u.text.runs[0].style.underline);
    capture(app, &pixels, &w, &h);
    free(pixels);
    CHECK(pc_ui_shell_app_invoke(app, "edit.checkSpelling", NULL, &out, NULL) == PC_OK);
    CHECK(ui->type_panels.has_spell && ui->type_panels.spell.items.doc);
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_json_value_fini(&out);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_shape_preview_modifier_and_commit(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":96,\"height\":96}");
    invoke(app, "tools.setColors", "{\"foreground\":[1,0,0,0.5],\"background\":[0,0,1,1]}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_RECTANGLE;
    ui->tool_options.stroke_width = 3;
    ui->tool_options.corner_radius = 6;
    ui->views[0].zoom = 3;
    ui->views[0].fit_pending = false;
    ui->views[0].center[0] = ui->views[0].center[1] = 48;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float start[2], end[2], inside[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 20, 20, start) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 60, 40, end) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 40, 50, inside) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    motion(app, start[0], start[1]);
    button(app, true, start[0], start[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    motion(app, end[0], end[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(document(app)->nlayers == 1);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    size_t sample = ((size_t)lrintf(inside[1]) * (size_t)w + (size_t)lrintf(inside[0])) * 4;
    CHECK(pixels[sample] > 245 && pixels[sample + 1] > 245);
    free(pixels);
    nk_input_begin(ctx);
    key(app, SDLK_LSHIFT, true, SDL_KMOD_SHIFT);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    capture(app, &pixels, &w, &h);
    uint8_t green = pixels[sample + 1];
    CHECK(pixels[sample] > 245 && green > 110 && green < 145);
    free(pixels);
    nk_input_begin(ctx);
    button(app, false, end[0], end[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(document(app)->nlayers == 2);
    capture(app, &pixels, &w, &h);
    CHECK(abs((int)pixels[sample + 1] - green) <= 2);
    free(pixels);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_shape_layer *shape = &pc_document_layer(document(app), state->active_layer)->content.u.shape;
    CHECK(shape->has_live && shape->live.rect[2] == 40 && shape->live.rect[3] == 40);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)));
    CHECK(document(app)->nlayers == 1);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_pen_path_preview_undo_escape_and_paths_panel(void)
{
    if (!getenv("PC_TEST_GL")) {
        return;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_sync_views(app, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_PEN;
    ui->views[0].zoom = 2;
    ui->views[0].fit_pending = false;
    ui->views[0].center[0] = 100;
    ui->views[0].center[1] = 80;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float first[2], handle[2], second[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 20, 20, first) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 30, 20, handle) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 80, 60, second) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    motion(app, first[0], first[1]);
    button(app, true, first[0], first[1]);
    motion(app, handle[0], handle[1]);
    button(app, false, handle[0], handle[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(ui->has_pen && ui->pen.count == 1 && ui->pen.knots[0].points[2][0] == 30);
    nk_input_begin(ctx);
    motion(app, second[0], second[1]);
    button(app, true, second[0], second[1]);
    button(app, false, second[0], second[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(ui->pen.count == 2 && !document(app)->has_work_path);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    nk_input_begin(ctx);
    key(app, SDLK_Z, true, SDL_KMOD_CTRL);
    key(app, SDLK_Z, false, SDL_KMOD_CTRL);
    key(app, SDLK_LCTRL, false, 0);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(ui->pen.count == 1 && pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    nk_input_begin(ctx);
    button(app, true, second[0], second[1]);
    button(app, false, second[0], second[1]);
    key(app, SDLK_ESCAPE, true, 0);
    key(app, SDLK_ESCAPE, false, 0);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(!ui->has_pen && document(app)->has_work_path && !document(app)->work_path.subpaths[0].closed);
    invoke(app, "window.panel.paths", "{}");
    capture(app, &pixels, &w, &h);
    free(pixels);
    float rect[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "path.select.work", 0, rect));
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)));
    CHECK(!document(app)->has_work_path);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_direct_selection_preview_and_commit(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160}");
    invoke(app, "path.set", "{\"path\":{\"subpaths\":[{\"knots\":[[20,20],[100,20],[100,100],[20,100]]}]}}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_sync_views(app, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_DIRECT_SELECTION;
    ui->views[0].zoom = 2;
    ui->views[0].fit_pending = false;
    ui->views[0].center[0] = 100;
    ui->views[0].center[1] = 80;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float from[2], to[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 20, 20, from) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 35, 30, to) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    uint64_t rev = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    nk_input_begin(ctx);
    motion(app, from[0], from[1]);
    button(app, true, from[0], from[1]);
    motion(app, to[0], to[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(ui->direct.active && ui->direct.has_preview && ui->direct.count == 1);
    CHECK(document(app)->work_path.subpaths[0].knots[0].anchor.x == 20);
    CHECK(ui->direct.preview.subpaths[0].knots[0].anchor.x == 35);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    nk_input_begin(ctx);
    button(app, false, to[0], to[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(!ui->direct.active && document(app)->work_path.subpaths[0].knots[0].anchor.x == 35);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == rev + 1);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)));
    CHECK(document(app)->work_path.subpaths[0].knots[0].anchor.x == 20);
    ui->tool = PC_TOOL_PEN;
    nk_input_begin(ctx);
    motion(app, from[0], from[1]);
    button(app, true, from[0], from[1]);
    button(app, false, from[0], from[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(!ui->has_pen && document(app)->work_path.subpaths[0].nknots == 3);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_adjustment_dialog_preview_cancel_and_commit(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160,\"background\":\"white\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(app, "image.adjustments.exposure", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ui->n_dialogs == 1);
    if (!ui->n_dialogs) {
        pc_ui_shell_app_free(app);
        SDL_Quit();
        return;
    }
    uint64_t id = ui->dialogs[0].id;
    char json[160];
    yyjson_doc *request = NULL;
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu,\"field\":\"exposure\",\"value\":-1}",
                   (unsigned long long)id);
    CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.set", yyjson_doc_get_root(request), &out, NULL) == PC_OK);
    yyjson_doc_free(request);
    pc_json_value_fini(&out);
    pc_ui_dialog_runtime *runtime = pc_ui_shell_dialog_runtime(app);
    for (size_t i = 0; i < 100 && !runtime->preview.has_ready; i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(runtime->preview.has_ready && runtime->preview.result);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    float bounds[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "dialog.exposure", 0, bounds));
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu}", (unsigned long long)id);
    request = NULL;
    CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.cancel", yyjson_doc_get_root(request), &out, NULL) == PC_OK);
    yyjson_doc_free(request);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(!runtime->preview.result);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    CHECK(pc_ui_shell_app_invoke(app, "image.adjustments.exposure", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    id = ui->dialogs[0].id;
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu,\"field\":\"exposure\",\"value\":-1}",
                   (unsigned long long)id);
    request = NULL;
    CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.set", yyjson_doc_get_root(request), &out, NULL) == PC_OK);
    yyjson_doc_free(request);
    pc_json_value_fini(&out);
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu}", (unsigned long long)id);
    request = NULL;
    CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.confirm", yyjson_doc_get_root(request), &out, NULL) == PC_OK);
    yyjson_doc_free(request);
    pc_json_value_fini(&out);
    CHECK(ui->n_dialogs == 0 &&
          pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision + 1);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)));
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_curves_graph_histogram_and_drawing_mode(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160,\"background\":\"white\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(app, "image.adjustments.curves", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_dialog_runtime *r = pc_ui_shell_dialog_runtime(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    float graph[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "dialog.curve", 0, graph));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float x = graph[0] + 128.f / 255 * graph[2], y = graph[1] + (1 - 192.f / 255) * graph[3];
    nk_input_begin(ctx);
    motion(app, x, y);
    button(app, true, x, y);
    button(app, false, x, y);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(ui->dialogs[0].fields),
                   *points = yyjson_mut_obj_get(fields, "points");
    CHECK(r->curve.count == 3 && r->curve.has_selected && yyjson_mut_arr_size(points) == 3);
    CHECK(fabs(r->curve.points[1][0] - 128) < 1 && fabs(r->curve.points[1][1] - 192) < 1);
    for (size_t i = 0; i < 100 && !r->preview.has_histogram; i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(r->preview.has_histogram && r->preview.histogram.bins[0][255] > 0);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_widget(app, "dialog.curve.mode");
    click_widget(app, "dialog.curve.mode.draw");
    fields = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "pencil")));
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(fields, "points")) == 256);
    CHECK(pc_ui_shell_app_panel_mapping(app, "dialog.curve", 0, graph));
    x = graph[0] + graph[2] * .25f;
    y = graph[1] + graph[3] * .8f;
    nk_input_begin(ctx);
    motion(app, x, y);
    button(app, true, x, y);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    x = graph[0] + graph[2] * .75f;
    y = graph[1] + graph[3] * .2f;
    nk_input_begin(ctx);
    motion(app, x, y);
    button(app, false, x, y);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    fields = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_obj_get(fields, "points"), 128)) > 100);
    click_widget(app, "dialog.curve.mode");
    click_widget(app, "dialog.curve.mode.points");
    fields = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "pencil")));
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(fields, "points")) <= 19);
    click_dialog_action(app, "cancel");
    CHECK(ui->n_dialogs == 0 && pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void increment_widget(pc_ui_shell_app *app, const char *id)
{
    float rect[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, id, 0, rect));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float x = rect[0] + rect[2] - 8, y = rect[1] + rect[3] / 2;
    for (unsigned i = 0; i < 3; i++) {
        nk_input_begin(ctx);
        if (i < 2) {
            motion(app, x, y);
            button(app, i == 0, x, y);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
}
static void open_adjustment(pc_ui_shell_app *app, const char *id)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(app, id, NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
}
static yyjson_mut_val *adjustment_fields(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    return ui->n_dialogs ? yyjson_mut_doc_get_root(ui->dialogs[ui->n_dialogs - 1].fields) : NULL;
}
static void test_native_structured_adjustment_controls_preserve_ranges(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":100,\"height\":80}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    open_adjustment(app, "image.adjustments.colorBalance");
    click_widget(app, "dialog.balanceTone");
    click_widget(app, "dialog.balanceTone.1");
    increment_widget(app, "dialog.midtones.0");
    CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "midtones"), 0)) ==
          1);
    click_dialog_action(app, "cancel");
    open_adjustment(app, "image.adjustments.hueSaturation");
    click_widget(app, "dialog.hueRange");
    click_widget(app, "dialog.hueRange.1");
    increment_widget(app, "dialog.reds.hue");
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(yyjson_mut_obj_get(adjustment_fields(app), "reds"), "hue")) ==
          1);
    click_dialog_action(app, "cancel");
    open_adjustment(app, "image.adjustments.channelMixer");
    click_widget(app, "dialog.monochrome");
    increment_widget(app, "dialog.gray.0");
    CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "gray"), 0)) ==
          41);
    click_widget(app, "dialog.monochrome");
    CHECK(!yyjson_mut_obj_get(adjustment_fields(app), "gray"));
    click_widget(app, "dialog.monochrome");
    CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "gray"), 0)) ==
          41);
    click_dialog_action(app, "cancel");
    open_adjustment(app, "image.adjustments.selectiveColor");
    increment_widget(app, "dialog.reds.0");
    CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "reds"), 0)) == 1);
    click_widget(app, "dialog.selectiveRange");
    click_widget(app, "dialog.selectiveRange.2");
    increment_widget(app, "dialog.greens.0");
    CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "reds"), 0)) == 1);
    CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "greens"), 0)) ==
          1);
    CHECK(!yyjson_mut_obj_get(adjustment_fields(app), "cyan"));
    click_dialog_action(app, "cancel");
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_gradient_stops_and_nested_color_preview(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":100,\"height\":80}");
    invoke(app, "edit.fill", "{\"contents\":\"color\",\"color\":\"#808080\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    open_adjustment(app, "image.adjustments.gradientMap");
    click_widget(app, "dialog.gradient.add");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(adjustment_fields(app), "stops")) == 3);
    click_widget(app, "dialog.gradient.color");
    CHECK(ui->n_dialogs == 2);
    uint64_t child = ui->dialogs[1].id;
    char json[160];
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu,\"field\":\"color\",\"value\":\"#ff0000\"}",
                   (unsigned long long)child);
    yyjson_doc *request = NULL;
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.set", yyjson_doc_get_root(request), &out, NULL) == PC_OK);
    yyjson_doc_free(request);
    pc_json_value_fini(&out);
    pc_ui_dialog_runtime *r = pc_ui_shell_dialog_runtime(app);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    for (size_t i = 0; i < 100 && (!r->preview.has_ready || !r->preview.result); i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(r->preview.result != NULL);
    if (r->preview.result) {
        const pc_document *preview = pc_doc_arc_get(r->preview.result);
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        const pc_layer *layer = pc_document_layer(preview, state->active_layer);
        float pixel[4];
        pc_surface_rgba(&layer->content.u.raster, 50, 40, pixel);
        CHECK(pixel[0] > pixel[1] + .1f && pixel[0] > pixel[2] + .1f);
    }
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 1);
    yyjson_mut_val *stops = yyjson_mut_obj_get(adjustment_fields(app), "stops");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_arr_get(yyjson_mut_arr_get(stops, 1), 1)), "#ff0000");
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_widget(app, "dialog.gradient.delete");
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(adjustment_fields(app), "stops")) == 2);
    click_dialog_action(app, "cancel");
    CHECK(ui->n_dialogs == 0 && pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_layer_style_preview_color_and_single_commit(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160,\"background\":\"transparent\"}");
    invoke(app, "shape.create", "{\"kind\":\"rect\",\"rect\":[40,40,60,50],\"fill\":\"#ff0000\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    pc_layer_id layer = state->active_layer;
    uint64_t revision = state->revision;
    size_t history = pc_history_past_len(state->history);
    open_adjustment(app, "layer.layerStyle.dropShadow");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->n_dialogs == 1);
    CHECK(pc_document_layer(document(app), layer)->effects.nitems == 0);
    increment_widget(app, "style.size");
    yyjson_mut_val *entry = yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "effects"), 0);
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(yyjson_mut_obj_get(entry, "params"), "size")) == 6);
    click_widget(app, "style.contour");
    click_widget(app, "style.contour.2");
    entry = yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "effects"), 0);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_obj_get(entry, "params"), "contour")),
                 "Cone");
    click_widget(app, "style.color.color");
    CHECK(ui->n_dialogs == 2);
    uint64_t child = ui->dialogs[1].id;
    char json[160];
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu,\"field\":\"color\",\"value\":\"#0088ff\"}",
                   (unsigned long long)child);
    yyjson_doc *request = NULL;
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.set", yyjson_doc_get_root(request), &out, NULL) == PC_OK);
    yyjson_doc_free(request);
    pc_json_value_fini(&out);
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 1);
    CHECK(pc_document_layer(document(app), layer)->effects.nitems == 0 &&
          pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    pc_ui_dialog_runtime *r = pc_ui_shell_dialog_runtime(app);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    for (size_t i = 0; i < 100 && (!r->preview.has_ready || !r->preview.result); i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(r->preview.result != NULL);
    uint64_t generation = r->preview.generation;
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_widget(app, "style.blending");
    CHECK(r->preview.generation == generation);
    float rect[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "style.opacity", 0, rect));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float x = rect[0] + 8, y = rect[1] + rect[3] / 2;
    for (unsigned i = 0; i < 3; i++) {
        nk_input_begin(ctx);
        if (i < 2) {
            motion(app, x, y);
            button(app, i == 0, x, y);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
    CHECK(yyjson_mut_get_num(
              yyjson_mut_obj_get(yyjson_mut_obj_get(adjustment_fields(app), "blending"), "opacity")) == 99);
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 0);
    state = pc_engine_session_active(pc_ui_shell_app_session(app));
    CHECK(state->revision > revision && pc_history_past_len(state->history) == history + 1);
    const pc_layer *actual = pc_document_layer(document(app), layer);
    CHECK(actual->effects.nitems == 1 && actual->effects.items[0].u.shadow.size == 6);
    CHECK(actual->opacity == .99f);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)));
    CHECK(pc_document_layer(document(app), layer)->effects.nitems == 0 &&
          pc_document_layer(document(app), layer)->opacity == 1);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_layer_style_user_defaults(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":60,\"height\":60}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    open_adjustment(app, "layer.layerStyle.colorOverlay");
    float rect[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "style.opacity", 0, rect));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    for (unsigned f = 0; f < 3; f++) {
        nk_input_begin(ctx);
        if (f < 2) {
            motion(app, rect[0] + 8, rect[1] + rect[3] / 2);
            button(app, f == 0, rect[0] + 8, rect[1] + rect[3] / 2);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
    click_widget(app, "style.default.make");
    increment_widget(app, "style.opacity");
    click_widget(app, "style.default.reset");
    yyjson_mut_val *effects = yyjson_mut_obj_get(adjustment_fields(app), "effects"),
                   *params = yyjson_mut_obj_get(yyjson_mut_arr_get(effects, 0), "params");
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(params, "opacity")) == 99);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    click_dialog_action(app, "cancel");
    open_adjustment(app, "layer.layerStyle.colorOverlay");
    effects = yyjson_mut_obj_get(adjustment_fields(app), "effects");
    params = yyjson_mut_obj_get(yyjson_mut_arr_get(effects, 0), "params");
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(params, "opacity")) == 99);
    click_dialog_action(app, "cancel");
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void style_scroll(pc_ui_shell_app *app, float amount)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    motion(app, 1000, 390);
    nk_input_scroll(ctx, nk_vec2(0, amount));
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
}
static void test_native_layer_style_instances_contour_and_cancel(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    open_adjustment(app, "layer.layerStyle.dropShadow");
    click_widget(app, "style.duplicate");
    yyjson_mut_val *root = adjustment_fields(app), *effects = yyjson_mut_obj_get(root, "effects");
    CHECK(yyjson_mut_arr_size(effects) == 2);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "selected")), "fx2");
    click_widget(app, "style.up");
    effects = yyjson_mut_obj_get(adjustment_fields(app), "effects");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(effects, 0), "id")), "fx2");
    click_widget(app, "style.down");
    effects = yyjson_mut_obj_get(adjustment_fields(app), "effects");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(effects, 1), "id")), "fx2");
    click_widget(app, "style.contour.edit");
    CHECK(yyjson_mut_is_obj(yyjson_mut_obj_get(adjustment_fields(app), "__contourEditor")));
    style_scroll(app, -6);
    float graph[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "style.contour.graph", 0, graph));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float x = graph[0] + graph[2] / 2, y = graph[1] + graph[3] / 4;
    for (unsigned frame = 0; frame < 3; frame++) {
        nk_input_begin(ctx);
        if (frame < 2) {
            motion(app, x, y);
            button(app, frame == 0, x, y);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
    effects = yyjson_mut_obj_get(adjustment_fields(app), "effects");
    yyjson_mut_val *params = yyjson_mut_obj_get(yyjson_mut_arr_get(effects, 1), "params"),
                   *contour = yyjson_mut_obj_get(params, "contour");
    CHECK(yyjson_mut_is_obj(contour));
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(contour, "points")) == 3);
    style_scroll(app, -6);
    click_widget(app, "style.contour.corner");
    effects = yyjson_mut_obj_get(adjustment_fields(app), "effects");
    params = yyjson_mut_obj_get(yyjson_mut_arr_get(effects, 1), "params");
    contour = yyjson_mut_obj_get(params, "contour");
    CHECK(yyjson_mut_is_true(yyjson_mut_arr_get(yyjson_mut_obj_get(contour, "corners"), 1)));
    click_widget(app, "style.contour.cancel");
    effects = yyjson_mut_obj_get(adjustment_fields(app), "effects");
    CHECK(!yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_arr_get(effects, 1), "params"), "contour"));
    CHECK(!yyjson_mut_obj_get(adjustment_fields(app), "__contourEditor"));
    click_dialog_action(app, "cancel");
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    CHECK(
        pc_document_layer(document(app), pc_engine_session_active(pc_ui_shell_app_session(app))->active_layer)
            ->effects.nitems == 0);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_gallery_stack_preview_and_one_commit(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":100,\"height\":80,\"background\":\"white\"}");
    invoke(app, "edit.fill", "{\"color\":\"#dd7722\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    uint64_t revision = state->revision;
    size_t history = pc_history_past_len(state->history);
    open_adjustment(app, "filter.filterGallery");
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(adjustment_fields(app), "__gallery")));
    click_widget(app, "gallery.choose.cutout");
    increment_widget(app, "gallery.numberOfLevels");
    yyjson_mut_val *effects = yyjson_mut_obj_get(adjustment_fields(app), "effects");
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_arr_get(effects, 0), "params"),
                                                "numberOfLevels")) == 5);
    click_widget(app, "gallery.add");
    effects = yyjson_mut_obj_get(adjustment_fields(app), "effects");
    CHECK(yyjson_mut_arr_size(effects) == 2);
    click_widget(app, "gallery.down");
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(adjustment_fields(app), "__gallerySelected")) == 0);
    click_widget(app, "gallery.visible.1");
    effects = yyjson_mut_obj_get(adjustment_fields(app), "effects");
    CHECK(yyjson_mut_is_false(yyjson_mut_obj_get(yyjson_mut_arr_get(effects, 1), "visible")));
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    pc_ui_dialog_runtime *r = pc_ui_shell_dialog_runtime(app);
    for (size_t i = 0; i < 1000 && (!r->preview.has_ready || !r->preview.result); i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(r->preview.result != NULL && r->preview.status == PC_OK);
    uint8_t *pixels = NULL;
    int width, height;
    capture(app, &pixels, &width, &height);
    free(pixels);
    click_dialog_action(app, "confirm");
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 0);
    state = pc_engine_session_active(pc_ui_shell_app_session(app));
    CHECK(state->revision > revision && pc_history_past_len(state->history) == history + 1);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)));
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_camera_raw_panels_curves_preview_and_commit(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":120,\"height\":100,\"depth\":16,\"background\":\"white\"}");
    invoke(app, "edit.fill", "{\"color\":\"#778899\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    uint64_t revision = state->revision;
    size_t history = pc_history_past_len(state->history);
    open_adjustment(app, "filter.cameraRaw");
    increment_widget(app, "dialog.exposure");
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(adjustment_fields(app), "exposure")) > 0);
    click_widget(app, "camera.page");
    click_widget(app, "camera.page.3");
    float graph[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "camera.curve.graph", 0, graph));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float x = graph[0] + graph[2] / 2, y = graph[1] + graph[3] / 4;
    for (unsigned f = 0; f < 3; f++) {
        nk_input_begin(ctx);
        if (f < 2) {
            motion(app, x, y);
            button(app, f == 0, x, y);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(adjustment_fields(app), "pointCurve")) == 3);
    click_widget(app, "camera.page");
    click_widget(app, "camera.page.4");
    increment_widget(app, "camera.hslHue.0");
    CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "hslHue"), 0)) ==
          1);
    click_widget(app, "camera.mixer");
    click_widget(app, "camera.mixer.1");
    increment_widget(app, "camera.hslSat.1");
    CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_obj_get(adjustment_fields(app), "hslSat"), 1)) ==
          1);
    click_widget(app, "camera.page");
    click_widget(app, "camera.page.5");
    click_widget(app, "camera.grading");
    click_widget(app, "camera.grading.1");
    increment_widget(app, "camera.gradeMidtones.hue");
    CHECK(yyjson_mut_get_num(
              yyjson_mut_obj_get(yyjson_mut_obj_get(adjustment_fields(app), "gradeMidtones"), "hue")) == 1);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(adjustment_fields(app), "pointCurve")) == 3);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    pc_ui_dialog_runtime *r = pc_ui_shell_dialog_runtime(app);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    for (size_t j = 0;
         j < 1000 && (!r->preview.has_ready || !r->preview.result || pc_ui_filter_preview_busy(&r->preview));
         j++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(r->preview.result && r->preview.status == PC_OK && r->preview.has_histogram);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_dialog_action(app, "confirm");
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 0);
    state = pc_engine_session_active(pc_ui_shell_app_session(app));
    CHECK(state->revision > revision && pc_history_past_len(state->history) == history + 1);
    CHECK(pc_doc_arc_get(state->doc)->depth == PC_SAMPLE_TYPE_U16);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)));
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void click_fraction(pc_ui_shell_app *app, const char *id, float u, float v)
{
    float rect[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, id, 0, rect));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float x = rect[0] + u * rect[2], y = rect[1] + v * rect[3];
    for (unsigned f = 0; f < 3; f++) {
        nk_input_begin(ctx);
        if (f < 2) {
            motion(app, x, y);
            button(app, f == 0, x, y);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
}
static void wait_filter_preview(pc_ui_shell_app *app)
{
    pc_ui_dialog_runtime *r = pc_ui_shell_dialog_runtime(app);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    for (size_t i = 0;
         i < 1000 && (!r->preview.has_ready || !r->preview.result || pc_ui_filter_preview_busy(&r->preview) ||
                      memcmp(r->preview.ready, r->preview.wanted, 32));
         i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(r->preview.result && r->preview.status == PC_OK &&
          !memcmp(r->preview.ready, r->preview.wanted, 32));
}
static void test_native_color_range_eyedropper_preview_cancel_and_commit(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":100,\"height\":80,\"background\":\"white\"}");
    invoke(app, "select.rect", "{\"x\":0,\"y\":0,\"width\":50,\"height\":80}");
    invoke(app, "edit.fill", "{\"color\":\"#332211\"}");
    invoke(app, "select.deselect", "{}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    uint64_t revision = state->revision;
    size_t history = pc_history_past_len(state->history);
    open_adjustment(app, "select.colorRange");
    click_fraction(app, "range.preview", .25f, .5f);
    yyjson_mut_val *points = yyjson_mut_obj_get(adjustment_fields(app), "points");
    CHECK(yyjson_mut_arr_size(points) == 1);
    CHECK(fabs(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_arr_get(points, 0), 0)) - 25) <= 1);
    wait_filter_preview(app);
    pc_ui_dialog_runtime *r = pc_ui_shell_dialog_runtime(app);
    const pc_document *preview = pc_doc_arc_get(r->preview.result);
    CHECK(preview->has_selection && pc_surface_sample_channel(&preview->selection, 25, 40, 0) > .99f);
    CHECK(pc_surface_sample_channel(&preview->selection, 75, 40, 0) < .01f);
    CHECK(!document(app)->has_selection &&
          pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_widget(app, "range.tool.1");
    click_fraction(app, "range.preview", .75f, .5f);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(adjustment_fields(app), "points")) == 2);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(adjustment_fields(app), "order")) == 2);
    click_dialog_action(app, "cancel");
    CHECK(!document(app)->has_selection &&
          pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    open_adjustment(app, "select.colorRange");
    click_widget(app, "range.select");
    click_widget(app, "range.select.7");
    wait_filter_preview(app);
    preview = pc_doc_arc_get(r->preview.result);
    CHECK(preview->has_selection && pc_surface_sample_channel(&preview->selection, 75, 40, 0) > .99f);
    CHECK(pc_surface_sample_channel(&preview->selection, 25, 40, 0) < .01f);
    click_dialog_action(app, "confirm");
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 0 && document(app)->has_selection);
    CHECK(pc_history_past_len(pc_engine_session_active(pc_ui_shell_app_session(app))->history) ==
          history + 1);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)) && !document(app)->has_selection);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_about_credits_names_models_and_sort(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    open_adjustment(app, "help.about");
    pc_ui_dialog_runtime *r = pc_ui_shell_dialog_runtime(app);
    CHECK(r->has_credits && r->credits.count > 100 && r->credits.nmodels > 10);
    click_widget(app, "about.tab.1");
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(adjustment_fields(app), "tab")) == 1);
    click_widget(app, "about.names.2");
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(adjustment_fields(app), "names")) == 2);
    click_widget(app, "about.reverse");
    CHECK(yyjson_mut_is_false(yyjson_mut_obj_get(adjustment_fields(app), "ascending")));
    uint8_t *pixels = NULL;
    int width, height;
    capture(app, &pixels, &width, &height);
    free(pixels);
    click_widget(app, "about.tab.2");
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(adjustment_fields(app), "tab")) == 2);
    capture(app, &pixels, &width, &height);
    free(pixels);
    click_dialog_action(app, "confirm");
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 0);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_workspace_dialog_saved_menu_and_reset(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->dock_width = 419;
    ui->panels.history = true;
    open_adjustment(app, "window.workspace.newWorkspace");
    CHECK(ui->n_dialogs == 1);
    char json[160];
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu,\"field\":\"name\",\"value\":\"Review\"}",
                   (unsigned long long)ui->dialogs[0].id);
    yyjson_doc *request = NULL;
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.set", yyjson_doc_get_root(request), &result, NULL) == PC_OK);
    pc_json_value_fini(&result);
    yyjson_doc_free(request);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 0);
    CHECK_EQ_STR(ui->workspace, "Review");
    CHECK(pc_ui_shell_app_invoke(app, "window.workspace.painting", NULL, &result, NULL) == PC_OK);
    pc_json_value_fini(&result);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "menu.Window");
    click_widget(app, "menu.sub.Workspace");
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_widget(app, "window.workspace.saved.Review");
    capture(app, &pixels, &w, &h);
    free(pixels);
    CHECK_EQ_STR(ui->workspace, "Review");
    CHECK(ui->dock_width == 419 && ui->panels.history);
    ui->dock_width = 300;
    ui->panels.history = false;
    CHECK(pc_ui_shell_app_invoke(app, "window.workspace.resetWorkspace", NULL, &result, NULL) == PC_OK);
    pc_json_value_fini(&result);
    CHECK(ui->dock_width == 419 && ui->panels.history);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static pc_status native_gated_work(const pc_engine_job_ctx *ctx, void *user, void **payload, pc_error *err)
{
    (void)payload;
    atomic_bool *done = user;
    pc_engine_job_ctx_progress(ctx, .4f, "Processing preview");
    while (!atomic_load_explicit(done, memory_order_acquire)) {
        pc_status status = pc_engine_job_ctx_check(ctx, err);
        if (status != PC_OK)
            return status;
        SDL_Delay(1);
    }
    return PC_OK;
}
static pc_status native_gated_apply(pc_engine_session *s, void *payload, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)payload;
    (void)err;
    out->doc = yyjson_mut_doc_new(NULL);
    if (!out->doc)
        return PC_ERR_NOMEM;
    yyjson_mut_doc_set_root(out->doc, yyjson_mut_null(out->doc));
    return PC_OK;
}
static void test_native_background_job_commit_progress_cancel_and_escape(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":100,\"height\":80,\"background\":\"white\"}");
    invoke(app, "select.rect", "{\"x\":0,\"y\":0,\"width\":50,\"height\":80}");
    invoke(app, "edit.fill", "{\"color\":\"#332211\"}");
    invoke(app, "select.deselect", "{}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_shell_app_background_jobs(app, true);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    uint64_t revision = state->revision;
    size_t history = pc_history_past_len(state->history);
    invoke(app, "filter.blur.gaussianBlur", "{\"radius\":3}");
    CHECK(pc_engine_session_has_jobs(session));
    CHECK(pc_engine_session_active(session)->revision == revision);
    for (size_t i = 0; i < 1000 && pc_engine_session_has_jobs(session); i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(!pc_engine_session_has_jobs(session));
    CHECK(pc_history_past_len(pc_engine_session_active(session)->history) == history + 1);
    CHECK(pc_engine_session_undo(session));
    revision = pc_engine_session_active(session)->revision;
    for (size_t attempt = 0; attempt < 2; attempt++) {
        atomic_bool gate;
        atomic_init(&gate, false);
        bool job = false;
        uint64_t id = 0;
        pc_json_value result = PC_JSON_VALUE_INIT;
        CHECK(pc_engine_session_start_job(session, "test.nativeProgress", NULL, "Background image operation",
                                          true, native_gated_work, &gate, NULL, NULL, native_gated_apply,
                                          &job, &id, &result, NULL) == PC_OK);
        pc_json_value_fini(&result);
        CHECK(job && id > 0);
        SDL_Delay(270);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        CHECK(pc_ui_shell_jobs_runtime(app)->locked && pc_ui_shell_jobs_runtime(app)->modal);
        float rect[4];
        CHECK(pc_ui_shell_app_panel_mapping(app, "jobs.cancel", 0, rect));
        uint8_t *pixels = NULL;
        int w, h;
        capture(app, &pixels, &w, &h);
        free(pixels);
        if (!attempt)
            click_widget(app, "jobs.cancel");
        else {
            SDL_Event event = {0};
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.key = SDLK_ESCAPE;
            event.key.down = true;
            pc_ui_shell_input_event(app, &event);
            CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
            CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        }
        CHECK(!pc_engine_session_has_jobs(session) && !pc_ui_shell_jobs_runtime(app)->modal);
        CHECK(pc_engine_session_active(session)->revision == revision);
        pc_engine_session_join_cancelled_jobs(session);
    }
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_system_info_and_clipboard_copy(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    open_adjustment(app, "help.systemInfo");
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 1);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_widget(app, "systemInfo.copy");
    char *text = SDL_GetClipboardText();
    CHECK(text && strstr(text, "PhotoCraft") && strstr(text, "Canvas renderer:") &&
          strstr(text, "Monitor profile:"));
    SDL_free(text);
    click_dialog_action(app, "confirm");
    CHECK(pc_ui_shell_app_state(app)->n_dialogs == 0);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_duplicate_notice_titles_inspect_and_dismiss(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_shell_report(app, "Repeated error", "First detail", true);
    pc_ui_shell_report(app, "Repeated error", "Second detail", true);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_json_value info = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_control_inspect(app, &info, NULL) == PC_OK);
    yyjson_mut_val *notices = yyjson_mut_obj_get(pc_json_value_root(&info), "notices");
    CHECK(yyjson_mut_arr_size(notices) == 2);
    uint64_t first = yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_arr_get(notices, 0), "id")),
             second = yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_arr_get(notices, 1), "id"));
    CHECK(first && second && first != second);
    pc_json_value_fini(&info);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    char command[96];
    (void)snprintf(command, sizeof command, "ui.notice.dismiss.%llu", (unsigned long long)second);
    click_widget(app, command);
    CHECK(pc_ui_control_inspect(app, &info, NULL) == PC_OK);
    notices = yyjson_mut_obj_get(pc_json_value_root(&info), "notices");
    CHECK(yyjson_mut_arr_size(notices) == 1 &&
          yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_arr_get(notices, 0), "id")) == first);
    pc_json_value_fini(&info);
    char json[96];
    (void)snprintf(json, sizeof json, "{\"notice\":%llu}", (unsigned long long)first);
    yyjson_doc *params = NULL;
    CHECK(pc_json_parse(json, strlen(json), &params, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.notice.dismiss", yyjson_doc_get_root(params), &info, NULL) == PC_OK);
    pc_json_value_fini(&info);
    CHECK(pc_ui_control_handle(app, "ui.notice.dismiss", yyjson_doc_get_root(params), &info, NULL) ==
          PC_ERR_NOT_FOUND);
    yyjson_doc_free(params);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_brush_settings_sections_presets_and_preview(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(app, "window.panel.brushSettings", "{}");
    CHECK(pc_ui_shell_app_state(app)->panels.brush_settings);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(pc_ui_shell_app_session(app))->revision;
    click_widget(app, "brush.section.shapeDynamics");
    CHECK(pc_ui_shell_app_state(app)->brush_section == 1);
    CHECK(pc_engine_session_tools(pc_ui_shell_app_session(app))->brush.shape_dynamics.enabled);
    increment_widget(app, "brush.shapeDynamics.size.jitter");
    CHECK(pc_engine_session_tools(pc_ui_shell_app_session(app))->brush.shape_dynamics.size.jitter > 0);
    click_widget(app, "brush.locks.shapeDynamics");
    CHECK(pc_engine_session_tools(pc_ui_shell_app_session(app))->brush.locks.shape_dynamics);
    const char *sections[] = {"scattering", "texture",  "dualBrush", "colorDynamics", "transfer",      "pose",
                              "noise",      "wetEdges", "buildUp",   "smoothing",     "protectTexture"};
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        char id[128];
        (void)snprintf(id, sizeof id, "brush.section.%s", sections[i]);
        click_widget(app, id);
        CHECK(pc_ui_shell_app_state(app)->brush_section == i + 2);
    }
    click_widget(app, "brush.tab.presets");
    CHECK(pc_ui_shell_app_state(app)->brush_tab == 1);
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    CHECK(tools->npresets > 0);
    char preset_name[256], id[320];
    (void)snprintf(preset_name, sizeof preset_name, "%s", pc_str_c(&tools->presets[0].name));
    (void)snprintf(id, sizeof id, "brush.preset.%s", preset_name);
    click_widget(app, id);
    tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    CHECK(tools->current_preset && !strcmp(tools->current_preset, preset_name));
    CHECK(tools->brush.locks.shape_dynamics);
    CHECK(tools->brush.shape_dynamics.size.jitter > 0);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->revision == revision);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_state(app)->panels.brush_settings = false;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    invoke(app, "window.panel.brushSettings", "{}");
    CHECK(pc_ui_shell_app_state(app)->brush_tab == 0);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "brush.section.shapeDynamics");
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void drag_layer_row(pc_ui_shell_app *app, pc_layer_id source, pc_layer_id target, bool copy)
{
    float from[4], to[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "layer.select", source, from));
    CHECK(pc_ui_shell_app_panel_mapping(app, "layer.select", target, to));
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float x = from[0] + from[2] / 2, y = from[1] + from[3] / 2;
    SDL_SetModState(copy ? SDL_KMOD_ALT : 0);
    nk_input_begin(ctx);
    button(app, true, x, y);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    x = to[0] + to[2] / 2;
    y = to[1] + 2;
    nk_input_begin(ctx);
    motion(app, x, y);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    button(app, false, x, y);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    SDL_SetModState(0);
}
static void test_native_layer_drag_copy_and_inline_rename(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    pc_engine_session *s = pc_ui_shell_app_session(app);
    invoke(app, "layer.new.layer", "{\"name\":\"A\"}");
    pc_layer_id a = pc_engine_session_active(s)->active_layer;
    invoke(app, "layer.new.layer", "{\"name\":\"B\"}");
    invoke(app, "layer.new.layer", "{\"name\":\"C\"}");
    pc_layer_id c = pc_engine_session_active(s)->active_layer;
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->panels.history = ui->panels.properties = ui->panels.color = ui->panels.navigator =
        ui->panels.character = false;
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t before = history_current(app);
    drag_layer_row(app, a, c, false);
    CHECK(document(app)->layers[3].id == a);
    CHECK(history_current(app) == before + 1);
    invoke(app, "edit.undo", "{}");
    CHECK(document(app)->layers[1].id == a);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    drag_layer_row(app, a, c, true);
    CHECK(document(app)->nlayers == 5 && document(app)->layers[1].id == a);
    if (document(app)->nlayers > 4)
        CHECK_EQ_STR(pc_str_c(&document(app)->layers[4].name), "A copy");
    char request[128];
    (void)snprintf(request, sizeof request, "{\"layer\":%llu}", (unsigned long long)a);
    invoke(app, "layer.renameLayer", request);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    native_text(app, "Renamed Ω", false);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    nk_input_begin(ctx);
    key(app, SDLK_RETURN, true, 0);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    key(app, SDLK_RETURN, false, 0);
    CHECK_EQ_STR(pc_str_c(&pc_document_layer(document(app), a)->name), "Renamed Ω");
    invoke(app, "edit.undo", "{}");
    CHECK_EQ_STR(pc_str_c(&pc_document_layer(document(app), a)->name), "A");
    invoke(app, "layer.renameLayer", request);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    native_text(app, "Cancelled", false);
    key(app, SDLK_ESCAPE, true, 0);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK_EQ_STR(pc_str_c(&pc_document_layer(document(app), a)->name), "A");
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_mask_thumbnails_targets_colors_and_modifiers(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":32}");
    invoke(app, "layer.new.layer", "{\"name\":\"Paint\"}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    pc_engine_session *s = pc_ui_shell_app_session(app);
    pc_layer_id layer = pc_engine_session_active(s)->active_layer;
    invoke(app, "layer.layerMask.revealAll", "{}");
    invoke(app, "tools.setColors", "{\"foreground\":[0.2,0.4,0.8,1],\"background\":[0.9,0.7,0.3,1]}");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->panels.history = ui->panels.properties = ui->panels.color = ui->panels.navigator =
        ui->panels.character = false;
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    panel_click(app, "layer.mask.thumbnail", layer, 1);
    CHECK(ui->mask_target && pc_engine_session_tools(s)->mask_targeted);
    CHECK_NEAR(pc_engine_session_tools(s)->foreground[0], 1, 0);
    invoke(app, "tools.setColors", "{\"foreground\":[0.25,0.25,0.25,1]}");
    panel_click(app, "layer.thumbnail", layer, 1);
    CHECK(!ui->mask_target && !pc_engine_session_tools(s)->mask_targeted);
    CHECK_NEAR(pc_engine_session_tools(s)->foreground[0], .2f, 1e-6);
    CHECK_NEAR(pc_engine_session_tools(s)->foreground[2], .8f, 1e-6);
    panel_click(app, "layer.mask.thumbnail", layer, 1);
    CHECK_NEAR(pc_engine_session_tools(s)->foreground[0], .25f, 1e-6);
    key(app, SDLK_LSHIFT, true, SDL_KMOD_SHIFT);
    panel_click(app, "layer.mask.thumbnail", layer, 1);
    key(app, SDLK_LSHIFT, false, 0);
    CHECK(!pc_document_layer(document(app), layer)->mask.enabled);
    panel_click(app, "layer.layerMask.linked", layer, 1);
    CHECK(!pc_document_layer(document(app), layer)->mask.linked);
    key(app, SDLK_LALT, true, SDL_KMOD_ALT);
    panel_click(app, "layer.mask.thumbnail", layer, 1);
    key(app, SDLK_LALT, false, 0);
    CHECK(pc_engine_session_active(s)->has_layer_mask_view &&
          pc_engine_session_active(s)->layer_mask_view_mode == 0);
    panel_click(app, "layer.thumbnail", layer, 1);
    CHECK(!pc_engine_session_active(s)->has_layer_mask_view);
    CHECK(!ui->mask_target && !pc_engine_session_tools(s)->mask_targeted);
    key(app, SDLK_LCTRL, true, SDL_KMOD_CTRL);
    panel_click(app, "layer.mask.thumbnail", layer, 1);
    key(app, SDLK_LCTRL, false, 0);
    CHECK(document(app)->has_selection);
    invoke(app, "layer.vectorMask.revealAll", "{}");
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    panel_click(app, "layer.vectorMask.thumbnail", layer, 1);
    CHECK(ui->vector_mask_target && !ui->mask_target && !pc_engine_session_tools(s)->mask_targeted);
    key(app, SDLK_LSHIFT, true, SDL_KMOD_SHIFT);
    panel_click(app, "layer.vectorMask.thumbnail", layer, 1);
    key(app, SDLK_LSHIFT, false, 0);
    CHECK(!pc_document_layer(document(app), layer)->vector_mask.enabled);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_timeline_creation_playback_controls_and_workspace(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":32}");
    pc_engine_session *s = pc_ui_shell_app_session(app);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(app, "window.panel.timeline", "{}");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->timeline.open);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t history = history_current(app);
    click_widget(app, "timeline.create");
    CHECK(document(app)->has_timeline && document(app)->timeline.duration == 30);
    click_widget(app, "timeline.nextFrame");
    CHECK(document(app)->timeline.current == 1);
    click_widget(app, "timeline.previousFrame");
    CHECK(document(app)->timeline.current == 0);
    increment_widget(app, "timeline.duration");
    CHECK(document(app)->timeline.duration == 31);
    increment_widget(app, "timeline.fps");
    CHECK_NEAR(document(app)->timeline.fps, 31, .01);
    click_widget(app, "timeline.play");
    CHECK(ui->timeline.playing);
    size_t frame = document(app)->timeline.current;
    ui->timeline.last_time -= 1000;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(document(app)->timeline.current == (frame + 1) % 31);
    CHECK(!pc_engine_session_active(s)->has_last_damage);
    click_widget(app, "timeline.play");
    CHECK(!ui->timeline.playing);
    CHECK(history_current(app) == history);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    invoke(app, "window.workspace.painting", "{}");
    CHECK(!ui->timeline.open);
    invoke(app, "window.workspace.motion", "{}");
    CHECK(ui->timeline.open);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "timeline.delete");
    CHECK(!document(app)->has_timeline && !ui->timeline.playing);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_variables_draft_cancel_and_atomic_confirmation(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":32}");
    invoke(app, "layer.new.layer", "{}");
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_layer_id layer = pc_engine_session_active(session)->active_layer;
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(app, "image.variables.define", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->n_dialogs == 1);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t history = history_current(app);
    uint64_t revision = pc_engine_session_active(session)->revision;
    click_widget(app, "variables.newDefinition");
    CHECK(document(app)->variables.ndefs == 0);
    click_widget(app, "variables.dataSets");
    click_widget(app, "variables.newDataSet");
    click_widget(app, "variables.addValue.0");
    click_widget(app, "variables.visible.0");
    click_widget(app, "variables.apply");
    CHECK(pc_engine_session_active(session)->revision == revision);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    char json[128];
    (void)snprintf(json, sizeof json, "{\"dialog\":%llu}", (unsigned long long)ui->dialogs[0].id);
    yyjson_doc *request = NULL;
    CHECK(pc_json_parse(json, strlen(json), &request, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.confirm", yyjson_doc_get_root(request), &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    yyjson_doc_free(request);
    CHECK(ui->n_dialogs == 0 && document(app)->variables.ndefs == 1);
    CHECK(document(app)->variables.ndata_sets == 1);
    CHECK(!pc_document_layer(document(app), layer)->visible);
    CHECK(history_current(app) == history + 1);
    CHECK(pc_engine_session_undo(session));
    CHECK(document(app)->variables.ndefs == 0 && pc_document_layer(document(app), layer)->visible);
    CHECK(pc_ui_shell_app_invoke(app, "image.variables.dataSets", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "variables.newDataSet");
    pc_ui_dialog_runtime *runtime = pc_ui_shell_dialog_runtime(app);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float x = runtime->cancel[0] + runtime->cancel[2] / 2, y = runtime->cancel[1] + runtime->cancel[3] / 2;
    for (unsigned frame = 0; frame < 3; frame++) {
        nk_input_begin(ctx);
        if (frame < 2) {
            motion(app, x, y);
            button(app, frame == 0, x, y);
        }
        nk_input_end(ctx);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    }
    CHECK(ui->n_dialogs == 0 && document(app)->variables.ndata_sets == 0);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_export_and_document_forms(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":32}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(pc_ui_shell_app_invoke(app, "file.export.exportAs", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ui->n_dialogs == 1);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "export.estimate");
    yyjson_mut_val *root = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK(yyjson_mut_is_str(yyjson_mut_obj_get(root, "__estimateLabel")));
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 1);
    CHECK(strstr(pc_ui_shell_dialog_runtime(app)->error, "destination") != NULL);
    char tmp[] = "/tmp/photocraft-native-export-XXXXXX";
    CHECK(mkdtemp(tmp) != NULL);
    char path[256], request[512];
    (void)snprintf(path, sizeof path, "%s/image.png", tmp);
    uint64_t id = ui->dialogs[0].id;
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu,\"field\":\"path\",\"value\":\"%s\"}",
                   (unsigned long long)id, path);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(request, strlen(request), &parsed, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.set", yyjson_doc_get_root(parsed), &out, NULL) == PC_OK);
    yyjson_doc_free(parsed);
    pc_json_value_fini(&out);
    (void)snprintf(request, sizeof request, "{\"dialog\":%llu,\"field\":\"scale\",\"value\":50}",
                   (unsigned long long)id);
    CHECK(pc_json_parse(request, strlen(request), &parsed, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.set", yyjson_doc_get_root(parsed), &out, NULL) == PC_OK);
    yyjson_doc_free(parsed);
    pc_json_value_fini(&out);
    uint8_t *pixels = NULL;
    int width = 0, height = 0;
    capture(app, &pixels, &width, &height);
    free(pixels);
    size_t history = history_current(app);
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 0 && history_current(app) == history);
    CHECK(document(app)->size.width == 64 && document(app)->size.height == 32);
    pc_buf bytes = PC_BUF_INIT;
    CHECK(pc_file_read(path, 64 * 1024 * 1024, &bytes, NULL) == PC_OK);
    pc_cimage image = {0};
    CHECK(pc_cdecode(bytes.data, bytes.len, &image, NULL) == PC_OK);
    CHECK(image.width == 32 && image.height == 16);
    pc_cimage_fini(&image);
    pc_buf_fini(&bytes);
    CHECK(unlink(path) == 0 && rmdir(tmp) == 0);
    CHECK(pc_ui_shell_app_invoke(app, "image.imageSize", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    root = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "width")) == 64);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "height")) == 32);
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(root, "resolution")) > 0);
    click_dialog_action(app, "cancel");
    CHECK(ui->n_dialogs == 0);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_document_properties_resize_link_and_view_controls(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":32,\"background\":\"white\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->dock.has_height[PC_UI_GROUP_PROPERTIES] = true;
    ui->dock.heights[PC_UI_GROUP_PROPERTIES] = 500;
    ui->panels.color = false;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t history = history_current(app);
    increment_widget(app, "document.width");
    CHECK(document(app)->size.width == 65 && document(app)->size.height == 32);
    CHECK(history_current(app) == history + 1);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)));
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "document.link");
    increment_widget(app, "document.width");
    CHECK(document(app)->size.width == 65 && document(app)->size.height == 33);
    click_widget(app, "document.portrait");
    CHECK(document(app)->size.width == 33 && document(app)->size.height == 65);
    bool rulers = ui->extras.rulers;
    click_widget(app, "view.rulers");
    CHECK(ui->extras.rulers != rulers);
    bool guides = ui->extras.guides;
    click_widget(app, "view.show.guides");
    CHECK(ui->extras.guides != guides);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_rasterize_prompt_cancel_resume_filter_and_stale_target(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":160,\"height\":120}");
    invoke(app, "type.create", "{\"x\":20,\"y\":60,\"text\":\"Editable\",\"size\":24}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_BRUSH;
    pc_layer_id layer = pc_engine_session_active(session)->active_layer;
    size_t history = history_current(app);
    pc_ui_tool_point point = {.x = 30, .y = 40, .pressure = 1};
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_rasterize_needed(session, ui) != NULL);
    CHECK(pc_ui_shell_app_pointer(app, PC_UI_POINTER_DOWN, point, 0, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ui->n_dialogs == 1 && history_current(app) == history);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_dialog_action(app, "cancel");
    CHECK(ui->n_dialogs == 0 && history_current(app) == history);
    CHECK(pc_document_layer(document(app), layer)->content.kind == PC_LAYER_TEXT);
    CHECK(pc_ui_shell_app_pointer(app, PC_UI_POINTER_DOWN, point, 0, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 0 && history_current(app) == history + 2);
    CHECK(pc_document_layer(document(app), layer)->content.kind == PC_LAYER_RASTER);
    CHECK(pc_engine_session_undo(session) && pc_engine_session_undo(session));
    CHECK(pc_document_layer(document(app), layer)->content.kind == PC_LAYER_TEXT);
    bool smart = false;
    CHECK(pc_ui_rasterize_filter_needed(session, "filter.blur.gaussianBlur", &smart) && smart);
    CHECK(pc_ui_shell_app_invoke(app, "filter.blur.gaussianBlur", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "rasterize.smartObject");
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 1);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(ui->dialogs[0].fields);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "__command")), "filter.blur.gaussianBlur");
    CHECK(pc_document_layer(document(app), pc_engine_session_active(session)->active_layer)->content.kind ==
          PC_LAYER_SMART);
    click_dialog_action(app, "cancel");
    CHECK(pc_engine_session_undo(session));
    CHECK(pc_document_layer(document(app), layer)->content.kind == PC_LAYER_TEXT);
    CHECK(pc_ui_shell_app_pointer(app, PC_UI_POINTER_DOWN, point, 0, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(session, "layer.setProps", "{\"opacity\":0.5}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 1);
    CHECK(pc_document_layer(document(app), layer)->content.kind == PC_LAYER_TEXT);
    click_dialog_action(app, "cancel");
    invoke(app, "layer.layerMask.revealAll", "{}");
    invoke(app, "channel.target", "{\"channel\":\"mask\"}");
    CHECK(pc_ui_rasterize_needed(session, ui) == NULL);
    CHECK(pc_ui_shell_app_pointer(app, PC_UI_POINTER_DOWN, point, 0, &out, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_pointer(app, PC_UI_POINTER_UP, point, 0, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ui->n_dialogs == 0 && pc_document_layer(document(app), layer)->content.kind == PC_LAYER_TEXT);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_ui_font_shaping_joining_bidi_and_cache_bounds(void)
{
    const float scales[] = {1, 1.5f, 2};
    for (size_t scale = 0; scale < 3; scale++) {
        ui_font font = {0};
        CHECK(ui_font_init(&font, "assets/fonts/Inter-Regular.ttf", 18, scales[scale], NULL) == PC_OK);
        ui_font_shape_info info = {0};
        const char *joined = "لا";
        CHECK(ui_font_has_glyph(&font, 0x644) && ui_font_has_glyph(&font, 0x627));
        CHECK(ui_font_shape_metrics(&font, joined, (int)strlen(joined), &info));
        CHECK(info.glyphs == 1 && info.missing == 0 && info.width > 0);
        CHECK(info.width == font.nk.width(font.nk.userdata, font.nk.height, joined, (int)strlen(joined)));
        const char *mixed = "PhotoCraft العربية 123 עברית";
        CHECK(ui_font_shape_metrics(&font, mixed, (int)strlen(mixed), &info));
        CHECK(info.runs >= 4 && info.missing == 0 && info.width > 100);
        size_t glyphs = font.nglyphs;
        float width = info.width;
        for (unsigned i = 0; i < 80; i++) {
            CHECK(ui_font_shape_metrics(&font, mixed, (int)strlen(mixed), &info));
            CHECK(info.width == width && font.nglyphs == glyphs);
        }
        CHECK(!ui_font_shape_metrics(&font, "plain", 5, &info));
        CHECK(!ui_font_shape_metrics(&font, "\xff", 1, &info));
        CHECK(!ui_font_shape_metrics(&font, mixed, 20000, &info));
        CHECK(!ui_font_has_glyph(&font, 0x110000));
        for (unsigned i = 0; i < 160; i++) {
            char text[100];
            (void)snprintf(text, sizeof text, "العربية %u", i);
            CHECK(ui_font_shape_metrics(&font, text, (int)strlen(text), &info));
        }
        CHECK(font.nglyphs <= 8192);
        ui_font_fini(&font);
    }
}

static void test_native_shaped_multilingual_layer_names(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":160,\"height\":120,\"name\":\"PhotoCraft العربية עברית\"}");
    invoke(app, "layer.new.layer", "{\"name\":\"العربية لا\"}");
    invoke(app, "layer.new.layer", "{\"name\":\"עברית 123\"}");
    invoke(app, "layer.new.layer", "{\"name\":\"日本語 中文\"}");
    invoke(app, "layer.new.layer", "{\"name\":\"PhotoCraft العربية 123\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1.5f, true, NULL) == PC_OK);
    uint8_t *pixels = NULL;
    int width = 0, height = 0;
    capture(app, &pixels, &width, &height);
    CHECK(pixels != NULL && width > 0 && height > 0);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static bool drawn_text(struct nk_context *ctx, const char *expected)
{
    const struct nk_command *command;
    size_t length = strlen(expected);
    nk_foreach(command, ctx)
    {
        if (command->type == NK_COMMAND_TEXT) {
            const struct nk_command_text *text = (const struct nk_command_text *)command;
            if (text->length >= 0 && (size_t)text->length == length &&
                !memcmp(text->string, expected, length))
                return true;
        }
    }
    return false;
}
static void test_native_live_dialog_catalog_all_registered_languages(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":16,\"height\":12}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(app, "file.export.exportAs", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    for (size_t i = 0; i < pc_i18n_count(); i++) {
        const pc_i18n_lang *language = pc_i18n_at(i);
        char json[160];
        (void)snprintf(json, sizeof json, "{\"path\":\"interface.language\",\"value\":\"%s\"}",
                       pc_i18n_lang_code(language));
        CHECK(pc_engine_session_execute_json(session, "prefs.set", json, &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        CHECK(pc_i18n_current() == language);
        pc_ui_shell_dialogs_draw(app, pc_ui_shell_dialog_runtime(app), 1280, 800);
        CHECK(drawn_text(pc_ui_shell_app_nk(app), pc_i18n_tr(language, "Cancel")));
        CHECK(drawn_text(pc_ui_shell_app_nk(app), pc_i18n_tr(language, "Browse…")));
        nk_clear(pc_ui_shell_app_nk(app));
    }
    CHECK(pc_engine_session_execute_json(session, "prefs.set",
                                         "{\"path\":\"interface.language\",\"value\":\"fr\"}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    /* The direct command-list probe above deliberately builds an extra Nuklear frame.
     * Reopen through the normal host before checking its rendered result. */
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    CHECK(ui->n_dialogs == 1);
    CHECK(pc_ui_state_close_dialog(ui, ui->dialogs[0].id));
    CHECK(pc_ui_shell_app_invoke(app, "file.export.exportAs", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    uint8_t *pixels = NULL;
    int width = 0, height = 0;
    capture(app, &pixels, &width, &height);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
    pc_i18n_set_current(pc_i18n_en());
}

static void test_native_brush_picker_cards_controls_and_dismissal(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_ui_brush_preset_matches("Érable", "ér"));
    CHECK(!pc_ui_brush_preset_matches("Érable", "xyz"));
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_RIGHT;
    event.button.down = true;
    event.button.x = xf.x + 100;
    event.button.y = xf.y + 70;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    motion(app, event.button.x, event.button.y);
    pc_ui_shell_input_event(app, &event);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_state(app)->brush_picker.open);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    nk_input_begin(ctx);
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.down = false;
    pc_ui_shell_input_event(app, &event);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    const pc_engine_tool_state *tools = pc_engine_session_tools(session);
    CHECK(tools->npresets > 1);
    size_t history = history_current(app);
    click_widget(app, "picker.preset.1");
    CHECK_EQ_STR(tools->current_preset, pc_str_c(&tools->presets[1].name));
    float size = tools->brush.size;
    increment_widget(app, "picker.size");
    CHECK(tools->brush.size == size + 1 && history_current(app) == history);
    uint8_t *pixels = NULL;
    int width = 0, height = 0;
    capture(app, &pixels, &width, &height);
    free(pixels);
    click_widget(app, "picker.show.0");
    click_widget(app, "picker.show.1");
    pc_ui_brush_picker *picker = &pc_ui_shell_app_state(app)->brush_picker;
    CHECK(!picker->show_name && !picker->show_stroke && picker->show_tip);
    click_widget(app, "picker.show.2");
    CHECK(picker->show_tip);
    key(app, SDLK_ESCAPE, true, 0);
    key(app, SDLK_ESCAPE, false, 0);
    CHECK(!picker->open);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    yyjson_doc *params = NULL;
    const char *json = "{\"brushPicker\":[300,100],\"brushPickerScale\":0.3}";
    CHECK(pc_json_parse(json, strlen(json), &params, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.set", yyjson_doc_get_root(params), &out, NULL) == PC_OK);
    yyjson_doc_free(params);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    capture(app, &pixels, &width, &height);
    free(pixels);
    CHECK_NEAR(picker->point[0], 300, .1);
    nk_input_begin(ctx);
    motion(app, xf.x + 10, xf.y + 10);
    button(app, true, xf.x + 10, xf.y + 10);
    nk_input_end(ctx);
    CHECK(!picker->open && history_current(app) == history);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_json_value saved = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_prefs_get(session, "brushPicker", &saved, NULL) == PC_OK);
    yyjson_mut_val *saved_view = pc_json_value_root(&saved);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(saved_view, "showName")));
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(saved_view, "showTip")));
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_obj_get(saved_view, "scale")), .3, .00001);
    pc_json_value_fini(&saved);
    char *preferences = pc_engine_session_prefs_to_json(session);
    CHECK(preferences != NULL);
    pc_ui_shell_app *restored = NULL;
    CHECK(pc_ui_shell_app_new(&restored, NULL) == PC_OK);
    CHECK(pc_engine_session_load_prefs_json(pc_ui_shell_app_session(restored), preferences,
                                            strlen(preferences), NULL) == PC_OK);
    pc_ui_shell_apply_preferences(restored);
    CHECK(!pc_ui_shell_app_state(restored)->brush_picker.show_name);
    CHECK_NEAR(pc_ui_shell_app_state(restored)->brush_picker.scale, .3, .00001);
    free(preferences);
    pc_ui_shell_app_free(restored);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_free_transform_preview_local_history_apply_and_cancel(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":400,\"height\":300}");
    invoke(app, "layer.new.layer", "{}");
    invoke(app, "select.rect", "{\"x\":100,\"y\":100,\"width\":100,\"height\":60}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    invoke(app, "select.deselect", "{}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_sync_views(app, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_BRUSH; /* Transform input must outrank the ordinary painting route. */
    ui->views[0].zoom = 1;
    ui->views[0].center[0] = 200;
    ui->views[0].center[1] = 150;
    ui->views[0].fit_pending = false;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t history = history_current(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    uint64_t revision = pc_engine_session_active(session)->revision;
    invoke(app, "edit.freeTransform", "{}");
    CHECK(ui->transform.active && ui->transform.frame.rect[0] == 100);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float from[2], to[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 200, 160, from) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 250, 190, to) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    motion(app, from[0] + 3, from[1] + 3);
    button(app, true, from[0] + 3, from[1] + 3); /* Generous corner hit region. */
    motion(app, to[0], to[1]);
    button(app, false, to[0], to[1]);
    nk_input_end(ctx);
    CHECK(ui->transform.active);
    CHECK_NEAR(ui->transform.frame.quad[2][0], 250, 1e-6);
    CHECK_NEAR(ui->transform.frame.quad[2][1], 190, 1e-6);
    CHECK(history_current(app) == history && pc_engine_session_active(session)->revision == revision);
    CHECK(ui->transform.can_undo && !ui->transform.can_redo);
    pc_ui_transform_frame scaled = ui->transform.frame;
    ui->tool = PC_TOOL_HAND;
    float center_before[2] = {ui->views[0].center[0], ui->views[0].center[1]};
    CHECK(pc_ui_canvas_to_screen(&xf, 250, 190, from) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 260, 196, to) == PC_OK);
    nk_input_begin(ctx);
    motion(app, from[0], from[1]);
    button(app, true, from[0], from[1]);
    motion(app, to[0], to[1]);
    button(app, false, to[0], to[1]);
    nk_input_end(ctx);
    CHECK_NEAR(ui->transform.frame.quad[2][0], 260, 1e-6);
    CHECK(ui->views[0].center[0] == center_before[0] && ui->views[0].center[1] == center_before[1]);
    invoke(app, "edit.undo", "{}");
    ui->tool = PC_TOOL_BRUSH;

    invoke(app, "edit.undo", "{}");
    CHECK_NEAR(ui->transform.frame.quad[2][0], 200, 1e-6);
    CHECK(!ui->transform.can_undo && ui->transform.can_redo);
    invoke(app, "edit.redo", "{}");
    CHECK_NEAR(ui->transform.frame.quad[2][0], scaled.quad[2][0], 1e-6);
    key(app, SDLK_RIGHT, true, 0);
    key(app, SDLK_RIGHT, false, 0);
    CHECK_NEAR(ui->transform.frame.quad[0][0], 101, 1e-6);
    invoke(app, "edit.transform.flipHorizontal", "{}");
    CHECK(ui->transform.active && ui->transform.frame.quad[0][0] > ui->transform.frame.quad[1][0]);
    invoke(app, "edit.undo", "{}");
    CHECK_NEAR(ui->transform.frame.quad[0][0], 101, 1e-6);
    pc_ui_transform_runtime *runtime = pc_ui_shell_transform_runtime(app);
    for (size_t i = 0; i < 1000 && !runtime->preview.result; i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(runtime->preview.result != NULL);
    CHECK(history_current(app) == history);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    free(pixels);
    key(app, SDLK_ESCAPE, true, 0);
    key(app, SDLK_ESCAPE, false, 0);
    CHECK(!ui->transform.active && history_current(app) == history);
    invoke(app, "edit.freeTransform", "{}");
    key(app, SDLK_RIGHT, true, SDL_KMOD_SHIFT);
    key(app, SDLK_RIGHT, false, SDL_KMOD_SHIFT);
    CHECK_NEAR(ui->transform.frame.quad[0][0], 110, 1e-6);
    pc_ui_shell_app_background_jobs(app, true);
    key(app, SDLK_RETURN, true, 0);
    key(app, SDLK_RETURN, false, 0);
    CHECK(!ui->transform.active);
    for (size_t i = 0; i < 1000 && pc_engine_session_has_jobs(session); i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(!pc_engine_session_has_jobs(session) && history_current(app) == history + 1);
    const pc_layer *layer = pc_document_layer(document(app), pc_engine_session_active(session)->active_layer);
    CHECK(layer != NULL);
    if (layer)
        CHECK(pc_surface_content_bounds(pc_layer_surface(layer)).x0 == 110);
    CHECK(pc_engine_session_undo(session));
    invoke(app, "select.rect", "{\"x\":100,\"y\":100,\"width\":100,\"height\":60}");
    history = history_current(app);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(app, "select.transformSelection", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ui->transform.active && ui->transform.selection);
    key(app, SDLK_DOWN, true, 0);
    key(app, SDLK_DOWN, false, 0);
    invoke(app, "ui.transform.commit", "{}");
    for (size_t i = 0; i < 1000 && pc_engine_session_has_jobs(session); i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(history_current(app) == history + 1);
    CHECK(pc_surface_content_bounds(&document(app)->selection).y0 == 101);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void test_native_free_transform_path_type_scope_and_noop(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160}");
    invoke(app, "path.set", "{\"path\":{\"subpaths\":[{\"knots\":[[20,20],[100,20],[100,100],[20,100]]}]}}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_PATH_SELECTION;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t history = history_current(app);
    invoke(app, "edit.freeTransform", "{}");
    CHECK(ui->transform.active && ui->transform.path && !strcmp(ui->transform.path_name, "work"));
    invoke(app, "ui.transform.commit", "{}");
    CHECK(!ui->transform.active && history_current(app) == history);
    invoke(app, "edit.freeTransform", "{}");
    key(app, SDLK_RIGHT, true, SDL_KMOD_SHIFT);
    key(app, SDLK_RIGHT, false, SDL_KMOD_SHIFT);
    pc_ui_transform_runtime *runtime = pc_ui_shell_transform_runtime(app);
    for (size_t i = 0; i < 1000 && !runtime->preview.result; i++) {
        CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
        SDL_Delay(1);
    }
    CHECK(runtime->preview.result != NULL);
    CHECK(document(app)->work_path.subpaths[0].knots[0].anchor.x == 20);
    if (runtime->preview.result)
        CHECK(pc_doc_arc_get(runtime->preview.result)->work_path.subpaths[0].knots[0].anchor.x == 30);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    invoke(app, "ui.transform.commit", "{}");
    CHECK(document(app)->work_path.subpaths[0].knots[0].anchor.x == 30);
    CHECK(history_current(app) == history + 1);
    ui->tool = PC_TOOL_MOVE;
    invoke(app, "layer.new.layer", "{}");
    invoke(app, "select.rect", "{\"x\":20,\"y\":20,\"width\":30,\"height\":30}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    invoke(app, "select.deselect", "{}");
    history = history_current(app);
    size_t layers = document(app)->nlayers;
    invoke(app, "edit.freeTransformCopy", "{}");
    CHECK(ui->transform.active && ui->transform.copy);
    key(app, SDLK_ESCAPE, true, 0);
    key(app, SDLK_ESCAPE, false, 0);
    CHECK(!ui->transform.active && document(app)->nlayers == layers && history_current(app) == history);
    invoke(app, "edit.freeTransformCopy", "{}");
    key(app, SDLK_RIGHT, true, SDL_KMOD_SHIFT);
    key(app, SDLK_RIGHT, false, SDL_KMOD_SHIFT);
    pc_ui_shell_app_background_jobs(app, true);
    invoke(app, "ui.transform.commit", "{}");
    CHECK(!ui->transform.active && !pc_engine_session_has_jobs(pc_ui_shell_app_session(app)));
    CHECK(document(app)->nlayers == layers + 1 && history_current(app) == history + 1);
    CHECK(pc_engine_session_undo(pc_ui_shell_app_session(app)));
    CHECK(document(app)->nlayers == layers);
    invoke(app, "type.create", "{\"x\":20,\"y\":60,\"text\":\"Hello\",\"size\":36}");
    ui->tool = PC_TOOL_RECT_MARQUEE;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_layer *layer = pc_document_layer(document(app), state->active_layer);
    pc_rect whole = pc_surface_content_bounds(pc_layer_surface(layer));
    invoke(app, "select.rect", "{\"x\":20,\"y\":20,\"width\":2,\"height\":2}");
    invoke(app, "edit.freeTransform", "{}");
    CHECK(ui->transform.type_layer && !ui->transform.path);
    CHECK(ui->transform.frame.rect[0] == whole.x0 && ui->transform.frame.rect[2] == whole.x1);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_invoke(app, "edit.transform.distort", NULL, &out, NULL) != PC_OK);
    pc_json_value_fini(&out);
    history = history_current(app);
    pc_ui_tool_point point = {.x = (float)whole.x0, .y = (float)whole.y0, .pressure = 1};
    CHECK(pc_ui_shell_transform_pointer(app, PC_UI_POINTER_DOWN, point, 0, &out, NULL) == PC_OK);
    point.x = NAN;
    CHECK(pc_ui_shell_transform_pointer(app, PC_UI_POINTER_UP, point, 0, &out, NULL) != PC_OK);
    CHECK(!runtime->dragging && history_current(app) == history);
    pc_json_value_fini(&out);
    /* External engine changes invalidate the captured frame; it must never be applied to the new target. */
    CHECK(pc_engine_session_execute_json(pc_ui_shell_app_session(app), "layer.new.layer", "{}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(!ui->transform.active && history_current(app) == history + 1);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void right_button(pc_ui_shell_app *app, bool down, float x, float y)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_RIGHT;
    event.button.down = down;
    event.button.x = x;
    event.button.y = y;
    pc_ui_shell_input_event(app, &event);
}
static void test_native_context_menu_selection_transform_layer_mask_and_erase(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160}");
    invoke(app, "layer.new.layer", "{}");
    invoke(app, "select.rect", "{\"x\":20,\"y\":20,\"width\":100,\"height\":60}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_RECT_MARQUEE;
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float point[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 50, 40, point) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    size_t history = history_current(app);
    nk_input_begin(ctx);
    motion(app, point[0], point[1]);
    right_button(app, true, point[0], point[1]);
    right_button(app, false, point[0], point[1]);
    nk_input_end(ctx);
    CHECK(ui->context_menu.open && ui->context_menu.kind == PC_UI_CONTEXT_SELECTION);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_widget(app, "context.select.modify.feather");
    CHECK(!ui->context_menu.open && ui->n_dialogs == 1 && history_current(app) == history);
    key(app, SDLK_ESCAPE, true, 0);
    key(app, SDLK_ESCAPE, false, 0);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(ui->n_dialogs == 0);
    invoke(app, "select.deselect", "{}");
    invoke(app, "edit.freeTransform", "{}");
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    CHECK(pc_ui_canvas_to_screen(&xf, 50, 40, point) == PC_OK);
    nk_input_begin(ctx);
    motion(app, point[0], point[1]);
    right_button(app, true, point[0], point[1]);
    right_button(app, false, point[0], point[1]);
    nk_input_end(ctx);
    CHECK(ui->context_menu.kind == PC_UI_CONTEXT_TRANSFORM && ui->context_menu.open);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "context.edit.transform.flipHorizontal");
    CHECK(ui->transform.active && ui->transform.frame.quad[0][0] > ui->transform.frame.quad[1][0]);
    key(app, SDLK_ESCAPE, true, 0);
    key(app, SDLK_ESCAPE, false, 0);
    CHECK(!ui->transform.active);
    ui->tool = PC_TOOL_BRUSH;
    invoke(app, "prefs.set", "{\"path\":\"tools.rightClickWithPaintingTools\",\"value\":\"erase\"}");
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    CHECK(pc_ui_canvas_to_screen(&xf, 50, 40, point) == PC_OK);
    history = history_current(app);
    nk_input_begin(ctx);
    motion(app, point[0], point[1]);
    right_button(app, true, point[0], point[1]);
    motion(app, point[0] + 3, point[1]);
    right_button(app, false, point[0] + 3, point[1]);
    nk_input_end(ctx);
    CHECK(history_current(app) == history + 1 && !ui->brush_picker.open && ui->tool == PC_TOOL_BRUSH);
    invoke(app, "edit.undo", "{}");
    invoke(app, "layer.layerMask.revealAll", "{}");
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_layer_id active = pc_engine_session_active(pc_ui_shell_app_session(app))->active_layer;
    float rect[4];
    CHECK(pc_ui_shell_app_panel_mapping(app, "layer.mask.thumbnail", active, rect));
    nk_input_begin(ctx);
    right_button(app, true, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2);
    right_button(app, false, rect[0] + rect[2] / 2, rect[1] + rect[3] / 2);
    nk_input_end(ctx);
    CHECK(ui->context_menu.open && ui->context_menu.kind == PC_UI_CONTEXT_MASK);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "context.layer.layerMask.disable");
    CHECK(!pc_document_layer(document(app), active)->mask.enabled);
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static pc_status crop_retained_pixels(pc_document *doc, bool *has_active, pc_layer_id *active, void *context,
                                      pc_error *err)
{
    (void)has_active;
    (void)context;
    pc_layer *layer = pc_document_layer_mut(doc, *active);
    const float green[4] = {0, 1, 0, 1};
    return layer ? pc_surface_fill_rect(pc_layer_surface_mut(layer), pc_rect_make(-15, 30, -5, 45), green, 4,
                                        err)
                 : PC_ERR_INVALID;
}
static void crop_drag(pc_ui_shell_app *app, float x0, float y0, float x1, float y1)
{
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float from[2], to[2];
    CHECK(pc_ui_canvas_to_screen(&xf, x0, y0, from) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, x1, y1, to) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    motion(app, from[0], from[1]);
    button(app, true, from[0], from[1]);
    motion(app, to[0], to[1]);
    button(app, false, to[0], to[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
}
static void test_native_crop_pending_handles_plane_modes_focus_and_commit(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160}");
    invoke(app, "layer.new.layer", "{}");
    pc_engine_session *session = pc_ui_shell_app_session(app);
    CHECK(pc_engine_session_edit(session, "retained pixels", crop_retained_pixels, NULL, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    CHECK(pc_ui_shell_app_sync_views(app, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->extras.snap = false; /* Explicit geometry; snapping has separate control cases. */
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    ui->tool = PC_TOOL_CROP;
    ui->tool_options.crop_shield.classic_mode = true;
    ui->tool_options.crop_shield.enabled = false;
    ui->tool_options.crop_overlay_show = PC_UI_OVERLAY_ALWAYS;
    ui->views[0].zoom = 2;
    ui->views[0].center[0] = 100;
    ui->views[0].center[1] = 80;
    ui->views[0].fit_pending = false;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t history = history_current(app);
    const pc_doc_arc *before = pc_engine_session_active(session)->doc;
    CHECK(ui->has_crop && !ui->crop_editing);
    crop_drag(app, 20, 20, 120, 80);
    CHECK(ui->has_crop && ui->crop_editing && history_current(app) == history &&
          pc_engine_session_active(session)->doc == before);
    CHECK_NEAR(ui->crop_rect[0], 20, .001);
    CHECK_NEAR(ui->crop_rect[2], 120, .001);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float point[2];
    CHECK(pc_ui_canvas_to_screen(&xf, -10, 35, point) == PC_OK);
    uint8_t *pixels = NULL;
    int width, height;
    capture(app, &pixels, &width, &height);
    if (pixels && point[0] >= 0 && point[1] >= 0 && point[0] < (float)width && point[1] < (float)height) {
        const uint8_t *pixel = pixels + ((size_t)point[1] * (size_t)width + (size_t)point[0]) * 4;
        CHECK(pixel[1] > 240 && pixel[0] < 15 && pixel[2] < 15);
    }
    free(pixels);
    /* Escape dismisses an options popup before it can cancel the pending crop. */
    click_widget(app, "crop.preset");
    bool popup = false;
    for (const struct nk_window *window = ctx->begin; window; window = window->next)
        popup = popup || (!(window->flags & (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED)) && window->popup.active);
    CHECK(popup);
    key(app, SDLK_ESCAPE, true, 0);
    key(app, SDLK_ESCAPE, false, 0);
    CHECK(ui->crop_editing);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    crop_drag(app, 120, 80, 140, 90);
    CHECK_NEAR(ui->crop_rect[2], 140, .001);
    CHECK_NEAR(ui->crop_rect[3], 90, .001);
    invoke(app, "edit.undo", "{}");
    CHECK_NEAR(ui->crop_rect[2], 120, .001);
    invoke(app, "edit.redo", "{}");
    CHECK_NEAR(ui->crop_rect[2], 140, .001);
    key(app, SDLK_ESCAPE, true, 0);
    key(app, SDLK_ESCAPE, false, 0);
    CHECK(!ui->crop_editing && history_current(app) == history);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    ui->tool_options.crop_shield.classic_mode = false;
    crop_drag(app, 20, 20, 120, 80);
    float center[2] = {ui->views[0].center[0], ui->views[0].center[1]};
    crop_drag(app, 40, 40, 50, 45);
    CHECK_NEAR(ui->crop_rect[0], 10, .001);
    CHECK_NEAR(ui->crop_rect[1], 15, .001);
    CHECK_NEAR(ui->views[0].center[0], center[0] - 10, .001);
    CHECK_NEAR(ui->views[0].center[1], center[1] - 5, .001);
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    CHECK(pc_ui_canvas_to_screen(&xf, 40, 40, point) == PC_OK);
    double original_x = ui->crop_rect[0];
    float original_center = ui->views[0].center[0];
    nk_input_begin(ctx);
    motion(app, point[0], point[1]);
    button(app, true, point[0], point[1]);
    motion(app, point[0] + 20, point[1]);
    SDL_Event focus;
    SDL_zero(focus);
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    pc_ui_shell_input_event(app, &focus);
    nk_input_end(ctx);
    CHECK(!pc_ui_shell_crop_runtime(app)->drag.active);
    CHECK_NEAR(ui->crop_rect[0], original_x, .001);
    CHECK_NEAR(ui->views[0].center[0], original_center, .001);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    invoke(app, "ui.crop.straighten", "{}");
    crop_drag(app, 20, 20, 100, 35);
    CHECK(fabs(ui->crop_angle) > 1 && fabs(ui->crop_angle) < 45);
    CHECK_NEAR(ui->views[0].rotation, -ui->crop_angle, .001);
    CHECK(history_current(app) == history);
    ui->tool_options.crop_shield.enabled = true;
    capture(app, &pixels, &width, &height);
    free(pixels);
    key(app, SDLK_RETURN, true, 0);
    key(app, SDLK_RETURN, false, 0);
    CHECK(!ui->crop_editing && history_current(app) == history + 1);
    CHECK(pc_engine_session_undo(session));
    CHECK(document(app)->size.width == 200 && document(app)->size.height == 160);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}

static void lasso_click(pc_ui_shell_app *app, float x, float y, unsigned clicks)
{
    pc_ui_canvas_xform xf;
    float p[2];
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    CHECK(pc_ui_canvas_to_screen(&xf, x, y, p) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    motion(app, p[0], p[1]);
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = true;
    e.button.x = p[0];
    e.button.y = p[1];
    e.button.clicks = (Uint8)clicks;
    pc_ui_shell_input_event(app, &e);
    button(app, false, p[0], p[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
}
static void test_native_lasso_pending_double_click_retract_and_magnetic(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(app, "view.actualPixels", "{}");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_POLYGON_LASSO;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t history = history_current(app);
    lasso_click(app, 20, 20, 1);
    lasso_click(app, 120, 20, 1);
    lasso_click(app, 120, 120, 1);
    CHECK(ui->lasso.path.n == 3 && history_current(app) == history);
    float original_center = ui->views[0].center[0];
    pc_ui_canvas_xform nav;
    float nav_point[2];
    CHECK(pc_ui_shell_app_canvas_mapping(app, &nav));
    CHECK(pc_ui_canvas_to_screen(&nav, 70, 70, nav_point) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    key(app, SDLK_SPACE, true, 0);
    button(app, true, nav_point[0], nav_point[1]);
    motion(app, nav_point[0] + 10, nav_point[1]);
    button(app, false, nav_point[0] + 10, nav_point[1]);
    key(app, SDLK_SPACE, false, 0);
    nk_input_end(ctx);
    CHECK(ui->lasso.active && ui->lasso.path.n == 3);
    CHECK_NEAR(ui->views[0].center[0], original_center - 10, .001);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);

    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    key(app, SDLK_BACKSPACE, true, 0);
    key(app, SDLK_BACKSPACE, false, 0);
    CHECK(ui->lasso.path.n == 2);
    lasso_click(app, 120, 120, 1);
    lasso_click(app, 120, 120, 2);
    CHECK(!ui->lasso.active && document(app)->has_selection && history_current(app) == history + 1);
    lasso_click(app, 30, 30, 1);
    key(app, SDLK_ESCAPE, true, 0);
    key(app, SDLK_ESCAPE, false, 0);
    CHECK(!ui->lasso.active && history_current(app) == history + 1);
    ui->tool = PC_TOOL_MAGNETIC_LASSO;
    lasso_click(app, 20, 20, 1);
    lasso_click(app, 120, 20, 1);
    lasso_click(app, 120, 120, 1);
    CHECK(ui->lasso.active && ui->lasso.nanchors >= 3);
    capture(app, &pixels, &w, &h);
    free(pixels);
    key(app, SDLK_RETURN, true, SDL_KMOD_ALT);
    key(app, SDLK_RETURN, false, SDL_KMOD_ALT);
    CHECK(!ui->lasso.active && history_current(app) == history + 2);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_closed_document_releases_gpu_checkpoint(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":128,\"height\":128}");
    invoke(app, "layer.new.layer", "{}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_json_value info = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_shell_app_gpu_info(app, &info, NULL) == PC_OK);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&info), "prefixCacheBytes")) > 0);
    pc_json_value_fini(&info);
    CHECK(pc_engine_session_execute_json(pc_ui_shell_app_session(app), "file.close", "{}", &info, NULL) ==
          PC_OK);
    pc_json_value_fini(&info);
    capture(app, &pixels, &w, &h);
    free(pixels);
    CHECK(pc_ui_shell_app_gpu_info(app, &info, NULL) == PC_OK);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&info), "prefixCacheBytes")) == 0);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&info), "prefixCacheDocument")) == 0);
    pc_json_value_fini(&info);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_ui_locale_font_coverage_and_scale_matrix(void)
{
    const float scales[] = {1, 1.5f, 2};
    const char *keys[] = {"File", "Edit", "Select", "Filter",      "View",   "Window", "Help", "New",
                          "Open", "Save", "Layers", "Preferences", "Cancel", "OK",     "Brush"};
    for (size_t scale = 0; scale < 3; scale++) {
        ui_font font = {0};
        CHECK(ui_font_init(&font, "assets/fonts/Inter-Regular.ttf", 16, scales[scale], NULL) == PC_OK);
        for (size_t language = 0; language < pc_i18n_count(); language++) {
            const pc_i18n_lang *lang = pc_i18n_at(language);
            size_t missing = 0;
            for (size_t key_ = 0; key_ <= sizeof keys / sizeof *keys; key_++) {
                const char *text = key_ == sizeof keys / sizeof *keys ? pc_i18n_lang_name(lang)
                                                                      : pc_i18n_tr(lang, keys[key_]);
                size_t length = strlen(text), offset = 0;
                uint32_t cp;
                bool available = true;
                while (pc_utf8_next((const uint8_t *)text, length, &offset, &cp)) {
                    if (!ui_font_has_glyph(&font, cp)) {
                        available = false;
                        missing++;
                    }
                }
                ui_font_shape_info info = {0};
                bool shaped = ui_font_shape_metrics(&font, text, (int)length, &info);
                float width =
                    shaped ? info.width : font.nk.width(font.nk.userdata, font.nk.height, text, (int)length);
                CHECK(isfinite(width) && width > 0);
                if (shaped)
                    CHECK(info.runs > 0);
                if (available && shaped)
                    CHECK(info.missing == 0);
            }
            if (scale == 0 && missing)
                fprintf(stderr, "Locale %s: %zu missing glyph occurrences in installed fonts\n",
                        pc_i18n_lang_code(lang), missing);
        }
        /* Nonpreferred installed families must participate in missing-glyph protection too. */
        pc_fontdb *db = NULL;
        CHECK(pc_fontdb_with_system_fonts(&db, NULL) == PC_OK);
        if (pc_fontdb_has_family(db, "Kochi Gothic", 12) || pc_fontdb_has_family(db, "VL Gothic", 9))
            CHECK(ui_font_has_glyph(&font, 0x65e5) && ui_font_has_glyph(&font, 0x672c) &&
                  ui_font_has_glyph(&font, 0x8a9e));
        pc_fontdb_free(db);
        CHECK(font.nglyphs <= 8192 && font.atlas_w == 2048 && font.atlas_h == 2048);
        ui_font_fini(&font);
    }
}
static void test_native_extreme_pending_frames_clip_before_nuklear(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    invoke(app, "layer.new.layer", "{}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_CROP;
    ui->views[0].zoom = 32;
    ui->views[0].fit_pending = false;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    ui->crop_rect[0] = ui->crop_rect[1] = -1000000;
    ui->crop_rect[2] = ui->crop_rect[3] = 1000000;
    ui->crop_angle = 37;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    invoke(app, "ui.crop.cancel", "{}");
    ui->tool = PC_TOOL_MOVE;
    invoke(app, "edit.freeTransform", "{}");
    double huge[4] = {-1000000, -1000000, 1000000, 1000000};
    pc_ui_transform_frame_rect(&ui->transform.frame, huge);
    size_t history = history_current(app);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(history_current(app) == history);
    invoke(app, "ui.transform.cancel", "{}");
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_slice_draw_move_options_lock_and_auto_promotion(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":200,\"height\":160}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(app, "view.actualPixels", "{}");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_SLICE;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    crop_drag(app, 20, 20, 120, 80);
    CHECK(document(app)->slices.len == 1 && ui->slices.has_selected);
    ui->tool = PC_TOOL_SLICE_SELECT;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    crop_drag(app, 70, 50, 80, 55);
    CHECK(pc_rect_eq(document(app)->slices.list[0].rect, pc_rect_make(30, 25, 130, 85)));
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    lasso_click(app, 180, 140, 1);
    CHECK(ui->slices.has_selected && !pc_ui_slice_selected(&ui->slices)->has_id);
    click_widget(app, "ui.slice.options");
    CHECK(ui->n_dialogs == 1);
    if (!ui->n_dialogs) {
        pc_ui_shell_app_free(app);
        SDL_Quit();
        return;
    }
    char text[160];
    (void)snprintf(text, sizeof text, "{\"dialog\":%llu,\"field\":\"name\",\"value\":\"footer\"}",
                   (unsigned long long)ui->dialogs[0].id);
    yyjson_doc *params = NULL;
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_json_parse(text, strlen(text), &params, NULL) == PC_OK);
    CHECK(pc_ui_control_handle(app, "ui.dialog.set", yyjson_doc_get_root(params), &out, NULL) == PC_OK);
    yyjson_doc_free(params);
    pc_json_value_fini(&out);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_dialog_action(app, "confirm");
    CHECK(ui->n_dialogs == 0 && document(app)->slices.len == 2);
    invoke(app, "view.lockSlices", "{\"on\":true}");
    ui->tool = PC_TOOL_SLICE;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    crop_drag(app, 10, 10, 40, 40);
    CHECK(document(app)->slices.len == 2 && !ui->slices.dragging);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_artboard_draw_resize_options_properties_and_plus(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":400,\"height\":300}");
    invoke(app, "view.snap", "{\"on\":false}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    invoke(app, "view.actualPixels", "{}");
    pc_ui_shell_app_state(app)->tool = PC_TOOL_ARTBOARD;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    crop_drag(app, 20, 30, 140, 110);
    pc_layer_id id;
    pc_rect rect;
    CHECK(pc_ui_artboard_active(pc_engine_session_active(pc_ui_shell_app_session(app)), &id, &rect));
    CHECK(pc_rect_eq(rect, pc_rect_make(20, 30, 140, 110)));
    crop_drag(app, 140, 70, 160, 70);
    CHECK(pc_ui_artboard_active(pc_engine_session_active(pc_ui_shell_app_session(app)), &id, &rect));
    CHECK(pc_rect_width(rect) == 140);
    click_widget(app, "ui.artboard.portrait");
    CHECK(pc_ui_artboard_active(pc_engine_session_active(pc_ui_shell_app_session(app)), &id, &rect));
    CHECK(pc_rect_width(rect) == 80 && pc_rect_height(rect) == 140);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    size_t history = history_current(app);
    click_widget(app, "ui.artboard.addRight");
    CHECK(history_current(app) == history + 1);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_selection_outline_and_floating_pixels_display_and_drop(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":100,\"height\":80,\"background\":\"transparent\"}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    invoke(app, "select.rect", "{\"x\":10,\"y\":10,\"width\":20,\"height\":20}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_RECT_MARQUEE;
    ui->extras.snap = false;
    ui->view.show[4] = false;
    ui->views[0].zoom = 4;
    ui->views[0].center[0] = 50;
    ui->views[0].center[1] = 40;
    ui->views[0].fit_pending = ui->views[0].fill_pending = false;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    crop_drag(app, 15, 15, 20, 18);
    CHECK(pc_surface_sample_channel(&document(app)->selection, 32, 30, 0) == 1);
    invoke(app, "edit.undo", "{}");
    ui->tool = PC_TOOL_MOVE;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    crop_drag(app, 15, 15, 25, 15);
    int32_t offset[2];
    CHECK(pc_engine_floating_offset(pc_ui_shell_app_session(app), offset) && offset[0] == 10 &&
          offset[1] == 0);
    size_t history = history_current(app);
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    capture(app, &pixels, &w, &h);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float hole[2], piece[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 15.5f, 15.5f, hole) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 25.5f, 15.5f, piece) == PC_OK);
    size_t a = ((size_t)hole[1] * (size_t)w + (size_t)hole[0]) * 4,
           b = ((size_t)piece[1] * (size_t)w + (size_t)piece[0]) * 4;
    CHECK(pixels && abs((int)pixels[a] - (int)pixels[a + 1]) < 3);
    CHECK(pixels && pixels[b] > 230 && pixels[b + 1] < 20 && pixels[b + 2] < 20);
    free(pixels);
    key(app, SDLK_RETURN, true, SDL_KMOD_NONE);
    key(app, SDLK_RETURN, false, SDL_KMOD_NONE);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(!pc_engine_floating_offset(pc_ui_shell_app_session(app), NULL));
    CHECK(history_current(app) == history + 1);
    capture(app, &pixels, &w, &h);
    CHECK(pixels && pixels[b] > 230 && pixels[b + 1] < 20);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_move_transform_controls_start_real_resize_and_layer_edges(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64,\"background\":\"transparent\"}");
    invoke(app, "select.rect", "{\"x\":8,\"y\":8,\"width\":16,\"height\":16}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    invoke(app, "select.deselect", "{}");
    invoke(app, "prefs.set", "{\"path\":\"general.imageInterpolation\",\"value\":\"nearestNeighbor\"}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_MOVE;
    ui->tool_options.move_show_transform = true;
    ui->tool_options.move_auto_select = false;
    pc_ui_shell_app_background_jobs(app, false);
    ui->extras.snap = false;
    ui->view.show[4] = false;
    ui->view.show[0] = true;
    ui->views[0].zoom = 4;
    ui->views[0].center[0] = 32;
    ui->views[0].center[1] = 32;
    ui->views[0].fit_pending = ui->views[0].fill_pending = false;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_transform_frame frame;
    CHECK(pc_ui_shell_transform_controls_frame(app, &frame));
    CHECK(frame.rect[0] == 8 && frame.rect[2] == 24);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float corner[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 24, 24, corner) == PC_OK);
    CHECK(pc_ui_shell_transform_controls_hit(&frame, (double[2]){24, 24}, 4));
    CHECK(!pc_ui_shell_transform_controls_hit(&frame, (double[2]){16, 16}, 4));
    size_t history = history_current(app);
    crop_drag(app, 24, 24, 32, 32);
    CHECK(ui->transform.active && ui->transform.can_undo);
    CHECK(!strcmp(ui->transform.interpolation, "nearest"));
    CHECK(fabs(ui->transform.frame.quad[2][0] - 32) < .1 && fabs(ui->transform.frame.quad[2][1] - 32) < .1);
    CHECK(history_current(app) == history);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    invoke(app, "ui.transform.commit", "{}");
    CHECK(!ui->transform.active && history_current(app) == history + 1);
    pc_rect bounds = pc_surface_content_bounds(pc_layer_surface(pc_document_layer(
        document(app), pc_engine_session_active(pc_ui_shell_app_session(app))->active_layer)));
    CHECK(bounds.x0 == 8 && bounds.x1 == 32 && bounds.y0 == 8 && bounds.y1 == 32);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_ui_shell_transform_controls_frame(app, &frame) && frame.rect[2] == 32);
    capture(app, &pixels, &w, &h);
    free(pixels);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_eyedropper_ring_alt_sampling_and_color_sampler(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64,\"background\":\"transparent\"}");
    invoke(app, "edit.fill", "{\"color\":\"#0000ff\"}");
    invoke(app, "select.rect", "{\"x\":0,\"y\":0,\"width\":16,\"height\":64}");
    invoke(app, "edit.fill", "{\"color\":\"#ff0000\"}");
    invoke(app, "select.deselect", "{}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_EYEDROPPER;
    ui->views[0].zoom = 4;
    ui->views[0].center[0] = 32;
    ui->views[0].center[1] = 32;
    ui->views[0].fit_pending = ui->views[0].fill_pending = false;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    pc_ui_canvas_xform xf;
    CHECK(pc_ui_shell_app_canvas_mapping(app, &xf));
    float red[2], blue[2];
    CHECK(pc_ui_canvas_to_screen(&xf, 8, 24, red) == PC_OK);
    CHECK(pc_ui_canvas_to_screen(&xf, 32, 24, blue) == PC_OK);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_input_begin(ctx);
    motion(app, red[0], red[1]);
    button(app, true, red[0], red[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_ui_shell_sampling_runtime(app)->active && pc_ui_shell_sampling_runtime(app)->has_sample);
    CHECK(pc_engine_session_tools(pc_ui_shell_app_session(app))->foreground[0] == 1);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    nk_input_begin(ctx);
    button(app, false, red[0], red[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    size_t history = history_current(app);
    ui->tool = PC_TOOL_BRUSH;
    key(app, SDLK_LALT, true, SDL_KMOD_ALT);
    nk_input_begin(ctx);
    motion(app, red[0], red[1]);
    button(app, true, red[0], red[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    key(app, SDLK_LALT, false, SDL_KMOD_NONE);
    nk_input_begin(ctx);
    motion(app, blue[0], blue[1]);
    button(app, false, blue[0], blue[1]);
    nk_input_end(ctx);
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    CHECK(pc_engine_session_tools(pc_ui_shell_app_session(app))->foreground[2] == 1);
    CHECK(history_current(app) == history && !pc_ui_shell_sampling_runtime(app)->active);
    ui->tool = PC_TOOL_COLOR_SAMPLER;
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    crop_drag(app, 8, 24, 8, 24);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->ncolor_samplers == 1);
    crop_drag(app, 8, 24, 24, 24);
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->color_samplers[0][0] == 24);
    capture(app, &pixels, &w, &h);
    free(pixels);
    click_widget(app, "view.colorSamplers.clear");
    CHECK(pc_engine_session_active(pc_ui_shell_app_session(app))->ncolor_samplers == 0 &&
          history_current(app) == history);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
static void test_native_adjustment_layer_picker_buttons_sample_and_cancel(void)
{
    if (!getenv("PC_TEST_GL"))
        return;
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    pc_ui_shell_app *app = NULL;
    CHECK(pc_ui_shell_app_new(&app, NULL) == PC_OK);
    invoke(app, "file.new", "{\"width\":64,\"height\":64}");
    invoke(app, "edit.fill", "{\"color\":[0.6,0.5,0.4]}");
    invoke(app, "layer.newAdjustmentLayer.curves", "{}");
    CHECK(pc_ui_shell_app_open_window(app, "assets/fonts/Inter-Regular.ttf", 1, true, NULL) == PC_OK);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->tool = PC_TOOL_HAND;
    invoke(app, "view.actualPixels", "{}");
    CHECK(pc_ui_shell_app_render_frame(app, 1280, 800, NULL) == PC_OK);
    click_widget(app, "properties.picker.white");
    CHECK(ui->layer_picker.armed && ui->layer_picker.point == PC_TONE_PICK_WHITE);
    size_t history = history_current(app);
    crop_drag(app, 20, 20, 20, 20);
    CHECK(history_current(app) == history + 1 && ui->layer_picker.armed);
    pc_compose_buffer buffer = {0};
    CHECK(pc_compose_render(document(app), pc_rect_make(20, 20, 21, 21), &buffer, NULL) == PC_OK);
    CHECK(buffer.len && buffer.px[0][0] > .99f && buffer.px[0][1] > .99f && buffer.px[0][2] > .99f);
    pc_compose_buffer_fini(&buffer);
    uint8_t *pixels = NULL;
    int w, h;
    capture(app, &pixels, &w, &h);
    free(pixels);
    key(app, SDLK_ESCAPE, true, SDL_KMOD_NONE);
    key(app, SDLK_ESCAPE, false, SDL_KMOD_NONE);
    CHECK(!ui->layer_picker.armed);
    pc_ui_shell_app_free(app);
    SDL_Quit();
}
int main(int argc, char **argv)
{
    RUN(test_native_adjustment_layer_picker_buttons_sample_and_cancel);
    RUN(test_native_eyedropper_ring_alt_sampling_and_color_sampler);
    RUN(test_native_move_transform_controls_start_real_resize_and_layer_edges);
    RUN(test_native_selection_outline_and_floating_pixels_display_and_drop);
    RUN(test_native_artboard_draw_resize_options_properties_and_plus);
    RUN(test_native_slice_draw_move_options_lock_and_auto_promotion);
    RUN(test_native_extreme_pending_frames_clip_before_nuklear);
    RUN(test_ui_locale_font_coverage_and_scale_matrix);
    RUN(test_native_lasso_pending_double_click_retract_and_magnetic);
    RUN(test_native_closed_document_releases_gpu_checkpoint);
    RUN(test_native_crop_pending_handles_plane_modes_focus_and_commit);
    RUN(test_native_context_menu_selection_transform_layer_mask_and_erase);
    RUN(test_native_free_transform_path_type_scope_and_noop);
    RUN(test_native_free_transform_preview_local_history_apply_and_cancel);
    RUN(test_native_brush_picker_cards_controls_and_dismissal);
    RUN(test_native_live_dialog_catalog_all_registered_languages);
    RUN(test_native_shaped_multilingual_layer_names);
    RUN(test_ui_font_shaping_joining_bidi_and_cache_bounds);
    RUN(test_native_rasterize_prompt_cancel_resume_filter_and_stale_target);
    RUN(test_native_document_properties_resize_link_and_view_controls);
    RUN(test_native_export_and_document_forms);
    RUN(test_native_variables_draft_cancel_and_atomic_confirmation);
    RUN(test_native_duplicate_notice_titles_inspect_and_dismiss);
    RUN(test_native_timeline_creation_playback_controls_and_workspace);
    RUN(test_native_mask_thumbnails_targets_colors_and_modifiers);
    RUN(test_native_layer_drag_copy_and_inline_rename);
    RUN(test_native_brush_settings_sections_presets_and_preview);
    RUN(test_native_system_info_and_clipboard_copy);
    RUN(test_native_background_job_commit_progress_cancel_and_escape);
    RUN(test_native_workspace_dialog_saved_menu_and_reset);
    RUN(test_native_about_credits_names_models_and_sort);
    RUN(test_native_color_range_eyedropper_preview_cancel_and_commit);
    RUN(test_native_camera_raw_panels_curves_preview_and_commit);
    RUN(test_native_gallery_stack_preview_and_one_commit);
    RUN(test_native_layer_style_user_defaults);
    RUN(test_native_layer_style_instances_contour_and_cancel);
    RUN(test_native_layer_style_preview_color_and_single_commit);
    RUN(test_native_gradient_stops_and_nested_color_preview);
    RUN(test_native_structured_adjustment_controls_preserve_ranges);
    RUN(test_native_curves_graph_histogram_and_drawing_mode);
    RUN(test_native_adjustment_dialog_preview_cancel_and_commit);
    RUN(test_native_direct_selection_preview_and_commit);
    RUN(test_native_pen_path_preview_undo_escape_and_paths_panel);
    RUN(test_native_shape_preview_modifier_and_commit);
    RUN(test_native_type_styles_glyphs_and_spelling_windows);
    RUN(test_native_type_font_picker_and_character_panel);
    RUN(test_native_type_unicode_ime_selection_and_mask);
    RUN(test_native_held_move_and_erase_are_latched);
    RUN(test_native_discard_buttons_and_localized_menu);
    RUN(test_ui_font_dynamic_unicode_and_bounds);
    RUN(test_native_dock_resize_reorder_and_cancel);
    RUN(test_native_nested_menu_keyboard_and_modal);
    RUN(test_native_toolbar_and_panel_routes);
    RUN(test_native_preferences_sections);
    RUN(test_native_pen_pressure_and_duplicate_mouse);
    RUN(test_native_brush_preview_commit);
    RUN(test_native_dialog_forms);
    RUN(test_native_history_panel);
    RUN(test_native_layer_panels);
    RUN(test_native_overlays_and_guides);
    RUN(test_gl_colour_serving);
    RUN(test_gl_channel_serving);
    RUN(test_gl_tiled_detail);
    RUN(test_native_gestures_and_temporary_tools);
    RUN(test_served_gpu_fallback);
    return pc_t_finish();
}
