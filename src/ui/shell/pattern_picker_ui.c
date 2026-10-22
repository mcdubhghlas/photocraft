#include "pattern_picker_ui.h"
#include "shell_internal.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "ui/preset_thumb.h"
#include "engine/pattern_cmds.h"
#include "base/json.h"
#include "raster/pixel.h"
#include <blake3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Most patterns listed in the picker dropdown (hostile-library guard; real libraries are tiny). */
#define PC_PATTERN_PICKER_MAX 512

/* Resolve a pattern's thumbnail into a GL image (cached by id + size), rendering it through the
 * engine on a cache miss. false (no image) for an unknown/empty pattern or on any failure. */
static bool pattern_image(pc_ui_shell_app *app, const char *id, uint32_t px, struct nk_image *out)
{
    if (!id || !*id || px == 0)
        return false;
    blake3_hasher h;
    uint8_t key[32];
    blake3_hasher_init(&h);
    const char category[] = "pattern-thumb-v1";
    blake3_hasher_update(&h, category, sizeof category);
    blake3_hasher_update(&h, &px, sizeof px);
    blake3_hasher_update(&h, id, strlen(id));
    blake3_hasher_finalize(&h, key, sizeof key);
    if (pc_ui_shell_bitmap(app, key, (uint16_t)px, (uint16_t)px, NULL, out, NULL) == PC_OK)
        return true;
    pc_rgba8_image img;
    memset(&img, 0, sizeof img);
    bool found = false;
    pc_error err = {0};
    pc_status status = pc_engine_pattern_thumbnail(pc_ui_shell_app_session(app), id, strlen(id), px, &img,
                                                   &found, &err);
    bool ok = status == PC_OK && found && img.pixels &&
              pc_ui_shell_bitmap(app, key, (uint16_t)img.width, (uint16_t)img.height, img.pixels, out, NULL) ==
                  PC_OK;
    pc_rgba8_image_fini(&img);
    return ok;
}

void pc_ui_shell_pattern_picker(pc_ui_shell_app *app, pc_ui_dialog *dialog, yyjson_mut_val *params,
                                const char *key, const char *current, const char *mapping_prefix)
{
    if (!app || !dialog || !params || !key)
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    const char *prefix = mapping_prefix ? mapping_prefix : "pattern";
    uint32_t px = pc_ui_preset_thumb_px(40, (double)pc_ui_shell_app_ui_scale(app));
    const char *shown = current && *current ? current : NULL;

    nk_layout_row_begin(ctx, NK_STATIC, 44, 2);
    nk_layout_row_push(ctx, 44);
    struct nk_rect tb;
    enum nk_widget_layout_states st = nk_widget(&tb, ctx);
    if (st != NK_WIDGET_INVALID) {
        float side = fminf(tb.w, tb.h);
        struct nk_rect box = nk_rect(tb.x, tb.y, side, side);
        struct nk_image cur;
        if (shown && pattern_image(app, shown, px, &cur))
            nk_draw_image(canvas, box, &cur, nk_rgba(255, 255, 255, 255));
        else {
            nk_fill_rect(canvas, box, 2, pc_ui_nk_color(tokens->field));
            nk_stroke_rect(canvas, box, 2, 1, pc_ui_nk_color(tokens->field_border));
        }
    }
    nk_layout_row_push(ctx, 190);
    char mapping[160];
    (void)snprintf(mapping, sizeof mapping, "%s.pattern", prefix);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    if (nk_combo_begin_label(ctx, shown ? shown : pc_i18n_t("Choose pattern"), nk_vec2(320, 360))) {
        pc_ui_shell_dialog_runtime(app)->graph_blocked = true;
        pc_json_value list = PC_JSON_VALUE_INIT;
        pc_error err = {0};
        if (pc_engine_session_execute_json(pc_ui_shell_app_session(app), "pattern.list", "{}", &list, &err) ==
            PC_OK) {
            yyjson_mut_val *arr = pc_json_value_root(&list);
            size_t n = yyjson_mut_arr_size(arr);
            if (n > PC_PATTERN_PICKER_MAX)
                n = PC_PATTERN_PICKER_MAX;
            for (size_t i = 0; i < n; i++) {
                yyjson_mut_val *entry = yyjson_mut_arr_get(arr, i);
                const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "id"));
                const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "name"));
                if (!id)
                    continue;
                struct nk_image thumb;
                bool has = pattern_image(app, id, px, &thumb);
                nk_layout_row_dynamic(ctx, 40, 1);
                char item[192];
                (void)snprintf(item, sizeof item, "%s.item.%zu", prefix, i);
                pc_ui_shell_widget_mapping(app, item, nk_widget_bounds(ctx));
                nk_bool clicked = has ? nk_button_image_label(ctx, thumb, name ? name : id, NK_TEXT_LEFT)
                                      : nk_button_label(ctx, name ? name : id);
                if (clicked) {
                    yyjson_mut_val *k = yyjson_mut_strcpy(dialog->fields, key),
                                   *v = yyjson_mut_strcpy(dialog->fields, id);
                    if (k && v)
                        (void)yyjson_mut_obj_put(params, k, v);
                    nk_combo_close(ctx);
                }
            }
        }
        pc_json_value_fini(&list);
        nk_combo_end(ctx);
    }
    nk_layout_row_end(ctx);
}
