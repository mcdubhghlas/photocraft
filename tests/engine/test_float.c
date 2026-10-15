/* Floating selection: select.float / select.drop and the before-command drop (E4). */
#include <stdlib.h>

#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "raster/surface.h"
#include "harness.h"

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return rc;
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(exec(s, "file.new", "{\"width\":40,\"height\":20,\"background\":\"transparent\"}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#ff0000\"}") == PC_OK); /* fill the layer red */
    return s;
}

static void px(pc_engine_session *s, int32_t x, int32_t y, float out[4])
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_layer *l = pc_document_layer(doc, st->active_layer);
    pc_surface_rgba(pc_layer_surface(l), x, y, out);
}

static void test_float_and_drop(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":10}") == PC_OK);
    float before[4];
    px(s, 2, 2, before);
    CHECK(before[0] == 1.0f); /* red before floating */
    /* Float the selected pixels and move them +5 in x. The document is unchanged while floating. */
    CHECK(exec(s, "select.float", "{\"dx\":5,\"dy\":0}") == PC_OK);
    float during[4];
    px(s, 2, 2, during);
    CHECK(during[0] == 1.0f && during[3] == 1.0f); /* still red: not dropped yet */
    /* Drop: the piece lands +5; the cut hole (x<5) is transparent, the piece and the untouched
     * right edge are red. */
    CHECK(exec(s, "select.drop", "{}") == PC_OK);
    float got[4];
    px(s, 2, 2, got);
    CHECK(got[3] == 0.0f); /* the hole left by the cut */
    px(s, 7, 2, got);
    CHECK(got[0] == 1.0f && got[3] == 1.0f); /* the moved piece */
    px(s, 20, 2, got);
    CHECK(got[0] == 1.0f && got[3] == 1.0f); /* untouched area */
    pc_engine_session_free(s);
}

static void test_undo_returns_the_piece(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":10}") == PC_OK);
    CHECK(exec(s, "select.float", "{\"dx\":5,\"dy\":0}") == PC_OK);
    CHECK(pc_engine_session_active(s)->floating != NULL);
    /* Undo puts the floating piece back and is used up: the document is unchanged, nothing floats. */
    CHECK(exec(s, "edit.undo", "{}") == PC_OK);
    CHECK(pc_engine_session_active(s)->floating == NULL);
    float got[4];
    px(s, 2, 2, got);
    CHECK(got[0] == 1.0f && got[3] == 1.0f); /* unchanged */
    pc_engine_session_free(s);
}

static void test_other_command_drops_first(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":10}") == PC_OK);
    CHECK(exec(s, "select.float", "{\"dx\":5,\"dy\":0}") == PC_OK);
    /* A non-floating command drops the piece first (so it lands where shown). */
    CHECK(exec(s, "edit.fill", "{\"color\":\"#00ff00\",\"opacity\":0}") == PC_OK);
    CHECK(pc_engine_session_active(s)->floating == NULL);
    float got[4];
    px(s, 2, 2, got);
    CHECK(got[3] == 0.0f); /* the drop happened */
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk();
    /* No selection: float is disabled / an error. */
    CHECK(exec(s, "select.float", "{}") != PC_OK);
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":5,\"height\":5}") == PC_OK);
    const char *bad[] = {"[]", "\"x\"", "3"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        (void)exec(s, "select.float", bad[i]);
        (void)exec(s, "select.drop", bad[i]);
    }
    /* Drop with nothing floating: dropped:false, no crash. */
    CHECK(exec(s, "select.drop", "{}") == PC_OK);
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(exec(empty, "select.float", "{}") != PC_OK);
    CHECK(exec(empty, "select.drop", "{}") != PC_OK); /* disabled without a document */
    pc_engine_session_free(empty);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_float_and_drop);
    RUN(test_undo_returns_the_piece);
    RUN(test_other_command_drops_first);
    RUN(test_adversarial);
    return pc_t_finish();
}
