/* Edit > Content-Aware Scale: edit.contentAwareScale (content_scale_cmds.c).
 *
 * Scaling the active pixel layer's content resizes it in one undoable step; percent, Amount and
 * skin-tone protection all run; adversarial params and no-document are graceful errors (never a
 * crash, REWRITE_PLAN.md rule 9). */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/content_scale_cmds.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
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

static pc_status add_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    pc_layer_id *idp = v;
    pc_layer l;
    if (pc_layer_raster(&l, "L", PC_PIXEL_FORMAT_RGBA8, e) != PC_OK) {
        return e != NULL ? e->code : PC_ERR_NOMEM;
    }
    pc_surface *su = pc_layer_surface_mut(&l);
    if (su != NULL) {
        /* A gradient so seams have structure. */
        for (int32_t y = 8; y < 38; y++) {
            for (int32_t x = 8; x < 48; x++) {
                float r = (float)(x - 8) / 40.0f;
                float px[4] = {r, 1.0f - r, 0.5f, 1.0f};
                (void)pc_surface_write_region(su, pc_rect_from_xywh(x, y, 1, 1), px, 4, e);
            }
        }
    }
    pc_layer_id id = 0;
    pc_status rc = pc_document_insert_above(doc, NULL, &l, &id, e);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    *idp = id;
    *ha = true;
    *active = id;
    return PC_OK;
}

static pc_layer_id add_layer(pc_engine_session *s)
{
    pc_layer_id id = 0;
    CHECK(pc_engine_session_edit(s, "add", add_edit, &id, NULL) == PC_OK);
    return id;
}

static pc_rect active_bounds(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    CHECK(l != NULL);
    const pc_surface *su = pc_layer_surface(l);
    return su != NULL ? pc_surface_content_bounds(su) : PC_RECT_EMPTY;
}

static pc_status run_json(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_content_scale_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("edit.contentAwareScale") != NULL);
}

static void test_scale_down_and_undo(void)
{
    pc_engine_session *s = mk(64, 48);
    (void)add_layer(s);
    pc_rect before = active_bounds(s);
    CHECK_EQ_INT((int)pc_rect_width(before), 40);
    CHECK(run_json(s, "edit.contentAwareScale", "{\"width\":20}") == PC_OK);
    pc_rect after = active_bounds(s);
    CHECK_EQ_INT((int)pc_rect_width(after), 20);
    CHECK_EQ_INT((int)pc_rect_height(after), 30);
    CHECK(pc_engine_session_undo(s));
    pc_rect undone = active_bounds(s);
    CHECK_EQ_INT((int)pc_rect_width(undone), 40);
    CHECK_EQ_INT((int)pc_rect_height(undone), 30);
    pc_engine_session_free(s);
}

static void test_percent_amount_skin(void)
{
    pc_engine_session *s = mk(64, 48);
    (void)add_layer(s);
    /* scaleX 50% -> width 20. */
    CHECK(run_json(s, "edit.contentAwareScale", "{\"scaleX\":50}") == PC_OK);
    CHECK_EQ_INT((int)pc_rect_width(active_bounds(s)), 20);
    /* Amount blend and skin-tone protection both run. */
    CHECK(run_json(s, "edit.contentAwareScale", "{\"width\":30,\"amount\":40}") == PC_OK);
    CHECK(run_json(s, "edit.contentAwareScale", "{\"height\":20,\"protectSkinTones\":true}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    /* No document / no pixel layer. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "edit.contentAwareScale", "{\"width\":10}") != PC_OK);
    pc_engine_session_free(s);

    s = mk(32, 32);
    (void)add_layer(s);
    CHECK(run_json(s, "edit.contentAwareScale", "{\"width\":0}") != PC_OK);
    CHECK(run_json(s, "edit.contentAwareScale", "{\"width\":70000}") != PC_OK);
    CHECK(run_json(s, "edit.contentAwareScale", "{\"width\":20000,\"height\":20000}") != PC_OK);
    CHECK(run_json(s, "edit.contentAwareScale", "{\"width\":20,\"x\":1e9}") != PC_OK);
    CHECK(run_json(s, "edit.contentAwareScale", "{\"width\":20,\"protect\":\"no such channel\"}") != PC_OK);
    /* A non-numeric size is ignored (treated as "no change", as the reference does): no crash. */
    CHECK(run_json(s, "edit.contentAwareScale", "{\"width\":\"big\"}") == PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_scale_down_and_undo);
    RUN(test_percent_amount_skin);
    RUN(test_graceful);
    return pc_t_finish();
}
