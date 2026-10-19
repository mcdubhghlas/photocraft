#include "docking.h"
#include "engine/prefs.h"
#include "ui/widgets.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static bool inside(struct nk_rect r, float x, float y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
void pc_ui_docking_begin(pc_ui_docking *r)
{
    r->ntabs = 0;
    r->count = 0;
}
void pc_ui_docking_tab(pc_ui_docking *r, pc_ui_group group, size_t tab, struct nk_rect rect)
{
    if (r->ntabs < 24) {
        r->tabs[r->ntabs++] = (pc_ui_dock_tab_hit){group, tab, rect};
    }
}
void pc_ui_docking_group(pc_ui_docking *r, pc_ui_group group, struct nk_rect rect, float height)
{
    if (r->count < PC_UI_GROUP_N) {
        size_t i = r->count++;
        r->groups[i] = group;
        r->rects[i] = rect;
        r->heights[i] = height;
    }
}
static void restore(pc_ui_state *ui, pc_ui_docking *r)
{
    ui->dock_width = r->width;
    memcpy(ui->dock.has_height, r->has_height, sizeof r->has_height);
    memcpy(ui->dock.heights, r->stored_height, sizeof r->stored_height);
}
bool pc_ui_docking_cancel(pc_ui_shell_app *app, pc_ui_docking *r, const SDL_Event *e)
{
    if (!r->drag) {
        return false;
    }
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST ||
        (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE)) {
        restore(pc_ui_shell_app_state(app), r);
        r->drag = 0;
        return e->type == SDL_EVENT_KEY_DOWN;
    }
    return false;
}
bool pc_ui_docking_capture(pc_ui_shell_app *app, const pc_ui_docking *r, const SDL_Event *e)
{
    if (r->drag) {
        return true;
    }
    if (e->type != SDL_EVENT_MOUSE_BUTTON_DOWN || e->button.button != SDL_BUTTON_LEFT) {
        return false;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float x = ctx->input.mouse.pos.x, y = ctx->input.mouse.pos.y;
    if (inside(r->width_grip, x, y)) {
        return true;
    }
    for (size_t i = 0; i + 1 < r->count; i++) {
        struct nk_rect grip = r->rects[i];
        grip.y += grip.h;
        grip.h = PC_UI_DOCK_GAP;
        if (inside(grip, x, y)) {
            return true;
        }
    }
    return false;
}
void pc_ui_docking_draw(pc_ui_shell_app *app, pc_ui_docking *r, struct nk_rect width_grip)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    r->width_grip = width_grip;
    bool locked = false;
    pc_json_value value = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "workspaceLocked", &value, NULL) == PC_OK) {
        locked = yyjson_mut_get_bool(pc_json_value_root(&value));
    }
    pc_json_value_fini(&value);
    if (locked || ui->n_dialogs || pc_ui_menu_nav_is_open(pc_ui_shell_app_nav(app))) {
        if (r->drag) {
            restore(ui, r);
            r->drag = 0;
        }
        return;
    }
    float x = ctx->input.mouse.pos.x, y = ctx->input.mouse.pos.y;
    bool press = nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) != 0;
    if (!r->drag && press) {
        if (inside(width_grip, x, y)) {
            r->drag = 1;
        }
        for (size_t i = 0; !r->drag && i + 1 < r->count; i++) {
            struct nk_rect grip = r->rects[i];
            grip.y += grip.h;
            grip.h = PC_UI_DOCK_GAP;
            if (inside(grip, x, y) && !pc_ui_dock_is_collapsed(&ui->dock, r->groups[i])) {
                r->drag = 2;
                r->index = i;
            }
        }
        for (size_t i = 0; !r->drag && i < r->ntabs; i++) {
            if (inside(r->tabs[i].rect, x, y)) {
                r->drag = 3;
                r->group = r->tabs[i].group;
                r->tab = r->tabs[i].tab;
            }
        }
        if (r->drag) {
            r->origin[0] = x;
            r->origin[1] = y;
            r->width = ui->dock_width;
            memcpy(r->has_height, ui->dock.has_height, sizeof r->has_height);
            memcpy(r->stored_height, ui->dock.heights, sizeof r->stored_height);
            r->frozen_count = r->count;
            memcpy(r->frozen_groups, r->groups, r->count * sizeof r->groups[0]);
            memcpy(r->frozen_heights, r->heights, r->count * sizeof r->heights[0]);
        }
    }
    if (r->drag == 1) {
        ui->dock_width = fmaxf(250, fminf(520, r->width + r->origin[0] - x));
    } else if (r->drag == 2) {
        if (r->count != r->frozen_count ||
            memcmp(r->groups, r->frozen_groups, r->count * sizeof r->groups[0])) {
            restore(ui, r);
            r->drag = 0;
        } else {
            memcpy(ui->dock.has_height, r->has_height, sizeof r->has_height);
            memcpy(ui->dock.heights, r->stored_height, sizeof r->stored_height);
            pc_ui_dock_resize(&ui->dock, r->frozen_groups, r->frozen_heights, r->frozen_count, r->index,
                              y - r->origin[1]);
        }
    } else if (r->drag == 3 && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
        float dx = x - r->origin[0], dy = y - r->origin[1];
        if (dx * dx + dy * dy >= 64) {
            bool tab_moved = false;
            for (size_t i = 0; i < r->ntabs; i++) {
                if (r->tabs[i].group == r->group && inside(r->tabs[i].rect, x, y)) {
                    size_t before = r->tabs[i].tab;
                    pc_ui_dock_move_tab(&ui->dock, r->group, r->tab, &before,
                                        pc_ui_theme_kind_is_pro(ui->theme));
                    tab_moved = true;
                    break;
                }
            }
            if (!tab_moved && fabsf(dy) >= 8) {
                pc_ui_group order[PC_UI_GROUP_N], before;
                pc_ui_dock_order(&ui->dock, order);
                float tops[PC_UI_GROUP_N], bottoms[PC_UI_GROUP_N];
                for (size_t i = 0; i < r->count; i++) {
                    tops[i] = r->rects[i].y;
                    bottoms[i] = tops[i] + r->rects[i].h;
                }
                bool found = pc_ui_dock_drop_before(order, PC_UI_GROUP_N, r->groups, tops, bottoms, r->count,
                                                    r->group, y, &before);
                pc_ui_dock_move_group(&ui->dock, r->group, found ? &before : NULL);
            }
        }
        r->drag = 0;
    }
    if (r->drag && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
        r->drag = 0;
    }
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    if (width_grip.w > 0) {
        pc_ui_shell_widget_mapping(app, "dock.width", width_grip);
        if (r->drag == 1 || inside(width_grip, x, y)) {
            nk_fill_rect(canvas, width_grip, 0, pc_ui_nk_color(tokens->accent_border));
        }
    }
    for (size_t i = 0; i + 1 < r->count; i++) {
        struct nk_rect grip = r->rects[i];
        grip.y += grip.h;
        grip.h = PC_UI_DOCK_GAP;
        char id[64];
        (void)snprintf(id, sizeof id, "dock.split.%s", pc_ui_group_key(r->groups[i]));
        pc_ui_shell_widget_mapping(app, id, grip);
        if ((r->drag == 2 && r->index == i) || inside(grip, x, y)) {
            nk_fill_rect(canvas, grip, 0, pc_ui_nk_color(tokens->accent_border));
        }
    }
}
