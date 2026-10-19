#include "layer_interaction.h"
#include "context_ui.h"
#include "shell_internal.h"
#include "jobs_ui.h"
#include "ui/widgets.h"
#include "base/mem.h"
#include "base/num.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void cancel_drag(pc_ui_panel_runtime *r)
{
    free(r->drag.layers);
    pc_doc_arc_release(r->drag.snapshot);
    memset(&r->drag, 0, sizeof r->drag);
}
void pc_ui_layer_interaction_fini(pc_ui_panel_runtime *r)
{
    cancel_drag(r);
    memset(&r->rename, 0, sizeof r->rename);
}
static void unfocus(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (!ctx)
        return;
    for (struct nk_window *w = ctx->begin; w; w = w->next)
        if (!strcmp(w->name_string, "PhotoCraft"))
            w->edit.active = false;
    ctx->text_edit.active = false;
}
static bool rename_live(pc_ui_shell_app *app, pc_ui_panel_runtime *r)
{
    const pc_engine_doc_state *s = pc_engine_session_active(pc_ui_shell_app_session(app));
    return r->rename.active && s && pc_doc_arc_get(s->doc)->id == r->rename.document &&
           s->revision == r->rename.revision && pc_document_layer(pc_doc_arc_get(s->doc), r->rename.layer);
}
static pc_status rename_params(pc_ui_shell_app *app, pc_ui_panel_runtime *r, bool deferred, pc_error *err)
{
    if (!rename_live(app, r)) {
        r->rename.active = false;
        unfocus(app);
        return PC_OK;
    }
    const pc_engine_doc_state *s = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_layer *layer = pc_document_layer(pc_doc_arc_get(s->doc), r->rename.layer);
    if (!strcmp(pc_str_c(&layer->name), r->rename.name)) {
        r->rename.active = false;
        unfocus(app);
        return PC_OK;
    }
    bool nonblank = false;
    for (size_t i = 0; r->rename.name[i]; i++) {
        unsigned char ch = (unsigned char)r->rename.name[i];
        nonblank =
            nonblank || (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n' && ch != '\f' && ch != '\v');
    }
    if (!nonblank)
        return pc_fail(err, PC_ERR_INVALID, "a layer name cannot be empty");
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    pc_status status = PC_OK;
    if (!p || !yyjson_mut_obj_add_uint(d, p, "layer", r->rename.layer) ||
        !yyjson_mut_obj_add_strcpy(d, p, "name", r->rename.name))
        status = pc_fail(err, PC_ERR_NOMEM, "layer rename");
    if (status == PC_OK && deferred)
        status = pc_ui_command_queue_push(&r->commands, "layer.renameLayer", p, err);
    else if (status == PC_OK) {
        pc_buf json = PC_BUF_INIT;
        pc_json_value out = PC_JSON_VALUE_INIT;
        status = pc_json_write(p, 0, &json, err);
        if (status == PC_OK)
            status = pc_engine_session_execute_json(pc_ui_shell_app_session(app), "layer.renameLayer",
                                                    pc_buf_cstr(&json), &out, err);
        pc_json_value_fini(&out);
        pc_buf_fini(&json);
        pc_ui_shell_app_changed(app);
    }
    yyjson_mut_doc_free(d);
    if (status == PC_OK) {
        r->rename.active = false;
        unfocus(app);
    }
    return status;
}
pc_status pc_ui_layer_rename_start(pc_ui_shell_app *app, pc_ui_panel_runtime *r, pc_layer_id id,
                                   pc_error *err)
{
    if (pc_ui_shell_app_state(app)->n_dialogs || pc_ui_shell_jobs_locked(app))
        return pc_fail(err, PC_ERR_INVALID, "finish the current operation before renaming");
    if (r->rename.active && r->rename.layer == id && rename_live(app, r))
        return PC_OK;
    pc_status status = r->rename.active ? rename_params(app, r, false, err) : PC_OK;
    if (status != PC_OK)
        return status;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    const pc_layer *layer = doc ? pc_document_layer(doc, id) : NULL;
    if (!layer)
        return pc_fail(err, PC_ERR_NOT_FOUND, "no layer to rename");
    if (layer->name.len >= sizeof r->rename.name)
        return pc_fail(err, PC_ERR_LIMIT, "layer name exceeds the inline editor limit");
    cancel_drag(r);
    r->rename.active = r->rename.focus = true;
    r->rename.document = doc->id;
    r->rename.revision = state->revision;
    r->rename.layer = id;
    memcpy(r->rename.name, pc_str_c(&layer->name), layer->name.len + 1);
    return PC_OK;
}
static const pc_ui_panel_hit *row_at(const pc_ui_panel_runtime *r, float x, float y)
{
    for (size_t i = 0; i < r->nhits; i++) {
        const pc_ui_panel_hit *h = &r->hits[i];
        if (!strcmp(h->command, "layer.select") && x >= h->rect[0] && y >= h->rect[1] &&
            x < h->rect[0] + h->rect[2] && y < h->rect[1] + h->rect[3])
            return h;
    }
    return NULL;
}
static void drop_target(pc_ui_shell_app *app, pc_ui_panel_runtime *r, float x, float y)
{
    r->drag.point[0] = x;
    r->drag.point[1] = y;
    r->drag.allowed = false;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_ui_panel_hit *hit = row_at(r, x, y);
    if (!state || !hit)
        return;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const pc_layer *layer = pc_document_layer(doc, hit->layer);
    if (!layer)
        return;
    r->drag.copy = ((SDL_GetModState() & SDL_KMOD_ALT) || (pc_ui_shell_input_modifiers(app) & PC_UI_MOD_ALT));
    r->drag.target = layer->id;
    r->drag.position = pc_ui_layer_drop_zone(pc_layer_is_group(layer), (y - hit->rect[1]) / hit->rect[3]);
    r->drag.allowed = pc_ui_layer_drop_allowed(doc, r->drag.layers, r->drag.count, layer->id,
                                               r->drag.position, r->drag.copy);
    memcpy(r->drag.rect, hit->rect, sizeof r->drag.rect);
}
bool pc_ui_layer_interaction_event(pc_ui_shell_app *app, pc_ui_panel_runtime *r, const SDL_Event *e)
{
    if (!e)
        return false;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        cancel_drag(r);
        if (r->rename.active) {
            pc_error error;
            if (rename_params(app, r, false, &error) != PC_OK)
                pc_ui_shell_report(app, "Rename Layer", error.msg, true);
        }
        return false;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE && (r->rename.active || r->drag.pressed)) {
        r->rename.active = false;
        unfocus(app);
        cancel_drag(r);
        return true;
    }
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state || pc_ui_shell_app_state(app)->n_dialogs || pc_ui_shell_jobs_locked(app)) {
        cancel_drag(r);
        return false;
    }
    if (r->drag.pressed &&
        (pc_doc_arc_get(state->doc)->id != r->drag.document || state->revision != r->drag.revision)) {
        if (state->doc == r->drag.snapshot && state->has_last_damage &&
            pc_rect_is_empty(state->last_damage) && state->has_active_layer &&
            state->active_layer == r->drag.source) {
            /* Selecting an unselected source on press precedes crossing the drag threshold. */
            r->drag.revision = state->revision;
        } else {
            cancel_drag(r);
            struct nk_context *ctx = pc_ui_shell_app_nk(app);
            if (ctx) {
                ctx->input.mouse.buttons[NK_BUTTON_LEFT].down = false;
                ctx->input.mouse.buttons[NK_BUTTON_LEFT].clicked = 0;
            }
            return false;
        }
    }
    bool button = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (!button && e->type != SDL_EVENT_MOUSE_MOTION)
        return false;
    float scale = pc_ui_shell_app_ui_scale(app);
    SDL_Window *window = pc_ui_shell_app_sdl_window(app);
    float factor = (window ? SDL_GetWindowPixelDensity(window) : 1) / fmaxf(.01f, scale);
    float x = (button ? e->button.x : e->motion.x) * factor,
          y = (button ? e->button.y : e->motion.y) * factor;
    if (!isfinite(x) || !isfinite(y))
        return r->drag.active;
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_RIGHT && !r->rename.active) {
        const pc_ui_panel_hit *hit = NULL;
        for (size_t i = 0; i < r->nhits; i++) {
            const pc_ui_panel_hit *h = &r->hits[i];
            if ((!strcmp(h->command, "layer.select") || !strcmp(h->command, "layer.mask.thumbnail")) &&
                x >= h->rect[0] && y >= h->rect[1] && x < h->rect[0] + h->rect[2] &&
                y < h->rect[1] + h->rect[3]) {
                hit = h;
                break;
            }
        }
        if (!hit)
            return false;
        pc_error err = {0};
        pc_json_value out = PC_JSON_VALUE_INIT;
        bool selected = false;
        for (size_t i = 0; i < state->nselected; i++)
            selected = selected || state->selected_layers[i] == hit->layer;
        pc_status status = PC_OK;
        if (!state->has_active_layer || state->active_layer != hit->layer) {
            char params[128];
            (void)snprintf(params, sizeof params, "{\"layer\":%llu,\"mode\":\"%s\"}",
                           (unsigned long long)hit->layer, selected ? "add" : "replace");
            status = pc_engine_session_execute_json(pc_ui_shell_app_session(app), "layer.select", params,
                                                    &out, &err);
        }
        double anchor[2] = {0, 0};
        if (status == PC_OK)
            status = pc_ui_shell_context_open(
                app, !strcmp(hit->command, "layer.mask.thumbnail") ? PC_UI_CONTEXT_MASK : PC_UI_CONTEXT_LAYER,
                x, y, anchor, hit->layer, &err);
        if (status != PC_OK)
            pc_ui_shell_report(app, "Layer Menu", err.msg, true);
        pc_json_value_fini(&out);
        return true;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT) {
        const pc_ui_panel_hit *hit = row_at(r, x, y);
        pc_layer_id row = hit ? hit->layer : 0;
        if (r->rename.active && row != r->rename.layer) {
            pc_error error;
            if (rename_params(app, r, false, &error) != PC_OK) {
                pc_ui_shell_report(app, "Rename Layer", error.msg, true);
                return true;
            }
            state = pc_engine_session_active(pc_ui_shell_app_session(app));
        }
        if (!row || r->rename.active || pc_ui_shell_keyboard_captured(app))
            return false;
        if (e->button.clicks >= 2) {
            pc_error error;
            if (pc_ui_layer_rename_start(app, r, row, &error) != PC_OK)
                pc_ui_shell_report(app, "Rename Layer", error.msg, true);
            return false;
        }
        cancel_drag(r);
        pc_layer_id *ids = NULL;
        size_t count = 0;
        if (pc_engine_doc_state_selected_layers(state, &ids, &count, NULL) != PC_OK)
            return false;
        bool selected = false;
        for (size_t i = 0; i < count; i++)
            selected = selected || ids[i] == row;
        if (!selected) {
            free(ids);
            ids = pc_malloc_array(1, sizeof *ids);
            if (!ids)
                return false;
            ids[0] = row;
            count = 1;
        }
        r->drag.pressed = true;
        r->drag.source = row;
        r->drag.layers = ids;
        r->drag.snapshot = pc_doc_arc_retain(state->doc);
        r->drag.count = count;
        r->drag.document = pc_doc_arc_get(state->doc)->id;
        r->drag.revision = state->revision;
        r->drag.press[0] = r->drag.point[0] = x;
        r->drag.press[1] = r->drag.point[1] = y;
    } else if (r->drag.pressed && e->type == SDL_EVENT_MOUSE_MOTION) {
        if (hypotf(x - r->drag.press[0], y - r->drag.press[1]) >= 4)
            r->drag.active = true;
        if (r->drag.active)
            drop_target(app, r, x, y);
        return r->drag.active;
    } else if (r->drag.pressed && e->type == SDL_EVENT_MOUSE_BUTTON_UP &&
               e->button.button == SDL_BUTTON_LEFT) {
        bool consumed = r->drag.active;
        drop_target(app, r, x, y);
        if (consumed && r->drag.allowed) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL, *ids = d ? yyjson_mut_arr(d) : NULL;
            bool ok = p && ids;
            for (size_t i = 0; ok && i < r->drag.count; i++)
                ok = yyjson_mut_arr_add_uint(d, ids, r->drag.layers[i]);
            ok = ok && yyjson_mut_obj_add_val(d, p, "layers", ids) &&
                 yyjson_mut_obj_add_uint(d, p, "target", r->drag.target) &&
                 yyjson_mut_obj_add_str(d, p, "position", pc_ui_layer_drop_name(r->drag.position)) &&
                 yyjson_mut_obj_add_bool(d, p, "copy", r->drag.copy);
            if (ok)
                pc_ui_shell_queue(app, "layer.moveTo", p);
            else
                pc_ui_shell_report(app, "Reorder Layers", "Not enough memory", true);
            yyjson_mut_doc_free(d);
        }
        cancel_drag(r);
        if (consumed) {
            struct nk_context *ctx = pc_ui_shell_app_nk(app);
            nk_input_button(ctx, NK_BUTTON_LEFT, pc_f32_to_i32(x), pc_f32_to_i32(y), false);
            ctx->input.mouse.buttons[NK_BUTTON_LEFT].clicked = 0;
        }
        return consumed;
    }
    return false;
}
bool pc_ui_layer_rename_row(pc_ui_shell_app *app, pc_ui_panel_runtime *r, const pc_engine_doc_state *state,
                            const pc_layer *layer)
{
    if (!r->rename.active)
        return false;
    if (r->rename.document != pc_doc_arc_get(state->doc)->id || r->rename.revision != state->revision ||
        !pc_document_layer(pc_doc_arc_get(state->doc), r->rename.layer)) {
        r->rename.active = false;
        unfocus(app);
        return false;
    }
    if (r->rename.layer != layer->id)
        return false;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (r->rename.focus) {
        nk_edit_focus(ctx, NK_EDIT_ALWAYS_INSERT_MODE | NK_EDIT_AUTO_SELECT);
        int length = nk_utf_len(r->rename.name, (int)strlen(r->rename.name));
        ctx->current->edit.cursor = length;
        ctx->current->edit.sel_start = 0;
        ctx->current->edit.sel_end = length;
        r->rename.focus = false;
    }
    nk_flags edited =
        nk_edit_string_zero_terminated(ctx, (nk_flags)NK_EDIT_FIELD | NK_EDIT_SIG_ENTER | NK_EDIT_AUTO_SELECT,
                                       r->rename.name, sizeof r->rename.name, nk_filter_default);
    if ((edited & NK_EDIT_COMMITED) || (edited & NK_EDIT_DEACTIVATED)) {
        pc_error error;
        if (rename_params(app, r, true, &error) != PC_OK) {
            pc_ui_shell_report(app, "Rename Layer", error.msg, true);
            r->rename.focus = true;
        }
    }
    return true;
}
void pc_ui_layer_drag_feedback(pc_ui_shell_app *app, pc_ui_panel_runtime *r)
{
    if (!r->drag.active)
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    if (r->drag.allowed) {
        const float *p = r->drag.rect;
        if (r->drag.position == PC_UI_LAYER_INTO)
            nk_stroke_rect(canvas, nk_rect(p[0], p[1], p[2], p[3]), 0, 2, pc_ui_nk_color(tokens->accent));
        else {
            float y = p[1] + (r->drag.position == PC_UI_LAYER_BELOW ? p[3] : 0);
            nk_stroke_line(canvas, p[0], y, p[0] + p[2], y, 2, pc_ui_nk_color(tokens->accent));
        }
    }
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_layer *layer = state ? pc_document_layer(pc_doc_arc_get(state->doc), r->drag.source) : NULL;
    if (!layer)
        return;
    char text[256];
    (void)snprintf(text, sizeof text, "%s%s", r->drag.copy ? "+ " : "", pc_str_c(&layer->name));
    struct nk_rect ghost = nk_rect(r->drag.point[0] + 12, r->drag.point[1] + 12, 180, 22);
    nk_fill_rect(canvas, ghost, 3, pc_ui_nk_color(tokens->chrome));
    nk_draw_text(canvas, ghost, text, (int)strlen(text), ctx->style.font, pc_ui_nk_color(tokens->chrome),
                 pc_ui_nk_color(tokens->text));
}
