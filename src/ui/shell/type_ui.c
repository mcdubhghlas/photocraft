#include "jobs_ui.h"
#include "shell_internal.h"
#include "base/mem.h"
#include "base/num.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static void report(pc_ui_shell_app *app, pc_status status, const pc_error *error)
{
    pc_ui_shell_app_changed(app);
    if (status != PC_OK && status != PC_ERR_NOT_FOUND) {
        pc_ui_state *ui = pc_ui_shell_app_state(app);
        free(ui->status);
        ui->status = pc_strndup(error->msg, strlen(error->msg));
        ui->status_error = true;
    }
}
pc_status pc_ui_shell_type_clipboard(pc_ui_shell_app *app, SDL_Keycode key, pc_json_value *out, pc_error *err)
{
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (key == SDLK_V) {
        char *value = SDL_GetClipboardText();
        if (!value) {
            return pc_fail(err, PC_ERR_IO, "clipboard: %s", SDL_GetError());
        }
        size_t length = strnlen(value, ((size_t)1 << 20) + 1);
        pc_status status = pc_ui_type_insert(session, ui, value, length, out, err);
        SDL_free(value);
        return status;
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    const pc_layer *layer = doc ? pc_document_layer(doc, ui->text_edit.layer) : NULL;
    if (!layer || layer->content.kind != PC_LAYER_TEXT ||
        (ui->text_edit.document && doc->id != ui->text_edit.document)) {
        return pc_fail(err, PC_ERR_INVALID, "text document changed");
    }
    const pc_str *text = &layer->content.u.text.text;
    size_t a = pc_text_byte_index(pc_str_c(text), text->len, ui->text_edit.anchor),
           b = pc_text_byte_index(pc_str_c(text), text->len, ui->text_edit.caret);
    if (a > b) {
        size_t t = a;
        a = b;
        b = t;
    }
    if (a == b) {
        return PC_OK;
    }
    char *copy = pc_strndup(pc_str_c(text) + a, b - a);
    if (!copy) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    bool copied = SDL_SetClipboardText(copy);
    free(copy);
    if (!copied) {
        return pc_fail(err, PC_ERR_IO, "clipboard: %s", SDL_GetError());
    }
    return key == SDLK_X ? pc_ui_type_insert(session, ui, "", 0, out, err) : PC_OK;
}
bool pc_ui_shell_type_input(pc_ui_shell_app *app, const SDL_Event *e)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        pc_ui_type_pointer *p = pc_ui_shell_type_pointer(app);
        p->pressed = p->selecting = false;
    }
    if (!ui->has_text_edit || ui->n_dialogs || pc_ui_shell_jobs_locked(app) ||
        pc_ui_shell_keyboard_captured(app)) {
        return false;
    }
    if (e->type != SDL_EVENT_TEXT_INPUT && e->type != SDL_EVENT_TEXT_EDITING &&
        e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) {
        return false;
    }
    if (e->type == SDL_EVENT_KEY_UP) {
        return true;
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error error = {0};
    pc_status status = PC_OK;
    pc_engine_session *session = pc_ui_shell_app_session(app);
    if (e->type == SDL_EVENT_TEXT_INPUT || e->type == SDL_EVENT_TEXT_EDITING) {
        const char *text = e->type == SDL_EVENT_TEXT_INPUT ? e->text.text : e->edit.text;
        size_t length = text ? strnlen(text, ((size_t)1 << 20) + 1) : 0;
        status = e->type == SDL_EVENT_TEXT_INPUT
                     ? pc_ui_type_text_event(session, ui, text ? text : "", length, &out, &error)
                     : pc_ui_type_preedit(session, ui, text ? text : "", length, &out, &error);
    } else {
        unsigned mods = pc_ui_shell_input_mods(e->key.mod);
        if ((mods & (PC_UI_MOD_COMMAND | PC_UI_MOD_CTRL)) &&
            (e->key.key == SDLK_C || e->key.key == SDLK_X || e->key.key == SDLK_V)) {
            status = pc_ui_shell_type_clipboard(app, e->key.key, &out, &error);
        } else {
            status = pc_ui_type_key(session, ui, &pc_ui_shell_type_pointer(app)->layout,
                                    pc_ui_shell_input_key(e->key.key), mods, &out, &error);
        }
    }
    pc_json_value_fini(&out);
    report(app, status, &error);
    return true;
}
bool pc_ui_shell_type_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *e,
                             float factor)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_type_pointer *p = pc_ui_shell_type_pointer(app);
    if (ui->n_dialogs || pc_ui_shell_jobs_locked(app) || pc_ui_shell_keyboard_captured(app) ||
        (!p->pressed && !pc_ui_tool_is_type(ui->tool))) {
        return false;
    }
    bool move = e->type == SDL_EVENT_MOUSE_MOTION, down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!move && e->type != SDL_EVENT_MOUSE_BUTTON_DOWN && e->type != SDL_EVENT_MOUSE_BUTTON_UP) {
        return false;
    }
    if (!move && e->button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    float x = (move ? e->motion.x : e->button.x) * factor, y = (move ? e->motion.y : e->button.y) * factor;
    if (!p->pressed &&
        (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height)) {
        return false;
    }
    float doc[2];
    if (pc_ui_canvas_to_doc(xf, x, y, doc) != PC_OK) {
        return p->pressed;
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error error = {0};
    pc_status status = pc_ui_shell_app_pointer(app,
                                               down   ? PC_UI_POINTER_DOWN
                                               : move ? PC_UI_POINTER_MOVE
                                                      : PC_UI_POINTER_UP,
                                               (pc_ui_tool_point){.x = doc[0], .y = doc[1], .pressure = 1},
                                               pc_ui_shell_input_modifiers(app), &out, &error);
    pc_json_value_fini(&out);
    report(app, status, &error);
    return status != PC_ERR_NOT_FOUND;
}
static bool screen(const pc_text_layer *text, const pc_ui_canvas_xform *xf, float x, float y, float out[2])
{
    pc_point point = pc_affine_apply(text->transform, (pc_point){x, y});
    return pc_ui_canvas_to_screen(xf, (float)point.x, (float)point.y, out) == PC_OK;
}
void pc_ui_shell_type_draw(pc_ui_shell_app *app, struct nk_command_buffer *canvas)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_canvas_xform xf;
    if (!ui->has_text_edit || !pc_ui_shell_app_canvas_mapping(app, &xf)) {
        return;
    }
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    const pc_layer *layer = doc ? pc_document_layer(doc, ui->text_edit.layer) : NULL;
    if (!layer || layer->content.kind != PC_LAYER_TEXT ||
        (ui->text_edit.document && doc->id != ui->text_edit.document)) {
        return;
    }
    pc_ui_type_pointer *pointer = pc_ui_shell_type_pointer(app);
    if (pc_ui_type_layout_get(&pointer->layout, state, layer->id, NULL) != PC_OK) {
        return;
    }
    const pc_text_layer *text = &layer->content.u.text;
    const pc_text_layout *layout = &pointer->layout.layout;
    size_t a = pc_text_byte_index(pc_str_c(&text->text), text->text.len, ui->text_edit.anchor),
           b = pc_text_byte_index(pc_str_c(&text->text), text->text.len, ui->text_edit.caret);
    size_t low = a < b ? a : b, high = a > b ? a : b;
    struct nk_rect clip = canvas->clip;
    nk_push_scissor(canvas, nk_rect(xf.x, xf.y, xf.width, xf.height));
    for (size_t i = 0; i < layout->nclusters; i++) {
        const pc_cluster_info *c = &layout->clusters[i];
        if (c->end <= low || c->start >= high || c->line >= layout->nlines) {
            continue;
        }
        const pc_line_info *line = &layout->lines[c->line];
        float left = c->x, right = c->x + c->advance, top = line->baseline - line->ascent,
              bottom = line->baseline + line->descent;
        const float xs[4] = {left, right, right, left}, ys[4] = {top, top, bottom, bottom};
        float polygon[8];
        bool valid = true;
        for (size_t j = 0; j < 4; j++) {
            float x, y;
            pc_text_layout_to_text(layout, xs[j], ys[j], &x, &y);
            valid = screen(text, &xf, x, y, polygon + j * 2) && valid;
        }
        if (valid) {
            nk_fill_polygon(canvas, polygon, 4, nk_rgba(60, 130, 240, 95));
        }
    }
    if (text->shape.kind == PC_TEXT_SHAPE_BOX) {
        float x = text->shape.x, y = text->shape.y, w = text->shape.width, h = text->shape.height;
        const float spots[8][2] = {{x, y},         {x + w, y},         {x + w, y + h},     {x, y + h},
                                   {x + w / 2, y}, {x + w, y + h / 2}, {x + w / 2, y + h}, {x, y + h / 2}};
        float polygon[8];
        bool valid = true;
        for (size_t i = 0; i < 4; i++) {
            valid = screen(text, &xf, spots[i][0], spots[i][1], polygon + i * 2) && valid;
        }
        if (valid) {
            nk_stroke_polygon(canvas, polygon, 4, 1, nk_rgb(80, 160, 255));
        }
        for (size_t i = 0; i < 8; i++) {
            float at[2];
            if (screen(text, &xf, spots[i][0], spots[i][1], at)) {
                nk_fill_rect(canvas, nk_rect(at[0] - 3, at[1] - 3, 6, 6), 0, nk_rgb(255, 255, 255));
                nk_stroke_rect(canvas, nk_rect(at[0] - 3, at[1] - 3, 6, 6), 0, 1, nk_rgb(40, 100, 190));
            }
        }
    }
    float caret[4], start[2], end[2];
    pc_text_layout_caret_segment(layout, b, caret);
    if (screen(text, &xf, caret[0], caret[1], start) && screen(text, &xf, caret[2], caret[3], end)) {
        if ((SDL_GetTicks() / 500) % 2 == 0 || a != b) {
            nk_stroke_line(canvas, start[0], start[1], end[0], end[1], 3, nk_rgb(0, 0, 0));
            nk_stroke_line(canvas, start[0], start[1], end[0], end[1], 1, nk_rgb(255, 255, 255));
        }
        SDL_Window *window = pc_ui_shell_app_sdl_window(app);
        if (window && fmaxf(start[0], end[0]) >= xf.x && fminf(start[0], end[0]) <= xf.x + xf.width &&
            fmaxf(start[1], end[1]) >= xf.y && fminf(start[1], end[1]) <= xf.y + xf.height) {
            float factor = pc_ui_shell_app_ui_scale(app) / SDL_GetWindowPixelDensity(window);
            SDL_Rect area = {pc_f32_to_i32(fminf(start[0], end[0]) * factor),
                             pc_f32_to_i32(fminf(start[1], end[1]) * factor),
                             pc_f32_to_i32(fminf(10000, fabsf(end[0] - start[0]) * factor)) + 2,
                             pc_f32_to_i32(fminf(10000, fabsf(end[1] - start[1]) * factor)) + 2};
            (void)SDL_SetTextInputArea(window, &area, 0);
        }
    }
    nk_push_scissor(canvas, clip);
}
