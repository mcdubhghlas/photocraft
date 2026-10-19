#include "brush_picker_ui.h"
#include "brush_panel_ui.h"
#include "shell_internal.h"
#include "filter_ui.h"
#include "ui/brush_settings.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "paint/render.h"
#include "engine/prefs.h"
#include <blake3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const section_names[] = {
    "Brush Tip Shape", "Shape Dynamics", "Scattering", "Texture",  "Dual Brush", "Color Dynamics", "Transfer",
    "Brush Pose",      "Noise",          "Wet Edges",  "Build-up", "Smoothing",  "Protect Texture"};
static const char *const section_keys[] = {
    "",     "shapeDynamics", "scattering", "texture", "dualBrush", "colorDynamics", "transfer",
    "pose", "noise",         "wetEdges",   "buildUp", "smoothing", "protectTexture"};
static const char *const controls[] = {"off",         "fade",     "penPressure",      "penTilt",
                                       "stylusWheel", "rotation", "initialDirection", "direction"};
static const char *const masks[] = {"multiply",  "subtract",   "darken",  "overlay",      "colorDodge",
                                    "colorBurn", "linearBurn", "hardMix", "linearHeight", "height"};

void pc_ui_brush_panel_fini(pc_ui_brush_panel *p)
{
    if (p->has_preview)
        pc_paint_brush_fini(&p->preview_brush);
    *p = (pc_ui_brush_panel){0};
}
static void map(pc_ui_shell_app *app, const char *path)
{
    char id[512];
    int length = snprintf(id, sizeof id, "brush.%s", path);
    if (length < 0 || (size_t)length >= sizeof id)
        return;
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(pc_ui_shell_app_nk(app)));
}
static void patch(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *path,
                  yyjson_mut_val *value)
{
    yyjson_mut_val *fields = NULL;
    pc_error err;
    if (pc_ui_brush_setting_patch(d, path, value, &fields, &err) != PC_OK) {
        pc_ui_shell_report(app, "Brush Settings", err.msg, true);
        return;
    }
    yyjson_mut_val *params = yyjson_mut_obj(d);
    char key[280];
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (!nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
        panel->gesture++;
    (void)snprintf(key, sizeof key, "brush-panel:%llu:%s", (unsigned long long)panel->gesture, path);
    if (!params || !yyjson_mut_obj_add_val(d, params, "brush", fields) ||
        !yyjson_mut_obj_add_strcpy(d, params, "coalesce", key)) {
        pc_ui_shell_report(app, "Brush Settings", "Not enough memory to change brush", true);
        return;
    }
    pc_ui_shell_queue(app, "tools.setBrush", params);
}
static void boolean(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *label,
                    const char *path, bool value)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_bool checked = value;
    map(app, path);
    if (nk_checkbox_label(ctx, pc_i18n_t(label), &checked))
        patch(app, panel, d, path, yyjson_mut_bool(d, checked));
}
static void numeric(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *label,
                    const char *path, const char *key, yyjson_mut_val *value)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float min = 0, max = 1, scale = 100;
    if (!strcmp(key, "size")) {
        min = 1;
        max = 5000;
        scale = 1;
    } else if (!strcmp(key, "angle")) {
        min = -180;
        max = 180;
        scale = 1;
    } else if (!strcmp(key, "rotation")) {
        max = 360;
        scale = 1;
    } else if (!strcmp(key, "tiltX") || !strcmp(key, "tiltY")) {
        min = -90;
        max = 90;
        scale = 1;
    } else if (!strcmp(key, "spacing") || !strcmp(key, "scale")) {
        min = .01f;
        max = 10;
    } else if (!strcmp(key, "count")) {
        min = 1;
        max = 16;
        scale = 1;
    } else if (!strcmp(key, "fadeSteps")) {
        min = 1;
        max = 10000;
        scale = 1;
    } else if (!strcmp(key, "buildUpRate")) {
        min = 1;
        max = 200;
        scale = 1;
    } else if (!strcmp(key, "tiltScale"))
        max = 2;
    else if (!strcmp(path, "scattering.scatter.jitter") || !strcmp(path, "dualBrush.scatter"))
        max = 10;
    else if (!strcmp(key, "brightness") || !strcmp(key, "contrast") || !strcmp(key, "purity"))
        min = -1;
    else if (!strcmp(path, "flow") || !strcmp(key, "roundness"))
        min = .01f;
    double number = yyjson_mut_get_num(value);
    float shown = (float)number * scale, before = shown;
    map(app, path);
    nk_property_float(ctx, pc_i18n_t(label), min * scale, &shown, max * scale, 1, .25f);
    if (shown != before) {
        yyjson_mut_val *next = yyjson_mut_is_int(value)
                                   ? yyjson_mut_uint(d, (uint64_t)fmaxf(0, roundf(shown / scale)))
                                   : yyjson_mut_real(d, (double)(shown / scale));
        patch(app, panel, d, path, next);
    }
}
static void enumeration(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *label,
                        const char *path, const char *key, const char *current)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    char display[160];
    pc_ui_shell_parameter_label(current, display);
    nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
    map(app, path);
    if (nk_combo_begin_label(ctx, pc_i18n_t(display), nk_vec2(240, 280))) {
        const char *const *names = !strcmp(key, "control") ? controls : masks;
        size_t count = !strcmp(key, "control") ? 8 : 10;
        bool blend = !strcmp(path, "mode");
        if (blend)
            count = PC_BLEND_COUNT - 1;
        nk_layout_row_dynamic(ctx, 22, 1);
        for (size_t i = 0; i < count; i++) {
            const char *name = blend ? pc_blend_name((pc_blend_mode)(i + 1)) : names[i];
            pc_ui_shell_parameter_label(name, display);
            if (nk_combo_item_label(ctx, pc_i18n_t(display), NK_TEXT_LEFT))
                patch(app, panel, d, path, yyjson_mut_strcpy(d, name));
        }
        nk_combo_end(ctx);
    }
}
static void fields(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, yyjson_mut_val *object,
                   const char *prefix, size_t depth)
{
    if (depth > 5 || !yyjson_mut_is_obj(object))
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_obj_iter it;
    yyjson_mut_obj_iter_init(object, &it);
    yyjson_mut_val *entry;
    while ((entry = yyjson_mut_obj_iter_next(&it))) {
        const char *key = yyjson_mut_get_str(entry);
        yyjson_mut_val *value = yyjson_mut_obj_iter_get_val(entry);
        if (!strcmp(key, "locks") || !strcmp(key, "enabled"))
            continue;
        char path[256], label[160];
        int size = snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "." : "", key);
        if (size < 0 || (size_t)size >= sizeof path)
            continue;
        pc_ui_shell_parameter_label(key, label);
        if (yyjson_mut_is_obj(value)) {
            if (!*prefix)
                continue;
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
            fields(app, panel, d, value, path, depth + 1);
        } else if (yyjson_mut_is_bool(value)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            boolean(app, panel, d, label, path, yyjson_mut_get_bool(value));
        } else if (yyjson_mut_is_num(value)) {
            const char *control = yyjson_mut_get_str(yyjson_mut_obj_get(object, "control"));
            bool disabled = !strcmp(key, "minimum") && control && !strcmp(control, "off");
            nk_layout_row_dynamic(ctx, 26, 1);
            if (disabled)
                nk_widget_disable_begin(ctx);
            numeric(app, panel, d, label, path, key, value);
            if (disabled)
                nk_widget_disable_end(ctx);
        } else if (yyjson_mut_is_str(value)) {
            nk_layout_row_dynamic(ctx, 24, 2);
            enumeration(app, panel, d, label, path, key, yyjson_mut_get_str(value));
        }
    }
}
static void sections(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, yyjson_mut_val *root)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->brush_section >= 13)
        ui->brush_section = 0;
    nk_layout_row_begin(ctx, NK_STATIC, 420, 2);
    nk_layout_row_push(ctx, 255);
    if (nk_group_begin(ctx, "brush-sections", 0)) {
        yyjson_mut_val *locks = yyjson_mut_obj_get(root, "locks");
        for (size_t i = 0; i < 13; i++) {
            nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
            nk_layout_row_push(ctx, 20);
            yyjson_mut_val *section = yyjson_mut_obj_get(root, section_keys[i]);
            bool object = yyjson_mut_is_obj(section);
            if (i) {
                char path[96];
                (void)snprintf(path, sizeof path, "%s%s", section_keys[i], object ? ".enabled" : "");
                boolean(app, panel, d, "", path,
                        yyjson_mut_get_bool(object ? yyjson_mut_obj_get(section, "enabled") : section));
            } else
                nk_spacing(ctx, 1);
            nk_layout_row_push(ctx, 140);
            char select[80];
            (void)snprintf(select, sizeof select, "section.%s", section_keys[i]);
            map(app, select);
            if (nk_select_label(ctx, pc_i18n_t(section_names[i]), NK_TEXT_LEFT, ui->brush_section == i)) {
                ui->brush_section = i;
                if (object) {
                    char path[96];
                    (void)snprintf(path, sizeof path, "%s.enabled", section_keys[i]);
                    patch(app, panel, d, path, yyjson_mut_bool(d, true));
                }
            }
            nk_layout_row_push(ctx, 55);
            if (i) {
                char path[96];
                (void)snprintf(path, sizeof path, "locks.%s", section_keys[i]);
                boolean(app, panel, d, "Lock", path,
                        yyjson_mut_get_bool(yyjson_mut_obj_get(locks, section_keys[i])));
            } else
                nk_spacing(ctx, 1);
            nk_layout_row_end(ctx);
        }
        nk_group_end(ctx);
    }
    nk_layout_row_push(ctx, fmaxf(160, nk_window_get_content_region(ctx).w - 275));
    if (nk_group_begin(ctx, "brush-properties", 0)) {
        size_t selected = ui->brush_section;
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_label(ctx, pc_i18n_t(section_names[selected]), NK_TEXT_LEFT);
        if (!selected)
            fields(app, panel, d, root, "", 0);
        else {
            yyjson_mut_val *section = yyjson_mut_obj_get(root, section_keys[selected]);
            if (yyjson_mut_is_obj(section))
                fields(app, panel, d, section, section_keys[selected], 0);
            if (selected == 6)
                fields(app, panel, d, yyjson_mut_obj_get(root, "mixer"), "mixer", 0);
            if (selected == 8 || selected == 9 || selected == 12) {
                nk_layout_row_dynamic(ctx, 60, 1);
                const char *note =
                    selected == 8 ? "Adds random variation to the soft edges of the brush tip."
                    : selected == 9
                        ? "Builds up paint along the edges of the stroke."
                        : "Keeps the current pattern and scale when selecting another textured preset.";
                nk_label_wrap(ctx, pc_i18n_t(note));
            }
            if (selected == 10)
                numeric(app, panel, d, "Rate", "buildUpRate", "buildUpRate",
                        yyjson_mut_obj_get(root, "buildUpRate"));
        }
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
}
static void preset_action(pc_ui_shell_app *app, const char *command, const char *name)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    if (p && yyjson_mut_obj_add_strcpy(d, p, !strcmp(command, "tools.setBrush") ? "preset" : "name", name))
        pc_ui_shell_queue(app, command, p);
    else
        pc_ui_shell_report(app, "Brush Presets", "Not enough memory", true);
    yyjson_mut_doc_free(d);
}
static void presets(pc_ui_shell_app *app, pc_ui_brush_panel *panel)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_dynamic(ctx, 25, 2);
    nk_label(ctx, pc_i18n_t("Search"), NK_TEXT_LEFT);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, panel->search, sizeof panel->search,
                                   nk_filter_default);
    pc_json_value data = PC_JSON_VALUE_INIT;
    pc_engine_session *session = pc_ui_shell_app_session(app);
    (void)pc_engine_session_execute(session, "brush.presets.list", NULL, &data, NULL);
    yyjson_mut_val *items = yyjson_mut_obj_get(pc_json_value_root(&data), "presets");
    nk_layout_row_dynamic(ctx, 335, 1);
    if (nk_group_begin(ctx, "brush-preset-list", 0)) {
        if (pc_ui_shell_app_state(app)->brushes_grid)
            nk_layout_row_dynamic(ctx, 86, 3);
        for (size_t i = 0; i < yyjson_mut_arr_size(items); i++) {
            yyjson_mut_val *item = yyjson_mut_arr_get(items, i);
            const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(item, "name"));
            if (!name || !pc_ui_brush_preset_matches(name, panel->search))
                continue;
            if (pc_ui_shell_app_state(app)->brushes_grid) {
                if (i == 0)
                    nk_layout_row_dynamic(ctx, 86, 3);
                pc_ui_brush_picker view;
                pc_ui_brush_picker_default(&view);
                view.scale = .8f;
                const pc_engine_tool_state *tools = pc_engine_session_tools(session);
                if (i < tools->npresets)
                    pc_ui_shell_brush_preset_card(app, i, tools, &view);
                continue;
            }
            nk_layout_row_dynamic(ctx, 28, 1);
            char id[256];
            (void)snprintf(id, sizeof id, "preset.%s", name);
            map(app, id);
            bool selected = pc_engine_session_tools(session)->current_preset &&
                            !strcmp(pc_engine_session_tools(session)->current_preset, name);
            if (nk_select_label(ctx, name, NK_TEXT_LEFT, selected)) {
                preset_action(app, "tools.setBrush", name);
                (void)snprintf(panel->name, sizeof panel->name, "%s", name);
            }
        }
        nk_group_end(ctx);
    }
    pc_json_value_fini(&data);
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t("Preset Name"), NK_TEXT_LEFT);
    map(app, "preset.name");
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, panel->name, sizeof panel->name, nk_filter_default);
    const char *labels[] = {"New", "Update", "Delete"};
    const char *commands[] = {"brush.presets.save", "brush.presets.update", "brush.presets.delete"};
    nk_layout_row_dynamic(ctx, 28, 3);
    if (!*panel->name)
        nk_widget_disable_begin(ctx);
    for (size_t i = 0; i < 3; i++) {
        map(app, commands[i]);
        if (nk_button_label(ctx, pc_i18n_t(labels[i])))
            preset_action(app, commands[i], panel->name);
    }
    if (!*panel->name)
        nk_widget_disable_end(ctx);
}
static void preview(pc_ui_shell_app *app, pc_ui_brush_panel *panel)
{
    const pc_paint_brush *brush = &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush;
    if (!panel->has_preview || !pc_paint_brush_eq(brush, &panel->preview_brush)) {
        pc_paint_brush small;
        pc_paint_brush_copy(&small, brush);
        float factor = fminf(1, 36 / fmaxf(1, small.size));
        small.size = fmaxf(3, small.size * factor);
        small.dual_brush.size = fminf(36, small.dual_brush.size * factor);
        small.spacing = fmaxf(.08f, small.spacing);
        small.build_up = false;
        small.erase = false;
        const float white[4] = {1, 1, 1, 1};
        pc_surface surface;
        pc_surface_init_default(&surface, PC_PIXEL_FORMAT_RGBA8, white, 4);
        pc_surface clip;
        const float zero[1] = {0}, one[1] = {1};
        pc_surface_init_default(&clip, PC_PIXEL_FORMAT_GRAY8, zero, 1);
        pc_rect rect = pc_rect_make(0, 0, 256, 64), damage;
        pc_status status = pc_surface_fill_rect(&clip, rect, one, 1, NULL);
        pc_paint_point points[32];
        for (size_t i = 0; i < 32; i++) {
            double t = (double)i / 31;
            points[i] = pc_paint_point_make(12 + t * 232, 32 - sin(t * 6.283185307179586) * 10,
                                            (float)(.2 + .8 * sin(t * 3.141592653589793)));
            points[i].time = t * 1000;
        }
        if (status == PC_OK)
            status = pc_paint_render_stroke(&surface, &small, points, 32, &clip, false, 1, &damage, NULL);
        if (status == PC_OK)
            status =
                pc_surface_read_rgba8_into(&surface, rect, (uint8_t (*)[4])panel->pixels, 256 * 64, NULL);
        pc_surface_fini(&surface);
        pc_surface_fini(&clip);
        pc_paint_brush_fini(&small);
        if (status != PC_OK)
            return;
        if (panel->has_preview)
            pc_paint_brush_fini(&panel->preview_brush);
        pc_paint_brush_copy(&panel->preview_brush, brush);
        panel->has_preview = true;
        blake3_hasher hash;
        blake3_hasher_init(&hash);
        blake3_hasher_update(&hash, panel->pixels, sizeof panel->pixels);
        blake3_hasher_finalize(&hash, panel->key, sizeof panel->key);
    }
    struct nk_image image;
    if (pc_ui_shell_bitmap(app, panel->key, 256, 64, panel->pixels, &image, NULL) == PC_OK) {
        struct nk_context *ctx = pc_ui_shell_app_nk(app);
        nk_layout_row_dynamic(ctx, 64, 1);
        nk_image(ctx, image);
    }
}
void pc_ui_brush_panel_draw(pc_ui_shell_app *app, pc_ui_brush_panel *panel)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (!ui->panels.brush_settings) {
        panel->shown = false;
        return;
    }
    bool first = !panel->shown;
    panel->shown = true;
    if (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
        panel->gesture++;
    if (nk_begin(ctx, "Brush Settings", nk_rect(150, 100, 760, 610),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE)) {
        if (first)
            nk_widget_disable_begin(ctx);
        nk_layout_row_dynamic(ctx, 28, 2);
        map(app, "tab.settings");
        if (nk_select_label(ctx, pc_i18n_t("Brush Settings"), NK_TEXT_CENTERED, ui->brush_tab == 0))
            ui->brush_tab = 0;
        map(app, "tab.presets");
        if (nk_select_label(ctx, pc_i18n_t("Brushes"), NK_TEXT_CENTERED, ui->brush_tab != 0))
            ui->brush_tab = 1;
        if (!ui->brush_tab) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *root =
                pc_ui_brush_settings_fields(d, &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush);
            if (root)
                sections(app, panel, d, root);
            yyjson_mut_doc_free(d);
        } else
            presets(app, panel);
        preview(app, panel);
        if (first)
            nk_widget_disable_end(ctx);
    }
    nk_end(ctx);
    if (nk_window_is_closed(ctx, "Brush Settings")) {
        ui->panels.brush_settings = false;
        panel->shown = false;
        nk_window_show(ctx, "Brush Settings", NK_SHOWN);
    }
}
