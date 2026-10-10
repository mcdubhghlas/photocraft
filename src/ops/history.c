/* Undo/redo history (port of crates/ops/src/lib.rs). See ops.h.
 *
 * Every mutating function first makes every allocation it needs (label copy, layer target
 * copies, array room), then commits with steps that cannot fail, so a failure leaves the
 * history untouched. The tile counter is the exception by design: it is a cache, and an
 * update that runs out of memory discards it (drop_counter) instead of failing the step. */
#include <stdlib.h>
#include <string.h>

#include "ops/ops_internal.h"

typedef struct state {
    pc_str label;
    pc_doc_arc *doc;
    pc_layer_target layers;
} state;

struct pc_history {
    /* Past states, oldest first; the last one is the state *before* the current document. */
    state *undo;
    size_t nundo, capundo;
    /* Redo states; the last one is the next redo. */
    state *redo;
    size_t nredo, capredo;
    /* Created by trim when a byte budget is used; unlimited histories pay no counting cost. */
    pc_ops_tiles *tiles;
    size_t max_states;
    size_t max_bytes;
    pc_str current_label;           /* label of the step that produced the current document */
    pc_layer_target current_layers; /* layers targeted when the current document was created */
};

static const pc_layer_target NO_TARGET = {false, 0, NULL, 0};

static void state_fini(state *s)
{
    pc_str_fini(&s->label);
    pc_doc_arc_release(s->doc);
    s->doc = NULL;
    pc_layer_target_fini(&s->layers);
}

static void drop_counter(pc_history *h)
{
    if (h->tiles != NULL) {
        pc_ops_tiles_fini(h->tiles);
        free(h->tiles);
        h->tiles = NULL;
    }
}

static void counter_add(pc_history *h, const pc_doc_arc *doc)
{
    if (h->tiles != NULL && pc_ops_tiles_add(h->tiles, pc_doc_arc_get(doc)) != PC_OK) {
        drop_counter(h);
    }
}

static void counter_remove(pc_history *h, const pc_doc_arc *doc)
{
    if (h->tiles != NULL && pc_ops_tiles_remove(h->tiles, pc_doc_arc_get(doc)) != PC_OK) {
        drop_counter(h);
    }
}

/* Removes the first n undo states (which must exist). */
static void drop_oldest(pc_history *h, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        counter_remove(h, h->undo[i].doc);
        state_fini(&h->undo[i]);
    }
    if (n > 0) {
        memmove(h->undo, h->undo + n, (h->nundo - n) * sizeof *h->undo);
        h->nundo -= n;
    }
}

pc_status pc_history_new(size_t max_states, pc_history **out, pc_error *err)
{
    pc_history *h;
    pc_status st;
    if (out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no output");
    }
    h = pc_ops_calloc_array(1, sizeof *h);
    if (h == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    h->max_states = max_states > 1 ? max_states : 1;
    st = pc_ops_str_copy("Open", 4, &h->current_label, err);
    if (st != PC_OK) {
        free(h);
        return st;
    }
    *out = h;
    return PC_OK;
}

void pc_history_free(pc_history *h)
{
    if (h == NULL) {
        return;
    }
    for (size_t i = 0; h->undo != NULL && i < h->nundo; i++) {
        state_fini(&h->undo[i]);
    }
    for (size_t i = 0; h->redo != NULL && i < h->nredo; i++) {
        state_fini(&h->redo[i]);
    }
    free(h->undo);
    free(h->redo);
    drop_counter(h);
    pc_str_fini(&h->current_label);
    pc_layer_target_fini(&h->current_layers);
    free(h);
}

static pc_status clone_states(const state *src, size_t n, state **out, size_t *cap, pc_error *err)
{
    state *arr = NULL;
    *out = NULL;
    *cap = 0;
    if (n == 0) {
        return PC_OK;
    }
    arr = pc_ops_calloc_array(n, sizeof *arr);
    if (arr == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < n; i++) {
        pc_status st = pc_ops_str_copy(src[i].label.ptr, src[i].label.len, &arr[i].label, err);
        if (st == PC_OK) {
            st = pc_ops_layer_target_clone(&src[i].layers, &arr[i].layers, err);
        }
        if (st != PC_OK) {
            for (size_t k = 0; k <= i; k++) {
                state_fini(&arr[k]);
            }
            free(arr);
            return st;
        }
        arr[i].doc = pc_doc_arc_retain(src[i].doc);
    }
    *out = arr;
    *cap = n;
    return PC_OK;
}

pc_status pc_history_clone(const pc_history *h, pc_history **out, pc_error *err)
{
    pc_history *c;
    pc_status st;
    if (h == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no history");
    }
    c = pc_ops_calloc_array(1, sizeof *c);
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    c->max_states = h->max_states;
    c->max_bytes = h->max_bytes;
    st = clone_states(h->undo, h->nundo, &c->undo, &c->capundo, err);
    if (st == PC_OK) {
        c->nundo = h->nundo;
        st = clone_states(h->redo, h->nredo, &c->redo, &c->capredo, err);
    }
    if (st == PC_OK) {
        c->nredo = h->nredo;
        st = pc_ops_str_copy(h->current_label.ptr, h->current_label.len, &c->current_label, err);
    }
    if (st == PC_OK) {
        st = pc_ops_layer_target_clone(&h->current_layers, &c->current_layers, err);
    }
    if (st == PC_OK && h->tiles != NULL) {
        c->tiles = pc_ops_calloc_array(1, sizeof *c->tiles);
        if (c->tiles == NULL || pc_ops_tiles_clone(h->tiles, c->tiles) != PC_OK) {
            free(c->tiles);
            c->tiles = NULL;
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    if (st != PC_OK) {
        pc_history_free(c);
        return st;
    }
    *out = c;
    return PC_OK;
}

size_t pc_history_max_states(const pc_history *h)
{
    return h->max_states;
}

void pc_history_set_max_states(pc_history *h, size_t n)
{
    h->max_states = n;
}

size_t pc_history_max_bytes(const pc_history *h)
{
    return h->max_bytes;
}

void pc_history_set_max_bytes(pc_history *h, size_t n)
{
    h->max_bytes = n;
}

void pc_history_clear_redo(pc_history *h)
{
    for (size_t i = 0; i < h->nredo; i++) {
        counter_remove(h, h->redo[i].doc);
        state_fini(&h->redo[i]);
    }
    h->nredo = 0;
}

pc_status pc_history_record(pc_history *h, const char *label, size_t len, pc_doc_arc *before,
                            const pc_layer_target *layers, pc_error *err)
{
    pc_str new_label = PC_STR_INIT;
    pc_layer_target new_layers = NO_TARGET;
    pc_status st;
    state *grown;
    if (h == NULL || before == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no history or document");
    }
    st = pc_ops_str_copy(label, len, &new_label, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_ops_layer_target_clone(layers != NULL ? layers : &NO_TARGET, &new_layers, err);
    if (st != PC_OK) {
        pc_str_fini(&new_label);
        return st;
    }
    grown =
        h->nundo == SIZE_MAX ? NULL : pc_ops_grow_array(h->undo, &h->capundo, h->nundo + 1, sizeof *grown);
    if (grown == NULL) {
        pc_str_fini(&new_label);
        pc_layer_target_fini(&new_layers);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    h->undo = grown;

    if (h->max_bytes == 0) {
        drop_counter(h);
    }
    counter_add(h, before);
    h->undo[h->nundo++] = (state){h->current_label, pc_doc_arc_retain(before), h->current_layers};
    h->current_label = new_label;
    h->current_layers = new_layers;
    pc_history_clear_redo(h);
    if (h->nundo > h->max_states) {
        drop_oldest(h, h->nundo - h->max_states);
    }
    return PC_OK;
}

bool pc_history_can_undo(const pc_history *h)
{
    return h->nundo > 0;
}

bool pc_history_can_redo(const pc_history *h)
{
    return h->nredo > 0;
}

const pc_str *pc_history_undo_label(const pc_history *h)
{
    return h->nundo > 0 ? &h->current_label : NULL;
}

const pc_str *pc_history_redo_label(const pc_history *h)
{
    return h->nredo > 0 ? &h->redo[h->nredo - 1].label : NULL;
}

size_t pc_history_redo_count(const pc_history *h)
{
    return h->nredo;
}

const pc_str *pc_history_redo_label_at(const pc_history *h, size_t i)
{
    return i < h->nredo ? &h->redo[h->nredo - 1 - i].label : NULL;
}

pc_status pc_history_set_current_layers(pc_history *h, const pc_layer_target *layers, pc_error *err)
{
    pc_layer_target t;
    pc_status st = pc_ops_layer_target_clone(layers != NULL ? layers : &NO_TARGET, &t, err);
    if (st != PC_OK) {
        return st;
    }
    pc_layer_target_fini(&h->current_layers);
    h->current_layers = t;
    return PC_OK;
}

/* Undo (from = undo stack, to = redo stack) and redo (the reverse) are one move: the last state
 * of `from` becomes current, and the current document with its label and target is pushed on
 * `to`. */
static pc_status step(pc_history *h, bool undo, pc_doc_arc *current, pc_doc_arc **out_doc,
                      pc_layer_target *out_layers, pc_error *err)
{
    state **from = undo ? &h->undo : &h->redo;
    size_t *nfrom = undo ? &h->nundo : &h->nredo;
    state **to = undo ? &h->redo : &h->undo;
    size_t *nto = undo ? &h->nredo : &h->nundo;
    size_t *capto = undo ? &h->capredo : &h->capundo;
    pc_layer_target copy = NO_TARGET;
    state *grown;
    state prev;
    if (*nfrom == 0) {
        *out_doc = NULL;
        if (out_layers != NULL) {
            *out_layers = NO_TARGET;
        }
        return PC_OK;
    }
    if (out_layers != NULL) {
        pc_status st = pc_ops_layer_target_clone(&(*from)[*nfrom - 1].layers, &copy, err);
        if (st != PC_OK) {
            return st;
        }
    }
    grown = *nto == SIZE_MAX ? NULL : pc_ops_grow_array(*to, capto, *nto + 1, sizeof *grown);
    if (grown == NULL) {
        pc_layer_target_fini(&copy);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *to = grown;

    prev = (*from)[--*nfrom];
    counter_remove(h, prev.doc);
    counter_add(h, current);
    (*to)[(*nto)++] = (state){h->current_label, pc_doc_arc_retain(current), h->current_layers};
    h->current_label = prev.label;
    h->current_layers = prev.layers;
    *out_doc = prev.doc;
    if (out_layers != NULL) {
        *out_layers = copy;
    }
    return PC_OK;
}

pc_status pc_history_undo(pc_history *h, pc_doc_arc *current, pc_doc_arc **out_doc,
                          pc_layer_target *out_layers, pc_error *err)
{
    if (h == NULL || current == NULL || out_doc == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no history or document");
    }
    return step(h, true, current, out_doc, out_layers, err);
}

pc_status pc_history_redo(pc_history *h, pc_doc_arc *current, pc_doc_arc **out_doc,
                          pc_layer_target *out_layers, pc_error *err)
{
    if (h == NULL || current == NULL || out_doc == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no history or document");
    }
    return step(h, false, current, out_doc, out_layers, err);
}

size_t pc_history_entry_count(const pc_history *h)
{
    return h->nundo + 1;
}

const pc_str *pc_history_entry(const pc_history *h, size_t i)
{
    if (i < h->nundo) {
        return &h->undo[i].label;
    }
    return i == h->nundo ? &h->current_label : NULL;
}

pc_doc_arc *pc_history_state(const pc_history *h, size_t i)
{
    return i < h->nundo ? h->undo[i].doc : NULL;
}

size_t pc_history_past_len(const pc_history *h)
{
    return h->nundo;
}

pc_status pc_history_set_current_label(pc_history *h, const char *label, size_t len, pc_error *err)
{
    pc_str s;
    pc_status st = pc_ops_str_copy(label, len, &s, err);
    if (st != PC_OK) {
        return st;
    }
    pc_str_fini(&h->current_label);
    h->current_label = s;
    return PC_OK;
}

bool pc_history_purge_last(pc_history *h)
{
    pc_history_clear_redo(h);
    if (h->nundo == 0) {
        return false;
    }
    h->nundo--;
    counter_remove(h, h->undo[h->nundo].doc);
    state_fini(&h->undo[h->nundo]);
    return true;
}

void pc_history_clear(pc_history *h)
{
    for (size_t i = 0; i < h->nundo; i++) {
        state_fini(&h->undo[i]);
    }
    for (size_t i = 0; i < h->nredo; i++) {
        state_fini(&h->redo[i]);
    }
    h->nundo = 0;
    h->nredo = 0;
    drop_counter(h);
}

static size_t sat_add(size_t a, size_t b)
{
    return a > SIZE_MAX - b ? SIZE_MAX : a + b;
}

/* Bytes of current's tiles (*own) and of the states' tiles not in current (*rest, undo then
 * redo; each tile once), as the reference's unique_bytes / pixel_bytes fold them. */
static pc_status union_bytes(const pc_history *h, const pc_document *current, size_t *own, size_t *rest,
                             pc_error *err)
{
    pc_ops_ptrset seen = {NULL, 0, 0};
    size_t mine, n, total = 0;
    pc_status st = pc_ops_tile_bytes(current, &seen, &mine);
    for (size_t i = 0; i < h->nundo + h->nredo && st == PC_OK; i++) {
        const state *s = i < h->nundo ? &h->undo[i] : &h->redo[i - h->nundo];
        st = pc_ops_tile_bytes(pc_doc_arc_get(s->doc), &seen, &n);
        total = sat_add(total, n);
    }
    pc_ops_ptrset_fini(&seen);
    if (st != PC_OK) {
        return pc_fail(err, st, "out of memory");
    }
    *own = mine;
    *rest = total;
    return PC_OK;
}

pc_status pc_history_unique_bytes(const pc_history *h, const pc_document *current, size_t *out, pc_error *err)
{
    size_t own, rest;
    pc_status st;
    if (h == NULL || current == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no history or document");
    }
    st = union_bytes(h, current, &own, &rest, err);
    if (st == PC_OK) {
        *out = rest;
    }
    return st;
}

pc_status pc_history_pixel_bytes(const pc_history *h, const pc_document *current, size_t *out, pc_error *err)
{
    size_t own, rest;
    pc_status st;
    if (h == NULL || current == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no history or document");
    }
    if (h->tiles != NULL) {
        uint64_t b;
        if (pc_ops_tiles_bytes_with(h->tiles, current, &b) == PC_OK) {
            *out = b > SIZE_MAX ? SIZE_MAX : (size_t)b;
            return PC_OK;
        }
        /* Out of memory for the set: the exact count below gives the same number. */
    }
    st = union_bytes(h, current, &own, &rest, err);
    if (st == PC_OK) {
        *out = sat_add(own, rest);
    }
    return st;
}

pc_status pc_history_trim(pc_history *h, const pc_document *current, size_t *dropped, pc_error *err)
{
    pc_ops_ptrset seen = {NULL, 0, 0};
    size_t total, n, keep = 0, drop;
    pc_status st;
    if (h == NULL || current == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no history or document");
    }
    if (h->max_bytes == 0 || h->nundo <= 1) {
        if (dropped != NULL) {
            *dropped = 0;
        }
        return PC_OK;
    }
    if (h->tiles == NULL) {
        h->tiles = pc_ops_calloc_array(1, sizeof *h->tiles);
        for (size_t i = 0; i < h->nundo + h->nredo && h->tiles != NULL; i++) {
            const state *s = i < h->nundo ? &h->undo[i] : &h->redo[i - h->nundo];
            counter_add(h, s->doc);
        }
    }
    if (h->tiles != NULL) {
        /* Retained bytes are incremental; only current's tiles need examining below the limit.
         * Current can change without recording (coalescing), and tile transfers can carry a
         * different byte length, so a tile-count estimate must not underestimate its memory. */
        uint64_t with;
        if (h->tiles->bytes <= (uint64_t)h->max_bytes &&
            pc_ops_tiles_bytes_with(h->tiles, current, &with) == PC_OK && with <= (uint64_t)h->max_bytes) {
            if (dropped != NULL) {
                *dropped = 0;
            }
            return PC_OK;
        }
    }
    st = pc_ops_tile_bytes(current, &seen, &total);
    for (size_t i = h->nredo; i > 0 && st == PC_OK; i--) {
        st = pc_ops_tile_bytes(pc_doc_arc_get(h->redo[i - 1].doc), &seen, &n);
        total = sat_add(total, n);
    }
    for (size_t i = h->nundo; i > 0 && st == PC_OK; i--) {
        st = pc_ops_tile_bytes(pc_doc_arc_get(h->undo[i - 1].doc), &seen, &n);
        if (st != PC_OK) {
            break;
        }
        total = sat_add(total, n);
        if (total > h->max_bytes && keep >= 1) {
            break;
        }
        keep++;
    }
    pc_ops_ptrset_fini(&seen);
    if (st != PC_OK) {
        return pc_fail(err, st, "out of memory");
    }
    drop = h->nundo - keep;
    drop_oldest(h, drop);
    if (dropped != NULL) {
        *dropped = drop;
    }
    return PC_OK;
}

const pc_ops_tiles *pc_ops_history_counter(const pc_history *h)
{
    return h->tiles;
}
