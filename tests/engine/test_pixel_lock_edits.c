/* Clear, Cut and Image > Adjustments refuse a pixel-locked layer and leave it unchanged, as
 * painting does (#1103). Port of crates/engine/tests/pixel_lock_edits.rs.
 *
 * image.autoTone / autoContrast / autoColor are DEFERRED in the C port (they need pixels::
 * adjust_surface, E8; see edit_cmds.c), so they are skipped with a note rather than asserted on.
 * The reference's "a refused Cut leaves the clipboard alone" case needs the session clipboard's
 * bounds, which is private to edit_cmds.c (struct pc_engine_clip), so it is not ported here. */
#include "doc/doc.h"
#include "engine/engine.h"
#include "harness.h"

/* A layer with a red stroke across it, pixels locked. */
static pc_engine_session *locked(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":40,\"height\":30}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(
              s, "paint.stroke",
              "{\"points\":[[5,10],[20,10]],\"size\":8,\"hardness\":1.0,\"color\":\"#ff0000\"}", &out,
              NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "layer.lockLayers", "{\"pixels\":true}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    return s;
}

static void px(const pc_engine_session *s, float out[4])
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_layer *l = st->has_active_layer ? pc_document_layer(doc, st->active_layer) : NULL;
    const pc_surface *surf = l ? pc_layer_surface(l) : NULL;
    if (surf != NULL) {
        pc_surface_rgba(surf, 12, 10, out);
    }
}

static void test_locked_layer_edits_are_refused(void)
{
    static const char *const cmds[] = {"edit.clear", "edit.cut", "image.adjustments.invert",
                                       "image.autoTone", "image.autoContrast", "image.autoColor"};
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        if (pc_engine_command_find(cmds[i]) == NULL) {
            (void)printf("  (skipped %s: DEFERRED)\n", cmds[i]);
            continue;
        }
        pc_engine_session *s = locked();
        float before[4] = {0}, after[4] = {0};
        px(s, before);
        pc_json_value out = PC_JSON_VALUE_INIT;
        pc_error err = {0};
        if (pc_engine_session_execute_json(s, cmds[i], "{}", &out, &err) == PC_OK) {
            PC_T_FAIL("%s edited a locked layer", cmds[i]);
            pc_json_value_fini(&out);
        }
        px(s, after);
        for (int c = 0; c < 4; c++) {
            if (before[c] != after[c]) {
                PC_T_FAIL("%s changed a locked layer's pixels", cmds[i]);
                break;
            }
        }
        pc_engine_session_free(s);
    }
}

static void test_unlocked_layer_edits_still_work(void)
{
    pc_engine_session *s = locked();
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.lockLayers", "{\"pixels\":false}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "image.adjustments.invert", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    float p[4] = {0};
    px(s, p);
    CHECK_NEAR(p[0], 0.0, 1e-4);
    CHECK_NEAR(p[1], 1.0, 1e-4);
    CHECK_NEAR(p[2], 1.0, 1e-4);
    CHECK_NEAR(p[3], 1.0, 1e-4);
    CHECK(pc_engine_session_execute_json(s, "edit.clear", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    px(s, p);
    CHECK_NEAR(p[3], 0.0, 1e-4);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_locked_layer_edits_are_refused);
    RUN(test_unlocked_layer_edits_still_work);
    return pc_t_finish();
}
