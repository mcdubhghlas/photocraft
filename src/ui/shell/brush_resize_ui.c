#include "brush_resize_ui.h"
#include "jobs_ui.h"
#include "ui/brush_resize.h"
#include "ui/color_picker.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "engine/prefs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool vary_hardness(pc_ui_shell_app *app)
{
    pc_json_value v = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(pc_ui_shell_app_session(app), "tools.varyRoundBrushHardnessOnHud", &v, NULL);
    yyjson_mut_val *root = pc_json_value_root(&v);
    bool vary = !yyjson_mut_is_bool(root) || yyjson_mut_get_bool(root);
    pc_json_value_fini(&v);
    return vary;
}
static void update(pc_ui_shell_app *app, pc_ui_canvas_view *cv, float x, float y)
{
    if (!isfinite(x) || !isfinite(y))
        return;
    float values[2];
    pc_ui_brush_resized(cv->brush_resize.start[0], cv->brush_resize.start[1], x - cv->brush_resize.pointer[0],
                        y - cv->brush_resize.pointer[1], cv->brush_resize.zoom, values);
    const pc_paint_brush *brush = &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush;
    if (brush->size == values[0] && (cv->brush_resize.vary ? brush->hardness : brush->opacity) == values[1])
        return;
    char params[192];
    (void)snprintf(params, sizeof params,
                   "{\"brush\":{\"size\":%.0f,\"%s\":%.4f},\"coalesce\":\"brush-resize:%llu\"}",
                   (double)values[0], cv->brush_resize.vary ? "hardness" : "opacity", (double)values[1],
                   (unsigned long long)cv->brush_resize.sequence);
    yyjson_doc *parsed = NULL;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err;
    pc_status status = pc_json_parse(params, strlen(params), &parsed, &err);
    if (status == PC_OK)
        status = pc_ui_shell_app_invoke(app, "tools.setBrush", yyjson_doc_get_root(parsed), &out, &err);
    if (status != PC_OK)
        pc_ui_shell_report(app, "Brush Resize", err.msg, true);
    pc_json_value_fini(&out);
    yyjson_doc_free(parsed);
}
bool pc_ui_brush_resize_event(pc_ui_shell_app *app, pc_ui_canvas_view *cv, const pc_ui_canvas_xform *xf,
                              const SDL_Event *e, float factor)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (cv->brush_resize.active &&
        (!state || pc_doc_arc_get(state->doc)->id != cv->brush_resize.document ||
         state->revision != cv->brush_resize.revision || ui->tool != cv->brush_resize.tool || ui->n_dialogs ||
         pc_ui_shell_jobs_locked(app)))
        cv->brush_resize.active = false;
    if (cv->brush_resize.active) {
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) {
            cv->brush_resize.active = false;
            return true;
        }
        if (e->type == SDL_EVENT_MOUSE_MOTION) {
            if (!(e->motion.state & SDL_BUTTON_MASK(cv->brush_resize.button))) {
                cv->brush_resize.active = false;
                return true;
            }
            update(app, cv, e->motion.x * factor, e->motion.y * factor);
            return true;
        }
        if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && e->button.button == cv->brush_resize.button) {
            update(app, cv, e->button.x * factor, e->button.y * factor);
            cv->brush_resize.active = false;
            return true;
        }
    }
    if (e->type != SDL_EVENT_MOUSE_BUTTON_DOWN || !state || ui->n_dialogs || cv->dragging ||
        cv->gesture.active || cv->guide_drag.active || pc_ui_shell_jobs_locked(app) ||
        pc_ui_shell_keyboard_captured(app) || !pc_ui_brush_resize_applies(ui->tool))
        return false;
    SDL_Keymod mods = SDL_GetModState();
    bool alt = (mods & SDL_KMOD_ALT) || (cv->held_mods & PC_UI_MOD_ALT);
    bool ctrl = (mods & SDL_KMOD_CTRL) || (cv->held_mods & PC_UI_MOD_CTRL);
    bool gesture =
        alt && (e->button.button == SDL_BUTTON_RIGHT || (ctrl && e->button.button == SDL_BUTTON_LEFT));
    float x = e->button.x * factor, y = e->button.y * factor;
    if (!gesture || !isfinite(x) || !isfinite(y) || x < xf->x || y < xf->y || x >= xf->x + xf->width ||
        y >= xf->y + xf->height)
        return false;
    if (pc_ui_canvas_to_doc(xf, x, y, cv->brush_resize.anchor) != PC_OK)
        return false;
    cv->brush_resize.sequence++;
    cv->brush_resize.active = true;
    cv->brush_resize.button = e->button.button;
    cv->brush_resize.document = pc_doc_arc_get(state->doc)->id;
    cv->brush_resize.revision = state->revision;
    cv->brush_resize.tool = ui->tool;
    cv->brush_resize.pointer[0] = x;
    cv->brush_resize.pointer[1] = y;
    cv->brush_resize.zoom = xf->zoom;
    cv->brush_resize.vary = vary_hardness(app);
    const pc_paint_brush *brush = &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush;
    cv->brush_resize.start[0] = brush->size;
    cv->brush_resize.start[1] = cv->brush_resize.vary ? brush->hardness : brush->opacity;
    return true;
}
void pc_ui_brush_resize_draw(pc_ui_shell_app *app, pc_ui_canvas_view *cv)
{
    if (!cv->brush_resize.active)
        return;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state || cv->brush_resize.document != pc_doc_arc_get(state->doc)->id ||
        cv->brush_resize.revision != state->revision ||
        cv->brush_resize.tool != pc_ui_shell_app_state(app)->tool) {
        cv->brush_resize.active = false;
        return;
    }
    float center[2];
    if (pc_ui_canvas_to_screen(&cv->xf, cv->brush_resize.anchor[0], cv->brush_resize.anchor[1], center) !=
        PC_OK)
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    const pc_paint_brush *brush = &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush;
    float radius = fmaxf(1, brush->size * cv->xf.zoom / 2);
    pc_json_value color = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(pc_ui_shell_app_session(app), "cursors.brushPreviewColor", &color, NULL);
    const char *hex = yyjson_mut_get_str(pc_json_value_root(&color));
    float rgb[3] = {1, 0, 0};
    if (hex)
        (void)pc_ui_picker_parse_hex(hex, strlen(hex), rgb);
    pc_json_value_fini(&color);
    struct nk_color tint = nk_rgba_f(rgb[0], rgb[1], rgb[2], .07f);
    /* Fixed ring count keeps HUD work bounded for very large tips. */
    for (int ring = 16; ring >= 1; ring--) {
        float fraction = (float)ring / 16;
        if (fraction > brush->hardness && brush->hardness < 1)
            tint.a = (nk_byte)(18 * (1 - fraction) / fmaxf(.001f, 1 - brush->hardness));
        else
            tint.a = 18;
        float r = radius * fraction;
        nk_fill_circle(canvas, nk_rect(center[0] - r, center[1] - r, r * 2, r * 2), tint);
    }
    struct nk_rect outline = nk_rect(center[0] - radius, center[1] - radius, radius * 2, radius * 2);
    nk_stroke_circle(canvas, outline, 3, nk_rgba(0, 0, 0, 180));
    nk_stroke_circle(canvas, outline, 1, nk_rgba(255, 255, 255, 230));
    char label[160];
    (void)snprintf(label, sizeof label, "%s %.0f px   %s %.0f%%", pc_i18n_t("Diameter:"), (double)brush->size,
                   pc_i18n_t(cv->brush_resize.vary ? "Hardness:" : "Opacity:"),
                   (double)(cv->brush_resize.vary ? brush->hardness : brush->opacity) * 100);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    struct nk_rect readout =
        nk_rect(fminf(ctx->input.mouse.pos.x + 16, cv->xf.x + cv->xf.width - 310),
                fminf(ctx->input.mouse.pos.y + 20, cv->xf.y + cv->xf.height - 28), 300, 24);
    nk_fill_rect(canvas, readout, 3, pc_ui_nk_color(tokens->chrome));
    nk_draw_text(canvas, readout, label, (int)strlen(label), ctx->style.font, pc_ui_nk_color(tokens->chrome),
                 pc_ui_nk_color(tokens->text));
    pc_ui_shell_widget_mapping(app, "brush.resize.readout", readout);
}
