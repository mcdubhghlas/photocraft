/* The History panel menu (☰, #2966; port of crates/engine/src/history_menu_cmds.rs): Step Forward,
 * Step Backward and Clear History. Plus history.setSource (history_source_cmds.rs).
 *
 * Step Forward / Step Backward move one state along the History list, like Edit › Redo / Undo, but
 * report a step that isn't there as an error. Clear History and setSource need history-model
 * features the C src/ops history does not yet have (an undoable "collapse to the current state", and
 * a per-state/snapshot History Brush source): they are registered but report a clear "unsupported"
 * error (REWRITE_PLAN.md rule 9) until the src/ops history gains clear_states / brush-source. */
#include <stddef.h>

#include "base/json.h"
#include "ops/ops.h"

#include "engine_internal.h"
#include "history_menu_cmds.h"

/* {states, current, label}: the list length, the current state's index (0 = oldest) and its name. */
static pc_status history_list_result(const pc_engine_doc_state *d, pc_json_value *out, pc_error *err)
{
    pc_history *h = d->history;
    size_t entries = pc_history_entry_count(h);     /* past + current */
    size_t states = entries + pc_history_redo_count(h);
    size_t current = pc_history_past_len(h);
    const pc_str *label = entries > 0 ? pc_history_entry(h, entries - 1) : NULL;

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "states", states);
    yyjson_mut_obj_add_uint(rd, root, "current", current);
    if (label != NULL) {
        yyjson_mut_obj_add_strcpy(rd, root, "label", pc_str_c(label));
    } else {
        yyjson_mut_obj_add_null(rd, root, "label");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status can_step_forward(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return pc_history_can_redo(d->history) ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no later state");
}

static pc_status can_step_backward(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return pc_history_can_undo(d->history) ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no earlier state");
}

static pc_status step(pc_engine_session *s, bool forward, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    bool possible = forward ? pc_history_can_redo(d->history) : pc_history_can_undo(d->history);
    if (!possible) {
        return pc_engine_err_other(err, forward ? "no later state to step forward to"
                                                 : "no earlier state to step back to");
    }
    bool moved = forward ? pc_engine_session_redo(s) : pc_engine_session_undo(s);
    if (!moved) {
        return pc_engine_err_other(err, "the history can't step right now");
    }
    return history_list_result(pc_engine_session_active(s), out, err);
}

static pc_status run_step_forward(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return step(s, true, out, err);
}

static pc_status run_step_backward(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return step(s, false, out, err);
}

/* Clear History: an undoable collapse to the current state (needs pc_history_clear_states in
 * src/ops) — unsupported until that lands. */
static pc_status can_clear(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (pc_history_past_len(d->history) > 0 || pc_history_can_redo(d->history)) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "the current state is the only state");
}

static pc_status run_clear(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    (void)out;
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "Clear History needs an undoable history collapse (pc_history_clear_states) that "
                   "src/ops doesn't have yet");
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

static pc_status run_set_source(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    (void)out;
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "the History Brush source model (pc_history brush-source + per-snapshot source) "
                   "isn't ported yet");
}

static const pc_engine_command MENU_SPECS[] = {
    {"history.stepForward", "Step Forward", NULL, 0, NULL,
     "{} -> {states,current,label}: makes the next History state current (Err when there is none)",
     can_step_forward, run_step_forward, true},
    {"history.stepBackward", "Step Backward", NULL, 0, NULL,
     "{} -> {states,current,label}: makes the previous History state current (Err when there is none)",
     can_step_backward, run_step_backward, true},
    {"history.clear", "Clear History", NULL, 0, NULL,
     "{} -> {states,current,label}: removes every History state but the current one (unsupported: "
     "needs an undoable history collapse in src/ops)",
     can_clear, run_clear, true},
};

static const pc_engine_command SOURCE_SPECS[] = {
    {"history.setSource", "Set History Brush Source", NULL, 0, NULL,
     "{\"state\":row} | {\"snapshot\":id} -> {state,label} | {snapshot,name} (unsupported: the "
     "History Brush source model isn't ported yet)",
     has_doc, run_set_source, true},
};

const pc_engine_command *pc_engine_history_menu_specs(size_t *n)
{
    *n = sizeof MENU_SPECS / sizeof MENU_SPECS[0];
    return MENU_SPECS;
}

const pc_engine_command *pc_engine_history_source_specs(size_t *n)
{
    *n = sizeof SOURCE_SPECS / sizeof SOURCE_SPECS[0];
    return SOURCE_SPECS;
}
