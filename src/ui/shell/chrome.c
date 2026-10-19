#include "sampling_ui.h"
#include "artboard_ui.h"
#include "lasso_ui.h"
#include "slice_ui.h"
#include "transform_ui.h"
#include "crop_ui.h"
#include "ui/i18n.h"
#include "chrome.h"
#include "jobs_ui.h"
#include "type_controls.h"
#include "shell_internal.h"
#include "ui/icons.h"
#include "ui/toolbar.h"
#include "ui/menus.h"
#include "ui/hold_keys.h"
#include "ui/pen_tool.h"
#include "ui/widgets.h"
#include "engine/prefs.h"
#include <stdio.h>
#include <string.h>

static bool hidden(yyjson_mut_val *list, pc_ui_tool tool)
{
    size_t i, n;
    yyjson_mut_val *v;
    yyjson_mut_arr_foreach(list, i, n, v)
    {
        const char *name = yyjson_mut_get_str(v);
        if (name && !strcmp(name, pc_ui_tool_serde(tool))) {
            return true;
        }
    }
    return false;
}
static void select_tool(pc_ui_shell_app *app, pc_ui_tool tool)
{
    char id[80];
    pc_ui_tool_select_id(tool, id, sizeof id);
    pc_ui_shell_queue(app, id, NULL);
}
static bool tool_button(pc_ui_shell_app *app, pc_ui_tool tool, float side)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    struct nk_rect r = nk_widget_bounds(c);
    char id[80];
    pc_ui_tool_select_id(tool, id, sizeof id);
    pc_ui_shell_widget_mapping(app, id, r);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    bool selected = pc_ui_shell_app_state(app)->tool == tool;
    struct nk_style_item before = c->style.button.normal;
    struct nk_vec2 padding = c->style.button.padding;
    c->style.button.padding = nk_vec2((side - 20.f) * .5f, (side - 20.f) * .5f);
    if (selected) {
        c->style.button.normal = nk_style_item_color(pc_ui_nk_color(tokens->accent_soft));
    }
    struct nk_image image;
    bool clicked;
    if (pc_ui_shell_icon_image(app, pc_ui_tool_icon(tool), 20, &image)) {
        clicked = nk_button_label(c, pc_i18n_t("")) != 0;
        struct nk_command_buffer *canvas = nk_window_get_canvas(c);
        struct nk_rect icon = nk_rect(r.x + (r.w - 20) * .5f, r.y + (r.h - 20) * .5f, 20, 20);
        nk_draw_image(canvas, icon, &image, pc_ui_nk_color(tokens->icon));
    } else {
        clicked = nk_button_label(c, pc_ui_tool_glyph(tool)) != 0;
    }
    c->style.button.normal = before;
    c->style.button.padding = padding;
    if (nk_input_is_mouse_hovering_rect(&c->input, r)) {
        char tip[160];
        char key = pc_ui_tool_key(tool);
        (void)snprintf(tip, sizeof tip, "%s%s%c%s", pc_ui_tool_label(tool), key ? " (" : "", key ? key : ' ',
                       key ? ")" : "");
        nk_tooltip(c, tip);
    }
    return clicked;
}
void pc_ui_shell_toolbar(pc_ui_shell_app *app, float width, float height)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    pc_json_value preferences = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(pc_ui_shell_app_session(app), "toolbar", &preferences, NULL);
    yyjson_mut_val *list = yyjson_mut_obj_get(pc_json_value_root(&preferences), "hidden");
    bool dbl = ui->panels.toolbar_double;
    float side = tokens->pro ? 30.f : 36.f;
    nk_layout_row_dynamic(c, 15, 1);
    struct nk_rect header = nk_widget_bounds(c);
    pc_ui_shell_widget_mapping(app, "window.toolbar.double", header);
    if (nk_button_label(c, dbl ? "<" : ">")) {
        pc_ui_shell_queue(app, "window.toolbar.double", NULL);
    }
    /* A scrolling inner group keeps every tool and the color chips reachable on small screens. */
    nk_layout_row_dynamic(c, height > 30 ? height - 30 : 1, 1);
    struct nk_vec2 saved_padding = c->style.window.group_padding,
                   saved_scrollbar = c->style.window.scrollbar_size;
    c->style.window.group_padding = nk_vec2(0, 0);
    c->style.window.scrollbar_size = nk_vec2(4, 4);
    if (nk_group_begin(c, "toolbar-tools", 0)) {
        unsigned previous = 0;
        size_t column = 0;
        for (size_t slot = 0; slot < pc_ui_toolbar_slots(); slot++) {
            size_t n = 0;
            unsigned section = 0;
            const pc_ui_tool *tools = pc_ui_toolbar_slot(slot, &n, &section);
            pc_ui_tool shown = pc_ui_toolbar_current(ui, slot);
            size_t visible = 0;
            for (size_t i = 0; i < n; i++) {
                if (!hidden(list, tools[i])) {
                    visible++;
                }
            }
            if (!visible) {
                continue;
            }
            if (hidden(list, shown)) {
                for (size_t i = 0; i < n; i++) {
                    if (!hidden(list, tools[i])) {
                        shown = tools[i];
                        break;
                    }
                }
            }
            if (section != previous) {
                if (column) {
                    nk_layout_row_end(c);
                    column = 0;
                }
                nk_layout_row_dynamic(c, 4, 1);
                nk_spacing(c, 1);
                previous = section;
            }
            if (!column) {
                nk_layout_row_begin(c, NK_STATIC, side, dbl ? 2 : 1);
            }
            nk_layout_row_push(c, side);
            struct nk_rect rect = nk_widget_bounds(c);
            if (tool_button(app, shown, side)) {
                if (SDL_GetModState() & SDL_KMOD_SHIFT) {
                    size_t index = 0;
                    for (size_t i = 0; i < n; i++) {
                        if (tools[i] == shown) {
                            index = i;
                        }
                    }
                    for (size_t i = 1; i <= n; i++) {
                        pc_ui_tool next = tools[(index + i) % n];
                        if (!hidden(list, next)) {
                            shown = next;
                            break;
                        }
                    }
                }
                select_tool(app, shown);
            }
            if (visible > 1) {
                struct nk_command_buffer *cv = nk_window_get_canvas(c);
                nk_fill_triangle(cv, rect.x + rect.w - 6, rect.y + rect.h - 3, rect.x + rect.w - 3,
                                 rect.y + rect.h - 6, rect.x + rect.w - 3, rect.y + rect.h - 3,
                                 pc_ui_nk_color(tokens->text_dim));
                char popup[40];
                (void)snprintf(popup, sizeof popup, "tool-flyout-%zu", slot);
                (void)popup; /* Nuklear identifies contextual popups by their invocation order. */
                if (nk_contextual_begin(c, 0, nk_vec2(240, (float)visible * 30 + 12), rect)) {
                    nk_layout_row_dynamic(c, 27, 1);
                    for (size_t i = 0; i < n; i++) {
                        if (hidden(list, tools[i])) {
                            continue;
                        }
                        if (nk_contextual_item_label(c, pc_ui_tool_label(tools[i]), NK_TEXT_LEFT)) {
                            select_tool(app, tools[i]);
                        }
                    }
                    nk_contextual_end(c);
                }
            }
            column++;
            if (column == (dbl ? 2u : 1u)) {
                nk_layout_row_end(c);
                column = 0;
            }
        }
        if (column) {
            nk_layout_row_end(c);
        }
        nk_layout_row_dynamic(c, 20, 2);
        if (nk_button_label(c, pc_i18n_t("D"))) {
            pc_ui_shell_queue(app, "tools.defaultColors", NULL);
        }
        if (nk_button_label(c, pc_i18n_t("X"))) {
            pc_ui_shell_queue(app, "tools.swapColors", NULL);
        }
        const pc_engine_tool_state *state = pc_engine_session_tools(pc_ui_shell_app_session(app));
        nk_layout_row_dynamic(c, 28, 1);
        for (size_t i = 0; i < 2; i++) {
            const float *color = i ? state->background : state->foreground;
            struct nk_color nk = nk_rgba_f(color[0], color[1], color[2], color[3]);
            struct nk_rect r = nk_widget_bounds(c);
            pc_ui_shell_widget_mapping(app, i ? "color.background" : "color.foreground", r);
            if (nk_button_color(c, nk)) {
                yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                if (d) {
                    yyjson_mut_val *p = yyjson_mut_obj(d);
                    yyjson_mut_obj_add_str(d, p, "kind", "colorPicker");
                    yyjson_mut_obj_add_str(d, p, "target", i ? "background" : "foreground");
                    pc_ui_shell_queue(app, "ui.dialog.open", p);
                    yyjson_mut_doc_free(d);
                }
            }
        }
        nk_group_end(c);
    }
    c->style.window.group_padding = saved_padding;
    c->style.window.scrollbar_size = saved_scrollbar;
    pc_json_value_fini(&preferences);
    (void)width;
}
static void check(struct nk_context *c, const char *label, bool *value)
{
    nk_bool state = *value;
    nk_checkbox_label(c, label, &state);
    *value = state != 0;
}
static void brush_field(pc_ui_shell_app *app, const char *label, const char *key, float value, float min,
                        float max, float scale)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    float next = value * scale;
    nk_property_float(c, label, min, &next, max, 1, .25f);
    if (next != value * scale) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (d) {
            yyjson_mut_val *p = yyjson_mut_obj(d), *brush = yyjson_mut_obj(d);
            yyjson_mut_obj_add_real(d, brush, key, next / scale);
            yyjson_mut_obj_add_val(d, p, "brush", brush);
            yyjson_mut_obj_add_str(d, p, "coalesce", "optionsBar");
            pc_ui_shell_queue(app, "tools.setBrush", p);
            yyjson_mut_doc_free(d);
        }
    }
}
void pc_ui_shell_options(pc_ui_shell_app *app, float width)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->transform.active) {
        pc_ui_shell_transform_options(app);
        return;
    }
    if (pc_ui_sampling_tool_kind(ui->tool)) {
        pc_ui_shell_sampling_options(app);
        return;
    }
    if (ui->tool == PC_TOOL_ARTBOARD) {
        pc_ui_shell_artboard_options(app);
        return;
    }
    if (ui->tool == PC_TOOL_CROP) {
        pc_ui_shell_crop_options(app);
        return;
    }
    if (pc_ui_slice_tool(ui->tool)) {
        pc_ui_shell_slice_options(app);
        return;
    }
    if (pc_ui_lasso_tool(ui->tool)) {
        pc_ui_shell_lasso_options(app);
        return;
    }
    pc_ui_tool_options *o = &ui->tool_options;
    nk_layout_row_begin(c, NK_STATIC, 24, 10);
    nk_layout_row_push(c, 140);
    nk_label(c, pc_i18n_t(pc_ui_tool_label(ui->tool)), NK_TEXT_LEFT);
    if (pc_ui_tool_is_brushlike(ui->tool)) {
        const pc_paint_brush *brush = &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush;
        nk_layout_row_push(c, 150);
        brush_field(app, "Size", "size", brush->size, 1, 5000, 1);
        nk_layout_row_push(c, 150);
        brush_field(app, "Hardness", "hardness", brush->hardness, 0, 100, 100);
        nk_layout_row_push(c, 150);
        brush_field(app, "Opacity", "opacity", brush->opacity, 0, 100, 100);
        nk_layout_row_push(c, 150);
        brush_field(app, "Flow", "flow", brush->flow, 0, 100, 100);
        nk_layout_row_push(c, 90);
        struct nk_rect picker_button = nk_widget_bounds(c);
        pc_ui_shell_widget_mapping(app, "brush.picker", picker_button);
        if (nk_button_label(c, pc_i18n_t("Brushes"))) {
            ui->brush_picker.open = !ui->brush_picker.open;
            ui->brush_picker.point[0] = picker_button.x;
            ui->brush_picker.point[1] = picker_button.y + picker_button.h;
        }
        nk_layout_row_push(c, 36);
        pc_ui_shell_widget_mapping(app, "brush.settings", nk_widget_bounds(c));
        if (nk_button_label(c, "F5"))
            pc_ui_shell_queue(app, "window.panel.brushSettings", NULL);
    } else if (pc_ui_tool_is_type(ui->tool)) {
        pc_ui_shell_type_options(app);
    } else if (pc_ui_pen_tool(ui->tool)) {
        nk_layout_row_push(c, 120);
        const char *modes[] = {"Path", "Shape"};
        int selected = pc_str_eq_c(&o->vector_mode, "shape") ? 1 : 0;
        int mode = nk_combo(c, modes, 2, selected, 24, nk_vec2(150, 90));
        if (mode != selected) {
            (void)pc_str_set_c(&o->vector_mode, mode ? "shape" : "path", NULL);
        }
        if (ui->tool == PC_TOOL_PEN) {
            nk_layout_row_push(c, 140);
            nk_bool automatic = o->pen_auto_add_delete;
            nk_checkbox_label(c, pc_i18n_t("Auto Add/Delete"), &automatic);
            o->pen_auto_add_delete = automatic != 0;
        }
        if (ui->tool == PC_TOOL_FREEFORM_PEN) {
            nk_layout_row_push(c, 140);
            nk_property_float(c, "Curve Fit", .1f, &o->curve_fit, 10, .1f, .1f);
        }
        nk_layout_row_push(c, 140);
        nk_property_float(c, "Stroke", 0, &o->stroke_width, 10000, 1, .1f);
        if (ui->has_pen) {
            nk_layout_row_push(c, 70);
            if (nk_button_label(c, pc_i18n_t("Finish"))) {
                pc_ui_shell_queue(app, "ui.pen.finish", NULL);
            }
            nk_layout_row_push(c, 70);
            if (nk_button_label(c, pc_i18n_t("Cancel"))) {
                pc_ui_shell_queue(app, "ui.pen.cancel", NULL);
            }
        }
    } else if (pc_ui_is_shape_tool(ui->tool)) {
        nk_layout_row_push(c, 80);
        check(c, "Fill", &o->shape_fill);
        nk_layout_row_push(c, 150);
        nk_property_float(c, "Stroke", 0, &o->stroke_width, 10000, 1, .1f);
        if (ui->tool == PC_TOOL_RECTANGLE) {
            nk_layout_row_push(c, 150);
            nk_property_float(c, "Radius", 0, &o->corner_radius, 10000, 1, .1f);
        }
        if (ui->tool == PC_TOOL_POLYGON) {
            nk_layout_row_push(c, 150);
            int sides = (int)(o->polygon_sides > 1000 ? 1000 : o->polygon_sides);
            nk_property_int(c, "Sides", 3, &sides, 1000, 1, 1);
            o->polygon_sides = (uint32_t)sides;
        }
        if (ui->tool == PC_TOOL_LINE) {
            nk_layout_row_push(c, 150);
            nk_property_float(c, "Weight", 1, &o->line_weight, 10000, 1, .1f);
        }
        nk_layout_row_push(c, 115);
        const char *align[] = {"Inside", "Center", "Outside"};
        int next = nk_combo(c, align, 3, (int)o->shape_stroke.align, 24, nk_vec2(130, 120));
        if (next >= 0 && next < 3) {
            o->shape_stroke.align = (pc_stroke_align)next;
        }
    } else if (ui->tool == PC_TOOL_MOVE) {
        nk_layout_row_push(c, 120);
        check(c, "Auto-Select", &o->move_auto_select);
        nk_layout_row_push(c, 170);
        check(c, "Transform Controls", &o->move_show_transform);
    } else if (ui->tool == PC_TOOL_RECT_MARQUEE || ui->tool == PC_TOOL_ELLIPSE_MARQUEE ||
               ui->tool == PC_TOOL_LASSO) {
        nk_layout_row_push(c, 110);
        int mode = ui->selection_mode;
        const char *items[] = {"New", "Add", "Subtract", "Intersect"};
        mode = nk_combo(c, items, 4, mode, 24, nk_vec2(160, 130));
        ui->selection_mode = (uint8_t)mode;
        nk_layout_row_push(c, 130);
        nk_property_float(c, "Feather", 0, &o->feather, 1000, 1, .25f);
        nk_layout_row_push(c, 110);
        check(c, "Anti-alias", &o->anti_alias);
    } else if (ui->tool == PC_TOOL_PAINT_BUCKET || ui->tool == PC_TOOL_MAGIC_WAND ||
               ui->tool == PC_TOOL_MAGIC_ERASER) {
        nk_layout_row_push(c, 140);
        nk_property_float(c, "Tolerance", 0, &o->tolerance, 255, 1, .25f);
        nk_layout_row_push(c, 110);
        check(c, "Contiguous", &o->contiguous);
        nk_layout_row_push(c, 110);
        check(c, "Anti-alias", &o->anti_alias);
        nk_layout_row_push(c, 130);
        check(c, "All Layers", &o->sample_all_layers);
    } else if (ui->tool == PC_TOOL_CROP) {
        nk_layout_row_push(c, 180);
        check(c, "Delete Cropped Pixels", &o->crop_delete);
    } else if (ui->tool == PC_TOOL_HAND || ui->tool == PC_TOOL_ZOOM || ui->tool == PC_TOOL_ROTATE_VIEW) {
        nk_layout_row_push(c, 90);
        if (nk_button_label(c, pc_i18n_t("Fit Screen"))) {
            pc_ui_shell_queue(app, "view.fitOnScreen", NULL);
        }
        nk_layout_row_push(c, 90);
        if (nk_button_label(c, pc_i18n_t("100%"))) {
            pc_ui_shell_queue(app, "view.actualPixels", NULL);
        }
        nk_layout_row_push(c, 110);
        if (nk_button_label(c, pc_i18n_t("Reset Rotation"))) {
            pc_ui_shell_queue(app, "view.resetView", NULL);
        }
    }
    nk_layout_row_end(c);
    (void)width;
}
void pc_ui_shell_status(pc_ui_shell_app *app)
{
    if (pc_ui_shell_jobs_status(app))
        return;
    struct nk_context *c = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *active = pc_engine_session_active(pc_ui_shell_app_session(app));
    char text[256] = "Ready";
    if (ui->status && *ui->status) {
        (void)snprintf(text, sizeof text, "%s", ui->status);
    } else if (active) {
        const pc_document *doc = pc_doc_arc_get(active->doc);
        (void)snprintf(text, sizeof text, "%u x %u px  |  %s", doc->size.width, doc->size.height,
                       pc_ui_tool_label(ui->tool));
    }
    nk_layout_row_dynamic(c, 16, 1);
    nk_label(c, text, NK_TEXT_LEFT);
}
