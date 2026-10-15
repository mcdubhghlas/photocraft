/* plugin.* commands: WASM filter plug-ins. Port of crates/engine/src/plugin_cmds/tests.rs (E12).
 * Uses the invert example fixture (tests/fixtures/plugins/invert.wasm); run from the repo root. */
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "harness.h"
#include "plugins/plugins.h"

#define INVERT_ID "org.photocraft.example.invert"

static pc_status ex(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, NULL);
    pc_json_value_fini(&o);
    return rc;
}

/* base64-encode (standard alphabet) into a malloc'd NUL-terminated string. */
static char *b64(const uint8_t *b, size_t n)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = (n + 2) / 3 * 4;
    char *out = malloc(olen + 1);
    CHECK(out != NULL);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t x = (uint32_t)b[i] << 16;
        x |= (i + 1 < n ? (uint32_t)b[i + 1] : 0) << 8;
        x |= (i + 2 < n ? (uint32_t)b[i + 2] : 0);
        out[o++] = T[(x >> 18) & 63];
        out[o++] = T[(x >> 12) & 63];
        out[o++] = i + 1 < n ? T[(x >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? T[x & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

static bool fixture(pc_buf *out)
{
    return pc_file_read("tests/fixtures/plugins/invert.wasm", (size_t)64 << 20, out, NULL) == PC_OK;
}

static void test_install_list_run(void)
{
    pc_buf wasm = PC_BUF_INIT;
    if (!fixture(&wasm)) {
        fprintf(stderr, "SKIP: tests/fixtures/plugins/invert.wasm missing (run from repo root)\n");
        return;
    }
    char *data = b64(wasm.data, wasm.len);
    pc_buf_fini(&wasm);
    /* {"data":"<base64>"} */
    pc_buf params = PC_BUF_INIT;
    (void)pc_buf_append_str(&params, "{\"data\":\"", NULL);
    (void)pc_buf_append_str(&params, data, NULL);
    (void)pc_buf_append_str(&params, "\"}", NULL);
    free(data);

    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "plugin.install", (const char *)params.data, &r, NULL) ==
          PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&r), "id")), INVERT_ID);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&r), "name")),
                 "Invert (WebAssembly)");
    pc_json_value_fini(&r);

    pc_json_value l = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "plugin.list", "{}", &l, NULL) == PC_OK);
    yyjson_mut_val *arr = yyjson_mut_obj_get(pc_json_value_root(&l), "plugins");
    bool found = false;
    for (size_t i = 0; i < yyjson_mut_arr_size(arr); i++) {
        yyjson_mut_val *idv = yyjson_mut_obj_get(yyjson_mut_arr_get(arr, i), "id");
        if (idv != NULL && strcmp(yyjson_mut_get_str(idv), INVERT_ID) == 0) {
            found = true;
        }
    }
    CHECK(found);
    pc_json_value_fini(&l);

    /* new doc, a layer painted red, run the plug-in, expect inverted colours */
    CHECK(ex(s, "file.new", "{\"width\":16,\"height\":12}") == PC_OK);
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(ex(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":16,\"height\":12}") == PC_OK);
    CHECK(ex(s, "edit.fill", "{\"color\":\"#ff0000\"}") == PC_OK);
    CHECK(ex(s, "select.deselect", "{}") == PC_OK || true);

    pc_json_value run = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "plugin.run", "{\"id\":\"" INVERT_ID "\"}", &run, NULL) ==
          PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&run), "plugin")), INVERT_ID);
    pc_json_value_fini(&run);

    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_layer *lay = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    const pc_surface *surf = pc_layer_surface(lay);
    float px[4];
    pc_surface_read_pixel(surf, 4, 4, px);
    /* red inverted -> cyan (0,1,1) */
    CHECK_NEAR(px[0], 0.0, 0.02);
    CHECK_NEAR(px[1], 1.0, 0.02);
    CHECK_NEAR(px[2], 1.0, 0.02);

    /* one undo restores */
    CHECK(pc_engine_session_undo(s));
    pc_surface_read_pixel(pc_layer_surface(pc_document_layer(pc_doc_arc_get(
                              pc_engine_session_active(s)->doc),
                          pc_engine_session_active(s)->active_layer)),
                          4, 4, px);
    CHECK_NEAR(px[0], 1.0, 0.02);

    /* remove */
    CHECK(ex(s, "plugin.remove", "{\"id\":\"" INVERT_ID "\"}") == PC_OK);
    CHECK(ex(s, "plugin.run", "{\"id\":\"" INVERT_ID "\"}") != PC_OK);

    pc_buf_fini(&params);
    pc_engine_session_free(s);
}

static void test_bad_params(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "plugin.install", "{}") != PC_OK);
    CHECK(ex(s, "plugin.install", "{\"data\":\"not base64!!!\"}") != PC_OK);
    CHECK(ex(s, "plugin.install", "{\"data\":123}") != PC_OK);
    CHECK(ex(s, "plugin.remove", "{}") != PC_OK);
    CHECK(ex(s, "plugin.remove", "{\"id\":\"nope\"}") != PC_OK);
    CHECK(ex(s, "plugin.run", "{}") != PC_OK);
    const char *ids[] = {"plugin.list", "plugin.install", "plugin.remove", "plugin.reload", "plugin.run"};
    const char *ps[] = {"{}", "[]", "null", "123", "{\"id\":5}", "{\"data\":[]}", "{\"path\":\"\"}"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof ps / sizeof ps[0]; j++) {
            (void)ex(s, ids[i], ps[j]); /* must not crash */
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    /* The install/list/run path needs a real WASM runtime; skip it in a WITH_PLUGINS=0 build
     * (the -DPC_HAVE_WAMR define reaches src/plugins, not tests/engine, so detect at runtime). */
    if (pc_plugins_available()) {
        RUN(test_install_list_run);
    } else {
        (void)fprintf(stderr, "  plug-ins unavailable in this build: install/list/run skipped\n");
    }
    RUN(test_bad_params);
    return pc_t_finish();
}
