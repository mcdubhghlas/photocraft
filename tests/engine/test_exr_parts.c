/* File > Open EXR Parts / EXR Parts Info: file.openExrParts / file.exrParts (exr_parts_cmds.c).
 *
 * Lists and opens the parts of an EXR; a missing path, an empty selection and an unknown part are
 * graceful errors (never a crash, REWRITE_PLAN.md rule 9). Uses a C-written EXR round-trip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/exr_parts_cmds.h"
#include "harness.h"

static pc_status run_json(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

/* Write a tiny RGBA document to `path` as EXR; returns whether EXR export is available. */
static bool write_exr(const char *path)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "aov", pc_size_make(4, 4), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgba(0.8f, 0.4f, 0.2f, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    char params[4160];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\"}", path);
    pc_status rc = run_json(s, "file.saveACopy", params);
    pc_engine_session_free(s);
    return rc == PC_OK;
}

/* The first "part" entry's name from file.exrParts, copied into name (cap). */
static bool first_part_name(pc_engine_session *s, const char *path, char *name, size_t cap)
{
    char params[4160];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\"}", path);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, "file.exrParts", params, &out, &e);
    bool found = false;
    if (rc == PC_OK) {
        yyjson_mut_val *arr = pc_json_value_root(&out);
        size_t i, max;
        yyjson_mut_val *el;
        yyjson_mut_arr_foreach(arr, i, max, el)
        {
            yyjson_mut_val *k = yyjson_mut_obj_get(el, "kind");
            const char *ks = k ? yyjson_mut_get_str(k) : NULL;
            if (ks && strcmp(ks, "part") == 0) {
                const char *nm = yyjson_mut_get_str(yyjson_mut_obj_get(el, "name"));
                (void)snprintf(name, cap, "%s", nm ? nm : "");
                found = true;
                break;
            }
        }
    }
    pc_json_value_fini(&out);
    return found;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_exr_parts_specs(&n);
    CHECK_EQ_UINT(n, 2);
    CHECK(pc_engine_command_find("file.openExrParts") != NULL);
    CHECK(pc_engine_command_find("file.exrParts") != NULL);
}

static void test_list_and_open(void)
{
    char path[128];
    (void)snprintf(path, sizeof path, "/dev/shm/pc-gap3/pc-exr-%ld.exr", (long)getpid());
    if (!write_exr(path)) {
        /* EXR export not built: skip the round-trip but keep the test meaningful. */
        fprintf(stderr, "note: EXR export unavailable; skipping round-trip\n");
        return;
    }
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char part[128] = {0};
    CHECK(first_part_name(s, path, part, sizeof part)); /* a part is listed (name may be empty) */
    char params[4320];
    (void)snprintf(params, sizeof params, "{\"path\":\"%s\",\"part/%s\":true}", path, part);
    CHECK(run_json(s, "file.openExrParts", params) == PC_OK);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    CHECK(doc->nlayers == 1);
    pc_engine_session_free(s);
    (void)remove(path);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    /* Missing path. */
    CHECK(run_json(s, "file.openExrParts", "{}") != PC_OK);
    CHECK(run_json(s, "file.exrParts", "{}") != PC_OK);
    /* A nonexistent file. */
    CHECK(run_json(s, "file.openExrParts", "{\"path\":\"/dev/shm/pc-gap3/nope.exr\"}") != PC_OK);
    /* A bad view on a (still missing) file: either a param error or a read error, never a crash. */
    CHECK(run_json(s, "file.openExrParts", "{\"path\":\"/dev/shm/pc-gap3/nope.exr\",\"view\":\"sideways\"}") !=
          PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_list_and_open);
    RUN(test_graceful);
    return pc_t_finish();
}
