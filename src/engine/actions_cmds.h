/* Window > Actions: the recorded-action state carried on the Session, plus the cross-module
 * helpers automate_cmds and the dispatch recording seam share. Port of the pub(crate) surface of
 * crates/engine/src/actions_cmds.rs (E12). Internal to the engine. */
#ifndef PC_ENGINE_ACTIONS_CMDS_H
#define PC_ENGINE_ACTIONS_CMDS_H

#include "engine_internal.h"

/* One recorded step: a command id and its JSON params. */
typedef struct pc_action_step {
    char *id;             /* owned */
    pc_json_value params; /* owned */
} pc_action_step;

/* One recorded action. */
typedef struct pc_action {
    char *name; /* owned */
    pc_action_step *steps;
    size_t nsteps, cap_steps;
    size_t *disabled; /* sorted unique step indices excluded from playback */
    size_t ndisabled;
    size_t *dialogs; /* sorted unique step indices whose dialog Photoshop shows */
    size_t ndialogs;
    bool has_set;
    char *set; /* owned; the action set (folder) */
} pc_action;

/* Actions list and the recording / playback flags (Session::actions). Only `list` is persisted. */
typedef struct pc_action_state {
    pc_action *list;
    size_t nlist, cap_list;
    bool has_recording;
    size_t rec_from, rec_idx; /* journal length when recording started; action index */
    uint8_t playing;          /* actions.play nesting depth */
    char **playback_stack;    /* active action names (cycle detection); owned strings */
    size_t nplayback, cap_playback;
    uint64_t rev; /* bumped whenever `list` changes */
} pc_action_state;


/* The recording seam used by the dispatcher (jobs.c): a replayable command is recorded as one
 * step, so recording pauses while it runs. take() removes the active recording (returns whether
 * there was one); restore() puts it back only if the command did not start its own. */
bool pc_engine_actions_take_recording(pc_engine_session *s, size_t *from, size_t *idx);
void pc_engine_actions_restore_recording(pc_engine_session *s, bool had, size_t from, size_t idx);

/* Cross-module helpers (automate_cmds). */
bool pc_engine_actions_shell_save_command(const char *id);
bool pc_engine_actions_shell_view_command(const char *id);
/* Nested playback failure promoted to the caller (actions.play / actions.conditional): the owned
 * message, or NULL. Caller frees. */
char *pc_engine_actions_nested_failure(const char *id, yyjson_mut_val *value);

#endif
