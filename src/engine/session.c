/* The session: open documents, the active one, history, and the edit/undo/redo core
 * (port of crates/engine/src/lib.rs Session, DocState, ToolState). The command registry,
 * dispatch and the job table live in command.c and jobs.c. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "engine_internal.h"
#include "prefs.h"
#include "preset_store.h" /* E2b: free the attached brush-preset store */

/* ---- small owned-string helpers ----------------------------------------------------------- */

static char *dup_cstr(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    return pc_strndup(s, strlen(s));
}

static bool cstr_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

/* ---- layer-id vectors --------------------------------------------------------------------- */

static bool ids_contains(const pc_layer_id *v, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++) {
        if (v[i] == id) {
            return true;
        }
    }
    return false;
}

static pc_layer_id *ids_dup(const pc_layer_id *v, size_t n)
{
    pc_layer_id *out = pc_malloc_array(n == 0 ? 1 : n, sizeof *out);
    if (out != NULL && n > 0) {
        memcpy(out, v, n * sizeof *out);
    }
    return out;
}

/* ---- ToolState ---------------------------------------------------------------------------- */

static void tool_state_default(pc_engine_tool_state *t)
{
    t->foreground[0] = 0.0f;
    t->foreground[1] = 0.0f;
    t->foreground[2] = 0.0f;
    t->foreground[3] = 1.0f;
    t->background[0] = 1.0f;
    t->background[1] = 1.0f;
    t->background[2] = 1.0f;
    t->background[3] = 1.0f;
}

/* ---- DocState ----------------------------------------------------------------------------- */

/* A pc_layer_target mirroring the state's active/selected layers (owns a copy of selected). */
static pc_status doc_state_layer_target(const pc_engine_doc_state *st, pc_layer_target *out, pc_error *err)
{
    out->has_active = st->has_active_layer;
    out->active = st->active_layer;
    out->nselected = st->nselected;
    out->selected = NULL;
    if (st->nselected > 0) {
        out->selected = ids_dup(st->selected_layers, st->nselected);
        if (out->selected == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "layer target");
        }
    }
    return PC_OK;
}

pc_status pc_engine_doc_state_init(pc_engine_doc_state *st, pc_document *doc, const char *path,
                                   size_t max_states, size_t max_bytes, pc_error *err)
{
    memset(st, 0, sizeof *st);
    pc_layer_id top;
    bool has_top = pc_document_top_layer(doc, &top);
    if (pc_doc_arc_new(doc, &st->doc, err) != PC_OK) { /* moves doc on success */
        return PC_ERR_NOMEM;
    }
    if (pc_history_new(max_states, &st->history, err) != PC_OK) {
        pc_doc_arc_release(st->doc);
        st->doc = NULL;
        return PC_ERR_NOMEM;
    }
    pc_history_set_max_bytes(st->history, max_bytes);
    st->has_active_layer = has_top;
    st->active_layer = top;
    st->has_anchor = has_top;
    st->layer_anchor = top;
    if (has_top) {
        st->selected_layers = ids_dup(&top, 1);
        if (st->selected_layers == NULL) {
            pc_history_free(st->history);
            pc_doc_arc_release(st->doc);
            memset(st, 0, sizeof *st);
            return pc_fail(err, PC_ERR_NOMEM, "doc state");
        }
        st->nselected = 1;
    }
    /* history.set_current_layers({active, selected}) */
    pc_layer_target tgt = {0};
    if (doc_state_layer_target(st, &tgt, err) == PC_OK) {
        (void)pc_history_set_current_layers(st->history, &tgt, err);
        pc_layer_target_fini(&tgt);
    }
    st->path = dup_cstr(path);
    st->revision = 1;
    st->saved_revision = 1;
    return PC_OK;
}

void pc_engine_doc_state_fini(pc_engine_doc_state *st)
{
    if (st == NULL) {
        return;
    }
    pc_doc_arc_release(st->doc);
    pc_history_free(st->history);
    free(st->selected_layers);
    free(st->isolated_layers); /* E5 */
    free(st->fx_collapsed); /* E9 */
    pc_engine_channel_view_free(st->channel_view); /* E7 */
    pc_engine_symmetry_free(st->symmetry); /* E10 */
    pc_engine_floating_free(st->floating); /* E4 */
    free(st->color_samplers); /* EF: Color Sampler points */
    free(st->path);
    free(st->coalesce);
    memset(st, 0, sizeof *st);
}

bool pc_engine_doc_state_is_dirty(const pc_engine_doc_state *d)
{
    return d->revision != d->saved_revision;
}

pc_status pc_engine_doc_state_selected_layers(const pc_engine_doc_state *d, pc_layer_id **out,
                                              size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    if (!d->has_active_layer) {
        return PC_OK;
    }
    const pc_layer_id *set = d->selected_layers;
    size_t setn = d->nselected;
    if (!ids_contains(set, setn, d->active_layer)) {
        set = &d->active_layer;
        setn = 1;
    }
    pc_walk w = {0};
    if (pc_document_walk(pc_doc_arc_get(d->doc), &w, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_layer_id *res = pc_malloc_array(w.len == 0 ? 1 : w.len, sizeof *res);
    if (res == NULL) {
        pc_walk_fini(&w);
        return pc_fail(err, PC_ERR_NOMEM, "selected layers");
    }
    size_t count = 0;
    for (size_t i = 0; i < w.len; i++) {
        pc_layer_id id = w.items[i].layer->id;
        if (ids_contains(set, setn, id)) {
            res[count++] = id;
        }
    }
    pc_walk_fini(&w);
    *out = res;
    *n = count;
    return PC_OK;
}

/* Keep the layer selection valid after the document or active layer changed (fix_selection). */
void pc_engine_fix_selection(pc_engine_doc_state *st)
{
    if (!st->has_active_layer) {
        free(st->selected_layers);
        st->selected_layers = NULL;
        st->nselected = 0;
        return;
    }
    pc_layer_id active = st->active_layer;
    if (!ids_contains(st->selected_layers, st->nselected, active)) {
        pc_layer_id *one = ids_dup(&active, 1);
        if (one != NULL) {
            free(st->selected_layers);
            st->selected_layers = one;
            st->nselected = 1;
        }
    } else {
        pc_walk w = {0};
        if (pc_document_walk(pc_doc_arc_get(st->doc), &w, NULL) == PC_OK) {
            size_t out = 0;
            for (size_t i = 0; i < st->nselected; i++) {
                pc_layer_id id = st->selected_layers[i];
                bool live = false;
                for (size_t j = 0; j < w.len; j++) {
                    if (w.items[j].layer->id == id) {
                        live = true;
                        break;
                    }
                }
                if (!live) {
                    continue;
                }
                if (!ids_contains(st->selected_layers, out, id)) { /* dedup */
                    st->selected_layers[out++] = id;
                }
            }
            st->nselected = out;
            pc_walk_fini(&w);
        }
    }
    if (st->has_anchor && !ids_contains(st->selected_layers, st->nselected, st->layer_anchor)) {
        st->layer_anchor = active;
    }
}

static void fix_active(pc_engine_doc_state *st)
{
    if (!st->has_active_layer || pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer) == NULL) {
        pc_layer_id top;
        st->has_active_layer = pc_document_top_layer(pc_doc_arc_get(st->doc), &top);
        st->active_layer = top;
    }
    pc_engine_fix_selection(st);
    pc_engine_hook_fix_view(st);
}

/* Undo/redo: target the layers the restored state targeted, as far as they still exist. */
static void restore_target(pc_engine_doc_state *st, const pc_layer_target *layers)
{
    if (!layers->has_active && layers->nselected == 0) {
        st->has_active_layer = false;
        free(st->selected_layers);
        st->selected_layers = NULL;
        st->nselected = 0;
        st->has_anchor = false;
        pc_engine_hook_fix_view(st);
        return;
    }
    if (layers->has_active && pc_document_layer(pc_doc_arc_get(st->doc), layers->active) != NULL) {
        st->has_active_layer = true;
        st->active_layer = layers->active;
        pc_layer_id *copy = ids_dup(layers->selected, layers->nselected);
        if (copy != NULL) {
            free(st->selected_layers);
            st->selected_layers = copy;
            st->nselected = layers->nselected;
        }
    }
    fix_active(st);
}

/* ---- Session lifecycle -------------------------------------------------------------------- */

pc_status pc_engine_session_new(pc_engine_session **out, pc_error *err)
{
    pc_engine_session *s = pc_calloc_array(1, sizeof *s);
    if (s == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "session");
    }
    tool_state_default(&s->tools);
    if (!pc_engine_tool_state_painting_default(&s->tools)) { /* E10: brush, presets, mixer */
        free(s);
        return pc_fail(err, PC_ERR_NOMEM, "session");
    }
    s->color_restrict = SIZE_MAX;
    /* E7: Quick Mask Options default -- red overlay at 50 % over masked areas. */
    s->quick_mask_color = PC_ALPHA_CHANNEL_DEFAULT_COLOR;
    s->quick_mask_opacity = 0.5f;
    s->quick_mask_indicates = PC_INDICATES_MASKED_AREAS;
    pc_engine_jobs_init(&s->jobs);
    pc_engine_prefs_init(&s->prefs); /* E2 */
    pc_preset_state_init(&s->presets); /* E2b: preset-panel defaults */
    pc_engine_pattern_library_init(s); /* E10: Window > Patterns built-ins + panel folders */
    pc_engine_color_init(&s->color); /* E8 */
    if (s->prefs.values == NULL) {
        pc_engine_pattern_library_fini(s);
        pc_preset_state_fini(&s->presets);
        pc_engine_color_fini(&s->color);
        pc_engine_jobs_fini(&s->jobs);
        pc_engine_tool_state_fini(&s->tools); /* E10 */
        free(s);
        return pc_fail(err, PC_ERR_NOMEM, "session");
    }
    *out = s;
    return PC_OK;
}

void pc_engine_session_free(pc_engine_session *s)
{
    if (s == NULL) {
        return;
    }
    pc_engine_jobs_fini(&s->jobs);
    pc_engine_prefs_fini(&s->prefs); /* E2 */
    pc_engine_preset_store_free(s->preset_store); /* E2b */
    s->preset_store = NULL;
    pc_engine_pattern_library_fini(s); /* E10 */
    pc_preset_state_fini(&s->presets); /* E2b */
    pc_engine_color_fini(&s->color); /* E8 */
    pc_engine_tool_state_fini(&s->tools); /* E10 */
    pc_engine_vector_clipboard_fini(s);   /* E11 */
    pc_engine_type_defaults_fini(s);      /* E11 (type) */
    pc_engine_clipboard_free(s->clipboard); /* E4 */
    s->clipboard = NULL;
    pc_engine_analysis_fini(s);           /* E8 (analysis) */
    pc_engine_actions_session_fini(s);    /* E12 (Actions) */
    free(s->image_assets);                /* E12 (automate FileMenuState) */
    pc_json_value_fini(&s->last_print);
    pc_json_value_fini(&s->last_web);
    for (size_t i = 0; i < s->nevent_log; i++) {
        pc_json_value_fini(&s->event_log[i]);
    }
    free(s->event_log);
    free(s->smart_links);         /* EF (E6): Edit-Contents links */
    for (size_t i = 0; i < s->nlink_stamps; i++) { /* EF (E6): link-watch stamps */
        free(s->link_stamps[i].path);
    }
    free(s->link_stamps);
    free(s->fade_label);          /* EF (ET2): Edit > Fade source */
    pc_engine_edit_state_fini(s); /* ET2: custom shape library */
    for (size_t i = 0; i < s->ndocs; i++) {
        pc_engine_doc_state_fini(&s->docs[i]);
    }
    free(s->docs);
    for (size_t i = 0; i < s->njournal; i++) {
        free(s->journal[i].id);
        pc_json_value_fini(&s->journal[i].params);
    }
    free(s->journal);
    free(s->coalesce_request);
    free(s);
}

const pc_engine_doc_state *pc_engine_session_documents(const pc_engine_session *s, size_t *n)
{
    *n = s->ndocs;
    return s->docs;
}

bool pc_engine_session_active_index(const pc_engine_session *s, size_t *out)
{
    if (s->has_active) {
        *out = s->active;
    }
    return s->has_active;
}

const pc_engine_doc_state *pc_engine_session_active(const pc_engine_session *s)
{
    return s->has_active && s->active < s->ndocs ? &s->docs[s->active] : NULL;
}

pc_engine_doc_state *pc_engine_session_active_mut(pc_engine_session *s)
{
    return s->has_active && s->active < s->ndocs ? &s->docs[s->active] : NULL;
}

const pc_engine_tool_state *pc_engine_session_tools(const pc_engine_session *s)
{
    return &s->tools;
}

bool pc_engine_session_set_active(pc_engine_session *s, size_t index)
{
    if (index >= s->ndocs) {
        return false;
    }
    s->has_active = true;
    s->active = index;
    return true;
}

size_t pc_engine_session_journal_len(const pc_engine_session *s)
{
    return s->njournal;
}

bool pc_engine_session_journal_at(const pc_engine_session *s, size_t i, const char **id,
                                  const yyjson_mut_val **params)
{
    if (i >= s->njournal) {
        return false;
    }
    if (id != NULL) {
        *id = s->journal[i].id;
    }
    if (params != NULL) {
        *params = pc_json_value_root(&s->journal[i].params);
    }
    return true;
}

/* Record one journal entry (id + owned params copy). */
pc_status pc_engine_journal_push(pc_engine_session *s, const char *id, yyjson_val *params, pc_error *err)
{
    pc_engine_journal_entry *grown =
        pc_grow_array(s->journal, &s->cap_journal, s->njournal + 1, sizeof *s->journal);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "journal");
    }
    s->journal = grown;
    pc_engine_journal_entry *e = &s->journal[s->njournal];
    e->id = dup_cstr(id);
    memset(&e->params, 0, sizeof e->params);
    if (params != NULL) {
        (void)pc_json_value_from_val(params, &e->params, NULL);
    }
    if (e->id == NULL) {
        pc_json_value_fini(&e->params);
        return pc_fail(err, PC_ERR_NOMEM, "journal id");
    }
    s->njournal++;
    return PC_OK;
}

pc_status pc_engine_session_add_document(pc_engine_session *s, pc_document *doc, const char *path,
                                         size_t *out_index, pc_error *err)
{
    /* Persisted ids can overlap the allocator; keep the document's id unique among open ones. */
    for (;;) {
        bool clash = false;
        for (size_t i = 0; i < s->ndocs; i++) {
            if (pc_doc_arc_get(s->docs[i].doc)->id == doc->id) {
                clash = true;
                break;
            }
        }
        if (!clash) {
            break;
        }
        doc->id = pc_doc_id_fresh();
    }
    pc_engine_doc_state *grown = pc_grow_array(s->docs, &s->cap_docs, s->ndocs + 1, sizeof *s->docs);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "documents");
    }
    s->docs = grown;
    pc_engine_doc_state *st = &s->docs[s->ndocs];
    /* New documents start with the history limits from the performance preferences (E2), as the
     * reference's Session::add_document does. */
    size_t max_states = PC_HISTORY_DEFAULT_MAX_STATES, max_bytes = 0;
    if (s->prefs.values != NULL) {
        pc_engine_prefs_history_limits(s, &max_states, &max_bytes);
    }
    if (pc_engine_doc_state_init(st, doc, path, max_states, max_bytes, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    s->ndocs++;
    s->has_active = true;
    s->active = s->ndocs - 1;
    if (out_index != NULL) {
        *out_index = s->active;
    }
    return PC_OK;
}

bool pc_engine_session_close(pc_engine_session *s, size_t index)
{
    if (index >= s->ndocs) {
        return false;
    }
    pc_engine_hook_smart_on_close(s, index);
    pc_engine_cancel_jobs_on(s, pc_doc_arc_get(s->docs[index].doc)->id);
    pc_engine_doc_state_fini(&s->docs[index]);
    memmove(&s->docs[index], &s->docs[index + 1], (s->ndocs - index - 1) * sizeof *s->docs);
    s->ndocs--;
    if (s->ndocs == 0) {
        s->has_active = false;
    } else if (s->has_active) {
        if (s->active > index) {
            s->active--;
        } else if (s->active == index) {
            s->active = index < s->ndocs ? index : s->ndocs - 1;
        }
    }
    return true;
}

bool pc_engine_move_layer_id(pc_layer_id *v, size_t n, size_t i, size_t to, size_t *out_to)
{
    if (i >= n) {
        return false;
    }
    pc_layer_id x = v[i];
    memmove(&v[i], &v[i + 1], (n - i - 1) * sizeof *v);
    if (to > n - 1) {
        to = n - 1;
    }
    memmove(&v[to + 1], &v[to], (n - 1 - to) * sizeof *v);
    v[to] = x;
    if (out_to != NULL) {
        *out_to = to;
    }
    return true;
}

bool pc_engine_session_move_document(pc_engine_session *s, size_t from, size_t to, size_t *out_to)
{
    if (from >= s->ndocs) {
        return false;
    }
    pc_doc_id active_id = 0;
    bool had_active = s->has_active && s->active < s->ndocs;
    if (had_active) {
        active_id = pc_doc_arc_get(s->docs[s->active].doc)->id;
    }
    pc_engine_doc_state moved = s->docs[from];
    memmove(&s->docs[from], &s->docs[from + 1], (s->ndocs - from - 1) * sizeof *s->docs);
    if (to > s->ndocs - 1) {
        to = s->ndocs - 1;
    }
    memmove(&s->docs[to + 1], &s->docs[to], (s->ndocs - 1 - to) * sizeof *s->docs);
    s->docs[to] = moved;
    if (had_active) {
        for (size_t i = 0; i < s->ndocs; i++) {
            if (pc_doc_arc_get(s->docs[i].doc)->id == active_id) {
                s->active = i;
                break;
            }
        }
    }
    if (out_to != NULL) {
        *out_to = to;
    }
    return true;
}

/* ---- edit / undo / redo ------------------------------------------------------------------- */

/* Floor of v to an i32, saturating (NaN -> INT32_MIN); keeps the float->int cast in range. */
static int32_t floor_i32_sat(double v)
{
    double f = floor(v);
    if (!(f >= (double)INT32_MIN)) { /* also catches NaN */
        return INT32_MIN;
    }
    if (f > (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)f;
}

/* EF: after a canvas-size edit, move the Color Sampler points with their pixels (had_map, through
 * `map`) or -- for any other size change -- drop the ones now outside the canvas. Accumulates how
 * many were removed into s->samplers_removed. View state: not a history step. Port of the sampler
 * block in Session::edit (crates/engine/src/lib.rs). */
static void apply_sampler_map(pc_engine_session *s, pc_engine_doc_state *st, bool had_map,
                              pc_affine map, pc_size before_size)
{
    if (st->ncolor_samplers == 0) {
        return;
    }
    pc_rect bounds = pc_document_bounds(pc_doc_arc_get(st->doc));
    pc_size after_size = pc_doc_arc_get(st->doc)->size;
    if (!had_map && after_size.width == before_size.width && after_size.height == before_size.height) {
        return;
    }
    size_t before = st->ncolor_samplers;
    size_t out = 0;
    for (size_t i = 0; i < st->ncolor_samplers; i++) {
        double x = st->color_samplers[i][0];
        double y = st->color_samplers[i][1];
        if (had_map) {
            pc_point q = pc_affine_apply(map, (pc_point){x, y});
            x = q.x;
            y = q.y;
        }
        if (pc_rect_contains(bounds, floor_i32_sat(x), floor_i32_sat(y))) {
            st->color_samplers[out][0] = x;
            st->color_samplers[out][1] = y;
            out++;
        }
    }
    st->ncolor_samplers = out;
    s->samplers_removed += before - out;
}

pc_status pc_engine_session_edit(pc_engine_session *s, const char *label, pc_engine_edit_fn f,
                                 void *ctx, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    /* EF: consume the pending Color Sampler map (set by crop/Canvas Size/Image Size/rotation before
     * this edit), as Session::edit does with sampler_map.take(). */
    bool sampler_had_map = s->has_sampler_map;
    pc_affine sampler_map = s->sampler_map;
    s->has_sampler_map = false;
    pc_doc_arc *before = pc_doc_arc_retain(st->doc);
    pc_size before_size = pc_doc_arc_get(before)->size;
    pc_layer_target prior = {0};
    if (doc_state_layer_target(st, &prior, err) != PC_OK) {
        pc_doc_arc_release(before);
        return PC_ERR_NOMEM;
    }
    pc_document doc;
    if (pc_document_clone(pc_doc_arc_get(before), &doc, err) != PC_OK) {
        pc_layer_target_fini(&prior);
        pc_doc_arc_release(before);
        return PC_ERR_NOMEM;
    }
    bool has_active = st->has_active_layer;
    pc_layer_id active = st->active_layer;
    pc_status rc = f(&doc, &has_active, &active, ctx, err);
    if (rc != PC_OK) {
        pc_document_fini(&doc);
        pc_layer_target_fini(&prior);
        pc_doc_arc_release(before);
        return rc;
    }
    if (s->color_restrict != SIZE_MAX && has_active) {
        pc_engine_hook_restrict_to_color(pc_doc_arc_get(before), &doc, active, s->color_restrict);
    }
    pc_doc_arc *newarc = NULL;
    if (pc_doc_arc_new(&doc, &newarc, err) != PC_OK) { /* moves doc on success */
        pc_document_fini(&doc);
        pc_layer_target_fini(&prior);
        pc_doc_arc_release(before);
        return PC_ERR_NOMEM;
    }
    pc_doc_arc_release(st->doc);
    st->doc = newarc;
    st->has_active_layer = has_active;
    st->active_layer = active;
    pc_engine_fix_selection(st);
    pc_engine_hook_fix_view(st);
    apply_sampler_map(s, st, sampler_had_map, sampler_map, before_size); /* EF: Color Sampler points */

    bool dmg_present = false;
    pc_rect dmg = PC_RECT_EMPTY;
    pc_engine_hook_step_damage(pc_doc_arc_get(before), pc_doc_arc_get(st->doc), &dmg_present, &dmg);

    char *key = dup_cstr(s->coalesce_request);
    pc_layer_target layers = {0};
    if (doc_state_layer_target(st, &layers, err) != PC_OK) {
        free(key);
        pc_layer_target_fini(&prior);
        pc_doc_arc_release(before);
        return PC_ERR_NOMEM;
    }
    bool coalesce_into_prev =
        key != NULL && cstr_eq(st->coalesce, key) && pc_history_can_undo(st->history);
    if (!coalesce_into_prev) {
        (void)pc_history_set_current_layers(st->history, &prior, NULL);
        (void)pc_history_record(st->history, label, strlen(label), before, &layers, err);
        (void)pc_history_trim(st->history, pc_doc_arc_get(st->doc), NULL, NULL);
    } else {
        (void)pc_history_set_current_layers(st->history, &layers, NULL);
    }
    free(st->coalesce);
    st->coalesce = key;
    st->revision++;
    st->has_last_damage = dmg_present;
    st->last_damage = dmg;

    pc_layer_target_fini(&layers);
    pc_layer_target_fini(&prior);
    pc_doc_arc_release(before);
    return PC_OK;
}

pc_status pc_engine_session_select_layer(pc_engine_session *s, pc_layer_id id, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (pc_document_layer(pc_doc_arc_get(st->doc), id) == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    st->has_active_layer = true;
    st->active_layer = id;
    pc_layer_id *one = ids_dup(&id, 1);
    if (one == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "select");
    }
    free(st->selected_layers);
    st->selected_layers = one;
    st->nselected = 1;
    st->has_anchor = true;
    st->layer_anchor = id;
    pc_engine_hook_fix_view(st);
    bool clean = st->saved_revision == st->revision;
    st->revision++;
    if (clean) {
        st->saved_revision = st->revision;
    }
    st->has_last_damage = true;
    st->last_damage = PC_RECT_EMPTY;
    return PC_OK;
}

/* current (the arc to make redoable) and *out (undo or redo on the history). */
static bool do_history_move(pc_engine_session *s, bool redo)
{
    if (pc_engine_active_job(s)) { /* a job computes from the current state; don't move it */
        return false;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return false;
    }
    free(st->coalesce);
    st->coalesce = NULL;
    pc_doc_arc *newdoc = NULL;
    pc_layer_target layers = {0};
    pc_status rc = redo ? pc_history_redo(st->history, st->doc, &newdoc, &layers, NULL)
                        : pc_history_undo(st->history, st->doc, &newdoc, &layers, NULL);
    if (rc != PC_OK || newdoc == NULL) {
        pc_layer_target_fini(&layers);
        return false;
    }
    /* Save As changes the name outside history: keep the current name across the move. */
    const pc_document *nd = pc_doc_arc_get(newdoc);
    const pc_document *cur = pc_doc_arc_get(st->doc);
    if (!pc_str_eq(&nd->name, &cur->name)) {
        pc_document *mut_nd = NULL;
        if (pc_doc_arc_make_mut(&newdoc, &mut_nd, NULL) == PC_OK) {
            pc_str copy = {0};
            if (pc_str_clone(&cur->name, &copy, NULL) == PC_OK) {
                pc_str_fini(&mut_nd->name);
                mut_nd->name = copy;
            }
        }
    }
    bool dmg_present = false;
    pc_rect dmg = PC_RECT_EMPTY;
    if (redo) {
        pc_engine_hook_step_damage(pc_doc_arc_get(st->doc), pc_doc_arc_get(newdoc), &dmg_present, &dmg);
    } else {
        pc_engine_hook_step_damage(pc_doc_arc_get(newdoc), pc_doc_arc_get(st->doc), &dmg_present, &dmg);
    }
    pc_doc_arc_release(st->doc);
    st->doc = newdoc;
    restore_target(st, &layers);
    st->revision++;
    st->has_last_damage = dmg_present;
    st->last_damage = dmg;
    pc_layer_target_fini(&layers);
    return true;
}

bool pc_engine_session_undo(pc_engine_session *s)
{
    return do_history_move(s, false);
}

bool pc_engine_session_redo(pc_engine_session *s)
{
    return do_history_move(s, true);
}
