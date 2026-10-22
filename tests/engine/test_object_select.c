/* Object Selection (object_select_cmds.c + algo grabcut object_at): select.object in point and
 * rectangle modes finds an object; a click on the background changes nothing; modes combine;
 * adversarial params are graceful errors (rule 9). */
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/object_select_cmds.h"
#include "harness.h"
#include "raster/surface.h"

/* Paint a solid red square [x0,x1)x[y0,y1) into the active (background) layer. */
struct paint_box {
    int x0, y0, x1, y1;
};

static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    (void)ha;
    (void)active;
    struct paint_box *b = v;
    pc_layer *l = &doc->layers[0];
    pc_surface *su = pc_layer_surface_mut(l);
    if (su == NULL) {
        return pc_fail(e, PC_ERR_INVALID, "no surface");
    }
    for (int32_t y = b->y0; y < b->y1; y++) {
        for (int32_t x = b->x0; x < b->x1; x++) {
            float px[4] = {0.85f, 0.12f, 0.1f, 1.0f};
            if (pc_surface_write_region(su, pc_rect_from_xywh(x, y, 1, 1), px, 4, e) != PC_OK) {
                return e != NULL ? e->code : PC_ERR_NOMEM;
            }
        }
    }
    return PC_OK;
}

static pc_engine_session *disc_session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    /* A blue-ish background so the red square is a distinct object. */
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(120, 90), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(0.2f, 0.4f, 0.75f, 1.0f),
                                      NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    struct paint_box b = {40, 30, 80, 60}; /* 40x30 red square, ~11% of canvas */
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, &b, NULL) == PC_OK);
    return s;
}

static pc_status run(pc_engine_session *s, const char *params, pc_json_value *out)
{
    pc_error e = {0};
    return pc_engine_session_execute_json(s, "select.object", params, out, &e);
}

static bool result_changed(pc_engine_session *s, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(run(s, params, &out) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    bool changed = false;
    if (root != NULL) {
        yyjson_mut_val *c = yyjson_mut_obj_get(root, "changed");
        if (c != NULL) {
            changed = yyjson_mut_get_bool(c);
        }
    }
    pc_json_value_fini(&out);
    return changed;
}

/* Selection coverage at (x, y). */
static float cov(pc_engine_session *s, int32_t x, int32_t y)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!doc->has_selection) {
        return 0.0f;
    }
    return pc_surface_sample_channel(&doc->selection, x, y, 0);
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_object_select_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("select.object") != NULL);
}

static void test_point_selects_object(void)
{
    pc_engine_session *s = disc_session();
    CHECK(result_changed(s, "{\"point\":[60,45]}"));
    /* The square is selected, the background is not. */
    CHECK(cov(s, 60, 45) >= 0.5f);
    CHECK(cov(s, 5, 5) < 0.5f);
    pc_engine_session_free(s);
}

static void test_point_on_background_changes_nothing(void)
{
    pc_engine_session *s = disc_session();
    CHECK(!result_changed(s, "{\"point\":[3,3]}"));
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(!pc_doc_arc_get(d->doc)->has_selection);
    pc_engine_session_free(s);
}

static void test_rect_mode(void)
{
    pc_engine_session *s = disc_session();
    CHECK(result_changed(s, "{\"rect\":[36,26,48,38]}"));
    CHECK(cov(s, 60, 45) >= 0.5f);
    /* Negative width/height (drag up-left) are normalised. */
    pc_engine_session_free(s);
    s = disc_session();
    CHECK(result_changed(s, "{\"rect\":[84,64,-48,-38]}"));
    CHECK(cov(s, 60, 45) >= 0.5f);
    pc_engine_session_free(s);
}

static void test_modes_combine(void)
{
    pc_engine_session *s = disc_session();
    pc_json_value sa = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "select.all", "{}", &sa, NULL) == PC_OK);
    pc_json_value_fini(&sa);
    /* subtract the object from Select All. */
    (void)result_changed(s, "{\"point\":[60,45],\"mode\":\"subtract\"}");
    CHECK(cov(s, 60, 45) < 0.5f && cov(s, 3, 3) >= 0.5f);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(empty, "select.object", "{\"point\":[1,1]}", &o, NULL) != PC_OK);
    pc_json_value_fini(&o);
    pc_engine_session_free(empty);

    pc_engine_session *s = disc_session();
    const char *bad[] = {"{}", "{\"point\":[\"a\",1]}", "{\"point\":[1]}", "{\"rect\":[1,2]}",
                         "{\"rect\":[]}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(run(s, bad[i], &out) != PC_OK);
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_point_selects_object);
    RUN(test_point_on_background_changes_nothing);
    RUN(test_rect_mode);
    RUN(test_modes_combine);
    RUN(test_graceful);
    return pc_t_finish();
}
