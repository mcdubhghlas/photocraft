/* The Edit clipboard/clear commands (E4): cut/copy/copyMerged/paste/pasteInPlace, clear, layer via
 * copy/cut, merge visible, toggle last state, new from clipboard -- plus adversarial params. */
#include <stdlib.h>
#include <string.h>

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

/* file.new (transparent), paint a red rectangle into the one layer. */
static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(exec(s, "file.new", "{\"width\":40,\"height\":20,\"background\":\"transparent\"}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#ff0000\"}") == PC_OK);
    return s;
}

static void active_px(pc_engine_session *s, int32_t x, int32_t y, float out[4])
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_layer *l = pc_document_layer(doc, st->active_layer);
    pc_surface_rgba(pc_layer_surface(l), x, y, out);
}

static size_t nlayers(pc_engine_session *s)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    return pc_doc_arc_get(st->doc)->nlayers;
}

static void test_copy_paste_roundtrip(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":10}") == PC_OK);
    CHECK(exec(s, "edit.copy", "{}") == PC_OK);
    CHECK(pc_engine_session_active(s) != NULL);
    size_t before = nlayers(s);
    /* Paste in place: a new layer with the copied red pixels, deselected. */
    CHECK(exec(s, "edit.pasteSpecial.pasteInPlace", "{}") == PC_OK);
    CHECK(nlayers(s) == before + 1);
    float p[4];
    active_px(s, 2, 2, p);
    CHECK(p[0] == 1.0f && p[3] == 1.0f); /* red, opaque */
    active_px(s, 30, 2, p);
    CHECK(p[3] == 0.0f); /* outside the 10x10 copy: transparent */
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(!doc->has_selection); /* paste deselects */
    pc_engine_session_free(s);
}

static void test_cut_clears_and_fills_clipboard(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":8,\"height\":8}") == PC_OK);
    CHECK(exec(s, "edit.cut", "{}") == PC_OK);
    float p[4];
    active_px(s, 2, 2, p);
    CHECK(p[3] == 0.0f); /* the cut area is now transparent (non-Background layer) */
    active_px(s, 20, 2, p);
    CHECK(p[0] == 1.0f && p[3] == 1.0f); /* untouched */
    /* The clipboard now holds the cut pixels: paste restores them. */
    CHECK(exec(s, "edit.paste", "{}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_clear(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":5,\"y\":5,\"width\":6,\"height\":6}") == PC_OK);
    CHECK(exec(s, "edit.clear", "{}") == PC_OK);
    float p[4];
    active_px(s, 6, 6, p);
    CHECK(p[3] == 0.0f); /* cleared to transparent */
    active_px(s, 0, 0, p);
    CHECK(p[0] == 1.0f && p[3] == 1.0f); /* outside the selection: unchanged */
    pc_engine_session_free(s);
}

static void test_copy_merged(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "edit.copyMerged", "{}") == PC_OK);
    size_t before = nlayers(s);
    CHECK(exec(s, "edit.paste", "{}") == PC_OK);
    CHECK(nlayers(s) == before + 1);
    pc_engine_session_free(s);
}

static void test_layer_via_copy_and_cut(void)
{
    pc_engine_session *s = mk();
    size_t before = nlayers(s);
    /* No selection: Layer via Copy duplicates the whole layer. */
    CHECK(exec(s, "layer.new.layerViaCopy", "{}") == PC_OK);
    CHECK(nlayers(s) == before + 1);
    /* With a selection: Layer via Cut lifts the selected pixels to a new layer and clears them. */
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":6,\"height\":6}") == PC_OK);
    size_t n2 = nlayers(s);
    CHECK(exec(s, "layer.new.layerViaCut", "{}") == PC_OK);
    CHECK(nlayers(s) == n2 + 1);
    pc_engine_session_free(s);
}

static void test_merge_visible(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#00ff00\",\"opacity\":50}") == PC_OK);
    size_t before = nlayers(s);
    CHECK(before >= 2);
    CHECK(exec(s, "layer.mergeVisible", "{}") == PC_OK);
    CHECK(nlayers(s) < before); /* visible layers collapsed into one */
    pc_engine_session_free(s);
}

static void test_toggle_last_state(void)
{
    pc_engine_session *s = mk();
    /* fill already ran; toggling steps back then forward without crashing. */
    CHECK(exec(s, "edit.toggleLastState", "{}") == PC_OK);
    CHECK(exec(s, "edit.toggleLastState", "{}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_new_from_clipboard(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":10}") == PC_OK);
    CHECK(exec(s, "edit.copy", "{}") == PC_OK);
    /* New document from the clipboard, 10x10. */
    CHECK(exec(s, "file.newFromClipboard", "{}") == PC_OK);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(doc->size.width == 10 && doc->size.height == 10);
    /* Paste with no document open: a session with just a clipboard makes a document. */
    pc_engine_session_free(s);
}

static void test_paste_no_document(void)
{
    /* A fresh session with a clipboard but no active document: edit.paste makes a new document. */
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":5,\"height\":5}") == PC_OK);
    CHECK(exec(s, "edit.copy", "{}") == PC_OK);
    /* Close the only document; the clipboard survives. */
    CHECK(pc_engine_session_close(s, 0));
    CHECK(pc_engine_session_active(s) == NULL);
    CHECK(exec(s, "edit.paste", "{}") == PC_OK); /* new document from the clipboard */
    CHECK(pc_engine_session_active(s) != NULL);
    pc_engine_session_free(s);
}

/* Every registered command with adversarial params must error (not crash). */
static void test_adversarial(void)
{
    const char *cmds[] = {"edit.cut",
                          "edit.copy",
                          "edit.copyMerged",
                          "edit.paste",
                          "file.newFromClipboard",
                          "edit.pasteSpecial.pasteInPlace",
                          "edit.clear",
                          "layer.new.layerViaCopy",
                          "layer.new.layerViaCut",
                          "layer.mergeVisible",
                          "edit.toggleLastState"};
    const char *params[] = {"{}", "[]", "\"x\"", "3", "null", "{\"center\":\"no\"}",
                            "{\"target\":123}", "{\"center\":[1e40,-1e40]}", "{\"layer\":-7}"};
    /* With a document. */
    pc_engine_session *s = mk();
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            (void)exec(s, cmds[i], params[j]); /* must not crash */
        }
    }
    pc_engine_session_free(s);
    /* Without a document (and empty clipboard). */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            (void)exec(empty, cmds[i], params[j]);
        }
    }
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_copy_paste_roundtrip);
    RUN(test_cut_clears_and_fills_clipboard);
    RUN(test_clear);
    RUN(test_copy_merged);
    RUN(test_layer_via_copy_and_cut);
    RUN(test_merge_visible);
    RUN(test_toggle_last_state);
    RUN(test_new_from_clipboard);
    RUN(test_paste_no_document);
    RUN(test_adversarial);
    return pc_t_finish();
}
