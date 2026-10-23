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
/* Reveal a dock group (its per-group visibility flag) after a panel is re-parented into it. */
static void show_group(pc_ui_state *ui, pc_ui_group g)
{
    switch (g) {
    case PC_UI_GROUP_COLOR:
        ui->panels.color = true;
        break;
    case PC_UI_GROUP_PROPERTIES:
        ui->panels.properties = true;
        break;
    case PC_UI_GROUP_CHARACTER:
        ui->panels.character = true;
        break;
    case PC_UI_GROUP_NAVIGATOR:
        ui->panels.navigator = true;
        break;
    case PC_UI_GROUP_HISTORY:
        ui->panels.history = true;
        break;
    case PC_UI_GROUP_LAYERS:
        ui->panels.layers = true;
        break;
    default:
        break;
    }
}
void pc_ui_docking_begin(pc_ui_docking *r)
{
    r->ntabs = 0;
    r->count = 0;
}
void pc_ui_docking_peek(pc_ui_docking *r, pc_ui_group group)
{
    r->has_peek = true;
    r->peek_group = group;
}
void pc_ui_docking_tab(pc_ui_docking *r, pc_ui_group group, size_t tab, const char *label,
                       struct nk_rect rect)
{
    if (r->ntabs < 24) {
        r->tabs[r->ntabs++] = (pc_ui_dock_tab_hit){group, tab, label, rect};
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
    /* Auto-Collapse Iconic Panels: a peeked group re-collapses on the first press outside it. The
     * tab click that opened the peek is inside the group, so the peek survives its own press. */
    bool auto_collapse = false;
    pc_json_value ac = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "autoCollapseIconPanels", &ac, NULL) == PC_OK) {
        auto_collapse = yyjson_mut_get_bool(pc_json_value_root(&ac));
    }
    pc_json_value_fini(&ac);
    if (!auto_collapse) {
        r->has_peek = false;
    } else if (r->has_peek) {
        if (pc_ui_dock_is_collapsed(&ui->dock, r->peek_group)) {
            r->has_peek = false; /* collapsed by other means */
        } else if (press) {
            bool outside = true;
            for (size_t i = 0; i < r->count; i++) {
                if (r->groups[i] == r->peek_group && inside(r->rects[i], x, y)) {
                    outside = false;
                    break;
                }
            }
            if (outside) {
                pc_ui_dock_set_collapsed(&ui->dock, r->peek_group, true);
                r->has_peek = false;
            }
        }
    }
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
                r->drag_label = r->tabs[i].label;
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
            bool pro = pc_ui_theme_kind_is_pro(ui->theme);
            bool tab_moved = false;
            /* Dropped on a tab: a same-group drop reorders; a drop onto another group's tab
             * re-parents the dragged panel just before that tab (cross-group move). */
            for (size_t i = 0; i < r->ntabs; i++) {
                if (!inside(r->tabs[i].rect, x, y)) {
                    continue;
                }
                if (r->tabs[i].group == r->group) {
                    if (ui->dock.customized) {
                        pc_ui_dock_move_panel(&ui->dock, r->drag_label, r->group, r->tabs[i].label, pro);
                    } else {
                        size_t before = r->tabs[i].tab;
                        pc_ui_dock_move_tab(&ui->dock, r->group, r->tab, &before, pro);
                    }
                } else {
                    pc_ui_dock_unfloat(&ui->dock, r->group);
                    pc_ui_dock_move_panel(&ui->dock, r->drag_label, r->tabs[i].group, r->tabs[i].label,
                                          pro);
                    show_group(ui, r->tabs[i].group);
                }
                tab_moved = true;
                break;
            }
            /* Dropped onto another group's strip/body (not a specific tab): append to that group. */
            if (!tab_moved) {
                for (size_t i = 0; i < r->count; i++) {
                    if (r->groups[i] != r->group && inside(r->rects[i], x, y)) {
                        pc_ui_dock_unfloat(&ui->dock, r->group);
                        pc_ui_dock_move_panel(&ui->dock, r->drag_label, r->groups[i], NULL, pro);
                        show_group(ui, r->groups[i]);
                        tab_moved = true;
                        break;
                    }
                }
            }
            /* Dragging a tab out of the dock column (left past a margin) detaches its whole group
             * into a floating window at the drop point. (The reference detaches a single panel via
             * the external craft_ui tree; with fixed group membership the group floats as a unit.) */
            if (!tab_moved && r->count > 0) {
                float dock_left = r->rects[0].x;
                for (size_t i = 1; i < r->count; i++) {
                    if (r->rects[i].x < dock_left) {
                        dock_left = r->rects[i].x;
                    }
                }
                if (x < dock_left - 24.0f) {
                    float gh = pc_ui_group_default_height(r->group) + 56.0f;
                    for (size_t i = 0; i < r->count; i++) {
                        if (r->groups[i] == r->group) {
                            gh = r->rects[i].h;
                            break;
                        }
                    }
                    float fw = PC_UI_DOCK_FLOAT_DEFAULT_W;
                    float rect[4] = {x - fw * 0.5f, y - 14.0f, fw, gh};
                    if (rect[0] < 8.0f) {
                        rect[0] = 8.0f;
                    }
                    if (rect[1] < 8.0f) {
                        rect[1] = 8.0f;
                    }
                    pc_ui_dock_float(&ui->dock, r->group, rect);
                    r->float_shown[r->group] = false; /* place it at `rect` next frame */
                    tab_moved = true;
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
    /* Re-parent drop indicator: while a tab is held and dragged, show where it would land — an
     * insertion bar before the hovered tab, or a frame around a target group's strip. */
    if (r->drag == 3 && nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
        float ddx = x - r->origin[0], ddy = y - r->origin[1];
        if (ddx * ddx + ddy * ddy >= 64) {
            bool shown = false;
            for (size_t i = 0; i < r->ntabs; i++) {
                if (inside(r->tabs[i].rect, x, y)) {
                    struct nk_rect t = r->tabs[i].rect;
                    bool cross = r->tabs[i].group != r->group;
                    struct nk_rect bar = nk_rect(t.x - 1.0f, t.y, 2.5f, t.h);
                    nk_fill_rect(canvas, bar, 0, pc_ui_nk_color(tokens->accent_border));
                    if (cross) {
                        for (size_t k = 0; k < r->count; k++) {
                            if (r->groups[k] == r->tabs[i].group) {
                                nk_stroke_rect(canvas, r->rects[k], 0, 2.0f,
                                               pc_ui_nk_color(tokens->accent_border));
                                break;
                            }
                        }
                    }
                    shown = true;
                    break;
                }
            }
            if (!shown) {
                for (size_t i = 0; i < r->count; i++) {
                    if (r->groups[i] != r->group && inside(r->rects[i], x, y)) {
                        nk_stroke_rect(canvas, r->rects[i], 0, 2.0f,
                                       pc_ui_nk_color(tokens->accent_border));
                        break;
                    }
                }
            }
        }
    }
}
