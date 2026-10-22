/* Patch Tool live preview helper (pc_engine_patch_preview in retouch_jobs.inc): returns a doc copy
 * with the patch applied (coarse membrane) and the damaged rect, without touching the session or its
 * history. Graceful failure on bad params / no selection / no document (rule 9). */
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/retouch_cmds.h"
#include "ops/ops.h"
#include "raster/surface.h"
#include "harness.h"

/* Uniform mid-grey with a bright blemish inside the selection area, so healing it away is visible:
 * seamless-cloning a clean grey source over the blemish, with grey surroundings, removes it. */
static pc_status split_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    (void)ha;
    (void)active;
    (void)v;
    pc_surface *su = pc_layer_surface_mut(&doc->layers[0]);
    if (su == NULL) {
        return pc_fail(e, PC_ERR_INVALID, "no surface");
    }
    for (int32_t y = 0; y < 48; y++) {
        for (int32_t x = 0; x < 64; x++) {
            float g = 0.5f;
            if (x >= 12 && x < 20 && y >= 12 && y < 20) {
                g = 0.95f; /* the blemish */
            }
            float px[4] = {g, g, g, 1.0f};
            if (pc_surface_write_region(su, pc_rect_from_xywh(x, y, 1, 1), px, 4, e) != PC_OK) {
                return e != NULL ? e->code : PC_ERR_NOMEM;
            }
        }
    }
    return PC_OK;
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(64, 48), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(0.1f, 0.1f, 0.1f, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_engine_session_edit(s, "paint", split_edit, NULL, NULL) == PC_OK);
    return s;
}

static void select_rect(pc_engine_session *s, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "select.rect", p, &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
}

static float layer0_px(const pc_document *doc, int32_t x, int32_t y)
{
    const pc_surface *su = pc_layer_surface(&doc->layers[0]);
    return su != NULL ? pc_surface_sample_channel(su, x, y, 0) : -1.0f;
}

static yyjson_val *parse(const char *json, yyjson_doc **hold)
{
    *hold = NULL;
    CHECK(pc_json_parse(json, strlen(json), hold, NULL) == PC_OK);
    return yyjson_doc_get_root(*hold);
}

static void test_preview_applies_without_touching_session(void)
{
    pc_engine_session *s = mk();
    /* Select a 16x16 square in the dark half; sample from the light half (offset +40). */
    select_rect(s, "{\"x\":8,\"y\":8,\"width\":16,\"height\":16}");
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    float orig = layer0_px(pc_doc_arc_get(d->doc), 16, 16);
    CHECK(orig > 0.9f); /* the bright blemish */

    yyjson_doc *hold = NULL;
    yyjson_val *p = parse("{\"offset\":[30,0]}", &hold);
    pc_doc_arc *preview = NULL;
    pc_rect dmg = PC_RECT_EMPTY;
    pc_error e = {0};
    CHECK(pc_engine_patch_preview(s, p, 64, 1, &preview, &dmg, &e) == PC_OK);
    CHECK(preview != NULL);
    CHECK(!pc_rect_is_empty(dmg));
    /* The preview healed the blemish away towards the clean grey surroundings. */
    float healed = layer0_px(pc_doc_arc_get(preview), 16, 16);
    CHECK(healed < orig - 0.2f);
    pc_doc_arc_release(preview);
    yyjson_doc_free(hold);

    /* The session itself is untouched: the preview worked on a copy, so the blemish remains. */
    d = pc_engine_session_active(s);
    CHECK(layer0_px(pc_doc_arc_get(d->doc), 16, 16) > 0.9f);
    pc_engine_session_free(s);
}

static void test_min_step_coarsens(void)
{
    pc_engine_session *s = mk();
    select_rect(s, "{\"x\":8,\"y\":8,\"width\":16,\"height\":16}");
    yyjson_doc *hold = NULL;
    yyjson_val *p = parse("{\"offset\":[40,0]}", &hold);
    pc_doc_arc *preview = NULL;
    pc_rect dmg = PC_RECT_EMPTY;
    /* A large min_step forces the coarse membrane path. */
    CHECK(pc_engine_patch_preview(s, p, 1, 8, &preview, &dmg, NULL) == PC_OK);
    CHECK(preview != NULL && !pc_rect_is_empty(dmg));
    pc_doc_arc_release(preview);
    yyjson_doc_free(hold);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    /* No document. */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    yyjson_doc *h1 = NULL;
    yyjson_val *p1 = parse("{\"offset\":[1,0]}", &h1);
    pc_doc_arc *prev = NULL;
    pc_rect dmg = PC_RECT_EMPTY;
    CHECK(pc_engine_patch_preview(empty, p1, 64, 1, &prev, &dmg, NULL) != PC_OK);
    CHECK(prev == NULL);
    yyjson_doc_free(h1);
    pc_engine_session_free(empty);

    /* A document with no selection. */
    pc_engine_session *s = mk();
    yyjson_doc *h2 = NULL;
    yyjson_val *p2 = parse("{\"offset\":[1,0]}", &h2);
    CHECK(pc_engine_patch_preview(s, p2, 64, 1, &prev, &dmg, NULL) != PC_OK);
    yyjson_doc_free(h2);

    /* A selection but no offset, and a zero offset. */
    select_rect(s, "{\"x\":8,\"y\":8,\"width\":16,\"height\":16}");
    yyjson_doc *h3 = NULL;
    yyjson_val *p3 = parse("{}", &h3);
    CHECK(pc_engine_patch_preview(s, p3, 64, 1, &prev, &dmg, NULL) != PC_OK);
    yyjson_doc_free(h3);
    yyjson_doc *h4 = NULL;
    yyjson_val *p4 = parse("{\"offset\":[0,0]}", &h4);
    CHECK(pc_engine_patch_preview(s, p4, 64, 1, &prev, &dmg, NULL) != PC_OK);
    yyjson_doc_free(h4);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_preview_applies_without_touching_session);
    RUN(test_min_step_coarsens);
    RUN(test_graceful);
    return pc_t_finish();
}
