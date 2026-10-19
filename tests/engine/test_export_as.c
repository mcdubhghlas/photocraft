#include "harness.h"
#include "engine/engine_internal.h"
#include "base/file.h"
#include "codecs/codecs.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static pc_status ex(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    pc_error err = {0};
    pc_status status = pc_engine_session_execute_json(s, id, json, out, &err);
    if (status != PC_OK)
        fprintf(stderr, "%s: %s\n", id, err.msg);
    return status;
}
static void test_scaling_all_formats_leaves_document_and_history_unchanged(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "file.new", "{\"width\":1,\"height\":12,\"background\":\"transparent\"}", &out) == PC_OK);
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    uint64_t revision = pc_engine_session_active(s)->revision;
    size_t history = pc_history_past_len(pc_engine_session_active(s)->history);
    char tmp[] = "/tmp/photocraft-export-XXXXXX";
    CHECK(mkdtemp(tmp) != NULL);
    const char *formats[] = {"png", "jpg", "webp", "tif", "tga", "ico"};
    for (size_t i = 0; i < sizeof formats / sizeof *formats; i++) {
        char path[256], json[512];
        (void)snprintf(path, sizeof path, "%s/out.%s", tmp, formats[i]);
        (void)snprintf(
            json, sizeof json,
            "{\"format\":\"%s\",\"path\":\"%s\",\"scale\":50,\"transparency\":false,\"quality\":75}",
            formats[i], path);
        CHECK(ex(s, "file.export.exportAs", json, &out) == PC_OK);
        CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "width")) == 1);
        CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "height")) == 6);
        pc_buf bytes = PC_BUF_INIT;
        CHECK(pc_engine_read_file(path, &bytes, NULL) == PC_OK);
        pc_cimage image = {0};
        CHECK(pc_cdecode(bytes.data, bytes.len, &image, NULL) == PC_OK);
        CHECK(image.width == 1 && image.height == 6);
        pc_cimage_fini(&image);
        pc_buf_fini(&bytes);
        CHECK(unlink(path) == 0);
        CHECK(pc_engine_session_active(s)->doc == before);
        CHECK(pc_engine_session_active(s)->revision == revision);
        CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == history);
    }
    CHECK(ex(s, "file.export.exportAs", "{\"scale\":50}", &out) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "estimate")));
    const char *bad[] = {"{\"scale\":0}",
                         "{\"quality\":false}",
                         "{\"format\":\"exe\"}",
                         "{\"transparency\":5}",
                         "{\"path\":5}",
                         "{\"estimate\":false}",
                         "{\"path\":\"/tmp/wrong.jpg\",\"format\":\"png\"}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(ex(s, "file.export.exportAs", bad[i], &out) != PC_OK);
    CHECK(pc_engine_session_active(s)->doc == before);
    for (unsigned bits = 24; bits <= 32; bits += 8) {
        char path[256], json[512];
        (void)snprintf(path, sizeof path, "%s/bits.tga", tmp);
        (void)snprintf(json, sizeof json, "{\"format\":\"tga\",\"path\":\"%s\",\"tgaBits\":\"%u\"}", path,
                       bits);
        CHECK(ex(s, "file.export.exportAs", json, &out) == PC_OK);
        pc_buf bytes = PC_BUF_INIT;
        CHECK(pc_engine_read_file(path, &bytes, NULL) == PC_OK);
        CHECK(bytes.len > 18 && bytes.data[16] == bits);
        pc_buf_fini(&bytes);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(tmp) == 0);
    pc_json_value_fini(&out);
    pc_doc_arc_release(before);
    pc_engine_session_free(s);
}
static void test_runtime_models_depths_and_encoding_warnings(void)
{
    const char *models[] = {"rgb", "gray", "cmyk", "lab"};
    const unsigned depths[] = {8, 16, 32};
    for (size_t model = 0; model < 4; model++)
        for (size_t depth = 0; depth < 3; depth++) {
            pc_engine_session *s = NULL;
            CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
            char json[200];
            (void)snprintf(json, sizeof json, "{\"width\":8,\"height\":6,\"mode\":\"%s\",\"depth\":%u}",
                           models[model], depths[depth]);
            pc_json_value out = PC_JSON_VALUE_INIT;
            CHECK(ex(s, "file.new", json, &out) == PC_OK);
            pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
            CHECK(ex(s, "file.export.exportAs", "{\"format\":\"png\",\"scale\":50}", &out) == PC_OK);
            CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "width")) == 4);
            CHECK(yyjson_mut_is_arr(yyjson_mut_obj_get(pc_json_value_root(&out), "warnings")));
            CHECK(ex(s, "file.export.exportAs", "{\"format\":\"jpg\"}", &out) == PC_OK);
            yyjson_mut_val *warnings = yyjson_mut_obj_get(pc_json_value_root(&out), "warnings");
            CHECK(yyjson_mut_is_arr(warnings));
            if (depths[depth] == 32)
                CHECK(yyjson_mut_arr_size(warnings) > 0);
            CHECK(pc_engine_session_active(s)->doc == before);
            pc_doc_arc_release(before);
            pc_json_value_fini(&out);
            pc_engine_session_free(s);
        }
}

int main(int argc, char **argv)
{
    RUN(test_scaling_all_formats_leaves_document_and_history_unchanged);
    RUN(test_runtime_models_depths_and_encoding_warnings);
    return pc_t_finish();
}
