/* Window > Actions as engine commands (actions.record/stop/play/list/get/delete/move/rename/
 * toggle). Port of crates/engine/src/actions_cmds.rs (E12).
 *
 * Recording copies replayable journal entries into an action; the dispatcher (jobs.c) pauses
 * recording while a replayable command runs and drops the entries of the commands it ran itself,
 * through pc_engine_hook_replayable (defined here) and pc_engine_actions_take/restore_recording.
 *
 * DEVIATION (§7): actions.rename does not follow a function-key shortcut override (the Preferences
 * shortcut map wiring is left to U2); it still updates nested Play Action calls. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "actions_cmds.h"

#define PLAY_SHORTCUT_PREFIX "actions.play:"

/* ---- state lifecycle ---------------------------------------------------------------------- */

static pc_action_state *actions_ensure(pc_engine_session *s)
{
    if (s->actions == NULL) {
        s->actions = calloc(1, sizeof *s->actions);
    }
    return s->actions;
}

static void step_fini(pc_action_step *st)
{
    free(st->id);
    pc_json_value_fini(&st->params);
}

static void action_fini(pc_action *a)
{
    free(a->name);
    for (size_t i = 0; i < a->nsteps; i++) {
        step_fini(&a->steps[i]);
    }
    free(a->steps);
    free(a->disabled);
    free(a->dialogs);
    free(a->set);
}

void pc_engine_actions_session_fini(pc_engine_session *s)
{
    if (s == NULL || s->actions == NULL) {
        return;
    }
    pc_action_state *a = s->actions;
    for (size_t i = 0; i < a->nlist; i++) {
        action_fini(&a->list[i]);
    }
    free(a->list);
    for (size_t i = 0; i < a->nplayback; i++) {
        free(a->playback_stack[i]);
    }
    free(a->playback_stack);
    free(a);
    s->actions = NULL;
}

static void touch(pc_action_state *a)
{
    a->rev = a->rev + 1;
}

/* ---- sorted-set helpers (size_t) ---------------------------------------------------------- */

static bool set_contains(const size_t *v, size_t n, size_t x)
{
    for (size_t i = 0; i < n; i++) {
        if (v[i] == x) {
            return true;
        }
    }
    return false;
}

static void set_insert(size_t **v, size_t *n, size_t x)
{
    if (set_contains(*v, *n, x)) {
        return;
    }
    size_t *g = realloc(*v, (*n + 1) * sizeof **v);
    if (g == NULL) {
        return;
    }
    *v = g;
    size_t i = *n;
    while (i > 0 && g[i - 1] > x) {
        g[i] = g[i - 1];
        i--;
    }
    g[i] = x;
    *n += 1;
}

static void set_remove(size_t *v, size_t *n, size_t x)
{
    for (size_t i = 0; i < *n; i++) {
        if (v[i] == x) {
            memmove(&v[i], &v[i + 1], (*n - i - 1) * sizeof *v);
            *n -= 1;
            return;
        }
    }
}

/* ---- helpers ------------------------------------------------------------------------------ */

bool pc_engine_actions_shell_save_command(const char *id)
{
    return strcmp(id, "file.save") == 0 || strcmp(id, "file.saveAs") == 0 ||
           strcmp(id, "file.saveACopy") == 0;
}

bool pc_engine_actions_shell_view_command(const char *id)
{
    return strcmp(id, "view.fitOnScreen") == 0 || strcmp(id, "view.actualPixels") == 0 ||
           strcmp(id, "view.zoomIn") == 0 || strcmp(id, "view.zoomOut") == 0 ||
           strcmp(id, "view.twoHundredPercent") == 0 || strcmp(id, "view.printSize") == 0 ||
           strcmp(id, "view.fillScreen") == 0;
}

/* replayable: replay commands and named action calls, excluding action-editing commands and the
 * already-expanded Again / Browse wrappers. This IS pc_engine_hook_replayable (moved from hooks.c). */
bool pc_engine_hook_replayable(const char *id)
{
    if (id == NULL) {
        return false;
    }
    if (strcmp(id, "actions.play") == 0) {
        return true;
    }
    if (strncmp(id, "actions.", 8) == 0) {
        return false;
    }
    if (strcmp(id, "edit.transform.again") == 0 || strcmp(id, "edit.transform.againCopy") == 0 ||
        strcmp(id, "file.scripts.browse") == 0) {
        return false;
    }
    if (pc_engine_actions_shell_view_command(id) || pc_engine_actions_shell_save_command(id)) {
        return true;
    }
    const pc_engine_command *c = pc_engine_command_find(id);
    return c != NULL && c->journal;
}

/* The recording seam used by jobs.c. */
bool pc_engine_actions_take_recording(pc_engine_session *s, size_t *from, size_t *idx)
{
    if (s->actions == NULL || !s->actions->has_recording) {
        return false;
    }
    *from = s->actions->rec_from;
    *idx = s->actions->rec_idx;
    s->actions->has_recording = false;
    return true;
}

void pc_engine_actions_restore_recording(pc_engine_session *s, bool had, size_t from, size_t idx)
{
    if (!had || s->actions == NULL) {
        return;
    }
    if (!s->actions->has_recording) { /* the command did not start its own recording */
        s->actions->has_recording = true;
        s->actions->rec_from = from;
        s->actions->rec_idx = idx;
    }
}

char *pc_engine_actions_nested_failure(const char *id, yyjson_mut_val *value)
{
    if (id == NULL || (strcmp(id, "actions.play") != 0 && strcmp(id, "actions.conditional") != 0)) {
        return NULL;
    }
    yyjson_mut_val *failure = yyjson_mut_obj_get(value, "failed");
    if (failure == NULL) {
        return NULL;
    }
    const char *action = yyjson_mut_get_str(yyjson_mut_obj_get(value, "action"));
    uint64_t step = yyjson_mut_get_uint(yyjson_mut_obj_get(failure, "step"));
    const char *error = yyjson_mut_get_str(yyjson_mut_obj_get(failure, "error"));
    char *out = malloc(512);
    if (out == NULL) {
        return NULL;
    }
    snprintf(out, 512, "action `%s` step %llu failed: %s", action != NULL ? action : "",
             (unsigned long long)(step + 1), error != NULL ? error : "unknown error");
    return out;
}

static bool num_of(yyjson_val *v, int64_t *out)
{
    if (pc_json_as_i64(v, out)) {
        return true;
    }
    double f;
    if (pc_json_as_f64(v, &f) && isfinite(f)) {
        *out = (int64_t)(f < 0 ? f - 0.5 : f + 0.5);
        return true;
    }
    return false;
}

/* resolve "action" (name or index) into a list index. */
static pc_status resolve(const pc_action_state *a, yyjson_val *p, const char *cmd, size_t *out, pc_error *err)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, "action") : NULL;
    if (v == NULL) {
        return pc_engine_err_bad_params(err, cmd, "needs \"action\" (a name or an index)");
    }
    int64_t i;
    if (num_of(v, &i)) {
        if (i < 0) {
            return pc_engine_err_bad_params(err, cmd, "action index is negative");
        }
        if ((uint64_t)i >= a->nlist) {
            return pc_engine_err_bad_params(err, cmd, "no action at index %lld", (long long)i);
        }
        *out = (size_t)i;
        return PC_OK;
    }
    size_t nl = 0;
    const char *name = pc_json_as_str(v, &nl);
    if (name == NULL) {
        return pc_engine_err_bad_params(err, cmd, "\"action\" must be a name or an index");
    }
    if (nl == 0) {
        return pc_engine_err_bad_params(err, cmd, "action name is empty");
    }
    for (size_t k = 0; k < a->nlist; k++) {
        if (a->list[k].name != NULL && strlen(a->list[k].name) == nl &&
            memcmp(a->list[k].name, name, nl) == 0) {
            *out = k;
            return PC_OK;
        }
    }
    return pc_engine_err_bad_params(err, cmd, "no action named `%.*s`", (int)nl, name);
}

static pc_status not_playing(const pc_action_state *a, const char *cmd, pc_error *err)
{
    if (a != NULL && a->playing > 0) {
        return pc_engine_err_bad_params(err, cmd, "cannot edit actions during playback");
    }
    return PC_OK;
}

/* Append a step (id + cloned params) to an action. */
static pc_status action_push_step(pc_action *a, const char *id, const pc_json_value *params, pc_error *err)
{
    pc_action_step *g = pc_grow_array(a->steps, &a->cap_steps, a->nsteps + 1, sizeof *a->steps);
    if (g == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    a->steps = g;
    pc_action_step *st = &a->steps[a->nsteps];
    memset(st, 0, sizeof *st);
    st->id = pc_strndup(id, strlen(id));
    if (st->id == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    if (params != NULL) {
        if (pc_json_value_clone(params, &st->params, err) != PC_OK) {
            free(st->id);
            return PC_ERR_NOMEM;
        }
    }
    a->nsteps++;
    return PC_OK;
}

/* flush_recording: commit the replayable journal entries recorded so far into the action. */
static pc_status flush_recording(pc_engine_session *s, pc_error *err)
{
    pc_action_state *a = s->actions;
    if (a == NULL || !a->has_recording) {
        return PC_OK;
    }
    size_t idx = a->rec_idx;
    if (idx >= a->nlist) {
        return pc_engine_err_bad_params(err, "actions.stop", "the action being recorded no longer exists");
    }
    size_t from = a->rec_from;
    bool any = false;
    for (size_t i = from; i < s->njournal; i++) {
        const char *jid = s->journal[i].id;
        if (jid == NULL || !pc_engine_hook_replayable(jid)) {
            continue;
        }
        pc_status rc = action_push_step(&a->list[idx], jid, &s->journal[i].params, err);
        if (rc != PC_OK) {
            return rc;
        }
        any = true;
    }
    a->rec_from = s->njournal;
    if (any) {
        touch(a);
    }
    return PC_OK;
}

/* count pending replayable journal entries for `idx`. */
static size_t pending_count(const pc_engine_session *s, size_t idx)
{
    const pc_action_state *a = s->actions;
    size_t from = (a != NULL && a->has_recording && a->rec_idx == idx) ? a->rec_from : s->njournal;
    size_t n = 0;
    for (size_t i = from; i < s->njournal; i++) {
        if (s->journal[i].id != NULL && pc_engine_hook_replayable(s->journal[i].id)) {
            n++;
        }
    }
    return n;
}

/* ---- result builders ---------------------------------------------------------------------- */

static pc_status result_action_index(pc_json_value *out, const char *name, size_t index, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "action", name);
    yyjson_mut_obj_add_uint(rd, root, "index", index);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- commands ----------------------------------------------------------------------------- */

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_action_state *a = s->actions;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    if (a != NULL) {
        for (size_t i = 0; i < a->nlist; i++) {
            yyjson_mut_val *o = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_strcpy(rd, o, "name", a->list[i].name ? a->list[i].name : "");
            yyjson_mut_obj_add_uint(rd, o, "steps", a->list[i].nsteps);
            if (a->list[i].has_set) {
                yyjson_mut_obj_add_strcpy(rd, o, "set", a->list[i].set ? a->list[i].set : "");
            } else {
                yyjson_mut_obj_add_null(rd, o, "set");
            }
            yyjson_mut_arr_append(arr, o);
        }
    }
    yyjson_mut_obj_add_val(rd, root, "actions", arr);
    if (a != NULL && a->has_recording && a->rec_idx < a->nlist) {
        yyjson_mut_obj_add_strcpy(rd, root, "recording", a->list[a->rec_idx].name);
    } else {
        yyjson_mut_obj_add_null(rd, root, "recording");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_get(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_action_state *a = actions_ensure(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    size_t idx = 0;
    pc_status rc = resolve(a, p, "actions.get", &idx, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (a->list == NULL || idx >= a->nlist) {
        return pc_fail(err, PC_ERR_INVALID, "no such action");
    }
    const pc_action *action = &a->list[idx];
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "name", action->name ? action->name : "");
    yyjson_mut_val *steps = yyjson_mut_arr(rd);
    for (size_t i = 0; i < action->nsteps; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_strcpy(rd, pair, action->steps[i].id);
        yyjson_mut_val *pr = pc_json_value_root(&action->steps[i].params);
        yyjson_mut_arr_append(pair, pr != NULL ? yyjson_mut_val_mut_copy(rd, pr) : yyjson_mut_obj(rd));
        yyjson_mut_arr_append(steps, pair);
    }
    yyjson_mut_obj_add_val(rd, root, "steps", steps);
    yyjson_mut_val *dis = yyjson_mut_arr(rd);
    for (size_t i = 0; i < action->ndisabled; i++) {
        yyjson_mut_arr_add_uint(rd, dis, action->disabled[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "disabled", dis);
    yyjson_mut_val *dlg = yyjson_mut_arr(rd);
    for (size_t i = 0; i < action->ndialogs; i++) {
        yyjson_mut_arr_add_uint(rd, dlg, action->dialogs[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "dialogs", dlg);
    if (action->has_set) {
        yyjson_mut_obj_add_strcpy(rd, root, "set", action->set ? action->set : "");
    } else {
        yyjson_mut_obj_add_null(rd, root, "set");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_record(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "actions.record";
    pc_action_state *a = actions_ensure(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    pc_status rc = not_playing(a, CMD, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (a->has_recording) {
        return pc_engine_err_bad_params(err, CMD, "already recording; stop first");
    }
    size_t idx = 0;
    if (p != NULL && pc_json_get(p, "action") != NULL) {
        rc = resolve(a, p, CMD, &idx, err);
        if (rc != PC_OK) {
            return rc;
        }
    } else {
        char name[128];
        yyjson_val *nv = p != NULL ? pc_json_get(p, "name") : NULL;
        if (nv == NULL || yyjson_is_null(nv)) {
            size_t n = a->nlist + 1;
            for (;;) {
                snprintf(name, sizeof name, "Action %zu", n);
                bool taken = false;
                for (size_t k = 0; k < a->nlist; k++) {
                    if (a->list[k].name != NULL && strcmp(a->list[k].name, name) == 0) {
                        taken = true;
                        break;
                    }
                }
                if (!taken) {
                    break;
                }
                n++;
            }
        } else {
            size_t nl = 0;
            const char *ns = pc_json_as_str(nv, &nl);
            if (ns == NULL) {
                return pc_engine_err_bad_params(err, CMD, "\"name\" must be a string");
            }
            if (nl == 0) {
                return pc_engine_err_bad_params(err, CMD, "\"name\" is empty");
            }
            if (nl >= sizeof name) {
                nl = sizeof name - 1;
            }
            memcpy(name, ns, nl);
            name[nl] = '\0';
            for (size_t k = 0; k < a->nlist; k++) {
                if (a->list[k].name != NULL && strcmp(a->list[k].name, name) == 0) {
                    return pc_engine_err_bad_params(err, CMD, "an action with this name already exists");
                }
            }
        }
        pc_action *g = pc_grow_array(a->list, &a->cap_list, a->nlist + 1, sizeof *a->list);
        if (g == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "actions");
        }
        a->list = g;
        memset(&a->list[a->nlist], 0, sizeof a->list[a->nlist]);
        a->list[a->nlist].name = pc_strndup(name, strlen(name));
        if (a->list[a->nlist].name == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "actions");
        }
        idx = a->nlist;
        a->nlist++;
        touch(a);
    }
    a->has_recording = true;
    a->rec_from = s->njournal;
    a->rec_idx = idx;
    if (a->list == NULL || idx >= a->nlist) {
        return pc_fail(err, PC_ERR_INVALID, "no such action");
    }
    return result_action_index(out, a->list[idx].name ? a->list[idx].name : "", idx, err);
}

static pc_status run_stop(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_action_state *a = actions_ensure(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    pc_status rc = not_playing(a, "actions.stop", err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!a->has_recording) {
        return pc_engine_err_bad_params(err, "actions.stop", "not recording");
    }
    size_t idx = a->rec_idx;
    rc = flush_recording(s, err);
    if (rc != PC_OK) {
        return rc;
    }
    a->has_recording = false;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "action", a->list[idx].name ? a->list[idx].name : "");
    yyjson_mut_obj_add_uint(rd, root, "steps", a->list[idx].nsteps);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- playback ----------------------------------------------------------------------------- */

/* Run action `action` (a cloned snapshot) from step `from`; returns ran count and optional
 * failure object (owned, set into *failed). */
static uint64_t run_recorded(pc_engine_session *s, const pc_action *action, size_t from, yyjson_mut_doc *rd,
                             yyjson_mut_val **failed)
{
    uint64_t ran = 0;
    *failed = NULL;
    for (size_t i = from; i < action->nsteps; i++) {
        if (set_contains(action->disabled, action->ndisabled, i)) {
            continue;
        }
        const char *id = action->steps[i].id;
        yyjson_doc *imut = action->steps[i].params.doc != NULL
                               ? yyjson_mut_doc_imut_copy(action->steps[i].params.doc, NULL)
                               : NULL;
        yyjson_val *prm = imut != NULL ? yyjson_doc_get_root(imut) : NULL;
        if (s->authorize != NULL) {
            pc_error ae = {0};
            if (s->authorize(id, prm, &ae) != PC_OK) {
                yyjson_mut_val *f = yyjson_mut_obj(rd);
                yyjson_mut_obj_add_uint(rd, f, "step", i);
                yyjson_mut_obj_add_strcpy(rd, f, "id", id);
                yyjson_mut_obj_add_strcpy(rd, f, "error", ae.msg);
                *failed = f;
                yyjson_doc_free(imut);
                break;
            }
        }
        pc_json_value value = PC_JSON_VALUE_INIT;
        pc_error ee = {0};
        pc_status rc = pc_engine_actions_shell_view_command(id) && s->shell_dispatch
                           ? s->shell_dispatch(s->shell_dispatch_ctx, id, prm, &value, &ee)
                           : pc_engine_session_execute(s, id, prm, &value, &ee);
        yyjson_doc_free(imut);
        if (rc != PC_OK) {
            yyjson_mut_val *f = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_uint(rd, f, "step", i);
            yyjson_mut_obj_add_strcpy(rd, f, "id", id);
            yyjson_mut_obj_add_strcpy(rd, f, "error", ee.msg);
            *failed = f;
            pc_json_value_fini(&value);
            break;
        }
        char *nested = pc_engine_actions_nested_failure(id, pc_json_value_root(&value));
        if (nested != NULL) {
            yyjson_mut_val *f = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_uint(rd, f, "step", i);
            yyjson_mut_obj_add_strcpy(rd, f, "id", id);
            yyjson_mut_obj_add_strcpy(rd, f, "error", nested);
            *failed = f;
            free(nested);
            pc_json_value_fini(&value);
            break;
        }
        ran++;
        pc_json_value_fini(&value);
    }
    return ran;
}

static bool playback_stack_has(const pc_action_state *a, const char *name)
{
    for (size_t i = 0; i < a->nplayback; i++) {
        if (a->playback_stack[i] != NULL && strcmp(a->playback_stack[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static pc_status run_play(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "actions.play";
    pc_action_state *a = actions_ensure(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    if (a->playing > 0 && a->nplayback == 0) {
        return pc_engine_err_bad_params(err, CMD, "an action is already playing");
    }
    size_t idx = 0;
    pc_status rc = resolve(a, p, CMD, &idx, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (a->list == NULL || idx >= a->nlist) {
        return pc_fail(err, PC_ERR_INVALID, "no such action");
    }
    const char *aname = a->list[idx].name ? a->list[idx].name : "";
    if (playback_stack_has(a, aname) || (a->has_recording && a->rec_idx == idx)) {
        return pc_engine_err_bad_params(err, CMD, "recursive action call to `%s`", aname);
    }
    if (a->nplayback >= 16) {
        return pc_engine_err_bad_params(err, CMD, "action nesting exceeds 16 levels");
    }
    size_t nsteps = a->list[idx].nsteps;
    size_t from = 0;
    yyjson_val *fv = p != NULL ? pc_json_get(p, "from") : NULL;
    if (fv != NULL && !yyjson_is_null(fv)) {
        int64_t fi;
        if (!num_of(fv, &fi)) {
            return pc_engine_err_bad_params(err, CMD, "\"from\" must be a step index");
        }
        if (fi < 0) {
            return pc_engine_err_bad_params(err, CMD, "\"from\" is negative");
        }
        if ((uint64_t)fi > nsteps) {
            return pc_engine_err_bad_params(err, CMD, "\"from\" %lld is past the last step (%zu)",
                                            (long long)fi, nsteps);
        }
        from = (size_t)fi;
    }
    /* clone the action (playback may edit the list via nested recording) */
    pc_action snap;
    memset(&snap, 0, sizeof snap);
    snap.name = pc_strndup(aname, strlen(aname));
    snap.nsteps = nsteps;
    snap.steps = nsteps > 0 ? pc_malloc_array(nsteps, sizeof *snap.steps) : NULL;
    if ((nsteps > 0 && snap.steps == NULL) || snap.name == NULL) {
        free(snap.name);
        free(snap.steps);
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    for (size_t i = 0; i < nsteps; i++) {
        memset(&snap.steps[i], 0, sizeof snap.steps[i]);
        snap.steps[i].id = pc_strndup(a->list[idx].steps[i].id, strlen(a->list[idx].steps[i].id));
        (void)pc_json_value_clone(&a->list[idx].steps[i].params, &snap.steps[i].params, NULL);
    }
    if (a->list[idx].ndisabled > 0) {
        snap.disabled = pc_malloc_array(a->list[idx].ndisabled, sizeof *snap.disabled);
        if (snap.disabled != NULL) {
            memcpy(snap.disabled, a->list[idx].disabled, a->list[idx].ndisabled * sizeof *snap.disabled);
            snap.ndisabled = a->list[idx].ndisabled;
        }
    }
    /* begin_playback: flush recording, suspend it, push the name, bump playing */
    rc = flush_recording(s, err);
    if (rc != PC_OK) {
        action_fini(&snap);
        return rc;
    }
    bool had_rec = a->has_recording;
    size_t rec_from = a->rec_from, rec_idx = a->rec_idx;
    a->has_recording = false;
    a->playing = (uint8_t)(a->playing + 1);
    char **pg =
        pc_grow_array(a->playback_stack, &a->cap_playback, a->nplayback + 1, sizeof *a->playback_stack);
    if (pg != NULL) {
        a->playback_stack = pg;
        a->playback_stack[a->nplayback++] = pc_strndup(aname, strlen(aname));
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        /* unwind playback state */
        if (a->nplayback > 0) {
            free(a->playback_stack[--a->nplayback]);
        }
        a->playing = (uint8_t)(a->playing - 1);
        action_fini(&snap);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *failed = NULL;
    uint64_t ran = run_recorded(s, &snap, from, rd, &failed);
    /* finish_playback */
    if (a->nplayback > 0) {
        free(a->playback_stack[--a->nplayback]);
    }
    a->playing = (uint8_t)(a->playing - 1);
    if (had_rec) {
        a->has_recording = true;
        a->rec_from = s->njournal;
        a->rec_idx = rec_idx;
        (void)rec_from;
        /* record the named call into the parent action's journal position: push a journal entry */
        yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
        if (pd != NULL) {
            yyjson_mut_val *po = yyjson_mut_obj(pd);
            yyjson_mut_obj_add_strcpy(pd, po, "action", aname);
            if (from > 0) {
                yyjson_mut_obj_add_uint(pd, po, "from", from);
            }
            yyjson_mut_doc_set_root(pd, po);
            yyjson_doc *imut = yyjson_mut_doc_imut_copy(pd, NULL);
            yyjson_mut_doc_free(pd);
            if (imut != NULL) {
                (void)pc_engine_journal_push(s, "actions.play", yyjson_doc_get_root(imut), NULL);
                yyjson_doc_free(imut);
            }
        }
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "action", aname);
    yyjson_mut_obj_add_uint(rd, root, "ran", ran);
    if (failed != NULL) {
        yyjson_mut_obj_add_val(rd, root, "failed", failed);
    }
    yyjson_mut_doc_set_root(rd, root);
    action_fini(&snap);
    return PC_OK;
}

/* ---- move / delete / toggle / rename ------------------------------------------------------ */

static size_t moved_index(size_t index, size_t from, size_t to)
{
    if (index == from) {
        return to;
    }
    if (from < index && index <= to) {
        return index - 1;
    }
    if (to <= index && index < from) {
        return index + 1;
    }
    return index;
}

static bool uint_param(yyjson_val *p, const char *key, size_t *out)
{
    yyjson_val *v = pc_json_get(p, key);
    uint64_t u;
    if (v != NULL && pc_json_as_u64(v, &u)) {
        *out = (size_t)u;
        return true;
    }
    return false;
}

static pc_status run_move(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "actions.move";
    pc_action_state *a = actions_ensure(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    pc_status rc = not_playing(a, CMD, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t idx = 0;
    rc = resolve(a, p, CMD, &idx, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (a->list == NULL || idx >= a->nlist) {
        return pc_fail(err, PC_ERR_INVALID, "no such action");
    }
    size_t to;
    if (!uint_param(p, "to", &to)) {
        return pc_engine_err_bad_params(err, CMD, "to must be a non-negative integer");
    }
    if (p != NULL && pc_json_get(p, "step") != NULL) {
        size_t step;
        if (!uint_param(p, "step", &step)) {
            return pc_engine_err_bad_params(err, CMD, "step must be a non-negative integer");
        }
        size_t len = a->list[idx].nsteps + pending_count(s, idx);
        if (step >= len || to >= len) {
            return pc_engine_err_bad_params(err, CMD, "step or destination is out of range");
        }
        if (a->has_recording && a->rec_idx == idx) {
            rc = flush_recording(s, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        pc_action *action = &a->list[idx];
        if (step >= action->nsteps || to >= action->nsteps) {
            return pc_engine_err_bad_params(err, CMD, "step or destination is out of range");
        }
        pc_action_step moved = action->steps[step];
        memmove(&action->steps[step], &action->steps[step + 1],
                (action->nsteps - step - 1) * sizeof *action->steps);
        memmove(&action->steps[to + 1], &action->steps[to], 0); /* placeholder */
        /* reinsert at `to` */
        if (to < action->nsteps - 1) {
            memmove(&action->steps[to + 1], &action->steps[to],
                    (action->nsteps - 1 - to) * sizeof *action->steps);
        }
        action->steps[to] = moved;
        /* remap disabled/dialogs */
        for (size_t i = 0; i < action->ndisabled; i++) {
            action->disabled[i] = moved_index(action->disabled[i], step, to);
        }
        for (size_t i = 0; i < action->ndialogs; i++) {
            action->dialogs[i] = moved_index(action->dialogs[i], step, to);
        }
        touch(a);
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, root, "action", action->name ? action->name : "");
        yyjson_mut_obj_add_uint(rd, root, "step", to);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    if (to >= a->nlist) {
        return pc_engine_err_bad_params(err, CMD, "destination is out of range");
    }
    pc_action moved = a->list[idx];
    memmove(&a->list[idx], &a->list[idx + 1], (a->nlist - idx - 1) * sizeof *a->list);
    a->nlist--;
    if (to < a->nlist) {
        memmove(&a->list[to + 1], &a->list[to], (a->nlist - to) * sizeof *a->list);
    }
    a->list[to] = moved;
    a->nlist++;
    if (a->has_recording) {
        a->rec_idx = moved_index(a->rec_idx, idx, to);
    }
    touch(a);
    return result_action_index(out, moved.name ? moved.name : "", to, err);
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "actions.delete";
    pc_action_state *a = actions_ensure(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    pc_status rc = not_playing(a, CMD, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t idx = 0;
    rc = resolve(a, p, CMD, &idx, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (a->list == NULL || idx >= a->nlist) {
        return pc_fail(err, PC_ERR_INVALID, "no such action");
    }
    if (p != NULL && pc_json_get(p, "step") != NULL) {
        size_t step;
        if (!uint_param(p, "step", &step)) {
            return pc_engine_err_bad_params(err, CMD, "step must be a non-negative integer");
        }
        size_t len = a->list[idx].nsteps + pending_count(s, idx);
        if (step >= len) {
            return pc_engine_err_bad_params(err, CMD, "step is out of range");
        }
        if (a->has_recording && a->rec_idx == idx) {
            rc = flush_recording(s, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        pc_action *action = &a->list[idx];
        if (step >= action->nsteps) {
            return pc_engine_err_bad_params(err, CMD, "step is out of range");
        }
        step_fini(&action->steps[step]);
        memmove(&action->steps[step], &action->steps[step + 1],
                (action->nsteps - step - 1) * sizeof *action->steps);
        action->nsteps--;
        /* remap disabled/dialogs */
        size_t w = 0;
        for (size_t i = 0; i < action->ndisabled; i++) {
            if (action->disabled[i] == step) {
                continue;
            }
            action->disabled[w++] =
                action->disabled[i] > step ? action->disabled[i] - 1 : action->disabled[i];
        }
        action->ndisabled = w;
        w = 0;
        for (size_t i = 0; i < action->ndialogs; i++) {
            if (action->dialogs[i] == step) {
                continue;
            }
            action->dialogs[w++] = action->dialogs[i] > step ? action->dialogs[i] - 1 : action->dialogs[i];
        }
        action->ndialogs = w;
        touch(a);
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, root, "action", action->name ? action->name : "");
        yyjson_mut_obj_add_uint(rd, root, "deletedStep", step);
        yyjson_mut_obj_add_uint(rd, root, "steps", action->nsteps);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    if (a->has_recording) {
        return pc_engine_err_bad_params(err, CMD, "stop recording before deleting an action");
    }
    char *name = a->list[idx].name;
    a->list[idx].name = NULL;
    action_fini(&a->list[idx]);
    memmove(&a->list[idx], &a->list[idx + 1], (a->nlist - idx - 1) * sizeof *a->list);
    a->nlist--;
    touch(a);
    yyjson_mut_doc *rd = NULL;
    pc_status r2 = pc_engine_result_doc(out, &rd, err);
    if (r2 == PC_OK) {
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, root, "deleted", name ? name : "");
        yyjson_mut_doc_set_root(rd, root);
    }
    free(name);
    return r2;
}

static pc_status run_toggle(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "actions.toggle";
    pc_action_state *a = actions_ensure(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    pc_status rc = not_playing(a, CMD, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t idx = 0;
    rc = resolve(a, p, CMD, &idx, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (a->list == NULL || idx >= a->nlist) {
        return pc_fail(err, PC_ERR_INVALID, "no such action");
    }
    int enabled = -1; /* -1 unset */
    yyjson_val *ev = p != NULL ? pc_json_get(p, "enabled") : NULL;
    if (ev != NULL && !yyjson_is_null(ev)) {
        if (!yyjson_is_bool(ev)) {
            return pc_engine_err_bad_params(err, CMD, "\"enabled\" must be true or false");
        }
        enabled = yyjson_get_bool(ev) ? 1 : 0;
    }
    bool have_step = false;
    size_t step = 0;
    yyjson_val *sv = p != NULL ? pc_json_get(p, "step") : NULL;
    if (sv != NULL && !yyjson_is_null(sv)) {
        if (!uint_param(p, "step", &step)) {
            return pc_engine_err_bad_params(err, CMD, "step must be a non-negative integer");
        }
        have_step = true;
    }
    size_t len = a->list[idx].nsteps + pending_count(s, idx);
    if (have_step && step >= len) {
        return pc_engine_err_bad_params(err, CMD, "step is out of range");
    }
    if (a->has_recording && a->rec_idx == idx) {
        rc = flush_recording(s, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    pc_action *action = &a->list[idx];
    if (have_step) {
        bool on = enabled >= 0 ? enabled != 0 : set_contains(action->disabled, action->ndisabled, step);
        if (on) {
            set_remove(action->disabled, &action->ndisabled, step);
        } else {
            set_insert(&action->disabled, &action->ndisabled, step);
        }
    } else {
        bool clear = enabled >= 0 ? enabled != 0 : (action->ndisabled > 0);
        if (clear) {
            action->ndisabled = 0;
        } else {
            action->ndisabled = 0;
            for (size_t i = 0; i < action->nsteps; i++) {
                set_insert(&action->disabled, &action->ndisabled, i);
            }
        }
    }
    touch(a);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "action", action->name ? action->name : "");
    yyjson_mut_val *dis = yyjson_mut_arr(rd);
    for (size_t i = 0; i < action->ndisabled; i++) {
        yyjson_mut_arr_add_uint(rd, dis, action->disabled[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "disabled", dis);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_rename(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "actions.rename";
    pc_action_state *a = actions_ensure(s);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "actions");
    }
    pc_status rc = not_playing(a, CMD, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t idx = 0;
    rc = resolve(a, p, CMD, &idx, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (a->list == NULL || idx >= a->nlist) {
        return pc_fail(err, PC_ERR_INVALID, "no such action");
    }
    yyjson_val *nv = p != NULL ? pc_json_get(p, "name") : NULL;
    if (nv == NULL) {
        return pc_engine_err_bad_params(err, CMD, "needs \"name\"");
    }
    size_t nl = 0;
    const char *ns = pc_json_as_str(nv, &nl);
    if (ns == NULL) {
        return pc_engine_err_bad_params(err, CMD, "\"name\" must be a string");
    }
    /* trim check */
    size_t t0 = 0, t1 = nl;
    while (t0 < t1 && (ns[t0] == ' ' || ns[t0] == '\t')) {
        t0++;
    }
    while (t1 > t0 && (ns[t1 - 1] == ' ' || ns[t1 - 1] == '\t')) {
        t1--;
    }
    if (t1 == t0) {
        return pc_engine_err_bad_params(err, CMD, "\"name\" is empty");
    }
    char name[128];
    if (nl >= sizeof name) {
        nl = sizeof name - 1;
    }
    memcpy(name, ns, nl);
    name[nl] = '\0';
    const char *old = a->list[idx].name ? a->list[idx].name : "";
    if (strcmp(old, name) == 0) {
        return result_action_index(out, name, idx, err);
    }
    for (size_t k = 0; k < a->nlist; k++) {
        if (a->list[k].name != NULL && strcmp(a->list[k].name, name) == 0) {
            return pc_engine_err_bad_params(err, CMD, "an action with this name already exists");
        }
    }
    /* update nested actions.play calls that reference the old name */
    char *oldcopy = pc_strndup(old, strlen(old));
    for (size_t k = 0; k < a->nlist; k++) {
        for (size_t i = 0; i < a->list[k].nsteps; i++) {
            if (strcmp(a->list[k].steps[i].id, "actions.play") != 0) {
                continue;
            }
            yyjson_mut_val *pr = pc_json_value_root(&a->list[k].steps[i].params);
            yyjson_mut_val *av = pr != NULL ? yyjson_mut_obj_get(pr, "action") : NULL;
            const char *cur = av != NULL ? yyjson_mut_get_str(av) : NULL;
            if (cur != NULL && oldcopy != NULL && strcmp(cur, oldcopy) == 0) {
                yyjson_mut_doc *d = a->list[k].steps[i].params.doc;
                yyjson_mut_obj_put(pr, yyjson_mut_strcpy(d, "action"), yyjson_mut_strcpy(d, name));
            }
        }
    }
    free(oldcopy);
    free(a->list[idx].name);
    a->list[idx].name = pc_strndup(name, strlen(name));
    touch(a);
    return result_action_index(out, name, idx, err);
}

static pc_status always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"actions.move", "Move Action or Step", NULL, 0, NULL,
     "{\"action\":name|index, \"step\":index?, \"to\":index}", always, run_move, false},
    {"actions.list", "List Actions", NULL, 0, NULL,
     "{} \xE2\x86\x92 {actions:[{name, steps:count, set:name|null}], recording:name|null}", always, run_list,
     false},
    {"actions.get", "Get Action", NULL, 0, NULL,
     "{\"action\":name|index} \xE2\x86\x92 {name, steps:[[id, params]\xE2\x80\xA6], disabled, dialogs, set}",
     always, run_get, false},
    {"actions.record", "Record Action", NULL, 0, NULL,
     "{\"name\":str? (new action, default \"Action N\")} or {\"action\":name|index} (append) \xE2\x86\x92 "
     "{action, index}",
     always, run_record, false},
    {"actions.stop", "Stop Recording", NULL, 0, NULL, "{} \xE2\x86\x92 {action, steps:count}", always,
     run_stop, false},
    {"actions.play", "Play Action", NULL, 0, NULL,
     "{\"action\":name|index, \"from\":step?} \xE2\x86\x92 {action, ran, failed?:{step, id, error}}", always,
     run_play, false},
    {"actions.delete", "Delete Action", NULL, 0, NULL,
     "{\"action\":name|index, \"step\":index?} \xE2\x86\x92 {deleted} or {action, deletedStep, steps}",
     always, run_delete, false},
    {"actions.toggle", "Toggle Action Steps", NULL, 0, NULL,
     "{\"action\":name|index, \"step\":index?, \"enabled\":bool?} \xE2\x86\x92 {action, disabled}", always,
     run_toggle, false},
    {"actions.rename", "Rename Action", NULL, 0, NULL,
     "{\"action\":name|index, \"name\":str} \xE2\x86\x92 {action, index}", always, run_rename, false},
};

const pc_engine_command *pc_engine_actions_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
