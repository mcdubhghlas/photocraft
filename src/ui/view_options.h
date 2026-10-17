/* Persistent shell ViewOptions (view_cmds.rs). Strings keep unknown values verbatim so newer
 * saved state can round-trip. A NULL string denotes its default without allocating at startup.
 * Single owner; fini releases strings/JSON. Parsing requires an initialised *out and replaces it only on
 * success. */
#ifndef PC_UI_VIEW_OPTIONS_H
#define PC_UI_VIEW_OPTIONS_H
#include "base/json.h"
#include "base/str.h"

enum { PC_UI_SHOW_COUNT = 13, PC_UI_SNAP_COUNT = 5 };
typedef struct pc_ui_view_options {
    pc_str screen_mode, pixel_aspect, arrange, font_preview_size, language_features;
    bool extras, flip_horizontal, pixel_aspect_correction, pattern_preview, pixel_art_preview;
    bool middle_eastern_composer, tile_grid;
    bool show[PC_UI_SHOW_COUNT], snap_to[PC_UI_SNAP_COUNT];
    pc_json_value guide_layout;
} pc_ui_view_options;

void pc_ui_view_options_init(pc_ui_view_options *o);
void pc_ui_view_options_fini(pc_ui_view_options *o);
pc_status pc_ui_view_options_to_json(const pc_ui_view_options *o, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                     pc_error *err);
pc_status pc_ui_view_options_from_json(yyjson_val *obj, pc_ui_view_options *out, pc_error *err);
bool *pc_ui_view_show_flag(pc_ui_view_options *o, const char *command_key);
bool *pc_ui_view_snap_flag(pc_ui_view_options *o, const char *command_key);
bool pc_ui_view_standard(const pc_ui_view_options *o);
bool pc_ui_view_hides_chrome(const pc_ui_view_options *o);
float pc_ui_pixel_aspect_ratio(const char *id);
float pc_ui_view_display_aspect(const pc_ui_view_options *o);
/* A stable borrowed value; NULL string fields return the reference default. */
const char *pc_ui_view_screen_mode(const pc_ui_view_options *o);
/* Camera display options only: flip, aspect correction/presets and screen mode. NOT_FOUND
 * leaves state/output untouched for another shell module to handle the id. */
pc_status pc_ui_view_options_command(pc_ui_view_options *o, const char *id, yyjson_val *params,
                                     pc_json_value *out, pc_error *err);
#endif
