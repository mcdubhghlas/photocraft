/* File > Scripts (Browse, Script Events Manager) and the File-menu session state shared by
 * slice_cmds and web_cmds. Port of crates/engine/src/automate_cmds.rs (E12).
 *
 * Also owns pc_engine_hook_automate_after_command (moved from hooks.c): after every successful
 * command it re-fits layer-based slices and fires the engine-side script events (New / Close).
 *
 * DEFERRED (owner: a later automate follow-up): File > Automate > Contact Sheet II / Create
 * Droplet / Run Droplet and File > Scripts > Statistics. They need the file_cmds batch pipeline,
 * the static file-name helpers (join/list_images/stem/native), type.create / image.imageSize /
 * layer.flattenImage and the smart-object stack-mode commands, several of which other chunks have
 * not exported or registered yet (REWRITE_PLAN.md §7). */
#include <stdlib.h>
#include <string.h>

#include "actions_cmds.h" /* pc_engine_actions_nested_failure */
#include "engine_internal.h"

static pc_status auto_bad(pc_error *err, const char *cmd, const char *msg)
{
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

/* The Script Events Manager event ids (Photoshop's list). */
static const char *const EVENT_IDS[] = {"startApplication", "newDocument", "openDocument",
                                        "saveDocument",     "closeDocument", "print",
                                        "export",           "everything"};
static const char *const EVENT_LABELS[] = {"Start Application", "New Document",  "Open Document",
                                           "Save Document",    "Close Document", "Print Document",
                                           "Export Document",  "Everything"};

/* ---- script parsing ----------------------------------------------------------------------- */

/* Append step [id, params] to the mut array `arr` from a parsed step value. */
static pc_status collect_step(yyjson_mut_doc *md, yyjson_mut_val *arr, yyjson_val *st,
                              const char *cmd, pc_error *err)
{
    const char *id = NULL;
    size_t idlen = 0;
    yyjson_val *params = NULL;
    if (yyjson_is_arr(st) && yyjson_arr_size(st) >= 1) {
        id = pc_json_as_str(yyjson_arr_get(st, 0), &idlen);
        if (id == NULL) {
            return auto_bad(err, cmd, "step id must be a string");
        }
        params = yyjson_arr_size(st) >= 2 ? yyjson_arr_get(st, 1) : NULL;
    } else if (yyjson_is_obj(st)) {
        yyjson_val *idv = pc_json_get(st, "command");
        if (idv == NULL) {
            idv = pc_json_get(st, "id");
        }
        id = idv != NULL ? pc_json_as_str(idv, &idlen) : NULL;
        if (id == NULL) {
            return auto_bad(err, cmd, "step needs \"command\"");
        }
        params = pc_json_get(st, "params");
    } else if (yyjson_is_str(st)) {
        id = pc_json_as_str(st, &idlen);
    } else {
        return auto_bad(err, cmd, "each step is [id, params], {\"command\", \"params\"} or an id");
    }
    yyjson_mut_val *pair = yyjson_mut_arr(md);
    yyjson_mut_arr_append(pair, yyjson_mut_strncpy(md, id, idlen));
    yyjson_mut_arr_append(pair, params != NULL ? yyjson_val_mut_copy(md, params) : yyjson_mut_obj(md));
    yyjson_mut_arr_append(arr, pair);
    return PC_OK;
}

/* Normalise a parsed action value (array of steps, {steps|action}, droplet) into a mut [[id,p]]. */
static pc_status collect_action(yyjson_mut_doc *md, yyjson_mut_val *arr, yyjson_val *v,
                                const char *cmd, pc_error *err)
{
    yyjson_val *steps = NULL;
    if (yyjson_is_arr(v)) {
        steps = v;
    } else if (yyjson_is_obj(v)) {
        steps = pc_json_get(v, "steps");
        if (steps == NULL) {
            yyjson_val *action = pc_json_get(v, "action");
            if (action != NULL) {
                steps = pc_json_get(action, "steps");
                if (steps == NULL) {
                    steps = action;
                }
            }
        }
        if (steps == NULL) {
            return auto_bad(err, cmd, "an action object needs \"steps\"");
        }
    } else {
        return auto_bad(err, cmd, "an action must be an array of steps or an object");
    }
    if (!yyjson_is_arr(steps)) {
        return auto_bad(err, cmd, "steps must be an array");
    }
    size_t idx, max;
    yyjson_val *st;
    yyjson_arr_foreach(steps, idx, max, st) {
        pc_status rc = collect_step(md, arr, st, cmd, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return PC_OK;
}

/* Parse a text script: JSON (array/object) or one `command.id {json}` per line. Fills the mut
 * array `arr` with [[id, params]...]. */
static pc_status parse_script_text(yyjson_mut_doc *md, yyjson_mut_val *arr, const char *text,
                                   size_t len, const char *cmd, pc_error *err)
{
    /* trim leading BOM/whitespace */
    size_t i = 0;
    if (len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) {
        i = 3;
    }
    while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
        i++;
    }
    if (i < len && (text[i] == '[' || text[i] == '{')) {
        yyjson_doc *d = NULL;
        if (pc_json_parse(text + i, len - i, &d, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        pc_status rc = collect_action(md, arr, yyjson_doc_get_root(d), cmd, err);
        yyjson_doc_free(d);
        return rc;
    }
    /* line based */
    size_t line_start = i;
    size_t line_no = 0;
    for (size_t j = i; j <= len; j++) {
        if (j == len || text[j] == '\n') {
            size_t ls = line_start, le = j;
            while (ls < le && (text[ls] == ' ' || text[ls] == '\t' || text[ls] == '\r')) {
                ls++;
            }
            while (le > ls && (text[le - 1] == ' ' || text[le - 1] == '\t' || text[le - 1] == '\r')) {
                le--;
            }
            line_no++;
            line_start = j + 1;
            if (le == ls) {
                continue;
            }
            if (text[ls] == '#' || (le - ls >= 2 && text[ls] == '/' && text[ls + 1] == '/')) {
                continue;
            }
            /* split id and the rest on first whitespace */
            size_t sp = ls;
            while (sp < le && text[sp] != ' ' && text[sp] != '\t') {
                sp++;
            }
            size_t idlen = sp - ls;
            size_t rs = sp;
            while (rs < le && (text[rs] == ' ' || text[rs] == '\t')) {
                rs++;
            }
            yyjson_mut_val *pair = yyjson_mut_arr(md);
            yyjson_mut_arr_append(pair, yyjson_mut_strncpy(md, text + ls, idlen));
            if (rs < le) {
                yyjson_doc *pd = NULL;
                if (pc_json_parse(text + rs, le - rs, &pd, err) != PC_OK) {
                    return pc_engine_err_bad_params(err, cmd, "line %zu: bad JSON params", line_no);
                }
                yyjson_mut_arr_append(pair, yyjson_val_mut_copy(md, yyjson_doc_get_root(pd)));
                yyjson_doc_free(pd);
            } else {
                yyjson_mut_arr_append(pair, yyjson_mut_obj(md));
            }
            yyjson_mut_arr_append(arr, pair);
        }
    }
    return PC_OK;
}

/* every step id must be a known command. */
static pc_status check_known(yyjson_mut_val *arr, const char *cmd, pc_error *err)
{
    size_t idx, max;
    yyjson_mut_val *pair;
    yyjson_mut_arr_foreach(arr, idx, max, pair) {
        const char *id = yyjson_mut_get_str(yyjson_mut_arr_get(pair, 0));
        if (id == NULL || pc_engine_command_find(id) == NULL) {
            return pc_engine_err_bad_params(err, cmd, "unknown command `%s` in the script",
                                            id != NULL ? id : "");
        }
    }
    return PC_OK;
}

/* run the collected steps; append {command,result[,error]} to `results`. Returns whether all ok. */
static bool run_steps(pc_engine_session *s, yyjson_mut_val *arr, yyjson_mut_doc *rd,
                      yyjson_mut_val *results)
{
    bool ok = true;
    size_t idx, max;
    yyjson_mut_val *pair;
    yyjson_mut_arr_foreach(arr, idx, max, pair) {
        const char *id = yyjson_mut_get_str(yyjson_mut_arr_get(pair, 0));
        yyjson_mut_val *pm = yyjson_mut_arr_get(pair, 1);
        /* convert params to immutable for execute */
        yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
        yyjson_doc *imut = NULL;
        if (tmp != NULL && pm != NULL) {
            yyjson_mut_doc_set_root(tmp, yyjson_mut_val_mut_copy(tmp, pm));
            imut = yyjson_mut_doc_imut_copy(tmp, NULL);
        }
        yyjson_mut_doc_free(tmp);
        yyjson_val *prm = imut != NULL ? yyjson_doc_get_root(imut) : NULL;
        pc_json_value value = PC_JSON_VALUE_INIT;
        pc_error ee = {0};
        pc_status rc = pc_engine_session_execute(s, id != NULL ? id : "", prm, &value, &ee);
        yyjson_doc_free(imut);
        yyjson_mut_val *entry = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, entry, "command", id != NULL ? id : "");
        if (rc != PC_OK) {
            yyjson_mut_obj_add_strcpy(rd, entry, "error", ee.msg);
            yyjson_mut_arr_append(results, entry);
            ok = false;
            pc_json_value_fini(&value);
            break;
        }
        char *nested = pc_engine_actions_nested_failure(id, pc_json_value_root(&value));
        yyjson_mut_val *rv = pc_json_value_root(&value);
        yyjson_mut_obj_add_val(rd, entry, "result",
                               rv != NULL ? yyjson_mut_val_mut_copy(rd, rv) : yyjson_mut_null(rd));
        if (nested != NULL) {
            yyjson_mut_obj_add_strcpy(rd, entry, "error", nested);
            free(nested);
            yyjson_mut_arr_append(results, entry);
            ok = false;
            pc_json_value_fini(&value);
            break;
        }
        yyjson_mut_arr_append(results, entry);
        pc_json_value_fini(&value);
    }
    return ok;
}

/* ---- File > Scripts > Browse -------------------------------------------------------------- */

static pc_status run_browse(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "file.scripts.browse";
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    pc_status rc;
    yyjson_val *path = p != NULL ? pc_json_get(p, "path") : NULL;
    yyjson_val *script = p != NULL ? pc_json_get(p, "script") : NULL;
    yyjson_val *steps = p != NULL ? pc_json_get(p, "steps") : NULL;
    if (path != NULL) {
        size_t pl = 0;
        const char *ps = pc_json_as_str(path, &pl);
        if (ps == NULL) {
            pc_json_value_fini(out);
            return auto_bad(err, CMD, "\"path\" must be a string");
        }
        char buf[4096];
        if (pl >= sizeof buf) {
            pc_json_value_fini(out);
            return auto_bad(err, CMD, "\"path\" is too long");
        }
        memcpy(buf, ps, pl);
        buf[pl] = '\0';
        pc_buf text = PC_BUF_INIT;
        rc = pc_engine_read_file(buf, &text, err);
        if (rc != PC_OK) {
            pc_json_value_fini(out);
            return rc;
        }
        rc = parse_script_text(rd, arr, (const char *)text.data, text.len, CMD, err);
        pc_buf_fini(&text);
    } else if (script != NULL) {
        size_t sl = 0;
        const char *ss = pc_json_as_str(script, &sl);
        if (ss == NULL) {
            pc_json_value_fini(out);
            return auto_bad(err, CMD, "\"script\" must be a string");
        }
        rc = parse_script_text(rd, arr, ss, sl, CMD, err);
    } else if (steps != NULL) {
        rc = collect_action(rd, arr, steps, CMD, err);
    } else {
        pc_json_value_fini(out);
        return auto_bad(err, CMD,
                        "give \"path\" (a script file), \"script\" (its text) or \"steps\"");
    }
    if (rc != PC_OK) {
        pc_json_value_fini(out);
        return rc;
    }
    rc = check_known(arr, CMD, err);
    if (rc != PC_OK) {
        pc_json_value_fini(out);
        return rc;
    }
    size_t nsteps = yyjson_mut_arr_size(arr);
    yyjson_mut_val *results = yyjson_mut_arr(rd);
    bool ok = run_steps(s, arr, rd, results);
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "steps", nsteps);
    yyjson_mut_obj_add_bool(rd, root, "ok", ok);
    yyjson_mut_obj_add_val(rd, root, "results", results);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- Script Events Manager ---------------------------------------------------------------- */

/* The scriptEvents object in the prefs doc, or NULL. */
static yyjson_mut_val *prefs_script_events(pc_engine_session *s)
{
    if (s->prefs.values == NULL) {
        return NULL;
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(s->prefs.values);
    return root != NULL ? yyjson_mut_obj_get(root, "scriptEvents") : NULL;
}

static bool event_known(const char *id)
{
    for (size_t i = 0; i < sizeof EVENT_IDS / sizeof EVENT_IDS[0]; i++) {
        if (strcmp(EVENT_IDS[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static pc_status run_script_events(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                   pc_error *err)
{
    const char *CMD = "file.scripts.scriptEventsManager";
    yyjson_mut_doc *d = s->prefs.values;
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    yyjson_mut_val *ev = yyjson_mut_obj_get(root, "scriptEvents");
    bool changed = false;
    if (ev == NULL) {
        ev = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, ev, "enabled", false);
        yyjson_mut_obj_add_val(d, ev, "bindings", yyjson_mut_arr(d));
        yyjson_mut_obj_add(root, yyjson_mut_strcpy(d, "scriptEvents"), ev);
        changed = true;
    }
    yyjson_mut_val *bindings = yyjson_mut_obj_get(ev, "bindings");
    if (bindings == NULL || !yyjson_mut_is_arr(bindings)) {
        bindings = yyjson_mut_arr(d);
        yyjson_mut_obj_put(ev, yyjson_mut_strcpy(d, "bindings"), bindings);
        changed = true;
    }
    bool enabled_b;
    yyjson_val *env = p != NULL ? pc_json_get(p, "enabled") : NULL;
    if (env != NULL && pc_json_as_bool(env, &enabled_b)) {
        yyjson_mut_obj_put(ev, yyjson_mut_strcpy(d, "enabled"), yyjson_mut_bool(d, enabled_b));
        changed = true;
    }
    bool rall;
    yyjson_val *rav = p != NULL ? pc_json_get(p, "removeAll") : NULL;
    if (rav != NULL && pc_json_as_bool(rav, &rall) && rall) {
        yyjson_mut_obj_put(ev, yyjson_mut_strcpy(d, "bindings"), yyjson_mut_arr(d));
        bindings = yyjson_mut_obj_get(ev, "bindings");
        changed = true;
    }
    int64_t rem;
    yyjson_val *rmv = p != NULL ? pc_json_get(p, "remove") : NULL;
    if (rmv != NULL && pc_json_as_i64(rmv, &rem) && rem >= 0) {
        size_t n = yyjson_mut_arr_size(bindings);
        if ((uint64_t)rem >= n) {
            return pc_engine_err_bad_params(err, CMD, "no binding %lld", (long long)rem);
        }
        yyjson_mut_arr_remove(bindings, (size_t)rem);
        changed = true;
    }
    /* add: {event, script?|steps?, name?} or the flat form (event + script at the top level) */
    yyjson_val *add = p != NULL ? pc_json_get(p, "add") : NULL;
    yyjson_val *flat_event = p != NULL ? pc_json_get(p, "event") : NULL;
    yyjson_val *flat_script = p != NULL ? pc_json_get(p, "script") : NULL;
    const char *ev_id = NULL;
    size_t ev_len = 0;
    yyjson_val *a_script = NULL, *a_steps = NULL, *a_name = NULL, *a_enable = NULL;
    if (add != NULL && yyjson_is_obj(add)) {
        yyjson_val *e = pc_json_get(add, "event");
        ev_id = e != NULL ? pc_json_as_str(e, &ev_len) : NULL;
        a_script = pc_json_get(add, "script");
        a_steps = pc_json_get(add, "steps");
        a_name = pc_json_get(add, "name");
        a_enable = pc_json_get(add, "enable");
    } else if (flat_event != NULL && flat_script != NULL) {
        size_t fsl = 0;
        const char *fs = pc_json_as_str(flat_script, &fsl);
        if (fs != NULL && fsl > 0) {
            ev_id = pc_json_as_str(flat_event, &ev_len);
            a_script = flat_script;
            a_name = p != NULL ? pc_json_get(p, "name") : NULL;
        }
    }
    if (ev_id != NULL) {
        char eb[64];
        if (ev_len >= sizeof eb) {
            ev_len = sizeof eb - 1;
        }
        memcpy(eb, ev_id, ev_len);
        eb[ev_len] = '\0';
        if (!event_known(eb)) {
            return pc_engine_err_bad_params(err, CMD, "unknown event `%s`", eb);
        }
        if (a_script == NULL && a_steps == NULL) {
            return auto_bad(err, CMD, "add needs \"script\" (a file) or \"steps\" (an action)");
        }
        if (a_steps != NULL) {
            yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *arr = tmp != NULL ? yyjson_mut_arr(tmp) : NULL;
            pc_status rc = arr != NULL ? collect_action(tmp, arr, a_steps, CMD, err) : PC_ERR_NOMEM;
            if (rc == PC_OK) {
                rc = check_known(arr, CMD, err);
            }
            yyjson_mut_doc_free(tmp);
            if (rc != PC_OK) {
                return rc;
            }
        }
        yyjson_mut_val *b = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, b, "event", eb);
        if (a_script != NULL) {
            size_t sl = 0;
            const char *ss = pc_json_as_str(a_script, &sl);
            yyjson_mut_obj_add_strncpy(d, b, "script", ss != NULL ? ss : "", ss != NULL ? sl : 0);
        }
        if (a_steps != NULL) {
            yyjson_mut_obj_add_val(d, b, "steps", yyjson_val_mut_copy(d, a_steps));
        }
        size_t nl = 0;
        const char *nm = a_name != NULL ? pc_json_as_str(a_name, &nl) : NULL;
        if (nm != NULL && nl > 0) {
            yyjson_mut_obj_add_strncpy(d, b, "name", nm, nl);
        } else {
            yyjson_mut_obj_add_str(d, b, "name", "Action");
        }
        yyjson_mut_arr_append(bindings, b);
        bool en = true;
        if (a_enable != NULL) {
            (void)pc_json_as_bool(a_enable, &en);
        }
        if (en) {
            yyjson_mut_obj_put(ev, yyjson_mut_strcpy(d, "enabled"), yyjson_mut_bool(d, true));
        }
        changed = true;
    }
    if (changed) {
        s->prefs.rev = s->prefs.rev + 1;
    }
    /* result: {enabled, bindings, events} */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *rroot = yyjson_mut_obj(rd);
    yyjson_mut_val *enabled_v = yyjson_mut_obj_get(ev, "enabled");
    yyjson_mut_obj_add_bool(rd, rroot, "enabled",
                            enabled_v != NULL && yyjson_mut_get_bool(enabled_v));
    yyjson_mut_obj_add_val(rd, rroot, "bindings", yyjson_mut_val_mut_copy(rd, bindings));
    yyjson_mut_val *events = yyjson_mut_arr(rd);
    for (size_t i = 0; i < sizeof EVENT_IDS / sizeof EVENT_IDS[0]; i++) {
        yyjson_mut_val *eo = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_str(rd, eo, "id", EVENT_IDS[i]);
        yyjson_mut_obj_add_str(rd, eo, "label", EVENT_LABELS[i]);
        yyjson_mut_arr_append(events, eo);
    }
    yyjson_mut_obj_add_val(rd, rroot, "events", events);
    yyjson_mut_doc_set_root(rd, rroot);
    return PC_OK;
}

/* ---- fire_event + the after-command hook -------------------------------------------------- */

/* Runs the scripts bound to `event` when Script Events are enabled. Returns how many ran. */
static size_t fire_event(pc_engine_session *s, const char *event)
{
    if (s->firing) {
        return 0;
    }
    yyjson_mut_val *ev = prefs_script_events(s);
    if (ev == NULL) {
        return 0;
    }
    yyjson_mut_val *enabled = yyjson_mut_obj_get(ev, "enabled");
    if (enabled == NULL || !yyjson_mut_get_bool(enabled)) {
        return 0;
    }
    yyjson_mut_val *bindings = yyjson_mut_obj_get(ev, "bindings");
    if (bindings == NULL || !yyjson_mut_is_arr(bindings)) {
        return 0;
    }
    size_t ran = 0;
    s->firing = true;
    size_t idx, max;
    yyjson_mut_val *b;
    yyjson_mut_arr_foreach(bindings, idx, max, b) {
        const char *be = yyjson_mut_get_str(yyjson_mut_obj_get(b, "event"));
        if (be == NULL || (strcmp(be, event) != 0 && strcmp(be, "everything") != 0)) {
            continue;
        }
        /* build the steps: inline "steps" or a "script" file */
        yyjson_mut_doc *rd = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *arr = rd != NULL ? yyjson_mut_arr(rd) : NULL;
        if (arr == NULL) {
            continue;
        }
        pc_status rc = PC_ERR_INVALID;
        yyjson_mut_val *steps = yyjson_mut_obj_get(b, "steps");
        yyjson_mut_val *scriptv = yyjson_mut_obj_get(b, "script");
        if (steps != NULL) {
            /* copy the mut steps into an immutable doc then collect */
            yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
            if (tmp != NULL) {
                yyjson_mut_doc_set_root(tmp, yyjson_mut_val_mut_copy(tmp, steps));
                yyjson_doc *imut = yyjson_mut_doc_imut_copy(tmp, NULL);
                if (imut != NULL) {
                    rc = collect_action(rd, arr, yyjson_doc_get_root(imut), "scriptEvent", NULL);
                    yyjson_doc_free(imut);
                }
                yyjson_mut_doc_free(tmp);
            }
        } else if (scriptv != NULL) {
            const char *path = yyjson_mut_get_str(scriptv);
            if (path != NULL) {
                pc_buf text = PC_BUF_INIT;
                if (pc_engine_read_file(path, &text, NULL) == PC_OK) {
                    rc = parse_script_text(rd, arr, (const char *)text.data, text.len, "scriptEvent",
                                           NULL);
                }
                pc_buf_fini(&text);
            }
        }
        if (rc == PC_OK) {
            yyjson_mut_val *results = yyjson_mut_arr(rd);
            (void)run_steps(s, arr, rd, results);
            ran++;
        }
        yyjson_mut_doc_free(rd);
    }
    s->firing = false;
    return ran;
}

/* Public wrapper so print_cmds (and other modules) can fire a named Script Events event. */
size_t pc_engine_automate_fire_event(pc_engine_session *s, const char *event)
{
    return fire_event(s, event);
}

/* Hook moved from hooks.c: re-fit layer-based slices and fire the New / Close script events. */
void pc_engine_hook_automate_after_command(pc_engine_session *s, const char *id)
{
    pc_engine_slices_refresh(s);
    if (id == NULL) {
        return;
    }
    const char *event = NULL;
    if (strcmp(id, "file.new") == 0) {
        event = "newDocument";
    } else if (strcmp(id, "file.close") == 0 || strcmp(id, "file.closeAll") == 0 ||
               strcmp(id, "file.closeOthers") == 0) {
        event = "closeDocument";
    }
    if (event != NULL) {
        (void)fire_event(s, event);
    }
}

static pc_status auto_native(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

static const char *const MENU_FILE_SCRIPTS[] = {"File", "Scripts"};

static const pc_engine_command SPECS[] = {
    {"file.scripts.browse", "Browse\xE2\x80\xA6", MENU_FILE_SCRIPTS, 2, NULL,
     "{\"path\":script file | \"script\":text | \"steps\":[[id,params]\xE2\x80\xA6]} \xE2\x86\x92 {steps, "
     "ok, results}",
     auto_native, run_browse, true},
    {"file.scripts.scriptEventsManager", "Script Events Manager\xE2\x80\xA6", MENU_FILE_SCRIPTS, 2, NULL,
     "{\"enabled\":bool?,\"add\":{\"event\":\"\xE2\x80\xA6\",\"script\":path?|\"steps\":[\xE2\x80\xA6]?,"
     "\"name\":str?}?,\"remove\":index?,\"removeAll\":bool?} \xE2\x86\x92 {enabled, bindings, events}",
     auto_native, run_script_events, true},
};

const pc_engine_command *pc_engine_automate_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
