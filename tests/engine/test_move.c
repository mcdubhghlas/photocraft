#include "harness.h"
#include "engine/move.h"
#include "engine/engine_internal.h"
#include "compose/compose.h"
#include <string.h>
static pc_status command(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value result = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_session_execute_json(s, id, params, &result, NULL);
    pc_json_value_fini(&result);
    return status;
}
static pc_engine_session *session(unsigned depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char params[128];
    (void)snprintf(params, sizeof params,
                   "{\"width\":64,\"height\":48,\"depth\":%u,\"background\":\"transparent\"}", depth);
    CHECK(command(s, "file.new", params) == PC_OK);
    CHECK(command(s, "paint.pencil",
                  "{\"points\":[[10,10],[15,10]],\"size\":4,\"smoothing\":0,\"color\":\"#c32044\"}") ==
          PC_OK);
    return s;
}
static void test_move_preview_commit_and_undo(void)
{
    unsigned depths[] = {8, 16, 32};
    for (size_t i = 0; i < 3; i++) {
        pc_engine_session *s = session(depths[i]);
        const pc_engine_doc_state *state = pc_engine_session_active(s);
        uint64_t revision = state->revision;
        pc_doc_arc *original = pc_doc_arc_retain(state->doc);
        yyjson_doc *params = NULL;
        const char *text = "{\"dx\":12,\"dy\":7}";
        CHECK(pc_json_parse(text, strlen(text), &params, NULL) == PC_OK);
        pc_doc_arc *preview = NULL;
        CHECK(pc_engine_move_preview(s, yyjson_doc_get_root(params), &preview, NULL) == PC_OK);
        yyjson_doc_free(params);
        CHECK(pc_engine_session_active(s)->revision == revision);
        CHECK(command(s, "layer.translate", text) == PC_OK);
        state = pc_engine_session_active(s);
        CHECK(state->revision == revision + 1);
        const pc_surface *a = pc_layer_surface(&pc_doc_arc_get(preview)->layers[0]);
        const pc_surface *b = pc_layer_surface(&pc_doc_arc_get(state->doc)->layers[0]);
        CHECK(a && b && pc_surface_eq(a, b));
        float color[4];
        pc_surface_rgba(b, 22, 17, color);
        CHECK(color[3] > 0.9f && color[0] > 0.5f);
        CHECK(command(s, "edit.undo", "{}") == PC_OK);
        CHECK(pc_surface_eq(pc_layer_surface(&pc_doc_arc_get(original)->layers[0]),
                            pc_layer_surface(&pc_doc_arc_get(pc_engine_session_active(s)->doc)->layers[0])));
        pc_doc_arc_release(preview);
        pc_doc_arc_release(original);
        pc_engine_session_free(s);
    }
}
static void test_move_rejections_are_atomic(void)
{
    pc_engine_session *s = session(16);
    uint64_t revision = pc_engine_session_active(s)->revision;
    const char *bad[] = {"{\"dx\":2147483648}", "{\"dy\":1e100}", "{\"units\":\"inch\",\"dx\":1}",
                         "{\"units\":\"pt\",\"dx\":1e100}", "{\"layer\":999999,\"dx\":2}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        CHECK(command(s, "layer.translate", bad[i]) != PC_OK);
        CHECK(pc_engine_session_active(s)->revision == revision);
    }
    CHECK(command(s, "layer.translate", "{\"dx\":0,\"dy\":0}") == PC_OK);
    CHECK(pc_engine_session_active(s)->revision == revision);
    CHECK(command(s, "layer.translate", "{\"dx\":2,\"dy\":1,\"units\":\"pt\"}") == PC_OK);
    CHECK(command(s, "layer.translate", "{\"dx\":1.5,\"dy\":-0.5}") == PC_OK);
    pc_engine_session_free(s);
}
static void select_id(pc_engine_session *s, pc_layer_id id, const char *mode)
{
    char params[160];
    (void)snprintf(params, sizeof params, "{\"layer\":%llu,\"mode\":\"%s\"}", (unsigned long long)id, mode);
    CHECK(command(s, "layer.select", params) == PC_OK);
}
static void compare_move_preview_commit(pc_engine_session *s, const char *text)
{
    yyjson_doc *params = NULL;
    CHECK(pc_json_parse(text, strlen(text), &params, NULL) == PC_OK);
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc), *preview = NULL;
    CHECK(pc_engine_move_preview(s, yyjson_doc_get_root(params), &preview, NULL) == PC_OK);
    yyjson_doc_free(params);
    CHECK(command(s, "layer.translate", text) == PC_OK);
    if (preview) {
        CHECK(pc_document_eq(pc_doc_arc_get(preview), pc_doc_arc_get(pc_engine_session_active(s)->doc)));
    }
    CHECK(pc_engine_session_undo(s));
    CHECK(pc_document_eq(pc_doc_arc_get(before), pc_doc_arc_get(pc_engine_session_active(s)->doc)));
    pc_doc_arc_release(preview);
    pc_doc_arc_release(before);
}
static void test_move_text_shape_smart_and_group_payloads(void)
{
    const char *ids[] = {"type.create", "shape.create", "layer.smartObjects.convertToSmartObject",
                         "layer.groupLayers"};
    const char *params[] = {"{\"text\":\"Move type\",\"size\":12,\"x\":5,\"y\":15}",
                            "{\"kind\":\"rect\",\"rect\":[8,6,20,15]}", "{}", "{}"};
    for (unsigned depth = 8; depth <= 32; depth *= 2) {
        for (size_t i = 0; i < 4; i++) {
            pc_engine_session *s = session(depth);
            CHECK(command(s, ids[i], params[i]) == PC_OK);
            compare_move_preview_commit(s, "{\"dx\":7,\"dy\":-3}");
            pc_engine_session_free(s);
        }
    }
}
static void test_move_links_group_dedup_and_locked_rollback(void)
{
    pc_engine_session *s = session(16);
    pc_layer_id first = pc_engine_session_active(s)->active_layer;
    CHECK(command(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(command(s, "paint.pencil",
                  "{\"points\":[[30,20],[35,20]],\"size\":3,\"smoothing\":0,\"color\":\"#00ff00\"}") ==
          PC_OK);
    pc_layer_id second = pc_engine_session_active(s)->active_layer;
    select_id(s, first, "add");
    CHECK(command(s, "layer.linkLayers", "{}") == PC_OK);
    select_id(s, first, "replace");
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    pc_rect a =
        pc_surface_content_bounds(&pc_document_layer(pc_doc_arc_get(before), first)->content.u.raster);
    pc_rect b =
        pc_surface_content_bounds(&pc_document_layer(pc_doc_arc_get(before), second)->content.u.raster);
    CHECK(command(s, "layer.translate", "{\"dx\":4,\"dy\":5}") == PC_OK);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    pc_rect moved_a = pc_surface_content_bounds(&pc_document_layer(doc, first)->content.u.raster);
    pc_rect moved_b = pc_surface_content_bounds(&pc_document_layer(doc, second)->content.u.raster);
    CHECK(moved_a.x0 == a.x0 + 4 && moved_a.y0 == a.y0 + 5 && moved_b.x0 == b.x0 + 4 &&
          moved_b.y0 == b.y0 + 5);
    CHECK(pc_engine_session_undo(s));
    pc_doc_arc_release(before);
    char lock[160];
    (void)snprintf(lock, sizeof lock, "{\"layer\":%llu,\"locks\":{\"position\":true}}",
                   (unsigned long long)second);
    CHECK(command(s, "layer.setProps", lock) == PC_OK);
    before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    uint64_t revision = pc_engine_session_active(s)->revision;
    CHECK(command(s, "layer.translate", "{\"dx\":4,\"dy\":5}") == PC_ERR_INVALID);
    CHECK(pc_engine_session_active(s)->revision == revision &&
          pc_document_eq(pc_doc_arc_get(before), pc_doc_arc_get(pc_engine_session_active(s)->doc)));
    pc_doc_arc_release(before);
    CHECK(pc_engine_session_undo(s));
    select_id(s, second, "add");
    CHECK(command(s, "layer.groupLayers", "{}") == PC_OK);
    pc_layer_id group = pc_engine_session_active(s)->active_layer;
    select_id(s, first, "add");
    select_id(s, group, "add");
    CHECK(command(s, "layer.translate", "{\"dx\":3,\"dy\":2}") == PC_OK);
    doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    moved_a = pc_surface_content_bounds(&pc_document_layer(doc, first)->content.u.raster);
    CHECK(moved_a.x0 == a.x0 + 3 && moved_a.y0 == a.y0 + 2);
    pc_engine_session_free(s);
}

static pc_status deny_duplicate_move(const char *command, yyjson_val *params, pc_error *err)
{
    (void)params;
    return !strcmp(command, "layer.duplicate") ? pc_fail(err, PC_ERR_INVALID, "duplicate denied") : PC_OK;
}
static void test_copy_move_multiple_targets_preview_undo_redo_and_policy(void)
{
    pc_engine_session *s = session(32);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    pc_layer_id first = state->active_layer;
    CHECK(command(s, "layer.new.layer", "{}") == PC_OK);
    pc_layer_id second = state->active_layer;
    CHECK(command(s, "paint.pencil", "{\"points\":[[24,20],[28,20]],\"size\":3,\"color\":\"#239bc0\"}") ==
          PC_OK);
    char text[180];
    (void)snprintf(text, sizeof text, "{\"layer\":%llu,\"mode\":\"add\"}", (unsigned long long)first);
    CHECK(command(s, "layer.select", text) == PC_OK);
    CHECK(state->nselected == 2);
    pc_doc_arc *before = pc_doc_arc_retain(state->doc);
    size_t n = pc_doc_arc_get(before)->nlayers, history = pc_history_past_len(state->history);
    const char *move = "{\"copy\":true,\"dx\":6,\"dy\":-3}";
    yyjson_doc *params = NULL;
    CHECK(pc_json_parse(move, strlen(move), &params, NULL) == PC_OK);
    pc_doc_arc *preview = NULL;
    pc_layer_id preview_active = 0;
    CHECK(pc_engine_move_preview_layer(s, yyjson_doc_get_root(params), &preview, &preview_active, NULL) ==
          PC_OK);
    CHECK(preview && pc_doc_arc_get(preview)->nlayers == n + 2);
    CHECK(preview && preview_active && pc_document_layer(pc_doc_arc_get(preview), preview_active));
    CHECK(state->doc == before && pc_history_past_len(state->history) == history);
    s->authorize = deny_duplicate_move;
    pc_doc_arc *denied = NULL;
    CHECK(pc_engine_move_preview(s, yyjson_doc_get_root(params), &denied, NULL) == PC_ERR_INVALID && !denied);
    CHECK(command(s, "layer.translate", move) == PC_ERR_INVALID);
    CHECK(state->doc == before && pc_history_past_len(state->history) == history);
    s->authorize = NULL;
    CHECK(command(s, "layer.translate", move) == PC_OK);
    const pc_document *doc = pc_doc_arc_get(state->doc);
    CHECK(doc->nlayers == n + 2 && state->nselected == 2 && state->active_layer != first &&
          state->active_layer != second);
    CHECK(pc_history_past_len(state->history) == history + 1);
    const pc_layer_id originals[] = {first, second};
    for (size_t i = 0; i < 2; i++)
        CHECK(pc_surface_eq(pc_layer_surface(pc_document_layer(doc, originals[i])),
                            pc_layer_surface(pc_document_layer(pc_doc_arc_get(before), originals[i]))));
    pc_compose_buffer a = {0}, b = {0};
    CHECK(pc_compose_render(doc, pc_rect_from_size(doc->size), &a, NULL) == PC_OK);
    CHECK(pc_compose_render(pc_doc_arc_get(preview), pc_rect_from_size(doc->size), &b, NULL) == PC_OK);
    CHECK(pc_compose_buffer_eq(&a, &b));
    pc_compose_buffer_fini(&a);
    pc_compose_buffer_fini(&b);
    CHECK(command(s, "edit.undo", "{}") == PC_OK);
    CHECK(pc_doc_arc_get(state->doc)->nlayers == n && state->nselected == 2);
    CHECK(command(s, "edit.redo", "{}") == PC_OK);
    CHECK(pc_doc_arc_get(state->doc)->nlayers == n + 2 && state->nselected == 2);
    pc_doc_arc_release(before);
    pc_doc_arc_release(preview);
    yyjson_doc_free(params);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_move_text_shape_smart_and_group_payloads);
    RUN(test_move_links_group_dedup_and_locked_rollback);
    RUN(test_copy_move_multiple_targets_preview_undo_redo_and_policy);
    RUN(test_move_preview_commit_and_undo);
    RUN(test_move_rejections_are_atomic);
    return pc_t_finish();
}
