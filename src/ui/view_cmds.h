/* View menu commands: the id classifier shared with the menu/parity layer, plus the pure
 * key→slot mappings for View › Show and View › Snap To. Port of the data-and-logic half of
 * crates/ui-egui/src/view_cmds.rs. The view-state mutations (zoom, rotate, screen mode, flip,
 * arrange, pixel aspect, font-preview, language) read and write PhotocraftApp view state and are
 * reimplemented when the app shell is assembled (later U-chunks). No GUI dependency. */
#ifndef PC_UI_VIEW_CMDS_H
#define PC_UI_VIEW_CMDS_H

#include <stdbool.h>

/* Does the View/Window/Type command layer handle `id` (a live menu item)? Mirrors
 * view_cmds::handles: the view.show.*, view.snapTo.*, view.screenMode.*, view.pixelAspectRatio.*,
 * type.fontPreviewSize.*, type.languageOptions.*, window.arrange.* families and a fixed set of
 * individual ids. */
bool pc_ui_view_cmds_handles(const char *id);

/* The View › Show slot key (after "view.show."), e.g. "layerEdges", maps to a flag; returns true
 * when `key` names a real slot. ("all" is handled by the command, not here.) */
bool pc_ui_view_show_slot_exists(const char *key);
/* The View › Snap To slot key (after "view.snapTo."); returns true when `key` names a real slot. */
bool pc_ui_view_snap_slot_exists(const char *key);

#endif /* PC_UI_VIEW_CMDS_H */
