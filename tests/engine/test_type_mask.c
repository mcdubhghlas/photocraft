#include "harness.h"
#include "engine/engine.h"
#include <stdlib.h>
#include <string.h>
static pc_status run(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_session_execute_json(s, id, params, &out, NULL);
    pc_json_value_fini(&out);
    return status;
}
static pc_engine_session *fresh(unsigned depth, const char *mode)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char params[160];
    (void)snprintf(params, sizeof params, "{\"width\":300,\"height\":300,\"depth\":%u,\"mode\":\"%s\"}",
                   depth, mode);
    CHECK(run(s, "file.new", params) == PC_OK);
    return s;
}
static const pc_document *document(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}
static void test_type_mask_glyphs_without_creating_a_layer(void)
{
    const char *modes[] = {"rgb", "grayscale", "cmyk", "lab"};
    for (unsigned depth = 8; depth <= 32; depth *= 2) {
        for (size_t m = 0; m < 4; m++) {
            pc_engine_session *s = fresh(depth, modes[m]);
            size_t count = document(s)->nlayers;
            CHECK(run(s, "type.createSelection",
                      "{\"text\":\"I\",\"x\":40,\"y\":120,\"size\":48,\"color\":[1,0,0,0]}") == PC_OK);
            CHECK(document(s)->has_selection && document(s)->nlayers == count);
            pc_rect selected = pc_surface_content_bounds(&document(s)->selection);
            CHECK(!pc_rect_is_empty(selected));
            CHECK(pc_engine_session_undo(s));
            CHECK(!document(s)->has_selection);
            CHECK(run(s, "type.create", "{\"text\":\"I\",\"x\":40,\"y\":120,\"size\":48}") == PC_OK);
            const pc_layer *type = pc_document_layer(document(s), pc_engine_session_active(s)->active_layer);
            CHECK(type && type->content.kind == PC_LAYER_TEXT && type->content.u.text.has_cache);
            pc_rect text = pc_rect_intersect(pc_surface_content_bounds(&type->content.u.text.cache),
                                             pc_document_bounds(document(s)));
            CHECK(selected.x0 == text.x0 && selected.y0 == text.y0 && selected.x1 == text.x1 &&
                  selected.y1 == text.y1);
            pc_engine_session_free(s);
        }
    }
    pc_engine_session *s = fresh(8, "rgb");
    CHECK(run(s, "type.createSelection",
              "{\"text\":\"HELLO\",\"x\":150,\"y\":40,\"size\":12,\"orientation\":\"vertical\"}") == PC_OK);
    pc_rect r = pc_surface_content_bounds(&document(s)->selection);
    CHECK(pc_rect_height(r) > 2 * pc_rect_width(r));
    pc_engine_session_free(s);
}
static void test_type_mask_validation_combination_and_atomic_remove(void)
{
    pc_engine_session *s = fresh(16, "rgb");
    const char *bad[] = {"{\"layer\":\"x\"}",    "{\"removeLayer\":true}",     "{\"operation\":3}",
                         "{\"operation\":null}", "{\"antialias\":\"blurry\"}", "{\"layer\":999}",
                         "{\"layer\":0}",        "{\"removeLayer\":1}",        "{\"size\":1e100}"};
    uint64_t revision = pc_engine_session_active(s)->revision;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(run(s, "type.createSelection", bad[i]) != PC_OK);
        CHECK(pc_engine_session_active(s)->revision == revision);
    }
    CHECK(run(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":10}") == PC_OK);
    CHECK(run(s, "type.createSelection",
              "{\"text\":\"I\",\"x\":40,\"y\":120,\"size\":12,\"operation\":\"add\"}") == PC_OK);
    float pixel[1];
    pc_surface_read_pixel(&document(s)->selection, 2, 2, pixel);
    CHECK(pixel[0] == 1);
    CHECK(run(s, "type.createSelection", "{\"text\":\"\",\"operation\":\"intersect\"}") == PC_OK);
    CHECK(!document(s)->has_selection);
    CHECK(run(s, "type.create", "{\"text\":\"Mask\",\"x\":20,\"y\":50}") == PC_OK);
    pc_layer_id id = pc_engine_session_active(s)->active_layer;
    char params[160];
    (void)snprintf(params, sizeof params, "{\"layer\":%llu,\"removeLayer\":true}", (unsigned long long)id);
    CHECK(run(s, "type.createSelection", params) == PC_OK);
    CHECK(!pc_document_layer(document(s), id) && document(s)->has_selection);
    CHECK(pc_engine_session_undo(s));
    CHECK(pc_document_layer(document(s), id) != NULL);
    pc_layer_id background = document(s)->layers[0].id;
    (void)snprintf(params, sizeof params, "{\"layer\":%llu}", (unsigned long long)background);
    CHECK(run(s, "layer.delete", params) == PC_OK);
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    (void)snprintf(params, sizeof params, "{\"layer\":%llu,\"removeLayer\":true}", (unsigned long long)id);
    CHECK(run(s, "type.createSelection", params) == PC_ERR_INVALID);
    CHECK(pc_document_eq(pc_doc_arc_get(before), document(s)));
    pc_doc_arc_release(before);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_type_mask_glyphs_without_creating_a_layer);
    RUN(test_type_mask_validation_combination_and_atomic_remove);
    return pc_t_finish();
}
