/* Keyboard navigation of the menu bar's menus (port of the input-free logic of
 * crates/ui-egui/src/menu_nav.rs). The reference is woven into egui's menu/popup internals
 * (MenuState, SubMenu, ScrollArea); the portable part is the keyboard state machine that drives an
 * open menu (↑/↓ move the highlight, → opens a submenu or the next menu, ← closes it or opens the
 * previous menu, ↩/Space run the highlighted item, Esc closes) and the "pointer heading for an open
 * submenu" geometry test (#1218). The egui-specific scroll-arrow rendering and the per-frame row
 * collection are reimplemented against Nuklear's own menu widgets by the shell; this layer holds the
 * state and the decisions so they stay unit-testable with no GUI dependency. */
#ifndef PC_UI_MENU_NAV_H
#define PC_UI_MENU_NAV_H

#include <stdbool.h>
#include <stddef.h>

#include "ui/shortcuts.h"

/* Menu nesting is at most three deep in Photoshop's tree (Layer › Smart Objects › Stack Mode). */
enum { PC_UI_MENU_MAX_LEVELS = 4 };

/* Keyboard / highlight state of the open menu bar. */
typedef struct pc_ui_menu_nav {
    int top;                        /* open top-level menu index, or -1 for none */
    int level;                      /* the level the arrows move in (0 = the top menu's list) */
    int hi[PC_UI_MENU_MAX_LEVELS];  /* highlighted row per level, -1 = none */
} pc_ui_menu_nav;

/* One actionable row of the active level, as the caller sees it this frame. */
typedef struct pc_ui_menu_row {
    bool enabled;
    bool submenu; /* opens a submenu rather than running a command */
} pc_ui_menu_row;

/* What a navigation key asks the caller to do. */
typedef enum pc_ui_menu_nav_action {
    PC_UI_MENU_NAV_NONE = 0,  /* highlight changed or key ignored; nothing to do */
    PC_UI_MENU_NAV_ACTIVATE,  /* run the row *row of the active level */
    PC_UI_MENU_NAV_OPEN_SUB,  /* open the submenu of row *row */
    PC_UI_MENU_NAV_CLOSE_SUB, /* step back to the parent level (close the current submenu) */
    PC_UI_MENU_NAV_SWITCH,    /* open top-level menu index *row instead */
    PC_UI_MENU_NAV_CLOSE      /* close all menus (Escape) */
} pc_ui_menu_nav_action;

/* Reset to "nothing open". */
void pc_ui_menu_nav_reset(pc_ui_menu_nav *nav);
/* Open top-level menu `top` (resets the per-level highlight). */
void pc_ui_menu_nav_open(pc_ui_menu_nav *nav, int top);
/* Is a top menu open? (Shortcuts leave the keys to it meanwhile.) */
bool pc_ui_menu_nav_is_open(const pc_ui_menu_nav *nav);
/* The highlighted row of a level, or -1. */
int pc_ui_menu_nav_highlighted(const pc_ui_menu_nav *nav, int level);

/* Handle one navigation key over the active level's rows (`rows`, `n_rows`), with `n_tops` top
 * menus for ←/→ switching. Returns the action and, for ACTIVATE/OPEN_SUB, the highlighted row in
 * *row; for SWITCH, the new top index in *row. The nav's highlight/level are updated in place.
 * Unhandled keys return NONE. Never reads out of bounds for any inputs. */
pc_ui_menu_nav_action pc_ui_menu_nav_key(pc_ui_menu_nav *nav, pc_ui_key key, const pc_ui_menu_row *rows,
                                         size_t n_rows, size_t n_tops, int *row);

/* The "pointer heading for an open submenu" test (menu_nav::heading, #1218): from the point it left
 * the submenu's row (`from`), is the pointer at `p` still inside the triangle aiming at the near
 * edge of the submenu rectangle (`tx,ty,tw,th`), with AIM_SLACK above and below? Pure geometry,
 * unit-tested. A degenerate (non-positive) target is never headed for. */
bool pc_ui_menu_heading(float from_x, float from_y, float px, float py, float tx, float ty, float tw,
                        float th);

#endif /* PC_UI_MENU_NAV_H */
