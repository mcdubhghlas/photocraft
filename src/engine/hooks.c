/* E1-safe default implementations of the HOOK SEAM declared in engine_internal.h.
 *
 * Each default makes the engine behave correctly for E1's own commands and never crash. When a
 * later engine chunk implements a hook for real, it MOVES that function's definition out of this
 * file into its own module (deleting the default here) -- a surgical edit, so there is never a
 * duplicate symbol. The comment on each default names the owning chunk. */
#include "engine_internal.h"

yyjson_mut_val *pc_engine_hook_inject_target(const pc_engine_session *s, const char *id,
                                             yyjson_mut_doc *doc, yyjson_mut_val *params)
{
    (void)s;
    (void)id;
    (void)doc;
    return params; /* channel_cmds (E7) fills in the target */
}

/* pc_engine_hook_param_check relocated to param_check.c, E4. */

/* pc_engine_hook_filter_validate is defined by filters_cmds.c (E9), which owns Filter-menu
 * parameter validation; its default was removed here when that module landed. */

/* pc_engine_hook_float_before_command relocated to float_cmds.c, E4. */

/* pc_engine_hook_filter_subtype relocated to plugin_cmds.c (E12): it dispatches the plug-in
 * smart-filter re-render. The distort (E4) / lens (E6) / adjust (E8) subtypes add their id cases
 * to that function when they land (no duplicate symbol). */

size_t pc_engine_hook_color_restriction(const pc_engine_session *s, const char *id, yyjson_val *params)
{
    (void)s;
    (void)id;
    (void)params;
    return SIZE_MAX; /* channel_cmds (E7): a single targeted colour channel */
}

/* pc_engine_hook_restrict_to_color and pc_engine_hook_fix_view relocated to channel_cmds.c, E7. */

/* pc_engine_hook_step_damage relocated to layer_multi_cmds.c (layer_damage.c.inc), E5. */

/* pc_engine_hook_replayable relocated to actions_cmds.c (E12). */

/* pc_engine_hook_mask_view_fix relocated to mask_view_cmds.c, E5. */

/* pc_engine_hook_edit_menu_after_command relocated to edit_menu_cmds.c (EF): records the Edit >
 * Fade source after every successful command. */

/* pc_engine_hook_automate_after_command relocated to automate_cmds.c (E12). */

/* pc_engine_hook_smart_on_close relocated to smart_cmds_run.c (EF): an edited Edit-Contents
 * document commits back into its parent smart object before it closes. */

/* pc_engine_hook_coalesce_journal relocated to brush_cmds.c, E10. */

/* pc_engine_hook_inspect_channels relocated to channel_cmds.c, E7. */

/* pc_engine_hook_inspect_mask_view relocated to mask_view_cmds.c, E5. */

void pc_engine_hook_inspect_adjustment(yyjson_mut_doc *doc, yyjson_mut_val *obj, const pc_layer *l)
{
    (void)doc;
    (void)obj;
    (void)l; /* adjust_params serde (E8) */
}

/* pc_engine_hook_transform_bounds relocated to transform_cmds.c, E4. */

/* pc_engine_hook_transform_shape relocated to vector_cmds.c, E11. */

/* pc_engine_hook_transform_text relocated to type_cmds.c, E11. */

/* pc_engine_hook_transform_smart relocated to smart_cmds.c, E6. */

/* pc_engine_hook_refresh_shape relocated to vector_cmds.c, E11. */

/* pc_engine_hook_refresh_text relocated to type_cmds.c, E11. */

/* pc_engine_hook_refresh_smart relocated to smart_cmds.c, E6. */
