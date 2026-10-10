/* Undo/redo history: every Rust test of crates/ops ported (lib.rs #[cfg(test)]), plus
 * never-crash tests (bad indices, empty history, NULL arguments, allocation failures at every
 * allocation, deep documents) and a threaded retain/release check for pc_doc_arc. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "ops/ops.h"
#include "ops/ops_internal.h"

#define TILE ((size_t)256 * 256 * 4)

static pc_doc_arc *doc_with_bg(pc_size size, pc_sample_type depth)
{
    pc_document d;
    pc_doc_arc *a = NULL;
    if (pc_document_with_background(&d, "h", size, PC_COLOR_MODE_RGB, depth, PC_COLOR_WHITE, NULL) != PC_OK) {
        return NULL;
    }
    if (pc_doc_arc_new(&d, &a, NULL) != PC_OK) {
        pc_document_fini(&d);
        return NULL;
    }
    return a;
}

static pc_doc_arc *base(void)
{
    return doc_with_bg(pc_size_make(64, 64), PC_SAMPLE_TYPE_U8);
}

static pc_history *hist(size_t max_states)
{
    pc_history *h = NULL;
    CHECK_EQ_INT(pc_history_new(max_states, &h, NULL), PC_OK);
    return h;
}

typedef void (*edit_fn)(pc_document *d, void *ctx);

/* Apply an edit the way the engine does: snapshot, mutate a clone, record. */
static void edit(pc_history *h, pc_doc_arc **cur, const char *label, edit_fn f, void *ctx)
{
    pc_doc_arc *before = pc_doc_arc_retain(*cur);
    pc_document *d = NULL;
    CHECK_EQ_INT(pc_doc_arc_make_mut(cur, &d, NULL), PC_OK);
    CHECK(*cur != before);
    if (d != NULL) {
        f(d, ctx);
    }
    CHECK_EQ_INT(pc_history_record(h, label, strlen(label), before, NULL, NULL), PC_OK);
    pc_doc_arc_release(before);
}

static void rename_to(pc_document *d, void *ctx)
{
    CHECK_EQ_INT(pc_str_set_c(&d->name, ctx, NULL), PC_OK);
}

static void new_layer(pc_document *d, void *ctx)
{
    pc_layer l;
    (void)ctx;
    CHECK_EQ_INT(pc_layer_raster(&l, "L", pc_document_pixel_format(d), NULL), PC_OK);
    CHECK_EQ_INT(pc_document_insert_above(d, NULL, &l, NULL, NULL), PC_OK);
}

/* Paint pixel (1, 1) of the bottom layer red = *(float *)ctx. */
static void paint(pc_document *d, void *ctx)
{
    float px[4] = {*(const float *)ctx, 0.0f, 0.0f, 1.0f};
    pc_surface *s = pc_layer_surface_mut(&d->layers[0]);
    CHECK(s != NULL);
    if (s != NULL) {
        CHECK_EQ_INT(pc_surface_write_pixel(s, 1, 1, px, 4, NULL), PC_OK);
    }
}

/* Undo/redo, keeping *cur current. Returns whether a step happened. */
static bool do_undo(pc_history *h, pc_doc_arc **cur, pc_layer_target *layers)
{
    pc_doc_arc *d = NULL;
    CHECK_EQ_INT(pc_history_undo(h, *cur, &d, layers, NULL), PC_OK);
    if (d == NULL) {
        return false;
    }
    pc_doc_arc_release(*cur);
    *cur = d;
    return true;
}

static bool do_redo(pc_history *h, pc_doc_arc **cur, pc_layer_target *layers)
{
    pc_doc_arc *d = NULL;
    CHECK_EQ_INT(pc_history_redo(h, *cur, &d, layers, NULL), PC_OK);
    if (d == NULL) {
        return false;
    }
    pc_doc_arc_release(*cur);
    *cur = d;
    return true;
}

static bool label_is(const pc_str *s, const char *want)
{
    return s != NULL && pc_str_eq_c(s, want);
}

static size_t tile_bytes(const pc_document *d)
{
    pc_ops_ptrset seen = {NULL, 0, 0};
    size_t n = 0;
    CHECK_EQ_INT(pc_ops_tile_bytes(d, &seen, &n), PC_OK);
    pc_ops_ptrset_fini(&seen);
    return n;
}

static size_t unique(const pc_history *h, const pc_document *cur)
{
    size_t n = 0;
    CHECK_EQ_INT(pc_history_unique_bytes(h, cur, &n, NULL), PC_OK);
    return n;
}

static size_t pixel(const pc_history *h, const pc_document *cur)
{
    size_t n = 0;
    CHECK_EQ_INT(pc_history_pixel_bytes(h, cur, &n, NULL), PC_OK);
    return n;
}

static size_t trim(pc_history *h, const pc_document *cur)
{
    size_t n = 99;
    CHECK_EQ_INT(pc_history_trim(h, cur, &n, NULL), PC_OK);
    return n;
}

static void test_undo_redo_cycle(void)
{
    pc_history *h = hist(PC_HISTORY_DEFAULT_MAX_STATES);
    pc_doc_arc *cur = base();
    pc_doc_arc *d = NULL;
    edit(h, &cur, "New Layer", new_layer, NULL);
    CHECK_EQ_UINT(pc_doc_arc_get(cur)->nlayers, 2);
    CHECK(label_is(pc_history_undo_label(h), "New Layer"));
    CHECK(do_undo(h, &cur, NULL));
    CHECK_EQ_UINT(pc_doc_arc_get(cur)->nlayers, 1);
    CHECK(label_is(pc_history_redo_label(h), "New Layer"));
    CHECK(do_redo(h, &cur, NULL));
    CHECK_EQ_UINT(pc_doc_arc_get(cur)->nlayers, 2);
    CHECK_EQ_INT(pc_history_redo(h, cur, &d, NULL, NULL), PC_OK);
    CHECK(d == NULL);
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static pc_layer_target target(pc_layer_id *id)
{
    return (pc_layer_target){true, *id, id, 1};
}

static void test_undo_and_redo_return_the_layers_each_state_targeted_when_created(void)
{
    pc_history *h = hist(PC_HISTORY_DEFAULT_MAX_STATES);
    pc_doc_arc *open = base();
    pc_doc_arc *layered = NULL, *filled = NULL, *cur;
    pc_document d;
    pc_layer l;
    pc_layer_id bg = pc_doc_arc_get(open)->layers[0].id, nid = 0;
    pc_layer_target t_bg = target(&bg), t_new, got = {0};
    CHECK_EQ_INT(pc_history_set_current_layers(h, &t_bg, NULL), PC_OK);
    /* New Layer targets the new layer; the background is then selected (not a step) and filled. */
    CHECK_EQ_INT(pc_document_clone(pc_doc_arc_get(open), &d, NULL), PC_OK);
    CHECK_EQ_INT(pc_layer_raster(&l, "L", pc_document_pixel_format(&d), NULL), PC_OK);
    CHECK_EQ_INT(pc_document_insert_above(&d, NULL, &l, &nid, NULL), PC_OK);
    CHECK_EQ_INT(pc_doc_arc_new(&d, &layered, NULL), PC_OK);
    t_new = target(&nid);
    CHECK_EQ_INT(pc_history_record(h, "New Layer", 9, open, &t_new, NULL), PC_OK);
    pc_doc_arc_release(open);
    CHECK_EQ_INT(pc_document_clone(pc_doc_arc_get(layered), &d, NULL), PC_OK);
    CHECK_EQ_INT(pc_str_set_c(&d.name, "filled", NULL), PC_OK);
    CHECK_EQ_INT(pc_doc_arc_new(&d, &filled, NULL), PC_OK);
    CHECK_EQ_INT(pc_history_record(h, "Fill", 4, layered, &t_bg, NULL), PC_OK);
    pc_doc_arc_release(layered);
    cur = filled;

    CHECK(do_undo(h, &cur, &got));
    CHECK(pc_layer_target_eq(&got, &t_new)); /* New Layer's state targeted the new layer */
    pc_layer_target_fini(&got);
    CHECK(do_undo(h, &cur, &got));
    CHECK(pc_layer_target_eq(&got, &t_bg)); /* the opened document's target */
    pc_layer_target_fini(&got);
    CHECK(do_redo(h, &cur, &got));
    CHECK(pc_layer_target_eq(&got, &t_new));
    pc_layer_target_fini(&got);
    CHECK(do_redo(h, &cur, &got));
    CHECK(pc_layer_target_eq(&got, &t_bg)); /* Fill's state targeted the background */
    pc_layer_target_fini(&got);
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_state_accessor_matches_entries(void)
{
    pc_history *h = hist(PC_HISTORY_DEFAULT_MAX_STATES);
    pc_doc_arc *cur = base();
    pc_doc_arc *open = pc_doc_arc_retain(cur);
    edit(h, &cur, "A", rename_to, "a");
    edit(h, &cur, "B", rename_to, "b");
    CHECK_EQ_UINT(pc_history_entry_count(h), pc_history_past_len(h) + 1);
    CHECK(pc_history_state(h, 0) == open);
    CHECK(pc_history_state(h, 1) != NULL && pc_str_eq_c(&pc_doc_arc_get(pc_history_state(h, 1))->name, "a"));
    CHECK(pc_history_state(h, 2) == NULL); /* the current document is not held */
    CHECK(do_undo(h, &cur, NULL));
    CHECK(do_undo(h, &cur, NULL));
    CHECK_EQ_UINT(pc_history_redo_count(h), 2);
    CHECK(label_is(pc_history_redo_label_at(h, 0), "A")); /* next redo first */
    CHECK(label_is(pc_history_redo_label_at(h, 1), "B"));
    CHECK(pc_history_redo_label_at(h, 2) == NULL);
    pc_doc_arc_release(open);
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_new_edit_clears_redo(void)
{
    pc_history *h = hist(PC_HISTORY_DEFAULT_MAX_STATES);
    pc_doc_arc *cur = base();
    edit(h, &cur, "A", rename_to, "a");
    CHECK(do_undo(h, &cur, NULL));
    edit(h, &cur, "B", rename_to, "b");
    CHECK(!pc_history_can_redo(h));
    CHECK_EQ_UINT(pc_history_entry_count(h), 2);
    CHECK(label_is(pc_history_entry(h, 0), "Open"));
    CHECK(label_is(pc_history_entry(h, 1), "B"));
    CHECK(pc_history_entry(h, 2) == NULL);
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_max_states_bound(void)
{
    pc_history *h = hist(3);
    pc_doc_arc *cur = base();
    int n = 0;
    for (int i = 0; i < 10; i++) {
        char label[32], name[32];
        (void)snprintf(label, sizeof label, "step %d", i);
        (void)snprintf(name, sizeof name, "%d", i);
        edit(h, &cur, label, rename_to, name);
    }
    while (do_undo(h, &cur, NULL)) {
        n++;
    }
    CHECK_EQ_INT(n, 3);
    CHECK(pc_str_eq_c(&pc_doc_arc_get(cur)->name, "6"));
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_byte_budget_drops_oldest_states_but_keeps_one(void)
{
    pc_history *h = hist(50);
    pc_doc_arc *cur = base();
    /* Each step repaints the background's tile: one unique tile per state. */
    for (int i = 0; i < 6; i++) {
        char label[32];
        float v = (float)i / 8.0f;
        (void)snprintf(label, sizeof label, "paint %d", i);
        edit(h, &cur, label, paint, &v);
    }
    CHECK_EQ_UINT(trim(h, pc_doc_arc_get(cur)), 0); /* unlimited by default */
    CHECK_EQ_UINT(pc_history_past_len(h), 6);
    /* The current tile plus three history tiles fit. */
    pc_history_set_max_bytes(h, 4 * TILE);
    CHECK_EQ_UINT(trim(h, pc_doc_arc_get(cur)), 3);
    CHECK_EQ_UINT(pc_history_past_len(h), 3);
    CHECK_EQ_UINT(unique(h, pc_doc_arc_get(cur)), 3 * TILE);
    CHECK_EQ_UINT(pixel(h, pc_doc_arc_get(cur)), 4 * TILE); /* the current tile plus history's */
    CHECK(label_is(pc_history_entry(h, 0), "paint 2"));
    /* A budget smaller than one state still keeps the last step undoable. */
    pc_history_set_max_bytes(h, 1);
    (void)trim(h, pc_doc_arc_get(cur));
    CHECK_EQ_UINT(pc_history_past_len(h), 1);
    CHECK(do_undo(h, &cur, NULL));
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_history_shares_untouched_tiles(void)
{
    pc_history *h = hist(PC_HISTORY_DEFAULT_MAX_STATES);
    pc_doc_arc *cur = base();
    float black = 0.0f;
    CHECK_EQ_UINT(unique(h, pc_doc_arc_get(cur)), 0);
    /* rename only: no pixel changes, history holds no unique tiles */
    edit(h, &cur, "Rename", rename_to, "x");
    CHECK_EQ_UINT(unique(h, pc_doc_arc_get(cur)), 0);
    /* paint one pixel: exactly one tile becomes unique to history */
    edit(h, &cur, "Paint", paint, &black);
    CHECK_EQ_UINT(unique(h, pc_doc_arc_get(cur)), TILE);
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void assert_counter_at(const pc_history *h, const pc_document *current, int line)
{
    const pc_ops_tiles *counter = pc_ops_history_counter(h);
    pc_document empty;
    uint64_t with = 0;
    int failed = pc_t_failed_checks;
    size_t total = tile_bytes(current) + unique(h, current);
    if (counter == NULL) {
        PC_T_FAIL("line %d: a budget initialises the counter", line);
        return;
    }
    CHECK_EQ_INT(pc_ops_tiles_bytes_with(counter, current, &with), PC_OK);
    CHECK_EQ_UINT(with, total);
    CHECK_EQ_UINT(pixel(h, current), total);
    /* Bytes retained by the states = their tiles' union = unique bytes against an empty doc. */
    CHECK_EQ_INT(
        pc_document_init(&empty, "empty", pc_size_make(1, 1), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL),
        PC_OK);
    CHECK_EQ_INT(pc_ops_tiles_bytes_with(counter, &empty, &with), PC_OK);
    CHECK_EQ_UINT(with, unique(h, &empty));
    CHECK_EQ_UINT(counter->bytes, with);
    pc_document_fini(&empty);
    if (pc_t_failed_checks > failed) {
        (void)fprintf(stderr, "  (assert_counter at line %d)\n", line);
    }
}
#define assert_counter(h, cur) assert_counter_at((h), (cur), __LINE__)

static void test_counter_tracks_eviction_undo_redo_branching_purge_and_clones(void)
{
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t k = 0; k < 3; k++) {
        pc_doc_arc *cur = doc_with_bg(pc_size_make(300, 64), depths[k]);
        pc_history *h = hist(4), *copy = NULL;
        pc_history_set_max_bytes(h, SIZE_MAX);
        for (int i = 0; i < 8; i++) {
            float v = (float)i / 10.0f;
            edit(h, &cur, "Paint", paint, &v);
            (void)trim(h, pc_doc_arc_get(cur));
            if (i >= 1) {
                assert_counter(h, pc_doc_arc_get(cur));
            }
        }
        CHECK(do_undo(h, &cur, NULL));
        assert_counter(h, pc_doc_arc_get(cur));
        CHECK(do_undo(h, &cur, NULL));
        assert_counter(h, pc_doc_arc_get(cur));
        CHECK(do_redo(h, &cur, NULL));
        assert_counter(h, pc_doc_arc_get(cur));
        CHECK_EQ_INT(pc_history_clone(h, &copy, NULL), PC_OK);
        edit(h, &cur, "Branch", rename_to, "branch");
        CHECK(!pc_history_can_redo(h));
        assert_counter(h, pc_doc_arc_get(cur));
        assert_counter(copy, pc_doc_arc_get(cur));
        CHECK(do_undo(h, &cur, NULL));
        pc_history_clear_redo(h);
        assert_counter(h, pc_doc_arc_get(cur));
        (void)pc_history_purge_last(h);
        assert_counter(h, pc_doc_arc_get(cur));
        pc_history_clear(h);
        CHECK(pc_ops_history_counter(h) == NULL);
        CHECK_EQ_UINT(pixel(h, pc_doc_arc_get(cur)), tile_bytes(pc_doc_arc_get(cur)));
        pc_history_free(copy);
        pc_history_free(h);
        pc_doc_arc_release(cur);
    }
}

static void test_shared_masks_channels_and_repeated_snapshot_references_count_once(void)
{
    pc_document doc;
    pc_surface gray;
    pc_layer *layers;
    pc_doc_arc *cur = NULL;
    pc_history *h = hist(50);
    float one = 1.0f;
    size_t bytes;
    CHECK_EQ_INT(pc_document_with_background(&doc, "h", pc_size_make(64, 64), PC_COLOR_MODE_RGB,
                                             PC_SAMPLE_TYPE_U8, PC_COLOR_WHITE, NULL),
                 PC_OK);
    pc_surface_init(&gray, PC_PIXEL_FORMAT_GRAY8);
    CHECK_EQ_INT(pc_surface_write_pixel(&gray, 1, 1, &one, 1, NULL), PC_OK);
    pc_layer_mask_reveal_all(&doc.layers[0].mask);
    pc_surface_fini(&doc.layers[0].mask.surface);
    CHECK_EQ_INT(pc_surface_clone(&gray, &doc.layers[0].mask.surface, NULL), PC_OK);
    doc.layers[0].has_mask = true;
    doc.channels = calloc(1, sizeof *doc.channels);
    CHECK(doc.channels != NULL);
    if (doc.channels != NULL) {
        pc_surface s;
        CHECK_EQ_INT(pc_surface_clone(&gray, &s, NULL), PC_OK);
        CHECK_EQ_INT(pc_alpha_channel_init(&doc.channels[0], "alpha", &s, NULL), PC_OK);
        doc.nchannels = 1;
    }
    CHECK_EQ_INT(pc_alpha_channel_init(&doc.quick_mask, "quick", &gray, NULL), PC_OK);
    doc.has_quick_mask = true;
    layers = realloc(doc.layers, 2 * sizeof *layers);
    CHECK(layers != NULL);
    if (layers != NULL) {
        doc.layers = layers;
        CHECK_EQ_INT(pc_layer_clone(&layers[0], &layers[1], NULL), PC_OK);
        doc.nlayers = 2;
    }
    CHECK_EQ_INT(pc_doc_arc_new(&doc, &cur, NULL), PC_OK);
    bytes = tile_bytes(pc_doc_arc_get(cur));
    CHECK_EQ_UINT(bytes, TILE + 256 * 256); /* the RGBA tile and the one gray tile, each once */
    /* Counting current separately would double count its tiles; the exact union fits. */
    pc_history_set_max_bytes(h, bytes);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ_INT(pc_history_record(h, "Same", 4, cur, NULL, NULL), PC_OK);
        CHECK_EQ_UINT(trim(h, pc_doc_arc_get(cur)), 0);
    }
    assert_counter(h, pc_doc_arc_get(cur));
    for (int i = 0; i < 3; i++) {
        CHECK(do_undo(h, &cur, NULL));
        assert_counter(h, pc_doc_arc_get(cur));
    }
    pc_history_clear_redo(h);
    (void)pc_history_purge_last(h);
    assert_counter(h, pc_doc_arc_get(cur));
    CHECK_EQ_UINT(pixel(h, pc_doc_arc_get(cur)), bytes);
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_threshold_rechecks_coalesced_current_and_changed_budget(void)
{
    pc_history *h = hist(50);
    pc_doc_arc *cur = base();
    pc_document *d = NULL;
    float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_history_set_max_bytes(h, SIZE_MAX);
    for (int i = 0; i < 6; i++) {
        float v = (float)i / 10.0f;
        edit(h, &cur, "Paint", paint, &v);
        (void)trim(h, pc_doc_arc_get(cur));
    }
    /* A coalesced edit may change current without recording another history state. */
    CHECK_EQ_INT(pc_doc_arc_make_mut(&cur, &d, NULL), PC_OK);
    if (d != NULL) {
        CHECK_EQ_INT(pc_surface_write_pixel(pc_layer_surface_mut(&d->layers[0]), 300, 1, white, 4, NULL),
                     PC_OK);
    }
    pc_history_set_max_bytes(h, 4 * TILE);
    CHECK_EQ_UINT(trim(h, pc_doc_arc_get(cur)), 4);
    CHECK_EQ_UINT(pc_history_past_len(h), 2);
    assert_counter(h, pc_doc_arc_get(cur));
    CHECK(do_undo(h, &cur, NULL));
    assert_counter(h, pc_doc_arc_get(cur));
    pc_history_set_max_bytes(h, 0);
    edit(h, &cur, "Unlimited", rename_to, "u");
    CHECK(pc_ops_history_counter(h) == NULL);
    pc_history_set_max_bytes(h, 1);
    (void)trim(h, pc_doc_arc_get(cur));
    CHECK_EQ_UINT(pc_history_past_len(h), 1); /* always retain the most recent undo */
    assert_counter(h, pc_doc_arc_get(cur));
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_transferred_tiles_cannot_make_the_budget_check_underestimate_current(void)
{
    pc_doc_arc *before = base();
    pc_document current;
    pc_surface large;
    pc_tile_entry *entries = NULL;
    size_t n = 0;
    float px[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    pc_history *h = hist(50);
    CHECK_EQ_INT(pc_document_clone(pc_doc_arc_get(before), &current, NULL), PC_OK);
    pc_surface_init(&large, (pc_pixel_format){PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_F32, true});
    CHECK_EQ_INT(pc_surface_write_pixel(&large, 256, 0, px, 5, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_take_tiles(&large, pc_rect_from_xywh(256, 0, 1, 1), &entries, &n, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_put_tiles(pc_layer_surface_mut(&current.layers[0]), entries, n, NULL), PC_OK);
    free(entries);
    pc_surface_fini(&large);
    pc_history_set_max_bytes(h, tile_bytes(&current) - 1);
    CHECK_EQ_INT(pc_history_record(h, "A", 1, before, NULL, NULL), PC_OK);
    CHECK_EQ_INT(pc_history_record(h, "B", 1, before, NULL, NULL), PC_OK);
    /* current alone exceeds the budget; only the mandatory undo survives */
    CHECK_EQ_UINT(trim(h, &current), 1);
    assert_counter(h, &current);
    pc_document_fini(&current);
    pc_doc_arc_release(before);
    pc_history_free(h);
}

/* ---- never crash ---- */

static void test_empty_history_and_bad_indices(void)
{
    pc_history *h = hist(0);
    pc_doc_arc *cur = base();
    pc_doc_arc *d = cur;
    pc_layer_target t = {true, 7, NULL, 0};
    size_t n = 1;
    CHECK_EQ_UINT(pc_history_max_states(h), 1); /* new() keeps at least one */
    CHECK(!pc_history_can_undo(h));
    CHECK(!pc_history_can_redo(h));
    CHECK(pc_history_undo_label(h) == NULL);
    CHECK(pc_history_redo_label(h) == NULL);
    CHECK(pc_history_redo_label_at(h, 0) == NULL);
    CHECK(pc_history_redo_label_at(h, SIZE_MAX) == NULL);
    CHECK_EQ_UINT(pc_history_entry_count(h), 1);
    CHECK(label_is(pc_history_entry(h, 0), "Open"));
    CHECK(pc_history_entry(h, 1) == NULL);
    CHECK(pc_history_entry(h, SIZE_MAX) == NULL);
    CHECK(pc_history_state(h, 0) == NULL);
    CHECK(pc_history_state(h, SIZE_MAX) == NULL);
    CHECK_EQ_INT(pc_history_undo(h, cur, &d, &t, NULL), PC_OK);
    CHECK(d == NULL);
    CHECK(!t.has_active && t.nselected == 0);
    d = cur;
    CHECK_EQ_INT(pc_history_redo(h, cur, &d, NULL, NULL), PC_OK);
    CHECK(d == NULL);
    CHECK_EQ_UINT(pc_doc_arc_refs(cur), 1); /* nothing kept */
    CHECK(!pc_history_purge_last(h));
    pc_history_clear_redo(h);
    pc_history_clear(h);
    CHECK_EQ_INT(pc_history_trim(h, pc_doc_arc_get(cur), &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, 0);
    CHECK_EQ_UINT(pixel(h, pc_doc_arc_get(cur)), TILE);
    CHECK_EQ_UINT(unique(h, pc_doc_arc_get(cur)), 0);
    /* max_states 0 via the field keeps nothing, as the reference does. */
    pc_history_set_max_states(h, 0);
    CHECK_EQ_INT(pc_history_record(h, "A", 1, cur, NULL, NULL), PC_OK);
    CHECK_EQ_UINT(pc_history_past_len(h), 0);
    CHECK(label_is(pc_history_entry(h, 0), "A"));
    CHECK_EQ_UINT(pc_doc_arc_refs(cur), 1);
    pc_doc_arc_release(cur);
    pc_history_free(h);
    pc_history_free(NULL);
}

static void test_null_arguments(void)
{
    pc_history *h = hist(5);
    pc_doc_arc *cur = base();
    pc_doc_arc *d = NULL;
    pc_document *m = NULL;
    pc_error err;
    pc_layer_target bad = {false, 0, NULL, 3};
    size_t n;
    CHECK_EQ_INT(pc_history_new(1, NULL, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_clone(NULL, &h, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_record(h, "x", 1, NULL, NULL, &err), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_record(NULL, "x", 1, cur, NULL, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_record(h, NULL, 3, cur, NULL, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_record(h, "x", 1, cur, &bad, NULL), PC_ERR_INVALID);
    CHECK_EQ_UINT(pc_history_past_len(h), 0);
    CHECK_EQ_INT(pc_history_record(h, NULL, 0, cur, NULL, NULL), PC_OK); /* empty label */
    CHECK(label_is(pc_history_undo_label(h), ""));
    CHECK_EQ_INT(pc_history_undo(h, NULL, &d, NULL, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_undo(h, cur, NULL, NULL, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_redo(NULL, cur, &d, NULL, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_unique_bytes(h, NULL, &n, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_pixel_bytes(h, pc_doc_arc_get(cur), NULL, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_trim(h, NULL, &n, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_trim(h, pc_doc_arc_get(cur), NULL, NULL), PC_OK); /* dropped is optional */
    CHECK_EQ_INT(pc_history_set_current_label(h, NULL, 2, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_set_current_layers(h, &bad, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_history_set_current_layers(h, NULL, NULL), PC_OK);
    CHECK_EQ_INT(pc_layer_target_clone(NULL, &bad, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_doc_arc_new(NULL, &d, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_doc_arc_from_clone(NULL, &d, NULL), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_doc_arc_make_mut(NULL, &m, NULL), PC_ERR_INVALID);
    d = NULL;
    CHECK_EQ_INT(pc_doc_arc_make_mut(&d, &m, NULL), PC_ERR_INVALID);
    CHECK(pc_doc_arc_retain(NULL) == NULL);
    pc_doc_arc_release(NULL);
    CHECK(pc_doc_arc_get(NULL) == NULL);
    CHECK_EQ_UINT(pc_doc_arc_refs(NULL), 0);
    pc_layer_target_fini(NULL);
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_labels_with_nul_bytes_and_layer_target_copies(void)
{
    pc_history *h = hist(5);
    pc_doc_arc *cur = base();
    pc_layer_id ids[3] = {3, 1, 2};
    pc_layer_target t = {true, 1, ids, 3}, got = {0};
    const pc_str *s;
    CHECK_EQ_INT(pc_history_record(h, "a\0b", 3, cur, &t, NULL), PC_OK);
    ids[0] = 99; /* the history holds a copy */
    s = pc_history_undo_label(h);
    CHECK(s != NULL && s->len == 3 && memcmp(s->ptr, "a\0b", 3) == 0);
    CHECK(do_undo(h, &cur, &got));
    CHECK(!got.has_active && got.nselected == 0); /* the "Open" state's default target */
    pc_layer_target_fini(&got);
    CHECK(do_redo(h, &cur, &got));
    CHECK(got.nselected == 3 && got.selected[0] == 3 && got.selected[2] == 2);
    pc_layer_target_fini(&got);
    t.has_active = false;
    CHECK(!pc_layer_target_eq(&t, &(pc_layer_target){true, 1, ids, 3}));
    CHECK(pc_layer_target_eq(&t, &(pc_layer_target){false, 5, ids, 3}));
    CHECK_EQ_INT(pc_history_set_current_label(h, "Fold", 4, NULL), PC_OK);
    CHECK(label_is(pc_history_undo_label(h), "Fold"));
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_make_mut_shares_only_when_needed(void)
{
    pc_doc_arc *a = base(), *b;
    pc_document *m = NULL;
    const pc_document *orig = pc_doc_arc_get(a);
    CHECK_EQ_INT(pc_doc_arc_make_mut(&a, &m, NULL), PC_OK);
    CHECK(m == orig); /* sole owner: in place */
    b = pc_doc_arc_retain(a);
    CHECK_EQ_UINT(pc_doc_arc_refs(a), 2);
    CHECK_EQ_INT(pc_doc_arc_make_mut(&a, &m, NULL), PC_OK);
    CHECK(a != b && m == pc_doc_arc_get(a));
    CHECK_EQ_UINT(pc_doc_arc_refs(b), 1);
    CHECK(pc_document_eq(pc_doc_arc_get(a), pc_doc_arc_get(b)));
    /* Tiles are shared until written. */
    CHECK(pc_surface_tile(pc_layer_surface(&pc_doc_arc_get(a)->layers[0]), (pc_tile){0, 0}) ==
          pc_surface_tile(pc_layer_surface(&pc_doc_arc_get(b)->layers[0]), (pc_tile){0, 0}));
    pc_doc_arc_release(a);
    pc_doc_arc_release(b);
}

/* A snapshot of everything observable, to compare before/after a failed call. */
typedef struct snap {
    size_t past, redo, pixel;
    char labels[512];
} snap;

static snap take_snap(const pc_history *h, const pc_document *cur)
{
    snap s;
    size_t off = 0;
    memset(&s, 0, sizeof s);
    s.past = pc_history_past_len(h);
    s.redo = pc_history_redo_count(h);
    s.pixel = pixel(h, cur);
    for (size_t i = 0; i < pc_history_entry_count(h) + s.redo; i++) {
        const pc_str *l = i < pc_history_entry_count(h)
                              ? pc_history_entry(h, i)
                              : pc_history_redo_label_at(h, i - pc_history_entry_count(h));
        int w = snprintf(s.labels + off, sizeof s.labels - off, "%s|", l != NULL ? pc_str_c(l) : "?");
        if (w > 0 && (size_t)w < sizeof s.labels - off) {
            off += (size_t)w;
        }
    }
    return s;
}

static bool snap_eq(const snap *a, const snap *b)
{
    return a->past == b->past && a->redo == b->redo && a->pixel == b->pixel &&
           strcmp(a->labels, b->labels) == 0;
}

/* A history with 3 undo and 2 redo states, a budget and its counter. */
static pc_history *busy(pc_doc_arc **cur)
{
    pc_history *h = hist(10);
    pc_layer_id id = 1;
    pc_layer_target t = target(&id);
    pc_history_set_max_bytes(h, SIZE_MAX);
    for (int i = 0; i < 5; i++) {
        float v = (float)i / 10.0f;
        char label[32];
        (void)snprintf(label, sizeof label, "p%d", i);
        CHECK_EQ_INT(pc_history_set_current_layers(h, &t, NULL), PC_OK);
        edit(h, cur, label, paint, &v);
        (void)trim(h, pc_doc_arc_get(*cur));
    }
    CHECK(do_undo(h, cur, NULL));
    CHECK(do_undo(h, cur, NULL));
    return h;
}

enum { OP_RECORD, OP_UNDO, OP_REDO, OP_TRIM, OP_CLONE, OP_LABEL, OP_LAYERS, OP_BYTES, OP_NEW, OP_COUNT };

/* Fail the n-th allocation of each operation, for n = 0, 1, ... until it succeeds without the
 * injection firing: every failure must leave the history unchanged, never crash or leak. */
static void test_allocation_failures_leave_history_unchanged(void)
{
    for (int op = 0; op < OP_COUNT; op++) {
        for (long n = 0; n < 10000; n++) {
            pc_doc_arc *cur = base();
            pc_history *h = busy(&cur);
            pc_history *c = NULL;
            pc_doc_arc *d = NULL;
            pc_layer_id ids[2] = {1, 2};
            pc_layer_target t = {true, 1, ids, 2}, got = {0};
            snap before = take_snap(h, pc_doc_arc_get(cur)), after;
            size_t x = 0;
            pc_status st = PC_OK;
            bool fired;
            pc_history_set_max_bytes(h, op == OP_TRIM ? 2 * TILE : SIZE_MAX);
            pc_ops_test_fail_alloc(n);
            switch (op) {
            case OP_RECORD:
                st = pc_history_record(h, "R", 1, cur, &t, NULL);
                break;
            case OP_UNDO:
                st = pc_history_undo(h, cur, &d, &got, NULL);
                break;
            case OP_REDO:
                st = pc_history_redo(h, cur, &d, &got, NULL);
                break;
            case OP_TRIM:
                st = pc_history_trim(h, pc_doc_arc_get(cur), &x, NULL);
                break;
            case OP_CLONE:
                st = pc_history_clone(h, &c, NULL);
                break;
            case OP_LABEL:
                st = pc_history_set_current_label(h, "L", 1, NULL);
                break;
            case OP_LAYERS:
                st = pc_history_set_current_layers(h, &t, NULL);
                break;
            case OP_BYTES:
                st = pc_history_unique_bytes(h, pc_doc_arc_get(cur), &x, NULL);
                if (st == PC_OK) {
                    st = pc_history_pixel_bytes(h, pc_doc_arc_get(cur), &x, NULL);
                }
                break;
            default:
                st = pc_history_new(3, &c, NULL);
                break;
            }
            fired = !pc_ops_test_fail_pending();
            pc_ops_test_fail_alloc(-1);
            if (st != PC_OK) {
                CHECK_EQ_INT(st, PC_ERR_NOMEM);
                CHECK(fired);
                CHECK(c == NULL && d == NULL);
                after = take_snap(h, pc_doc_arc_get(cur));
                if (!snap_eq(&before, &after)) {
                    PC_T_FAIL("op %d failing allocation %ld changed the history", op, n);
                }
            }
            if (d != NULL) {
                pc_doc_arc_release(cur);
                cur = d;
            }
            pc_layer_target_fini(&got);
            pc_history_free(c);
            pc_history_free(h);
            pc_doc_arc_release(cur);
            if (!fired) {
                CHECK_EQ_INT(st, PC_OK);
                /* Every operation allocates at least once (so failures were exercised). */
                if (n < 1) {
                    PC_T_FAIL("op %d never failed", op);
                }
                break;
            }
        }
    }
}

/* Counter updates that run out of memory drop the counter; trim rebuilds it exactly. */
static void test_counter_failure_drops_the_cache(void)
{
    pc_doc_arc *cur = base();
    pc_history *h = busy(&cur);
    size_t x = 0;
    CHECK(pc_ops_history_counter(h) != NULL);
    edit(h, &cur, "warm", paint, &(float){0.9f}); /* counter tables are grown by now */
    for (long n = 0; n < 4; n++) {
        float v = 0.05f * (float)n;
        pc_history_set_max_bytes(h, SIZE_MAX);
        (void)trim(h, pc_doc_arc_get(cur));
        CHECK(pc_ops_history_counter(h) != NULL);
        /* A 200-deep document needs a heap walk stack: failing it fails the counter update. */
        {
            pc_layer inner, *kids;
            pc_document *m = NULL;
            pc_doc_arc *before = pc_doc_arc_retain(cur);
            CHECK_EQ_INT(pc_doc_arc_make_mut(&cur, &m, NULL), PC_OK);
            CHECK_EQ_INT(pc_layer_raster(&inner, "deep", PC_PIXEL_FORMAT_RGBA8, NULL), PC_OK);
            CHECK_EQ_INT(
                pc_surface_write_pixel(&inner.content.u.raster, 0, 0, (float[4]){v, 0, 0, 1}, 4, NULL),
                PC_OK);
            for (int k = 0; k < 200; k++) {
                kids = malloc(sizeof *kids);
                if (kids == NULL) {
                    break;
                }
                kids[0] = inner;
                CHECK_EQ_INT(pc_layer_group(&inner, "g", kids, 1, NULL), PC_OK);
            }
            CHECK_EQ_INT(pc_document_insert_above(m, NULL, &inner, NULL, NULL), PC_OK);
            CHECK_EQ_INT(pc_history_record(h, "deep", 4, before, NULL, NULL), PC_OK);
            pc_doc_arc_release(before);
            /* Undo walks current (deep) when adding it to the counter: fail that walk. */
            pc_ops_test_fail_alloc(0);
            CHECK(do_undo(h, &cur, NULL));
            pc_ops_test_fail_alloc(-1);
            CHECK(pc_ops_history_counter(h) == NULL);
            CHECK(do_redo(h, &cur, NULL));
        }
        CHECK_EQ_INT(pc_history_trim(h, pc_doc_arc_get(cur), &x, NULL), PC_OK);
        assert_counter(h, pc_doc_arc_get(cur));
    }
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

/* 10,000 nested groups: the walks are iterative, so nothing overflows the C stack. */
static void test_deep_documents(void)
{
    pc_document d;
    pc_layer inner, *kids;
    pc_doc_arc *cur = NULL;
    pc_history *h = hist(5);
    size_t n = 0;
    float red[4] = {1, 0, 0, 1};
    CHECK_EQ_INT(
        pc_document_init(&d, "deep", pc_size_make(16, 16), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL),
        PC_OK);
    CHECK_EQ_INT(pc_layer_raster(&inner, "deep", PC_PIXEL_FORMAT_RGBA8, NULL), PC_OK);
    CHECK_EQ_INT(pc_surface_write_pixel(&inner.content.u.raster, 0, 0, red, 4, NULL), PC_OK);
    pc_layer_mask_reveal_all(&inner.mask);
    inner.has_mask = true;
    CHECK_EQ_INT(pc_surface_write_pixel(&inner.mask.surface, 0, 0, red, 1, NULL), PC_OK);
    for (int k = 0; k < 10000; k++) {
        kids = malloc(sizeof *kids);
        CHECK(kids != NULL);
        if (kids == NULL) {
            break;
        }
        kids[0] = inner;
        CHECK_EQ_INT(pc_layer_group(&inner, "g", kids, 1, NULL), PC_OK);
    }
    CHECK_EQ_INT(pc_document_insert_above(&d, NULL, &inner, NULL, NULL), PC_OK);
    CHECK_EQ_INT(pc_doc_arc_new(&d, &cur, NULL), PC_OK);
    CHECK_EQ_UINT(tile_bytes(pc_doc_arc_get(cur)), TILE + 256 * 256);
    pc_history_set_max_bytes(h, TILE + 256 * 256);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ_INT(pc_history_record(h, "x", 1, cur, NULL, NULL), PC_OK);
    }
    CHECK_EQ_INT(pc_history_trim(h, pc_doc_arc_get(cur), &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, 0); /* one tile set shared by every state: exactly the budget */
    CHECK_EQ_UINT(pc_history_past_len(h), 3);
    CHECK_EQ_UINT(unique(h, pc_doc_arc_get(cur)), 0);
    assert_counter(h, pc_doc_arc_get(cur));
    pc_history_set_max_bytes(h, 1); /* current alone is over: only the last undo stays */
    CHECK_EQ_INT(pc_history_trim(h, pc_doc_arc_get(cur), &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, 2);
    CHECK_EQ_UINT(pc_history_past_len(h), 1);
    assert_counter(h, pc_doc_arc_get(cur));
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

static void test_huge_limits(void)
{
    pc_history *h = hist(SIZE_MAX);
    pc_doc_arc *cur = base();
    CHECK_EQ_UINT(pc_history_max_states(h), SIZE_MAX);
    pc_history_set_max_bytes(h, SIZE_MAX);
    CHECK_EQ_UINT(pc_history_max_bytes(h), SIZE_MAX);
    for (int i = 0; i < 100; i++) {
        float v = (float)i / 100.0f;
        edit(h, &cur, "p", paint, &v);
        CHECK_EQ_UINT(trim(h, pc_doc_arc_get(cur)), 0);
    }
    CHECK_EQ_UINT(pc_history_past_len(h), 100);
    CHECK_EQ_UINT(pixel(h, pc_doc_arc_get(cur)), 101 * TILE);
    pc_doc_arc_release(cur);
    pc_history_free(h);
}

/* ---- threads ---- */

typedef struct share_ctx {
    pc_doc_arc *doc;
    size_t sum;
} share_ctx;

static void *share_worker(void *p)
{
    share_ctx *c = p;
    for (int i = 0; i < 2000; i++) {
        pc_doc_arc *mine = pc_doc_arc_retain(c->doc);
        c->sum += pc_doc_arc_get(mine)->nlayers;
        pc_doc_arc_release(mine);
    }
    pc_doc_arc_release(c->doc); /* each worker owns one reference */
    return NULL;
}

static void test_snapshots_shared_across_threads(void)
{
    pc_doc_arc *doc = base();
    pthread_t th[4];
    share_ctx ctx[4];
    int started = 0;
    for (int i = 0; i < 4; i++) {
        ctx[i] = (share_ctx){pc_doc_arc_retain(doc), 0};
        if (pthread_create(&th[i], NULL, share_worker, &ctx[i]) != 0) {
            pc_doc_arc_release(ctx[i].doc);
            break;
        }
        started++;
    }
    pc_doc_arc_release(doc); /* the last worker frees it */
    for (int i = 0; i < started; i++) {
        CHECK_EQ_INT(pthread_join(th[i], NULL), 0);
        CHECK_EQ_UINT(ctx[i].sum, 2000);
    }
    CHECK_EQ_INT(started, 4);
}

int main(int argc, char **argv)
{
    RUN(test_undo_redo_cycle);
    RUN(test_undo_and_redo_return_the_layers_each_state_targeted_when_created);
    RUN(test_state_accessor_matches_entries);
    RUN(test_new_edit_clears_redo);
    RUN(test_max_states_bound);
    RUN(test_byte_budget_drops_oldest_states_but_keeps_one);
    RUN(test_history_shares_untouched_tiles);
    RUN(test_counter_tracks_eviction_undo_redo_branching_purge_and_clones);
    RUN(test_shared_masks_channels_and_repeated_snapshot_references_count_once);
    RUN(test_threshold_rechecks_coalesced_current_and_changed_budget);
    RUN(test_transferred_tiles_cannot_make_the_budget_check_underestimate_current);
    RUN(test_empty_history_and_bad_indices);
    RUN(test_null_arguments);
    RUN(test_labels_with_nul_bytes_and_layer_target_copies);
    RUN(test_make_mut_shares_only_when_needed);
    RUN(test_allocation_failures_leave_history_unchanged);
    RUN(test_counter_failure_drops_the_cache);
    RUN(test_deep_documents);
    RUN(test_huge_limits);
    RUN(test_snapshots_shared_across_threads);
    return pc_t_finish();
}
