#include "brush_picker_ui.h"
#include "ui/i18n.h"
#include "engine/prefs.h"
#include "ui/widgets.h"
#include "paint/render.h"
#include "text/unicode.h"
#include <blake3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static bool image(pc_ui_shell_app *app, const pc_engine_tool_state *tools, size_t index, bool stroke,
                  struct nk_image *out)
{
    const pc_paint_brush *source = &tools->presets[index].brush;
    blake3_hasher hash;
    uint8_t key[32];
    blake3_hasher_init(&hash);
    const char category[] = "brush-card-v1";
    blake3_hasher_update(&hash, category, sizeof category);
    blake3_hasher_update(&hash, &tools->presets_rev, sizeof tools->presets_rev);
    blake3_hasher_update(&hash, &index, sizeof index);
    blake3_hasher_update(&hash, &stroke, sizeof stroke);
    blake3_hasher_finalize(&hash, key, sizeof key);
    uint16_t width = stroke ? 256 : 64;
    if (pc_ui_shell_bitmap(app, key, width, 64, NULL, out, NULL) == PC_OK)
        return true;
    pc_paint_brush brush;
    pc_paint_brush_copy(&brush, source);
    float factor = fminf(1, 40 / fmaxf(1, brush.size));
    brush.size = fmaxf(2, brush.size * factor);
    brush.dual_brush.size = fminf(40, brush.dual_brush.size * factor);
    brush.spacing = fmaxf(.08f, brush.spacing);
    brush.build_up = brush.erase = false;
    brush.seed = 0;
    brush.color[0] = brush.color[1] = brush.color[2] = 0;
    brush.color[3] = 1;
    const float white[4] = {1, 1, 1, 1}, zero[1] = {0}, one[1] = {1};
    pc_surface surface, clip;
    pc_surface_init_default(&surface, PC_PIXEL_FORMAT_RGBA8, white, 4);
    pc_surface_init_default(&clip, PC_PIXEL_FORMAT_GRAY8, zero, 1);
    pc_rect rect = pc_rect_make(0, 0, width, 64), damage;
    pc_status status = pc_surface_fill_rect(&clip, rect, one, 1, NULL);
    pc_paint_point points[32];
    size_t count = stroke ? 32 : 1;
    for (size_t i = 0; i < count; i++) {
        double t = count == 1 ? .5 : (double)i / (double)(count - 1);
        points[i] =
            pc_paint_point_make(stroke ? 12 + t * 232 : 32, stroke ? 32 - sin(t * 6.283185307179586) * 8 : 32,
                                stroke ? (float)(.2 + .8 * sin(t * 3.141592653589793)) : 1);
        points[i].time = t * 1000;
    }
    if (status == PC_OK)
        status = pc_paint_render_stroke(&surface, &brush, points, count, &clip, false, 1, &damage, NULL);
    uint8_t pixels[256 * 64 * 4];
    if (status == PC_OK)
        status = pc_surface_read_rgba8_into(&surface, rect, (uint8_t (*)[4])pixels, (size_t)width * 64, NULL);
    if (status == PC_OK)
        status = pc_ui_shell_bitmap(app, key, width, 64, pixels, out, NULL);
    pc_surface_fini(&surface);
    pc_surface_fini(&clip);
    pc_paint_brush_fini(&brush);
    return status == PC_OK;
}
static void set_brush(pc_ui_shell_app *app, const char *key, double number, const char *preset)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    bool ok = p && (preset ? yyjson_mut_obj_add_strcpy(d, p, "preset", preset)
                           : yyjson_mut_obj_add_real(d, p, key, number));
    if (ok)
        pc_ui_shell_queue(app, "tools.setBrush", p);
    yyjson_mut_doc_free(d);
}
bool pc_ui_brush_preset_matches(const char *name, const char *search)
{
    if (!*search)
        return true;
    pc_buf lower_name = PC_BUF_INIT, lower_search = PC_BUF_INIT;
    bool found = pc_text_lower(name, strlen(name), &lower_name, NULL) == PC_OK &&
                 pc_text_lower(search, strlen(search), &lower_search, NULL) == PC_OK &&
                 strstr(pc_buf_cstr(&lower_name), pc_buf_cstr(&lower_search));
    pc_buf_fini(&lower_name);
    pc_buf_fini(&lower_search);
    return found;
}
void pc_ui_shell_brush_preset_card(pc_ui_shell_app *app, size_t index, const pc_engine_tool_state *tools,
                                   const pc_ui_brush_picker *picker)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_rect rect = nk_widget_bounds(ctx), clip = ctx->current->layout->clip;
    if (rect.y + rect.h <= clip.y || rect.y >= clip.y + clip.h) {
        nk_spacing(ctx, 1);
        return;
    }
    const char *name = pc_str_c(&tools->presets[index].name);
    bool selected = tools->current_preset && !strcmp(tools->current_preset, name);
    char id[80];
    (void)snprintf(id, sizeof id, "picker.preset.%zu", index);
    pc_ui_shell_widget_mapping(app, id, rect);
    if (selected)
        nk_style_push_style_item(
            ctx, &ctx->style.button.normal,
            nk_style_item_color(pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent_soft)));
    bool clicked = nk_button_label(ctx, "");
    if (selected)
        nk_style_pop_style_item(ctx);
    if (clicked)
        set_brush(app, NULL, 0, name);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    float preview_height = fmaxf(.5f, picker->scale) * 56;
    float y = rect.y + 4, x = rect.x + 4, available = rect.w - 8;
    struct nk_image bitmap;
    if (picker->show_tip && image(app, tools, index, false, &bitmap)) {
        float side = fminf(preview_height, available);
        float tip_x = picker->show_stroke ? x : rect.x + (rect.w - side) / 2;
        nk_draw_image(canvas, nk_rect(tip_x, y, side, side), &bitmap, nk_rgba(255, 255, 255, 255));
        x += side + 4;
        available = fmaxf(0, available - side - 4);
    }
    if (picker->show_stroke && available > 4 && image(app, tools, index, true, &bitmap))
        nk_draw_image(canvas, nk_rect(x, y, available, preview_height), &bitmap, nk_rgba(255, 255, 255, 255));
    if (picker->show_name) {
        if (picker->show_stroke || picker->show_tip)
            y += preview_height + 2;
        nk_draw_text(canvas, nk_rect(rect.x + 4, y, rect.w - 8, 22), name, (int)strlen(name), ctx->style.font,
                     nk_rgba(0, 0, 0, 0), ctx->style.text.color);
    }
}
static bool same_view(const pc_ui_brush_picker *a, const pc_ui_brush_picker *b)
{
    return a->show_name == b->show_name && a->show_stroke == b->show_stroke && a->show_tip == b->show_tip &&
           a->scale == b->scale;
}
void pc_ui_shell_brush_picker_restore(pc_ui_shell_app *app, pc_ui_brush_picker_runtime *r)
{
    pc_json_value value = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "brushPicker", &value, NULL) != PC_OK)
        return;
    pc_ui_brush_picker saved;
    pc_ui_brush_picker_default(&saved);
    yyjson_mut_val *root = pc_json_value_root(&value);
    const char *keys[] = {"showName", "showStroke", "showTip"};
    bool *flags[] = {&saved.show_name, &saved.show_stroke, &saved.show_tip};
    for (size_t i = 0; i < 3; i++) {
        yyjson_mut_val *v = yyjson_mut_obj_get(root, keys[i]);
        if (yyjson_mut_is_bool(v))
            *flags[i] = yyjson_mut_get_bool(v);
    }
    yyjson_mut_val *scale = yyjson_mut_obj_get(root, "scale");
    double number = yyjson_mut_get_num(scale);
    if (yyjson_mut_is_num(scale) && isfinite(number))
        saved.scale = (float)fmax(.15, fmin(2, number));
    if (!saved.show_name && !saved.show_stroke && !saved.show_tip)
        saved.show_name = saved.show_stroke = saved.show_tip = true;
    if (!r->prefs_loaded || !same_view(&saved, &r->saved)) {
        pc_ui_brush_picker *picker = &pc_ui_shell_app_state(app)->brush_picker;
        picker->show_name = saved.show_name;
        picker->show_stroke = saved.show_stroke;
        picker->show_tip = saved.show_tip;
        picker->scale = saved.scale;
    }
    r->saved = saved;
    r->prefs_loaded = true;
    pc_json_value_fini(&value);
}
static void persist(pc_ui_shell_app *app, pc_ui_brush_picker_runtime *r)
{
    const pc_ui_brush_picker *p = &pc_ui_shell_app_state(app)->brush_picker;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    bool down = false;
    for (size_t i = 0; ctx && i < NK_BUTTON_MAX; i++)
        down |= ctx->input.mouse.buttons[i].down;
    if (down || !r->prefs_loaded || same_view(p, &r->saved))
        return;
    char json[256];
    (void)snprintf(json, sizeof json,
                   "{\"path\":\"brushPicker\",\"value\":{\"showName\":%s,\"showStroke\":%s,\"showTip\":%s,"
                   "\"scale\":%.9g}}",
                   p->show_name ? "true" : "false", p->show_stroke ? "true" : "false",
                   p->show_tip ? "true" : "false", (double)p->scale);
    pc_json_value out = PC_JSON_VALUE_INIT;
    if (pc_engine_session_execute_json(pc_ui_shell_app_session(app), "prefs.set", json, &out, NULL) == PC_OK)
        r->saved = *p;
    pc_json_value_fini(&out);
}
void pc_ui_shell_brush_picker_draw(pc_ui_shell_app *app, pc_ui_brush_picker_runtime *r, float width,
                                   float height)
{
    pc_ui_brush_picker *picker = &pc_ui_shell_app_state(app)->brush_picker;
    if (!picker->open) {
        persist(app, r);
        r->shown = false;
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    bool first = !r->shown;
    r->shown = true;
    float w = fmaxf(230, fminf(picker->size[0], width - 16)),
          h = fmaxf(170, fminf(picker->size[1], height - 16));
    struct nk_rect bounds = nk_rect(fmaxf(8, fminf(width - w - 8, picker->point[0])),
                                    fmaxf(8, fminf(height - h - 8, picker->point[1])), w, h);
    if (first || r->point[0] != picker->point[0] || r->point[1] != picker->point[1])
        nk_window_set_bounds(ctx, "Brush Preset Picker", bounds);
    nk_style_push_vec2(ctx, &ctx->style.window.min_size, nk_vec2(230, 170));
    if (nk_begin_titled(ctx, "Brush Preset Picker", pc_i18n_t("Brush Preset Picker"), bounds,
                        NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                            NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR)) {
        bounds = nk_window_get_bounds(ctx);
        r->point[0] = bounds.x;
        r->point[1] = bounds.y;
        picker->point[0] = bounds.x;
        picker->point[1] = bounds.y;
        picker->size[0] = fmaxf(230, bounds.w);
        picker->size[1] = fmaxf(170, bounds.h);
        if (first)
            nk_widget_disable_begin(ctx);
        const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
        double size = tools->brush.size, hardness = tools->brush.hardness * 100;
        nk_layout_row_dynamic(ctx, 26, 1);
        pc_ui_shell_widget_mapping(app, "picker.size", nk_widget_bounds(ctx));
        nk_property_double(ctx, pc_i18n_t("Size"), 1, &size, 5000, 1, 1);
        if (size != tools->brush.size)
            set_brush(app, "size", size, NULL);
        pc_ui_shell_widget_mapping(app, "picker.hardness", nk_widget_bounds(ctx));
        bool round = tools->brush.tip.kind == PC_PAINT_TIP_ROUND;
        if (!round)
            nk_widget_disable_begin(ctx);
        nk_property_double(ctx, pc_i18n_t("Hardness"), 0, &hardness, 100, 1, 1);
        if (!round)
            nk_widget_disable_end(ctx);
        if (round && hardness != tools->brush.hardness * 100)
            set_brush(app, "hardness", hardness / 100, NULL);
        nk_layout_row_dynamic(ctx, 24, 1);
        pc_ui_shell_widget_mapping(app, "picker.search", nk_widget_bounds(ctx));
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, r->search, sizeof r->search, nk_filter_default);
        nk_layout_row_dynamic(ctx, 24, 3);
        const char *labels[] = {"Name", "Stroke", "Tip"};
        bool *flags[] = {&picker->show_name, &picker->show_stroke, &picker->show_tip};
        for (size_t i = 0; i < 3; i++) {
            nk_bool value = *flags[i];
            char id[80];
            (void)snprintf(id, sizeof id, "picker.show.%zu", i);
            pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
            if (nk_checkbox_label(ctx, pc_i18n_t(labels[i]), &value)) {
                unsigned on =
                    (unsigned)picker->show_name + (unsigned)picker->show_stroke + (unsigned)picker->show_tip;
                if (value || on > 1)
                    *flags[i] = value != 0;
            }
        }
        float list_height = fmaxf(20, nk_window_get_content_region(ctx).h - 144);
        nk_layout_row_dynamic(ctx, list_height, 1);
        if (nk_group_begin(ctx, "brush-picker-presets", 0)) {
            float card_height =
                8.f + (picker->show_name ? 24.f : 0.f) +
                ((picker->show_tip || picker->show_stroke) ? fmaxf(.5f, picker->scale) * 56 : 0);
            int columns = (int)fmaxf(1, fminf(8, floorf((bounds.w - 20) / fmaxf(60, 250 * picker->scale))));
            nk_layout_row_dynamic(ctx, card_height, columns);
            for (size_t i = 0; i < tools->npresets; i++)
                if (pc_ui_brush_preset_matches(pc_str_c(&tools->presets[i].name), r->search))
                    pc_ui_shell_brush_preset_card(app, i, tools, picker);
            nk_group_end(ctx);
        }
        nk_layout_row_dynamic(ctx, 24, 1);
        pc_ui_shell_widget_mapping(app, "picker.scale", nk_widget_bounds(ctx));
        nk_property_float(ctx, pc_i18n_t("Scale"), .15f, &picker->scale, 2, .05f, .01f);
        if (first)
            nk_widget_disable_end(ctx);
    }
    nk_end(ctx);
    nk_style_pop_vec2(ctx);
    persist(app, r);
    if (nk_window_is_closed(ctx, "Brush Preset Picker")) {
        picker->open = r->shown = false;
        nk_window_show(ctx, "Brush Preset Picker", NK_SHOWN);
    }
}
bool pc_ui_shell_brush_picker_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_brush_picker *p = &pc_ui_shell_app_state(app)->brush_picker;
    if (!p->open)
        return false;
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
        SDL_Window *window = pc_ui_shell_app_sdl_window(app);
        float density = window ? SDL_GetWindowPixelDensity(window) : 1;
        float scale = pc_ui_shell_app_ui_scale(app);
        float factor = density / (scale > 0 ? scale : 1);
        float x = event->button.x * factor, y = event->button.y * factor;
        if (x < p->point[0] || y < p->point[1] || x >= p->point[0] + p->size[0] ||
            y >= p->point[1] + p->size[1]) {
            p->open = false;
            return true;
        }
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST ||
        (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE)) {
        p->open = false;
        return event->type == SDL_EVENT_KEY_DOWN;
    }
    return false;
}
