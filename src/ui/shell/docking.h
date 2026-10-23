#ifndef PC_UI_SHELL_DOCKING_H
#define PC_UI_SHELL_DOCKING_H
#include "shell_internal.h"
typedef struct pc_ui_dock_tab_hit {
    pc_ui_group group;
    size_t tab;         /* index into the group's effective label list (native or member order) */
    const char *label;  /* the panel's English label (static string); for cross-group re-parent */
    struct nk_rect rect;
} pc_ui_dock_tab_hit;
typedef struct pc_ui_docking {
    pc_ui_dock_tab_hit tabs[24];
    size_t ntabs;
    pc_ui_group groups[PC_UI_GROUP_N];
    float heights[PC_UI_GROUP_N];
    struct nk_rect rects[PC_UI_GROUP_N];
    size_t count;
    struct nk_rect width_grip;
    int drag; /* 1 width, 2 splitter, 3 tab/group */
    size_t index, tab;
    pc_ui_group group;
    const char *drag_label; /* label of the tab being dragged (for cross-group re-parent) */
    float origin[2], width;
    bool has_height[PC_UI_GROUP_N];
    float stored_height[PC_UI_GROUP_N];
    pc_ui_group frozen_groups[PC_UI_GROUP_N];
    float frozen_heights[PC_UI_GROUP_N];
    size_t frozen_count;
    /* Auto-Collapse Iconic Panels: a collapsed group expanded by a click on its tab strip is a
     * transient "peek" that the first press outside it collapses again (Preferences > Workspace). */
    bool has_peek;
    pc_ui_group peek_group;
    /* Floating dock windows: whether group g's window was shown last frame, so a (re)float places it
     * at its model rect once and then follows the user's drag (see app.c draw_floating_dock). */
    bool float_shown[PC_UI_GROUP_N];
} pc_ui_docking;
void pc_ui_docking_begin(pc_ui_docking *runtime);
/* Mark `group` as the peeked group (expanded from its collapsed strip by a tab click). */
void pc_ui_docking_peek(pc_ui_docking *runtime, pc_ui_group group);
void pc_ui_docking_tab(pc_ui_docking *runtime, pc_ui_group group, size_t tab, const char *label,
                       struct nk_rect rect);
void pc_ui_docking_group(pc_ui_docking *runtime, pc_ui_group group, struct nk_rect rect, float height);
void pc_ui_docking_draw(pc_ui_shell_app *app, pc_ui_docking *runtime, struct nk_rect width_grip);
bool pc_ui_docking_cancel(pc_ui_shell_app *app, pc_ui_docking *runtime, const SDL_Event *event);
bool pc_ui_docking_capture(pc_ui_shell_app *app, const pc_ui_docking *runtime, const SDL_Event *event);
#endif
