/* Image Size / Canvas Size unit maths (port of the self-contained logic from
 * crates/ui-egui/src/sizing.rs). The dialog UI (egui body/anchor grid) is reimplemented by U10;
 * U1 owns the unit conversions, the human-readable byte sizes and the unit/anchor tables that the
 * dialog and automation share. No GUI dependency. */
#ifndef PC_UI_SIZING_H
#define PC_UI_SIZING_H

#include <stdbool.h>

/* (key, label) for each sizing unit, in dialog order. */
extern const char *const PC_UI_SIZING_UNITS[7][2];
/* Canvas-size anchor ids, row-major 3x3. */
extern const char *const PC_UI_SIZING_ANCHORS[9];

/* Does `command` open the Image Size / Canvas Size dialog? */
bool pc_ui_is_sizing(const char *command);
/* Canonical engine interpolation id for a saved General preference (static lifetime). */
const char *pc_ui_resample_preference(const char *preference);

/* Pixels -> display unit (orig = the original pixel dimension, res = resolution in ppi). */
double pc_ui_to_unit(double px, const char *unit, double orig, double res);
/* Display unit -> pixels. */
double pc_ui_from_unit(double v, const char *unit, double orig, double res);

/* Human-readable byte size (serde layout: "138.1M", "6.40G", "20K"). Writes into out (>=16). */
void pc_ui_human_bytes(double bytes, char out[16]);

#endif
