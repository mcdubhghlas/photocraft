/* Layer > Align / Distribute (align_distribute_cmds.c): the 14 specs' enabled()/run() exercised
 * directly through pc_engine_align_distribute_specs (no command.c registration needed). Covers a
 * real align and a real distribute (verifying moved layer offsets) plus graceful failures on no
 * document, a bad `to` value and too few layers. */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "doc/doc.h"
#include "engine/align_distribute_cmds.h"
#include "engine/layer_cmds.h"
#include "harness.h"
#include "raster/surface.h"

/* ---- fixture helpers ---------------------------------------------------------------------- */

static pc_engine_session *mk(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

struct addctx {
    pc_rect r;
    pc_layer_id id;
};

static pc_status add_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    struct addctx *c = v;
    pc_layer l;
    if (pc_layer_raster(&l, "L", PC_PIXEL_FORMAT_RGBA8, e) != PC_OK) {
        return e != NULL ? e->code : PC_ERR_NOMEM;
    }
    pc_surface *s = pc_layer_surface_mut(&l);
    float col[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    if (s != NULL) {
        (void)pc_surface_fill_rect(s, c->r, col, 4, e);
    }
    pc_layer_id id = 0;
    pc_status rc = pc_document_insert_above(doc, NULL, &l, &id, e);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    c->id = id;
    *ha = true;
    *active = id;
    return PC_OK;
}

/* Add a raster layer with an opaque w*h rect at (x, y); returns its id. */
static pc_layer_id add_layer(pc_engine_session *s, int x, int y, int w, int h)
{
    struct addctx c = {pc_rect_from_xywh(x, y, (uint32_t)w, (uint32_t)h), 0};
    CHECK(pc_engine_session_edit(s, "add", add_edit, &c, NULL) == PC_OK);
    return c.id;
}

static void select_layers(pc_engine_session *s, const pc_layer_id *ids, size_t n)
{
    CHECK(pc_engine_set_selection(s, ids, n, true, ids[n - 1], false, 0, NULL) == PC_OK);
}

static pc_rect bounds_of(pc_engine_session *s, pc_layer_id id)
{
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    pc_rect r = PC_RECT_EMPTY;
    if (l != NULL) {
        (void)pc_engine_layer_bounds(l, &r);
    }
    return r;
}

static const pc_engine_command *find_spec(const char *id)
{
    size_t n = 0;
    const pc_engine_command *sp = pc_engine_align_distribute_specs(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(sp[i].id, id) == 0) {
            return &sp[i];
        }
    }
    return NULL;
}

/* Run a spec's run() directly. `json` may be NULL (no params). Returns the status. */
static pc_status run_cmd(pc_engine_session *s, const char *id, const char *json)
{
    const pc_engine_command *c = find_spec(id);
    CHECK(c != NULL);
    if (c == NULL) {
        return PC_ERR_INVALID;
    }
    yyjson_doc *pd = NULL;
    yyjson_val *params = NULL;
    if (json != NULL && pc_json_parse(json, strlen(json), &pd, NULL) == PC_OK) {
        params = yyjson_doc_get_root(pd);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = c->run(s, params, &out, &e);
    pc_json_value_fini(&out);
    if (pd != NULL) {
        yyjson_doc_free(pd);
    }
    return rc;
}

static pc_status enabled_of(pc_engine_session *s, const char *id)
{
    const pc_engine_command *c = find_spec(id);
    CHECK(c != NULL);
    if (c == NULL) {
        return PC_ERR_INVALID;
    }
    pc_error e = {0};
    return c->enabled(s, &e);
}

/* ---- tests -------------------------------------------------------------------------------- */

static void test_registry(void)
{
    size_t n = 0;
    const pc_engine_command *sp = pc_engine_align_distribute_specs(&n);
    CHECK_EQ_UINT(n, 14);
    /* Every id is present, journalled, and carries a two-level menu. */
    static const char *const ids[] = {
        "layer.align.topEdges",           "layer.align.verticalCenters",
        "layer.align.bottomEdges",        "layer.align.leftEdges",
        "layer.align.horizontalCenters",  "layer.align.rightEdges",
        "layer.distribute.topEdges",      "layer.distribute.verticalCenters",
        "layer.distribute.bottomEdges",   "layer.distribute.leftEdges",
        "layer.distribute.horizontalCenters", "layer.distribute.rightEdges",
        "layer.distribute.horizontally",  "layer.distribute.vertically"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const pc_engine_command *c = find_spec(ids[i]);
        CHECK(c != NULL);
        if (c != NULL) {
            CHECK(c->journal);
            CHECK_EQ_UINT(c->menu_len, 2);
            CHECK(c->shortcut == NULL);
            CHECK(c->enabled != NULL && c->run != NULL);
        }
    }
    (void)sp;
}

static void test_align_top_edges(void)
{
    pc_engine_session *s = mk(200, 200);
    /* Three 10x10 layers stacked at y = 5, 20, 35 (all x = 0). */
    pc_layer_id a = add_layer(s, 0, 5, 10, 10);
    pc_layer_id b = add_layer(s, 0, 20, 10, 10);
    pc_layer_id c = add_layer(s, 0, 35, 10, 10);
    pc_layer_id ids[3] = {a, b, c};
    select_layers(s, ids, 3);

    CHECK(enabled_of(s, "layer.align.topEdges") == PC_OK);
    CHECK(run_cmd(s, "layer.align.topEdges", NULL) == PC_OK);

    /* Every top edge lines up with the topmost (y0 == 5); x unchanged. */
    CHECK_EQ_INT(bounds_of(s, a).y0, 5);
    CHECK_EQ_INT(bounds_of(s, b).y0, 5);
    CHECK_EQ_INT(bounds_of(s, c).y0, 5);
    CHECK_EQ_INT(bounds_of(s, b).x0, 0);
    pc_engine_session_free(s);
}

static void test_distribute_vertical_centers(void)
{
    pc_engine_session *s = mk(200, 200);
    /* Centres at y = 10 (5..15), 30 (25..35), 100 (95..105). */
    pc_layer_id a = add_layer(s, 0, 5, 10, 10);
    pc_layer_id b = add_layer(s, 0, 25, 10, 10);
    pc_layer_id c = add_layer(s, 0, 95, 10, 10);
    pc_layer_id ids[3] = {a, b, c};
    select_layers(s, ids, 3);

    CHECK(enabled_of(s, "layer.distribute.verticalCenters") == PC_OK);
    CHECK(run_cmd(s, "layer.distribute.verticalCenters", NULL) == PC_OK);

    /* Outer centres stay (10, 100); the middle lands halfway at 55 -> rect 50..60. */
    CHECK_EQ_INT(bounds_of(s, a).y0, 5);
    CHECK_EQ_INT(bounds_of(s, c).y0, 95);
    pc_rect br = bounds_of(s, b);
    CHECK_EQ_INT(br.y0, 50);
    CHECK_EQ_INT(br.y1, 60);
    pc_engine_session_free(s);
}

static void test_graceful_failures(void)
{
    /* No document: enabled and run both refuse, no crash. */
    pc_engine_session *s0 = NULL;
    CHECK(pc_engine_session_new(&s0, NULL) == PC_OK);
    CHECK(enabled_of(s0, "layer.align.topEdges") != PC_OK);
    CHECK(run_cmd(s0, "layer.align.topEdges", NULL) != PC_OK);
    CHECK(run_cmd(s0, "layer.distribute.topEdges", NULL) != PC_OK);
    pc_engine_session_free(s0);

    /* A bad `to` value (and a wrong-typed one) must not crash. */
    pc_engine_session *s = mk(100, 100);
    pc_layer_id a = add_layer(s, 0, 5, 10, 10);
    pc_layer_id b = add_layer(s, 0, 20, 10, 10);
    pc_layer_id c = add_layer(s, 0, 35, 10, 10);
    pc_layer_id ids[3] = {a, b, c};
    select_layers(s, ids, 3);
    CHECK(run_cmd(s, "layer.align.topEdges", "{\"to\":\"bogus\"}") != PC_OK);
    /* A non-string `to` falls back to auto and runs cleanly. */
    CHECK(run_cmd(s, "layer.align.topEdges", "{\"to\":5}") == PC_OK);
    /* Explicit canvas/selection paths. */
    CHECK(run_cmd(s, "layer.align.leftEdges", "{\"to\":\"canvas\"}") == PC_OK);
    CHECK(run_cmd(s, "layer.align.leftEdges", "{\"to\":\"selection\"}") != PC_OK); /* none set */

    /* Distribute needs three layers: with one selected it refuses. */
    pc_layer_id one[1] = {a};
    select_layers(s, one, 1);
    CHECK(enabled_of(s, "layer.distribute.topEdges") != PC_OK);
    CHECK(run_cmd(s, "layer.distribute.topEdges", NULL) != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_align_top_edges);
    RUN(test_distribute_vertical_centers);
    RUN(test_graceful_failures);
    return pc_t_finish();
}
