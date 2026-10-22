/* Embedded Profile Mismatch model: interpret the `color` report file.openAs returns (produced by
 * the engine's pc_engine_color_open_policy) into the prompt the shell shows — whether to ask at all,
 * the embedded/working profile names, the already-applied action, and the choices to offer. Pure
 * logic, no GUI, so the gate and the option tables are unit-tested without a window (companion to the
 * shell prompt in src/ui/shell/profile_mismatch_ui.c; mirrors prefs_ui.rs open_mismatch /
 * mismatch_body). */
#ifndef PC_UI_PROFILE_MISMATCH_H
#define PC_UI_PROFILE_MISMATCH_H

#include "base/json.h"
#include <stdbool.h>
#include <stddef.h>

/* One choice the prompt offers: the color.profileMismatch action id and its label. */
typedef struct pc_ui_pm_option {
    const char *action;
    const char *label;
} pc_ui_pm_option;

/* The parsed report, with borrowed strings (valid while the report document lives). */
typedef struct pc_ui_profile_mismatch {
    bool missing;              /* true: untagged document (no embedded profile) */
    const char *embedded;      /* embedded profile name ("" when missing/absent) */
    const char *working;       /* working-space profile name ("" when absent) */
    const char *initial_action; /* the radio selection to start on: "preserve"/"convert"/"discard" */
} pc_ui_profile_mismatch;

/* Should the shell raise the prompt for this report? True only when the policy asked to and there is
 * a real mismatch or a missing profile (mirrors lib.rs: ask && (mismatch || missing)). NULL-safe. */
bool pc_ui_profile_mismatch_should_open(yyjson_mut_val *report);

/* Parse `report` into *out (borrowed strings). Returns false (leaving *out zeroed) when report is
 * NULL or not an object. */
bool pc_ui_profile_mismatch_parse(yyjson_mut_val *report, pc_ui_profile_mismatch *out);

/* The choices for a missing-profile prompt (missing=true) or an embedded-mismatch prompt
 * (missing=false). Writes the borrowed, static table into *out and returns its length. */
size_t pc_ui_profile_mismatch_options(bool missing, const pc_ui_pm_option **out);

#endif /* PC_UI_PROFILE_MISMATCH_H */
