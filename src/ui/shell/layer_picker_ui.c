#include "layer_picker_ui.h"
#include "canvas_view.h"
#include "jobs_ui.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include <stdio.h>
#include <string.h>
static const char *const POINTS[] = {"black", "gray", "white"};
static const char *const LABELS[] = {"Set Black Point", "Set Neutral Gray Point", "Set White Point"};
pc_status pc_ui_shell_layer_picker_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                           pc_json_value *out, pc_error *err)
{
    if (strcmp(id, "ui.layerPicker.arm"))
        return PC_ERR_NOT_FOUND;
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->n_dialogs || ui->transform.active || ui->crop_editing || pc_ui_shell_jobs_locked(app))
        return pc_fail(err, PC_ERR_INVALID, "finish the dialog or document job first");
    yyjson_val *value = pc_json_get(params, "point");
    if (value && yyjson_is_null(value))
        pc_ui_layer_picker_cancel(ui);
    else {
        const char *point = yyjson_get_str(value);
        unsigned index = 3;
        if (point && yyjson_get_len(value) == strlen(point))
            for (unsigned i = 0; i < 3; i++)
                if (!strcmp(point, POINTS[i]))
                    index = i;
        if (index == 3)
            return pc_fail(err, PC_ERR_INVALID, "point must be black, gray, white or null");
        pc_status status =
            pc_ui_layer_picker_arm(pc_ui_shell_app_session(app), ui, (pc_engine_tone_picker)index, err);
        if (status != PC_OK)
            return status;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *result = NULL;
    pc_status status = d ? pc_ui_layer_picker_to_json(&ui->layer_picker, d, &result, err)
                         : pc_fail(err, PC_ERR_NOMEM, "picker reply");
    if (status == PC_OK)
        status = pc_json_value_from_mut(result, out, err);
    yyjson_mut_doc_free(d);
    return status;
}
bool pc_ui_shell_layer_picker_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_layer_picker_prune(pc_ui_shell_app_session(app), ui);
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST)
        ui->layer_picker.down = false;
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE && !ui->n_dialogs &&
        ui->layer_picker.armed)
        return pc_ui_layer_picker_cancel(ui);
    return false;
}
bool pc_ui_shell_layer_picker_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf,
                                     const SDL_Event *event, float factor)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_layer_picker_prune(pc_ui_shell_app_session(app), ui);
    if (!pc_ui_layer_picker_armed(pc_ui_shell_app_session(app), ui) || ui->n_dialogs ||
        pc_ui_shell_jobs_locked(app))
        return false;
    bool moving = event->type == SDL_EVENT_MOUSE_MOTION, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!moving && !down && event->type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    if (!moving && event->button.button != SDL_BUTTON_LEFT)
        return false;
    float x = (moving ? event->motion.x : event->button.x) * factor,
          y = (moving ? event->motion.y : event->button.y) * factor;
    if (!ui->layer_picker.down &&
        (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float p[2];
    if (pc_ui_canvas_to_doc(xf, x, y, p) != PC_OK)
        return false;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = pc_ui_shell_app_pointer(app,
                                               down     ? PC_UI_POINTER_DOWN
                                               : moving ? PC_UI_POINTER_MOVE
                                                        : PC_UI_POINTER_UP,
                                               (pc_ui_tool_point){.x = p[0], .y = p[1], .pressure = 1},
                                               pc_ui_shell_input_modifiers(app), &out, &err);
    pc_json_value_fini(&out);
    if (status != PC_OK)
        pc_ui_shell_report(app, "Adjustment Picker", err.msg, true);
    pc_ui_shell_app_changed(app);
    return true;
}
void pc_ui_shell_layer_picker_controls(pc_ui_shell_app *app, const pc_layer *layer)
{
    if (!layer || layer->content.kind != PC_LAYER_ADJUSTMENT ||
        (layer->content.u.adjustment.kind != PC_ADJ_LEVELS &&
         layer->content.u.adjustment.kind != PC_ADJ_CURVES))
        return;
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_ui_layer_picker_prune(session, ui);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state)
        return;
    pc_color_mode mode = pc_doc_arc_get(state->doc)->mode;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 4);
    for (unsigned i = 0; i < 3; i++) {
        nk_layout_row_push(ctx, 28);
        char widget[80];
        (void)snprintf(widget, sizeof widget, "properties.picker.%s", POINTS[i]);
        pc_ui_shell_widget_mapping(app, widget, nk_widget_bounds(ctx));
        bool supported = !ui->n_dialogs && !ui->transform.active && !ui->crop_editing &&
                         !pc_ui_shell_jobs_locked(app) &&
                         pc_engine_tone_picker_supported(mode, (pc_engine_tone_picker)i),
             selected = ui->layer_picker.armed && ui->layer_picker.point == i;
        struct nk_style_button saved = ctx->style.button;
        ctx->style.button.border = selected ? 2 : 1;
        ctx->style.button.border_color = pc_ui_nk_color(selected ? pc_ui_shell_app_tokens(app)->accent_border
                                                                 : pc_ui_shell_app_tokens(app)->card_border);
        if (!supported)
            nk_widget_disable_begin(ctx);
        int tone = i == 0 ? 0 : i == 1 ? 128 : 255;
        bool clicked = nk_button_color(ctx, nk_rgb(tone, tone, tone)) != 0;
        if (!supported)
            nk_widget_disable_end(ctx);
        ctx->style.button = saved;
        if (clicked && supported) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
            if (p && yyjson_mut_obj_add_val(d, p, "point",
                                            selected ? yyjson_mut_null(d) : yyjson_mut_str(d, POINTS[i])))
                pc_ui_shell_queue(app, "ui.layerPicker.arm", p);
            yyjson_mut_doc_free(d);
        }
    }
    nk_layout_row_push(ctx, 180);
    nk_label(ctx, pc_i18n_t(ui->layer_picker.armed ? LABELS[ui->layer_picker.point] : ""), NK_TEXT_LEFT);
    nk_layout_row_end(ctx);
}
