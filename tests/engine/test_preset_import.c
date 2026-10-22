/* Importing .abr brush files (preset_import_cmds.c): a v2 file becomes a group of presets; the
 * base64 path and graceful failure on bad input (rule 9). The ABR bytes are built with the psd
 * writer and handed in as base64 "data" (no filesystem). */
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/json.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/preset_import_cmds.h"
#include "harness.h"
#include "paint/tile.h"
#include "psd/abr.h"

static void must(pc_status rc)
{
    CHECK(rc == PC_OK);
}

static pc_psd_abr_sample asample(uint32_t w, uint32_t h)
{
    pc_psd_abr_sample s;
    memset(&s, 0, sizeof s);
    s.width = w;
    s.height = h;
    s.depth = 8;
    s.data.len = (size_t)w * h;
    s.data.data = malloc(s.data.len + 1);
    CHECK(s.data.data != NULL);
    for (size_t i = 0; i < s.data.len; i++) {
        s.data.data[i] = (uint8_t)(i % 2 == 0 ? 255 : 0);
    }
    return s;
}

/* A base64 v2 .abr with two brushes (one computed, one sampled "Dune Grass"), owned (free()). */
static char *build_abr_b64(void)
{
    pc_psd_abr_legacy b[2];
    memset(b, 0, sizeof b);
    b[0].spacing = 25;
    b[0].anti_alias = true;
    b[0].tip = PC_PSD_ABR_COMPUTED;
    b[0].diameter = 13;
    b[0].hardness = 50;
    b[0].angle = 20;
    b[0].roundness = 60;
    must(pc_psd_str_set(&b[1].name, "Dune Grass", 10, NULL));
    b[1].spacing = 0;
    b[1].anti_alias = false;
    b[1].tip = PC_PSD_ABR_SAMPLED;
    b[1].sample = asample(30, 10);

    pc_psd_bytes bytes;
    memset(&bytes, 0, sizeof bytes);
    must(pc_psd_abr_write_v12(2, b, 2, true, &bytes, NULL));
    pc_buf b64;
    pc_buf_init(&b64);
    must(pc_paint_b64_encode(bytes.data, bytes.len, &b64, NULL));
    char *out = malloc(b64.len + 1);
    CHECK(out != NULL);
    memcpy(out, b64.data, b64.len);
    out[b64.len] = '\0';
    pc_buf_fini(&b64);
    pc_psd_bytes_fini(&bytes);
    pc_psd_abr_legacy_fini(&b[0]);
    pc_psd_abr_legacy_fini(&b[1]);
    return out;
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    return s;
}

static size_t idx(pc_engine_session *s, const char *name)
{
    for (size_t i = 0; i < s->tools.npresets; i++) {
        if (strcmp(pc_str_c(&s->tools.presets[i].name), name) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_preset_import_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("brush.presets.importAbr") != NULL);
}

static void test_import_data(void)
{
    pc_engine_session *s = mk();
    size_t before = s->tools.npresets;
    char *b64 = build_abr_b64();
    size_t jlen = strlen(b64) + 64;
    char *params = malloc(jlen);
    CHECK(params != NULL);
    (void)snprintf(params, jlen, "{\"data\":\"%s\",\"group\":\"Kit\",\"select\":true}", b64);

    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    CHECK(pc_engine_session_execute_json(s, "brush.presets.importAbr", params, &out, &e) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(root != NULL && yyjson_mut_is_obj(root));
    yyjson_mut_val *g = root != NULL ? yyjson_mut_obj_get(root, "group") : NULL;
    CHECK(g != NULL && strcmp(yyjson_mut_get_str(g), "Kit") == 0);
    yyjson_mut_val *cnt = root != NULL ? yyjson_mut_obj_get(root, "count") : NULL;
    CHECK(cnt != NULL && yyjson_mut_get_uint(cnt) == 2);
    yyjson_mut_val *ver = root != NULL ? yyjson_mut_obj_get(root, "version") : NULL;
    CHECK(ver != NULL && yyjson_mut_get_uint(ver) == 2);
    pc_json_value_fini(&out);

    CHECK(s->tools.npresets == before + 2);
    CHECK(idx(s, "Dune Grass") != (size_t)-1);
    /* The imported preset is in group "Kit". */
    size_t i = idx(s, "Dune Grass");
    CHECK(i != (size_t)-1 && pc_str_eq_c(&s->tools.presets[i].group, "Kit"));

    free(params);
    free(b64);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = mk();
    const char *bad[] = {
        "{}",
        "{\"data\":5}",
        "{\"data\":\"not valid base64 ****\"}",
        "{\"path\":\"\"}",
        "{\"path\":\"/nonexistent/does-not-exist.abr\"}",
        "{\"group\":\"\",\"data\":\"AAAA\"}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        pc_error e = {0};
        CHECK(pc_engine_session_execute_json(s, "brush.presets.importAbr", bad[i], &out, &e) != PC_OK);
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_import_data);
    RUN(test_graceful);
    return pc_t_finish();
}
