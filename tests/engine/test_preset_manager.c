/* Edit > Presets: Preset Manager (list/rename/delete/move across brushes, custom shapes, patterns)
 * and Export/Import Presets (round-trip of brushes + custom shapes through a portable JSON file),
 * plus adversarial params (Rule 9: every bad input is an error, never a crash). */
#include <string.h>

#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status ex(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, NULL);
    pc_json_value_fini(&o);
    return rc;
}

/* The array value for a kind in a presetManager/import result. */
static yyjson_mut_val *arr_of(pc_json_value *o, const char *kind)
{
    yyjson_mut_val *r = pc_json_value_root(o);
    return r != NULL ? yyjson_mut_obj_get(r, kind) : NULL;
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    return s;
}

static void test_list_all_kinds(void)
{
    pc_engine_session *s = mk();
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "edit.presets.presetManager", "{}", &o, NULL) == PC_OK);
    /* Default action is "list": every kind is present as an array. */
    CHECK(yyjson_mut_is_arr(arr_of(&o, "brushes")));
    CHECK(yyjson_mut_is_arr(arr_of(&o, "customShapes")));
    CHECK(yyjson_mut_is_arr(arr_of(&o, "patterns")));
    /* The built-in libraries are non-empty for brushes and patterns. */
    CHECK(yyjson_mut_arr_size(arr_of(&o, "brushes")) > 0);
    CHECK(yyjson_mut_arr_size(arr_of(&o, "patterns")) > 0);
    pc_json_value_fini(&o);

    /* A single kind: only that key. */
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "edit.presets.presetManager",
                                         "{\"action\":\"list\",\"kind\":\"patterns\"}", &o2,
                                         NULL) == PC_OK);
    CHECK(yyjson_mut_is_arr(arr_of(&o2, "patterns")));
    CHECK(arr_of(&o2, "brushes") == NULL);
    pc_json_value_fini(&o2);
    pc_engine_session_free(s);
}

static void test_rename_delete_move_brushes(void)
{
    pc_engine_session *s = mk();
    size_t before = s->tools.npresets;
    CHECK(before >= 2);

    /* Rename index 0 -> "Zeta" (trims whitespace); the result lists the new name. */
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "edit.presets.presetManager",
              "{\"action\":\"rename\",\"kind\":\"brushes\",\"index\":0,\"newName\":\"  Zeta  \"}",
              &o, NULL) == PC_OK);
    const char *n0 = yyjson_mut_get_str(yyjson_mut_arr_get(arr_of(&o, "brushes"), 0));
    CHECK(n0 != NULL && strcmp(n0, "Zeta") == 0);
    pc_json_value_fini(&o);
    /* A renamed built-in becomes the user's own preset. */
    CHECK(!s->tools.presets[0].builtin);

    /* Move it to the end, then confirm it is last. */
    char mv[96];
    (void)snprintf(mv, sizeof mv, "{\"action\":\"move\",\"kind\":\"brushes\",\"index\":0,\"to\":%zu}",
                   before - 1);
    CHECK(ex(s, "edit.presets.presetManager", mv) == PC_OK);
    CHECK(strcmp(pc_str_c(&s->tools.presets[before - 1].name), "Zeta") == 0);

    /* Delete by name. */
    CHECK(ex(s, "edit.presets.presetManager",
             "{\"action\":\"delete\",\"kind\":\"brushes\",\"name\":\"Zeta\"}") == PC_OK);
    CHECK(s->tools.npresets == before - 1);
    pc_engine_session_free(s);
}

/* Define a custom shape (needs a document with a work path), then rename/delete it. */
static void test_custom_shape_manager(void)
{
    pc_engine_session *s = mk();
    CHECK(ex(s, "file.new", "{\"width\":16,\"height\":16}") == PC_OK);
    CHECK(ex(s, "path.set",
             "{\"name\":\"work\",\"path\":{\"subpaths\":[{\"knots\":[[0,0],[8,0],[8,8]]}]}}") ==
          PC_OK);
    CHECK(ex(s, "edit.defineCustomShape", "{\"name\":\"Tri\",\"path\":\"work\"}") == PC_OK);
    CHECK(s->ncustom_shapes >= 1);
    CHECK(ex(s, "edit.presets.presetManager",
             "{\"action\":\"rename\",\"kind\":\"customShapes\",\"name\":\"Tri\",\"newName\":\"Tri2\"}") ==
          PC_OK);
    CHECK(s->custom_shapes[s->ncustom_shapes - 1].name != NULL &&
          strcmp(s->custom_shapes[s->ncustom_shapes - 1].name, "Tri2") == 0);
    size_t ns = s->ncustom_shapes;
    CHECK(ex(s, "edit.presets.presetManager",
             "{\"action\":\"delete\",\"kind\":\"customShapes\",\"name\":\"Tri2\"}") == PC_OK);
    CHECK(s->ncustom_shapes == ns - 1);
    pc_engine_session_free(s);
}

static void test_export_import_roundtrip(void)
{
    pc_engine_session *s1 = mk();
    /* Export every brush (built-ins included) into a preset file. */
    pc_json_value exp = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s1, "edit.presets.exportImportPresets",
              "{\"action\":\"export\",\"kinds\":[\"brushes\"],\"includeBuiltins\":true}", &exp,
              NULL) == PC_OK);
    yyjson_mut_val *er = pc_json_value_root(&exp);
    uint64_t nexp = yyjson_mut_get_uint(yyjson_mut_obj_get(er, "brushes"));
    CHECK(nexp > 0);
    yyjson_mut_val *data = yyjson_mut_obj_get(er, "data");
    CHECK(data != NULL);

    /* Build import params {action:import, data:<the exported object>} and run on a fresh session
     * whose brush library we first clear by renaming nothing -- a fresh session has the same
     * built-ins, so importing replaces them by name and reports every brush imported. */
    pc_engine_session *s2 = mk();
    yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
    CHECK(pd != NULL);
    yyjson_mut_val *proot = yyjson_mut_obj(pd);
    yyjson_mut_obj_add_str(pd, proot, "action", "import");
    yyjson_mut_val *karr = yyjson_mut_arr(pd);
    yyjson_mut_arr_add_strcpy(pd, karr, "brushes");
    yyjson_mut_obj_add_val(pd, proot, "kinds", karr);
    yyjson_mut_obj_add_val(pd, proot, "data", yyjson_mut_val_mut_copy(pd, data));
    yyjson_mut_doc_set_root(pd, proot);
    yyjson_doc *imut = yyjson_mut_doc_imut_copy(pd, NULL);
    yyjson_mut_doc_free(pd);
    CHECK(imut != NULL);

    pc_json_value imp = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute(s2, "edit.presets.exportImportPresets",
                                    yyjson_doc_get_root(imut), &imp, NULL) == PC_OK);
    uint64_t nimp = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&imp), "brushes"));
    CHECK_EQ_UINT(nimp, nexp);
    yyjson_doc_free(imut);
    pc_json_value_fini(&imp);
    pc_json_value_fini(&exp);
    pc_engine_session_free(s1);
    pc_engine_session_free(s2);
}

/* Rule 9: adversarial params never crash and return an error. */
static void test_graceful_failure(void)
{
    pc_engine_session *s = mk();
    CHECK(ex(s, "edit.presets.presetManager", "{\"action\":\"bogus\"}") != PC_OK);
    CHECK(ex(s, "edit.presets.presetManager", "{\"action\":\"rename\"}") != PC_OK); /* no kind */
    CHECK(ex(s, "edit.presets.presetManager",
             "{\"action\":\"rename\",\"kind\":\"weird\"}") != PC_OK); /* bad kind */
    CHECK(ex(s, "edit.presets.presetManager",
             "{\"action\":\"delete\",\"kind\":\"brushes\",\"index\":999999}") != PC_OK);
    CHECK(ex(s, "edit.presets.presetManager",
             "{\"action\":\"rename\",\"kind\":\"brushes\",\"index\":0}") != PC_OK); /* no newName */
    CHECK(ex(s, "edit.presets.presetManager",
             "{\"action\":\"rename\",\"kind\":\"brushes\",\"index\":0,\"newName\":\"   \"}") !=
          PC_OK); /* empty after trim */
    CHECK(ex(s, "edit.presets.presetManager",
             "{\"action\":\"move\",\"kind\":\"brushes\",\"index\":0}") != PC_OK); /* no to */
    /* Export/import adversarial. */
    CHECK(ex(s, "edit.presets.exportImportPresets", "{\"action\":\"bogus\"}") != PC_OK);
    CHECK(ex(s, "edit.presets.exportImportPresets", "{\"action\":\"import\"}") != PC_OK); /* no data */
    CHECK(ex(s, "edit.presets.exportImportPresets",
             "{\"action\":\"import\",\"data\":\"not json at all\"}") != PC_OK);
    CHECK(ex(s, "edit.presets.exportImportPresets",
             "{\"action\":\"import\",\"data\":{\"format\":\"wrong\"}}") != PC_OK);
    CHECK(ex(s, "edit.presets.exportImportPresets", "[]") != PC_OK); /* non-object params */
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_list_all_kinds);
    RUN(test_rename_delete_move_brushes);
    RUN(test_custom_shape_manager);
    RUN(test_export_import_roundtrip);
    RUN(test_graceful_failure);
    return pc_t_finish();
}
