#include "native_menus.h"
#include "ui/widgets.h"
#include "ui/i18n.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static const char *display_label(const pc_ui_menu_item *item, const pc_ui_tree_row *row)
{
    bool authored = !row->submenu && (!strncmp(item->id, "window.document.", 16) ||
                                      !strncmp(item->id, "file.openRecent.", 16) ||
                                      !strncmp(item->id, "window.workspace.saved.", 23));
    return authored ? row->label : pc_i18n_t(row->label);
}
static bool contains(const float r[4], float x, float y)
{
    return x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
}
static void close_menu(pc_ui_native_menus *m)
{
    pc_ui_menu_nav_reset(&m->nav);
    m->levels = 0;
}
static void open_top(pc_ui_native_menus *m, int top)
{
    pc_ui_menu_nav_open(&m->nav, top);
    m->levels = 1;
    memset(m->prefix, 0, sizeof m->prefix);
}
static size_t rows_for(const pc_ui_native_menus *m, const pc_ui_menu_item *items, size_t n, size_t level,
                       pc_ui_tree_row rows[PC_UI_MENU_ROWS])
{
    if (m->nav.top < 0 || m->nav.top >= 10 || level > 2) {
        return 0;
    }
    const char *path[] = {PC_UI_TOP_MENUS[m->nav.top], m->prefix[0], m->prefix[1]};
    size_t count = 0;
    (void)pc_ui_menu_tree_rows(items, n, path, level + 1, rows, PC_UI_MENU_ROWS, &count, NULL);
    return count;
}
static void open_sub(pc_ui_native_menus *m, size_t level, const pc_ui_tree_row *row, float anchor)
{
    if (level > 1 || strlen(row->label) >= sizeof m->prefix[0]) {
        return;
    }
    (void)snprintf(m->prefix[level], sizeof m->prefix[level], "%s", row->label);
    m->levels = level + 2;
    m->anchor_y[level + 1] = anchor;
    m->nav.hi[level + 1] = -1;
}
void pc_ui_native_menu_bar(pc_ui_shell_app *app, pc_ui_native_menus *m)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    nk_layout_row_begin(c, NK_STATIC, 24, 10);
    for (int t = 0; t < 10; t++) {
        const char *label = pc_i18n_t(PC_UI_TOP_MENUS[t]);
        float width =
            c->style.font->width(c->style.font->userdata, c->style.font->height, label, (int)strlen(label)) +
            20;
        nk_layout_row_push(c, width);
        struct nk_rect r = nk_widget_bounds(c);
        m->top_rect[t][0] = r.x;
        m->top_rect[t][1] = r.y;
        m->top_rect[t][2] = r.w;
        m->top_rect[t][3] = r.h;
        char id[64];
        (void)snprintf(id, sizeof id, "menu.%s", PC_UI_TOP_MENUS[t]);
        pc_ui_shell_widget_mapping(app, id, r);
        struct nk_style_item normal = c->style.button.normal;
        c->style.button.normal =
            nk_style_item_color(pc_ui_nk_color(m->nav.top == t ? tokens->accent_soft : tokens->chrome));
        if (nk_button_label(c, label)) {
            if (m->nav.top == t) {
                close_menu(m);
            } else {
                pc_ui_shell_modal_changed(app);
                open_top(m, t);
            }
        } else if (m->nav.top >= 0 && m->nav.top != t && nk_input_is_mouse_hovering_rect(&c->input, r) &&
                   (c->input.mouse.delta.x != 0 || c->input.mouse.delta.y != 0)) {
            open_top(m, t);
        }
        c->style.button.normal = normal;
    }
    nk_layout_row_end(c);
}
static void activate(pc_ui_shell_app *app, pc_ui_native_menus *m, const pc_ui_menu_item *item)
{
    pc_ui_shell_queue(app, item->id, NULL);
    close_menu(m);
}
void pc_ui_native_menu_popups(pc_ui_shell_app *app, pc_ui_native_menus *m, const pc_ui_menu_item *items,
                              size_t count, float width, float height)
{
    if (m->nav.top < 0 || m->nav.top >= 10 || !m->levels) {
        return;
    }
    struct nk_context *c = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    for (size_t level = 0; level < m->levels && level < 3; level++) {
        pc_ui_tree_row rows[PC_UI_MENU_ROWS];
        size_t n = rows_for(m, items, count, level, rows);
        float w = 320.f;
        for (size_t i = 0; i < n; i++) {
            const pc_ui_menu_item *item = &items[rows[i].item];
            const char *label = display_label(item, &rows[i]);
            float need = c->style.font->width(c->style.font->userdata, c->style.font->height, label,
                                              (int)strlen(label)) +
                         70;
            if (!rows[i].submenu && item->shortcut) {
                need += c->style.font->width(c->style.font->userdata, c->style.font->height, item->shortcut,
                                             (int)strlen(item->shortcut));
            }
            if (need > w) {
                w = need;
            }
        }
        if (w > width - 8) {
            w = width - 8;
        }
        if (w < 1) {
            w = 1;
        }
        float h = (float)n * 26.f + 12.f;
        if (h > height - 36) {
            h = height - 36;
        }
        if (h < 1) {
            h = 1;
        }
        float x = level ? m->rect[level - 1][0] + m->rect[level - 1][2] - 2 : m->top_rect[m->nav.top][0];
        float y = level ? m->anchor_y[level] : m->top_rect[m->nav.top][1] + m->top_rect[m->nav.top][3];
        if (x + w > width) {
            x = level ? m->rect[level - 1][0] - w + 2 : width - w;
        }
        if (x < 0) {
            x = 0;
        }
        if (y + h > height) {
            y = height - h;
        }
        if (y < 0) {
            y = 0;
        }
        m->rect[level][0] = x;
        m->rect[level][1] = y;
        m->rect[level][2] = w;
        m->rect[level][3] = h;
        char name[40];
        (void)snprintf(name, sizeof name, "menu-popup-%zu", level);
        struct nk_vec2 padding = c->style.window.padding;
        c->style.window.padding = nk_vec2(4, 4);
        nk_window_set_bounds(c, name, nk_rect(x, y, w, h));
        if (nk_begin(c, name, nk_rect(x, y, w, h), NK_WINDOW_BORDER)) {
            int highlighted = m->nav.hi[level];
            if (m->nav.level == (int)level && highlighted >= 0 && (size_t)highlighted < n &&
                !(c->input.mouse.delta.x != 0 || c->input.mouse.delta.y != 0)) {
                nk_uint sx = 0, sy = 0;
                nk_window_get_scroll(c, &sx, &sy);
                float top = (float)highlighted * 26.f;
                if (top < (float)sy) {
                    nk_window_set_scroll(c, sx, (nk_uint)top);
                } else if (top + 26 > (float)sy + h - 12) {
                    nk_window_set_scroll(c, sx, (nk_uint)(top + 38 - h));
                }
            }
            for (size_t i = 0; i < n; i++) {
                const pc_ui_menu_item *item = &items[rows[i].item];
                nk_layout_row_dynamic(c, 22, 1);
                struct nk_rect r = nk_widget_bounds(c);
                if (!rows[i].submenu) {
                    pc_ui_shell_widget_mapping(app, item->id, r);
                } else {
                    char mapping[96];
                    (void)snprintf(mapping, sizeof mapping, "menu.sub.%s", rows[i].label);
                    pc_ui_shell_widget_mapping(app, mapping, r);
                }
                bool hover = nk_input_is_mouse_hovering_rect(&c->input, r);
                bool motion = c->input.mouse.delta.x != 0 || c->input.mouse.delta.y != 0;
                bool aiming =
                    level + 1 < m->levels &&
                    pc_ui_menu_heading(m->last_x, m->last_y, c->input.mouse.pos.x, c->input.mouse.pos.y,
                                       m->rect[level + 1][0], m->rect[level + 1][1], m->rect[level + 1][2],
                                       m->rect[level + 1][3]);
                if (hover && motion && !aiming) {
                    if (m->nav.hi[level] != (int)i) {
                        m->hover_since = SDL_GetTicks();
                    }
                    m->nav.hi[level] = (int)i;
                    m->nav.level = (int)level;
                    if (!rows[i].submenu) {
                        m->levels = level + 1;
                    }
                }
                bool selected = m->nav.hi[level] == (int)i;
                struct nk_style_button style = c->style.button;
                style.normal =
                    nk_style_item_color(pc_ui_nk_color(selected ? tokens->accent_soft : tokens->chrome));
                style.text_alignment = NK_TEXT_LEFT;
                style.padding = nk_vec2(7, 1);
                if (!rows[i].enabled) {
                    style.text_normal = pc_ui_nk_color(tokens->text_faint);
                    style.text_hover = style.text_normal;
                }
                char label[512];
                (void)snprintf(label, sizeof label, "%s%s",
                               !rows[i].submenu && item->checked == 1 ? "* " : "   ",
                               display_label(item, &rows[i]));
                style.border = 0;
                style.rounding = 0;
                if (!strcmp(rows[i].label, "---")) {
                    nk_spacing(c, 1);
                    nk_stroke_line(nk_window_get_canvas(c), r.x + 5, r.y + r.h / 2, r.x + r.w - 5,
                                   r.y + r.h / 2, 1, pc_ui_nk_color(tokens->separator));
                    continue;
                }
                bool clicked = nk_button_label_styled(c, &style, label) != 0;
                const char *tail = rows[i].submenu ? ">" : item->shortcut;
                if (tail) {
                    /* Right-align shortcut without consuming another input widget. */
                    struct nk_command_buffer *cv = nk_window_get_canvas(c);
                    float tw = c->style.font->width(c->style.font->userdata, c->style.font->height, tail,
                                                    (int)strlen(tail));
                    struct nk_rect tr = nk_rect(r.x + r.w - tw - 10, r.y, tw + 2, r.h);
                    nk_draw_text(cv, tr, tail, (int)strlen(tail), c->style.font, style.normal.data.color,
                                 style.text_normal);
                }
                if (rows[i].enabled && rows[i].submenu &&
                    (clicked || (hover && selected && !aiming && SDL_GetTicks() - m->hover_since >= 150))) {
                    if (level + 1 >= m->levels || strcmp(m->prefix[level], rows[i].label)) {
                        open_sub(m, level, &rows[i], r.y);
                    }
                } else if (clicked && rows[i].enabled && !rows[i].submenu) {
                    activate(app, m, item);
                    break;
                }
            }
        }
        nk_end(c);
        c->style.window.padding = padding;
    }
    m->last_x = c->input.mouse.pos.x;
    m->last_y = c->input.mouse.pos.y;
    m->pointer_valid = true;
}
bool pc_ui_native_menu_key(pc_ui_shell_app *app, pc_ui_native_menus *m, const pc_ui_menu_item *items,
                           size_t count, const SDL_Event *event)
{
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        close_menu(m);
        return false;
    }
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.repeat) {
        return false;
    }
    if (event->key.key == SDLK_F10 && !pc_ui_shell_app_state(app)->n_dialogs) {
        if (m->nav.top >= 0) {
            close_menu(m);
        } else {
            pc_ui_shell_modal_changed(app);
            open_top(m, 0);
        }
        return true;
    }
    if ((event->key.mod & SDL_KMOD_ALT) && !pc_ui_shell_app_state(app)->n_dialogs) {
        const SDL_Keycode keys[] = {SDLK_F, SDLK_E, SDLK_I, SDLK_L, SDLK_T,
                                    SDLK_S, SDLK_R, SDLK_V, SDLK_W, SDLK_H};
        for (int i = 0; i < 10; i++) {
            if (event->key.key == keys[i]) {
                pc_ui_shell_modal_changed(app);
                open_top(m, i);
                return true;
            }
        }
    }
    if (m->nav.top < 0) {
        return false;
    }
    size_t level = (size_t)(m->nav.level < 0 ? 0 : m->nav.level);
    if (level > 2) {
        level = 2;
    }
    pc_ui_tree_row rows[PC_UI_MENU_ROWS];
    size_t n = rows_for(m, items, count, level, rows);
    pc_ui_menu_row nav_rows[PC_UI_MENU_ROWS];
    for (size_t i = 0; i < n; i++) {
        nav_rows[i] = (pc_ui_menu_row){rows[i].enabled, rows[i].submenu};
    }
    int row = -1;
    pc_ui_menu_nav_action action =
        pc_ui_menu_nav_key(&m->nav, pc_ui_shell_input_key(event->key.key), nav_rows, n, 10, &row);
    if (action == PC_UI_MENU_NAV_CLOSE) {
        close_menu(m);
    } else if (action == PC_UI_MENU_NAV_SWITCH) {
        open_top(m, row);
    } else if (action == PC_UI_MENU_NAV_CLOSE_SUB) {
        m->levels = (size_t)m->nav.level + 1;
    } else if (row >= 0 && (size_t)row < n) {
        if (action == PC_UI_MENU_NAV_OPEN_SUB) {
            open_sub(m, level, &rows[row], m->rect[level][1] + (float)row * 26);
        } else if (action == PC_UI_MENU_NAV_ACTIVATE) {
            activate(app, m, &items[rows[row].item]);
        }
    }
    return true;
}
bool pc_ui_native_menu_capture(pc_ui_shell_app *app, pc_ui_native_menus *m, const SDL_Event *event)
{
    if (m->nav.top < 0) {
        return false;
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        struct nk_context *ctx = pc_ui_shell_app_nk(app);
        float x = ctx->input.mouse.pos.x, y = ctx->input.mouse.pos.y;
        bool inside = false;
        for (size_t i = 0; i < m->levels && i < 3; i++) {
            inside |= contains(m->rect[i], x, y);
        }
        for (size_t i = 0; i < 10; i++) {
            inside |= contains(m->top_rect[i], x, y);
        }
        if (!inside) {
            close_menu(m);
        }
    }
    return true;
}
