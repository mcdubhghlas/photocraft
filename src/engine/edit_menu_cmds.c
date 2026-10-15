/* Edit > Purge (port of the purge commands in crates/engine/src/edit_menu_cmds.rs): free the
 * memory held by the undo of the active document, the clipboard, every document's history, or all
 * of them (plus the compositor's effect / mask / smart-filter caches). Each reports what it freed.
 *
 * Edit > Fade (edit.fade) is deferred: it needs the Session EditState.fade source recorded by the
 * edit_menu after-command hook and a per-layer before/after diff. Reported to the owner (ET2).
 *
 * Purge never changes a document's pixels, so it is not an undoable edit; it reports the bytes it
 * freed. Never crashes on any session state (REWRITE_PLAN.md rule 9). */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "compose/compose.h"
#include "compose/masks.h"

#include "engine_internal.h"
#include "smart_cmds.h"

enum purge_what { PURGE_UNDO, PURGE_CLIPBOARD, PURGE_HISTORIES, PURGE_ALL };

/* Unique history bytes of the active document (all=false) or every document (all=true). */
static size_t history_bytes(const pc_engine_session *s, bool all)
{
    size_t total = 0;
    if (all) {
        for (size_t i = 0; i < s->ndocs; i++) {
            size_t b = 0;
            if (pc_history_unique_bytes(s->docs[i].history, pc_doc_arc_get(s->docs[i].doc), &b, NULL) ==
                PC_OK) {
                total += b;
            }
        }
        return total;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL) {
        size_t b = 0;
        if (pc_history_unique_bytes(d->history, pc_doc_arc_get(d->doc), &b, NULL) == PC_OK) {
            total += b;
        }
    }
    return total;
}

static size_t sat_sub(size_t a, size_t b)
{
    return a > b ? a - b : 0;
}

static pc_status do_purge(pc_engine_session *s, enum purge_what what, pc_json_value *out, pc_error *err)
{
    size_t freed = 0;
    const char *items[8];
    size_t nitems = 0;

    const pc_engine_doc_state *active = pc_engine_session_active(s);
    if ((what == PURGE_UNDO || what == PURGE_ALL) && active != NULL &&
        pc_history_can_undo(active->history)) {
        size_t before = history_bytes(s, false);
        pc_engine_doc_state *am = pc_engine_session_active_mut(s);
        if (am != NULL) {
            (void)pc_history_purge_last(am->history);
        }
        freed += sat_sub(before, history_bytes(s, false));
        items[nitems++] = "undo";
    }

    if ((what == PURGE_CLIPBOARD || what == PURGE_ALL) && s->clipboard != NULL) {
        freed += pc_engine_clipboard_bytes(s->clipboard);
        pc_engine_clipboard_free(s->clipboard);
        s->clipboard = NULL;
        items[nitems++] = "clipboard";
    }

    if (what == PURGE_HISTORIES || what == PURGE_ALL) {
        size_t b = history_bytes(s, true);
        bool any = false;
        for (size_t i = 0; i < s->ndocs; i++) {
            if (pc_history_can_undo(s->docs[i].history) || pc_history_can_redo(s->docs[i].history)) {
                any = true;
                break;
            }
        }
        if (any) {
            for (size_t i = 0; i < s->ndocs; i++) {
                pc_history_clear(s->docs[i].history);
                free(s->docs[i].coalesce);
                s->docs[i].coalesce = NULL;
            }
            freed += b;
            items[nitems++] = "histories";
        }
    }

    if (what == PURGE_ALL) {
        size_t fx = pc_compose_purge_effect_cache();
        if (fx > 0) {
            freed += fx;
            items[nitems++] = "effect cache";
        }
        size_t masks = pc_compose_purge_mask_cache();
        if (masks > 0) {
            freed += masks;
            items[nitems++] = "mask cache";
        }
        size_t stages = pc_engine_smart_purge_stack_cache();
        if (stages > 0) {
            freed += stages;
            items[nitems++] = "smart filter cache";
        }
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nitems; i++) {
        yyjson_mut_arr_add_strcpy(rd, arr, items[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "purged", arr);
    yyjson_mut_obj_add_uint(rd, root, "bytes", (uint64_t)freed);
    if (nitems == 0) {
        yyjson_mut_obj_add_str(rd, root, "message", "nothing to purge");
    } else {
        char msg[256];
        size_t pos = 0;
        const char prefix[] = "purged ";
        memcpy(msg, prefix, sizeof prefix - 1);
        pos = sizeof prefix - 1;
        for (size_t i = 0; i < nitems; i++) {
            if (i > 0 && pos + 2 < sizeof msg) {
                msg[pos++] = ',';
                msg[pos++] = ' ';
            }
            size_t len = strlen(items[i]);
            if (pos + len < sizeof msg) {
                memcpy(msg + pos, items[i], len);
                pos += len;
            }
        }
        msg[pos < sizeof msg ? pos : sizeof msg - 1] = '\0';
        yyjson_mut_obj_add_strcpy(rd, root, "message", msg);
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status can_purge_undo(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && pc_history_can_undo(d->history)) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "nothing to purge");
}

static pc_status can_purge_clipboard(const pc_engine_session *s, pc_error *reason)
{
    if (s->clipboard != NULL) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "the clipboard is empty");
}

static pc_status can_purge_histories(const pc_engine_session *s, pc_error *reason)
{
    for (size_t i = 0; i < s->ndocs; i++) {
        if (pc_history_can_undo(s->docs[i].history) || pc_history_can_redo(s->docs[i].history)) {
            return PC_OK;
        }
    }
    return pc_fail(reason, PC_ERR_INVALID, "no history to purge");
}

static pc_status can_purge_all(const pc_engine_session *s, pc_error *reason)
{
    if (can_purge_histories(s, NULL) == PC_OK || can_purge_clipboard(s, NULL) == PC_OK) {
        return PC_OK;
    }
    if (pc_compose_effect_cache_bytes() > 0 || pc_compose_mask_cache_bytes() > 0 ||
        pc_engine_smart_stack_cache_bytes() > 0) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "nothing to purge");
}

static pc_status always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

/* ---- run entry points --------------------------------------------------------------------- */

static pc_status run_undo(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return do_purge(s, PURGE_UNDO, out, err);
}
static pc_status run_clipboard(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return do_purge(s, PURGE_CLIPBOARD, out, err);
}
static pc_status run_histories(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return do_purge(s, PURGE_HISTORIES, out, err);
}
static pc_status run_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return do_purge(s, PURGE_ALL, out, err);
}

static pc_status run_video_cache(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_val(rd, root, "purged", yyjson_mut_arr(rd));
    yyjson_mut_obj_add_uint(rd, root, "bytes", 0);
    yyjson_mut_obj_add_str(rd, root, "message", "nothing to purge (no video layers)");
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *const MENU_EDIT_PURGE[] = {"Edit", "Purge"};

static const pc_engine_command SPECS[] = {
    {"edit.purge.undo", "Undo", MENU_EDIT_PURGE, 2, NULL, "{}", can_purge_undo, run_undo, true},
    {"edit.purge.clipboard", "Clipboard", MENU_EDIT_PURGE, 2, NULL, "{}", can_purge_clipboard,
     run_clipboard, true},
    {"edit.purge.histories", "Histories", MENU_EDIT_PURGE, 2, NULL, "{}", can_purge_histories,
     run_histories, true},
    {"edit.purge.videoCache", "Video Cache", MENU_EDIT_PURGE, 2, NULL, "{}", always, run_video_cache,
     true},
    {"edit.purge.all", "All", MENU_EDIT_PURGE, 2, NULL, "{}", can_purge_all, run_all, true},
};

const pc_engine_command *pc_engine_edit_menu_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
