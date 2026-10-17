/* Select > Transform Selection (select_extra_cmds.c).
 *
 * Scales / rotates / warps the active selection mask. No selection, a collapsing transform and a
 * degenerate quad are graceful errors (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdlib.h>
#include <string.h>

#include "algo/selection.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "harness.h"

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

/* Put a rectangular selection on the active document. */
struct selctx {
    pc_rect r;
};
static pc_status sel_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    (void)ha;
    (void)active;
    struct selctx *c = v;
    uint32_t w = pc_rect_width(c->r), h = pc_rect_height(c->r);
    size_t n = (size_t)w * h;
    float *m = malloc(n * sizeof *m);
    if (m == NULL) {
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < n; i++) {
        m[i] = 1.0f;
    }
    pc_surface out;
    bool some = false;
    pc_status rc = pc_algo_selection_combine(NULL, m, n, c->r, PC_ALGO_SELECTION_REPLACE, &out, &some, e);
    free(m);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
    }
    doc->selection = out;
    doc->has_selection = some;
    return PC_OK;
}

static void set_selection(pc_engine_session *s, pc_rect r)
{
    struct selctx c = {r};
    CHECK(pc_engine_session_edit(s, "sel", sel_edit, &c, NULL) == PC_OK);
}

static pc_status run_json(pc_engine_session *s, const char *params, bool *changed, pc_json_value *out)
{
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, "select.transformSelection", params, out, &e);
    if (changed != NULL) {
        *changed = false;
        yyjson_mut_val *root = pc_json_value_root(out);
        if (root != NULL && yyjson_mut_is_obj(root)) {
            yyjson_mut_val *c = yyjson_mut_obj_get(root, "changed");
            if (c != NULL) {
                *changed = yyjson_mut_get_bool(c);
            }
        }
    }
    return rc;
}

static void test_scale(void)
{
    pc_engine_session *s = mk(64, 64);
    set_selection(s, pc_rect_from_xywh(16, 16, 16, 16));
    pc_json_value out = PC_JSON_VALUE_INIT;
    bool changed = false;
    CHECK(run_json(s, "{\"scaleX\":200,\"scaleY\":200}", &changed, &out) == PC_OK);
    CHECK(changed);
    pc_json_value_fini(&out);
    /* Identity transform reports changed=false. */
    CHECK(run_json(s, "{\"scaleX\":100,\"scaleY\":100}", &changed, &out) == PC_OK);
    CHECK(!changed);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_warp(void)
{
    pc_engine_session *s = mk(64, 64);
    set_selection(s, pc_rect_from_xywh(8, 8, 48, 48));
    pc_json_value out = PC_JSON_VALUE_INIT;
    bool changed = false;
    CHECK(run_json(s, "{\"style\":\"arc\",\"bend\":40}", &changed, &out) == PC_OK);
    CHECK(changed);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_quad(void)
{
    pc_engine_session *s = mk(64, 64);
    set_selection(s, pc_rect_from_xywh(8, 8, 16, 16));
    pc_json_value out = PC_JSON_VALUE_INIT;
    bool changed = false;
    CHECK(run_json(s, "{\"corners\":[[25,30],[35,30],[45,40],[15,40]]}", &changed, &out) == PC_OK);
    CHECK(changed);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    /* No selection. */
    pc_engine_session *s = mk(32, 32);
    CHECK(run_json(s, "{\"scaleX\":200}", NULL, &out) != PC_OK);
    pc_json_value_fini(&out);
    /* Collapsing transform (scaleX 0). */
    set_selection(s, pc_rect_from_xywh(4, 4, 8, 8));
    CHECK(run_json(s, "{\"scaleX\":0}", NULL, &out) != PC_OK);
    pc_json_value_fini(&out);
    /* A far off-canvas transform must return quickly without hanging or crashing (#); the
     * moved selection is small, so it succeeds. The document stays usable afterwards. */
    CHECK(run_json(s, "{\"dx\":1e5,\"dy\":-1e5,\"scaleX\":300}", NULL, &out) == PC_OK);
    pc_json_value_fini(&out);
    pc_json_value aout = PC_JSON_VALUE_INIT;
    pc_error ae = {0};
    CHECK(pc_engine_session_execute_json(s, "select.all", "{}", &aout, &ae) == PC_OK);
    pc_json_value_fini(&aout);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_scale);
    RUN(test_warp);
    RUN(test_quad);
    RUN(test_graceful);
    return pc_t_finish();
}
