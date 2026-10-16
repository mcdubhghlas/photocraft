/* Keyboard navigation state machine for the menu bar (see menu_nav.h). Port of the input-free
 * decisions of crates/ui-egui/src/menu_nav.rs (Nav::step, Nav::keys, the `heading` test). */
#include "ui/menu_nav.h"

/* How far above and below an open submenu the pointer may aim and still be heading for it. */
#define PC_UI_MENU_AIM_SLACK 24.0f

void pc_ui_menu_nav_reset(pc_ui_menu_nav *nav)
{
    nav->top = -1;
    nav->level = 0;
    for (int i = 0; i < PC_UI_MENU_MAX_LEVELS; i++) {
        nav->hi[i] = -1;
    }
}

void pc_ui_menu_nav_open(pc_ui_menu_nav *nav, int top)
{
    pc_ui_menu_nav_reset(nav);
    nav->top = top;
}

bool pc_ui_menu_nav_is_open(const pc_ui_menu_nav *nav)
{
    return nav->top >= 0;
}

int pc_ui_menu_nav_highlighted(const pc_ui_menu_nav *nav, int level)
{
    if (level < 0 || level >= PC_UI_MENU_MAX_LEVELS) {
        return -1;
    }
    return nav->hi[level];
}

static void set_hi(pc_ui_menu_nav *nav, int level, int row)
{
    if (level >= 0 && level < PC_UI_MENU_MAX_LEVELS) {
        nav->hi[level] = row;
    }
}

/* Drop the highlight of every level deeper than `level` (Rust's hi.truncate(level+1)). */
static void truncate_below(pc_ui_menu_nav *nav, int level)
{
    for (int l = level + 1; l < PC_UI_MENU_MAX_LEVELS; l++) {
        nav->hi[l] = -1;
    }
}

/* ↑ / ↓: the next enabled row (wrapping), or the first / last one (Nav::step). */
static void step(pc_ui_menu_nav *nav, int level, const pc_ui_menu_row *rows, size_t n_rows, bool down)
{
    int first = -1, last = -1;
    for (size_t i = 0; i < n_rows; i++) {
        if (rows[i].enabled) {
            if (first < 0) {
                first = (int)i;
            }
            last = (int)i;
        }
    }
    if (first < 0) {
        return; /* no enabled rows */
    }
    int cur = pc_ui_menu_nav_highlighted(nav, level);
    int next;
    if (cur < 0) {
        next = down ? first : last;
    } else if (down) {
        next = first;
        for (size_t i = 0; i < n_rows; i++) {
            if (rows[i].enabled && (int)i > cur) {
                next = (int)i;
                break;
            }
        }
    } else {
        next = last;
        for (size_t i = n_rows; i-- > 0;) {
            if (rows[i].enabled && (int)i < cur) {
                next = (int)i;
                break;
            }
        }
    }
    set_hi(nav, level, next);
    truncate_below(nav, level);
    nav->level = level;
}

pc_ui_menu_nav_action pc_ui_menu_nav_key(pc_ui_menu_nav *nav, pc_ui_key key, const pc_ui_menu_row *rows,
                                         size_t n_rows, size_t n_tops, int *row)
{
    if (row) {
        *row = -1;
    }
    if (nav->top < 0) {
        return PC_UI_MENU_NAV_NONE;
    }
    int level = nav->level;
    if (level < 0) {
        level = 0;
    }
    if (level >= PC_UI_MENU_MAX_LEVELS) {
        level = PC_UI_MENU_MAX_LEVELS - 1;
    }
    nav->level = level;
    int cur = pc_ui_menu_nav_highlighted(nav, level);
    bool cur_ok = cur >= 0 && (size_t)cur < n_rows;

    switch (key) {
    case PC_KEY_ESCAPE:
        return PC_UI_MENU_NAV_CLOSE;
    case PC_KEY_ARROWDOWN:
        step(nav, level, rows, n_rows, true);
        return PC_UI_MENU_NAV_NONE;
    case PC_KEY_ARROWUP:
        step(nav, level, rows, n_rows, false);
        return PC_UI_MENU_NAV_NONE;
    case PC_KEY_ARROWRIGHT:
        if (cur_ok && rows[cur].enabled && rows[cur].submenu) {
            nav->level = level + 1 < PC_UI_MENU_MAX_LEVELS ? level + 1 : level;
            truncate_below(nav, level);
            if (row) {
                *row = cur;
            }
            return PC_UI_MENU_NAV_OPEN_SUB;
        }
        if (n_tops > 0 && row) {
            *row = (int)(((size_t)nav->top + 1) % n_tops);
        }
        return PC_UI_MENU_NAV_SWITCH;
    case PC_KEY_ARROWLEFT:
        if (level > 0) {
            truncate_below(nav, level - 1);
            nav->level = level - 1;
            return PC_UI_MENU_NAV_CLOSE_SUB;
        }
        if (n_tops > 0 && row) {
            *row = (int)(((size_t)nav->top + n_tops - 1) % n_tops);
        }
        return PC_UI_MENU_NAV_SWITCH;
    case PC_KEY_ENTER:
    case PC_KEY_SPACE:
        if (cur_ok && rows[cur].enabled) {
            if (rows[cur].submenu) {
                nav->level = level + 1 < PC_UI_MENU_MAX_LEVELS ? level + 1 : level;
                truncate_below(nav, level);
                if (row) {
                    *row = cur;
                }
                return PC_UI_MENU_NAV_OPEN_SUB;
            }
            if (row) {
                *row = cur;
            }
            return PC_UI_MENU_NAV_ACTIVATE;
        }
        return PC_UI_MENU_NAV_NONE;
    default:
        return PC_UI_MENU_NAV_NONE;
    }
}

/* menu_nav::heading: p is inside the triangle (from, a, b) where a,b are the submenu's near edge
 * extended by AIM_SLACK. Same-sign test on the three edge cross products (a point on an edge counts
 * as inside). */
bool pc_ui_menu_heading(float from_x, float from_y, float px, float py, float tx, float ty, float tw,
                        float th)
{
    if (!(tw > 0.0f) || !(th > 0.0f)) {
        return false;
    }
    float left = tx, right = tx + tw;
    float center_x = tx + tw * 0.5f;
    float x = center_x >= from_x ? left : right;
    float ax = x, ay = ty - PC_UI_MENU_AIM_SLACK;
    float bx = x, by = ty + th + PC_UI_MENU_AIM_SLACK;
    /* side(u, v) = cross product of (v-u) and (p-u). */
    float d1 = (ax - from_x) * (py - from_y) - (ay - from_y) * (px - from_x);
    float d2 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    float d3 = (from_x - bx) * (py - by) - (from_y - by) * (px - bx);
    bool neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
    bool pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
    return !(neg && pos);
}
