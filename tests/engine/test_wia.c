/* File > Import > WIA Support (ported from crates/engine/src/wia_cmds.rs tests). */
#include <string.h>

#include "engine/engine_internal.h"
#include "harness.h"

static void test_reports_availability(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.import.wiaSupport", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(yyjson_mut_is_arr(yyjson_mut_obj_get(root, "devices")));
#if defined(_WIN32)
    bool want = true;
#else
    bool want = false;
#endif
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(root, "available")) == want);
    CHECK(yyjson_mut_obj_get(root, "note") != NULL);
    pc_json_value_fini(&out);
    /* Registered under File > Import. */
    const pc_engine_command *c = pc_engine_command_find("file.import.wiaSupport");
    CHECK(c != NULL && c->menu_len == 2 && strcmp(c->menu[0], "File") == 0 &&
          strcmp(c->menu[1], "Import") == 0);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_reports_availability);
    return pc_t_finish();
}
