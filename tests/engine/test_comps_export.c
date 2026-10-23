/* File > Export > Layer Comps to Files: file.export.layerCompsToFiles (comps_cmds.c).
 *
 * Each layer comp is written to its own file; no comps, no dir and no document are graceful errors
 * (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status run_json(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

static int count_files(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    int n = -1;
    if (rc == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&out);
        yyjson_mut_val *files = root ? yyjson_mut_obj_get(root, "files") : NULL;
        n = files ? (int)yyjson_mut_arr_size(files) : -1;
    }
    pc_json_value_fini(&out);
    return n;
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "MyDoc", pc_size_make(8, 8), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{\"name\":\"A\"}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    /* Two comps. */
    CHECK(run_json(s, "layerComp.new", "{\"name\":\"One\"}") == PC_OK);
    CHECK(run_json(s, "layerComp.new", "{\"name\":\"Two\"}") == PC_OK);
    return s;
}

static void test_exports_each_comp(void)
{
    CHECK(pc_engine_command_find("file.export.layerCompsToFiles") != NULL);
    pc_engine_session *s = mk();
    char dir[128];
    (void)snprintf(dir, sizeof dir, "/dev/shm/pc-gap3/comps-%ld", (long)getpid());
    (void)mkdir(dir, 0700);
    char params[256];
    (void)snprintf(params, sizeof params, "{\"dir\":\"%s\",\"format\":\"png\"}", dir);
    int n = count_files(s, "file.export.layerCompsToFiles", params);
    CHECK_EQ_INT(n, 2);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "file.export.layerCompsToFiles", "{\"dir\":\"/dev/shm/pc-gap3\"}") != PC_OK);
    pc_engine_session_free(s);

    /* Comps but no dir. */
    s = mk();
    CHECK(run_json(s, "file.export.layerCompsToFiles", "{}") != PC_OK);
    /* A comp id that doesn't exist -> no matching comps. */
    CHECK(run_json(s, "file.export.layerCompsToFiles",
                   "{\"dir\":\"/dev/shm/pc-gap3\",\"comps\":[99999]}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_exports_each_comp);
    RUN(test_graceful);
    return pc_t_finish();
}
