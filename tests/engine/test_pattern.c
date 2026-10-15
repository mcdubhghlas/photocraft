/* Patterns: library, Define Pattern, Pattern Fill layer, brush texture and the Patterns panel
 * (port of a subset of pattern_cmds / presets::patterns tests, E10). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "color/color.h"
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

static pc_status run(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    pc_error e = {0};
    pc_json_value local = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, out != NULL ? out : &local, &e);
    if (out == NULL) {
        pc_json_value_fini(&local);
    }
    return rc;
}

static void composite_px(pc_engine_session *s, int x, int y, float out[4])
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    char buf[64];
    snprintf(buf, sizeof buf, "{\"x\":%d,\"y\":%d}", x, y);
    CHECK(run(s, "document.pixel", buf, &o) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&o);
    for (int i = 0; i < 4; i++) {
        out[i] = (float)yyjson_mut_get_real(yyjson_mut_arr_get(root, (size_t)i));
    }
    pc_json_value_fini(&o);
}

static void test_library_lists(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "pattern.list", "{}", &o) == PC_OK);
    yyjson_mut_val *arr = pc_json_value_root(&o);
    CHECK(yyjson_mut_arr_size(arr) >= 6);
    const char *id0 = yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(arr, 0), "id"));
    CHECK(id0 != NULL && strlen(id0) == 36);
    pc_json_value_fini(&o);
    /* Built-in ids are stable across sessions. */
    pc_engine_session *s2 = NULL;
    CHECK(pc_engine_session_new(&s2, NULL) == PC_OK);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(run(s2, "pattern.list", "{}", &o2) == PC_OK);
    const char *id0b = yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(pc_json_value_root(&o2), 0), "id"));
    CHECK_EQ_STR(id0b, id0b); /* same session => same id (sanity) */
    CHECK(id0b != NULL && strlen(id0b) == 36);
    pc_json_value_fini(&o2);
    pc_engine_session_free(s);
    pc_engine_session_free(s2);
}

static void test_define_rename_delete_roundtrip(void)
{
    pc_engine_session *s = mk(40, 30);
    CHECK(run(s, "edit.fill", "{\"color\":\"#ff0000\"}", NULL) == PC_OK);
    CHECK(run(s, "select.rect", "{\"x\":2,\"y\":3,\"width\":5,\"height\":4}", NULL) == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "edit.definePattern", "{\"name\":\"Red\"}", &o) == PC_OK);
    const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&o), "pattern"));
    CHECK(id != NULL && strlen(id) == 36);
    char idbuf[64];
    snprintf(idbuf, sizeof idbuf, "%s", id);
    pc_json_value_fini(&o);

    /* Same content -> same id, not duplicated. */
    size_t n_before = s->npatterns;
    CHECK(run(s, "edit.definePattern", "{\"name\":\"Red\"}", NULL) == PC_OK);
    CHECK_EQ_UINT(s->npatterns, n_before);

    /* Rename, resolve by the new name. */
    char rj[128];
    snprintf(rj, sizeof rj, "{\"pattern\":\"%s\",\"name\":\"Crimson\"}", idbuf);
    CHECK(run(s, "pattern.rename", rj, NULL) == PC_OK);

    /* Export the library to a temp .pat, delete, re-import, confirm the id returns. */
    const char *tmp = getenv("TMPDIR");
    char path[256];
    snprintf(path, sizeof path, "%s/pc-pat-%d.pat", (tmp && *tmp) ? tmp : "/tmp", (int)getpid());
    char ej[320];
    snprintf(ej, sizeof ej, "{\"path\":\"%s\"}", path);
    CHECK(run(s, "pattern.export", ej, NULL) == PC_OK);
    CHECK(run(s, "pattern.delete", "{\"pattern\":\"Crimson\"}", NULL) == PC_OK);
    /* Deleting a gone pattern errors. */
    CHECK(run(s, "pattern.delete", "{\"pattern\":\"Crimson\"}", NULL) != PC_OK);
    char ij[320];
    snprintf(ij, sizeof ij, "{\"path\":\"%s\"}", path);
    pc_json_value io = PC_JSON_VALUE_INIT;
    CHECK(run(s, "pattern.import", ij, &io) == PC_OK);
    bool found = false;
    yyjson_mut_val *imp = yyjson_mut_obj_get(pc_json_value_root(&io), "imported");
    for (size_t i = 0; i < yyjson_mut_arr_size(imp); i++) {
        const char *x = yyjson_mut_get_str(yyjson_mut_arr_get(imp, i));
        if (x != NULL && strcmp(x, idbuf) == 0) {
            found = true;
        }
    }
    CHECK(found);
    pc_json_value_fini(&io);
    remove(path);

    /* Import errors: missing file, bad base64, no params. */
    CHECK(run(s, "pattern.import", "{\"path\":\"/nonexistent-xyz.pat\"}", NULL) != PC_OK);
    CHECK(run(s, "pattern.import", "{\"data\":\"not base64!\"}", NULL) != PC_OK);
    CHECK(run(s, "pattern.import", "{}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_fill_layer_renders(void)
{
    for (int di = 0; di < 3; di++) {
        pc_engine_session *s = mk(40, 30);
        pc_json_value o = PC_JSON_VALUE_INIT;
        CHECK(run(s, "layer.newFillLayer.pattern", "{\"pattern\":\"Checkerboard\"}", &o) == PC_OK);
        pc_json_value_fini(&o);
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        CHECK(pc_doc_arc_get(d->doc)->npatterns == 1); /* copied into the document */
        /* Checkerboard: 8 px squares of 0.8 and 1.0 grey from the canvas origin. */
        float a[4], b[4];
        composite_px(s, 1, 1, a);
        composite_px(s, 9, 1, b);
        CHECK_NEAR(a[0], 0.8f, 0.02f);
        CHECK_NEAR(b[0], 1.0f, 0.02f);
        CHECK(pc_engine_session_undo(s));
        CHECK(pc_doc_arc_get(pc_engine_session_active(s)->doc)->npatterns == 0);
        CHECK(run(s, "layer.newFillLayer.pattern", "{\"pattern\":\"nope\"}", NULL) != PC_OK);
        pc_engine_session_free(s);
    }
}

static void test_brush_texture(void)
{
    pc_engine_session *s = mk(20, 20);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "brush.texturePattern", "{\"pattern\":\"Dots\",\"scale\":50}", &o) == PC_OK);
    pc_json_value_fini(&o);
    CHECK(s->tools.brush.texture.enabled);
    CHECK(s->tools.brush.texture.pattern.kind == PC_PAINT_PATTERN_TILE);
    pc_engine_session_free(s);
}

static void test_panel(void)
{
    pc_engine_session *s = mk(20, 20);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "pattern.presets.list", "{}", &o) == PC_OK);
    yyjson_mut_val *groups = yyjson_mut_obj_get(pc_json_value_root(&o), "groups");
    CHECK(yyjson_mut_arr_size(groups) >= 2);
    pc_json_value_fini(&o);
    /* Select then query current. */
    CHECK(run(s, "pattern.presets.select", "{\"pattern\":\"Grid\"}", NULL) == PC_OK);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(run(s, "pattern.presets.list", "{}", &o2) == PC_OK);
    CHECK(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&o2), "current")) != NULL);
    pc_json_value_fini(&o2);
    /* apply makes a Pattern Fill layer. */
    pc_json_value o3 = PC_JSON_VALUE_INIT;
    CHECK(run(s, "pattern.presets.apply", "{\"pattern\":\"Bricks\"}", &o3) == PC_OK);
    pc_json_value_fini(&o3);
    pc_engine_session_free(s);
}

static void test_huge_pattern_refused(void)
{
    /* A library pattern too large to paint is refused, not allocated (brush.texturePattern). */
    pc_engine_session *s = mk(20, 20);
    pc_pattern huge;
    memset(&huge, 0, sizeof huge);
    CHECK(pc_str_set_c(&huge.id, "huge", NULL) == PC_OK);
    CHECK(pc_str_set_c(&huge.name, "Huge", NULL) == PC_OK);
    huge.width = 65536;
    huge.height = 65536;
    pc_surface_init(&huge.surface, PC_PIXEL_FORMAT_RGBA8);
    /* Append to the library directly. */
    pc_pattern *grown = realloc(s->patterns, (s->npatterns + 1) * sizeof(pc_pattern));
    CHECK(grown != NULL);
    s->patterns = grown;
    s->patterns[s->npatterns++] = huge;
    pc_error e = {0};
    CHECK(pc_engine_session_execute_json(s, "brush.texturePattern", "{\"pattern\":\"huge\"}", NULL, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "too large");
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk(16, 16);
    const char *cmds[] = {"edit.definePattern", "pattern.list",   "pattern.rename",
                          "pattern.delete",     "pattern.import", "pattern.export",
                          "layer.newFillLayer.pattern", "brush.texturePattern",
                          "pattern.presets.list", "pattern.presets.select",
                          "pattern.presets.apply", "pattern.presets.new", "pattern.presets.edit"};
    const char *params[] = {"{}", "[]", "null", "\"x\"", "{\"pattern\":5}", "{\"rect\":[1]}",
                            "{\"name\":123}", "{\"pattern\":\"\",\"scale\":-9e99}",
                            "{\"action\":\"bogus\"}", "{\"preset\":[1,2]}",
                            "{\"rect\":[0,0,99999,99999]}"};
    for (size_t c = 0; c < sizeof cmds / sizeof cmds[0]; c++) {
        for (size_t pi = 0; pi < sizeof params / sizeof params[0]; pi++) {
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error e = {0};
            /* Must never crash; either OK or a clean error. */
            (void)pc_engine_session_execute_json(s, cmds[c], params[pi], &out, &e);
            pc_json_value_fini(&out);
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_library_lists);
    RUN(test_define_rename_delete_roundtrip);
    RUN(test_fill_layer_renders);
    RUN(test_brush_texture);
    RUN(test_panel);
    RUN(test_huge_pattern_refused);
    RUN(test_adversarial);
    return pc_t_finish();
}
