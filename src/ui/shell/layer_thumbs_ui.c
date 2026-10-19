#include "ui/i18n.h"
#include "layer_thumbs_ui.h"
#include "shell_internal.h"
#include "ui/widgets.h"
#include "engine/channel_display.h"
#include <blake3.h>
#include <math.h>
#include <string.h>

static void select_layer(pc_ui_shell_app *app, yyjson_mut_doc *d, pc_layer_id layer)
{
    yyjson_mut_val *p = yyjson_mut_obj(d);
    if (p && yyjson_mut_obj_add_uint(d, p, "layer", layer))
        pc_ui_shell_queue(app, "layer.select", p);
}
static bool bitmap(pc_ui_shell_app *app, const pc_engine_doc_state *state, const pc_layer *layer,
                   pc_ui_layer_thumbnail_kind kind, struct nk_image *out)
{
    const pc_document *doc = pc_doc_arc_get(state->doc);
    blake3_hasher h;
    uint8_t key[32];
    blake3_hasher_init(&h);
    const char family[] = "layer-thumbnail-v1";
    blake3_hasher_update(&h, family, sizeof family);
    blake3_hasher_update(&h, &doc->id, sizeof doc->id);
    blake3_hasher_update(&h, &state->revision, sizeof state->revision);
    blake3_hasher_update(&h, &layer->id, sizeof layer->id);
    blake3_hasher_update(&h, &kind, sizeof kind);
    blake3_hasher_finalize(&h, key, sizeof key);
    if (pc_ui_shell_bitmap(app, key, 64, 64, NULL, out, NULL) == PC_OK)
        return true;
    pc_rgba8_image image = {0};
    pc_status status = pc_ui_layer_thumbnail(doc, layer, kind, 64, &image, NULL);
    if (status == PC_OK)
        status = pc_ui_shell_bitmap(app, key, 64, 64, image.pixels, out, NULL);
    pc_rgba8_image_fini(&image);
    return status == PC_OK;
}
static void clicked(pc_ui_shell_app *app, const pc_layer *layer, pc_ui_layer_thumbnail_kind kind)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    if (!p) {
        yyjson_mut_doc_free(d);
        return;
    }
    const char *channel = kind == PC_UI_THUMB_PIXEL_MASK    ? "mask"
                          : kind == PC_UI_THUMB_VECTOR_MASK ? "vectorMask"
                                                            : "composite";
    unsigned mods = pc_ui_shell_input_modifiers(app);
    const char *command = "channel.target";
    bool mask = kind != PC_UI_THUMB_LAYER;
    if (mods & PC_UI_MOD_COMMAND) {
        command = "select.loadSelection";
        const char *operation = (mods & PC_UI_MOD_ALT) ? (mods & PC_UI_MOD_SHIFT) ? "intersect" : "subtract"
                                : (mods & PC_UI_MOD_SHIFT) ? "add"
                                                           : "replace";
        yyjson_mut_obj_add_uint(d, p, "layer", layer->id);
        yyjson_mut_obj_add_str(d, p, "channel", mask ? channel : "transparency");
        yyjson_mut_obj_add_str(d, p, "operation", operation);
    } else {
        select_layer(app, d, layer->id);
        if (kind != PC_UI_THUMB_PIXEL_MASK && layer->has_mask) {
            yyjson_mut_val *off = yyjson_mut_obj(d);
            if (off && yyjson_mut_obj_add_uint(d, off, "layer", layer->id) &&
                yyjson_mut_obj_add_str(d, off, "mode", "off"))
                pc_ui_shell_queue(app, "view.layerMask", off);
        }
        if (kind == PC_UI_THUMB_PIXEL_MASK && (mods & PC_UI_MOD_ALT)) {
            command = "view.layerMask";
            yyjson_mut_obj_add_uint(d, p, "layer", layer->id);
            yyjson_mut_obj_add_str(d, p, "mode", (mods & PC_UI_MOD_SHIFT) ? "toggleOverlay" : "toggleGray");
        } else if (mask && (mods & PC_UI_MOD_SHIFT)) {
            command = kind == PC_UI_THUMB_PIXEL_MASK ? "layer.layerMask.enabled" : "layer.vectorMask.enabled";
            yyjson_mut_obj_add_uint(d, p, "layer", layer->id);
        } else {
            yyjson_mut_obj_add_str(d, p, "channel", channel);
            pc_ui_state *ui = pc_ui_shell_app_state(app);
            ui->mask_target = kind == PC_UI_THUMB_PIXEL_MASK;
            ui->vector_mask_target = kind == PC_UI_THUMB_VECTOR_MASK;
        }
    }
    pc_ui_shell_queue(app, command, p);
    yyjson_mut_doc_free(d);
}
void pc_ui_layer_thumb_widget(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime,
                              const pc_engine_doc_state *state, const pc_layer *layer,
                              pc_ui_layer_thumbnail_kind kind)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_rect rect = nk_widget_bounds(ctx), clip = ctx->current->layout->clip;
    if (rect.y >= clip.y + clip.h || rect.y + rect.h <= clip.y || rect.x >= clip.x + clip.w ||
        rect.x + rect.w <= clip.x) {
        nk_spacing(ctx, 1);
        return;
    }
    const char *id = kind == PC_UI_THUMB_PIXEL_MASK    ? "layer.mask.thumbnail"
                     : kind == PC_UI_THUMB_VECTOR_MASK ? "layer.vectorMask.thumbnail"
                                                       : "layer.thumbnail";
    pc_ui_shell_panel_hit(app, runtime, id, layer->id);
    struct nk_image image;
    bool loaded = bitmap(app, state, layer, kind, &image);
    bool pressed = loaded ? nk_button_image(ctx, image) : nk_button_label(ctx, pc_i18n_t(""));
    if (pressed)
        clicked(app, layer, kind);
    bool active = state->has_active_layer && state->active_layer == layer->id;
    const pc_ui_state *ui = pc_ui_shell_app_state(app);
    bool target = active && (kind == PC_UI_THUMB_PIXEL_MASK
                                 ? (ui->mask_target || pc_engine_targeted_mask(pc_ui_shell_app_session(app)))
                             : kind == PC_UI_THUMB_VECTOR_MASK
                                 ? ui->vector_mask_target
                                 : (!ui->mask_target && !ui->vector_mask_target &&
                                    !pc_engine_targeted_mask(pc_ui_shell_app_session(app))));
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    if (target)
        nk_stroke_rect(canvas, rect, 0, 2, pc_ui_nk_color(tokens->accent));
    bool enabled = kind == PC_UI_THUMB_PIXEL_MASK    ? layer->mask.enabled
                   : kind == PC_UI_THUMB_VECTOR_MASK ? layer->vector_mask.enabled
                                                     : true;
    if (!enabled) {
        nk_stroke_line(canvas, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, 2,
                       pc_ui_nk_color(tokens->danger));
        nk_stroke_line(canvas, rect.x + rect.w, rect.y, rect.x, rect.y + rect.h, 2,
                       pc_ui_nk_color(tokens->danger));
    }
}
void pc_ui_layer_mask_chain(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const pc_layer *layer,
                            bool vector)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const char *command = vector ? "layer.vectorMask.linked" : "layer.layerMask.linked";
    pc_ui_shell_panel_hit(app, runtime, command, layer->id);
    bool linked = vector ? layer->vector_mask.linked : layer->mask.linked;
    if (nk_button_label(ctx, linked ? "∞" : "·")) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        if (p && yyjson_mut_obj_add_uint(d, p, "layer", layer->id)) {
            select_layer(app, d, layer->id);
            pc_ui_shell_queue(app, command, p);
        }
        yyjson_mut_doc_free(d);
    }
}
