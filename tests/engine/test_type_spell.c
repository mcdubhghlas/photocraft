/* E11 spelling + glyph (type_spell_cmds.c): edit.checkSpelling, type.insertText + adversarial. */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(300, 100), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status ex(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, json, out, NULL);
}

static uint64_t uint_of(pc_json_value *o, const char *k)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(pc_json_value_root(o), k);
    return v != NULL ? yyjson_mut_get_uint(v) : 0;
}

static uint64_t create(pc_engine_session *s, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.create", json, &out) == PC_OK);
    uint64_t id = uint_of(&out, "layer");
    pc_json_value_fini(&out);
    return id;
}

static const char *text_of(pc_engine_session *s, uint64_t id)
{
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), (pc_layer_id)id);
    return pc_str_c(&l->content.u.text.text);
}

static void test_spell_list_change_learn(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":10,\"y\":50,\"text\":\"Teh quick brwon fox\",\"size\":12}");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "edit.checkSpelling", "{}", &out) == PC_OK);
    yyjson_mut_val *ms = yyjson_mut_obj_get(pc_json_value_root(&out), "misspellings");
    CHECK(yyjson_mut_arr_size(ms) >= 2); /* Teh, brwon */
    pc_json_value_fini(&out);
    char p[160];
    snprintf(p, sizeof p, "{\"action\":\"change\",\"layer\":%llu,\"start\":0,\"end\":3,\"word\":\"Teh\",\"replace\":\"The\"}",
             (unsigned long long)id);
    CHECK(ex(s, "edit.checkSpelling", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(strncmp(text_of(s, id), "The", 3) == 0);
    /* a wrong `word` verification fails */
    snprintf(p, sizeof p, "{\"action\":\"change\",\"layer\":%llu,\"start\":0,\"end\":3,\"word\":\"Teh\",\"replace\":\"X\"}",
             (unsigned long long)id);
    CHECK(ex(s, "edit.checkSpelling", p, &out) != PC_OK);
    if (out.doc) pc_json_value_fini(&out);
    /* changeAll */
    CHECK(ex(s, "edit.checkSpelling", "{\"action\":\"changeAll\",\"word\":\"brwon\",\"replace\":\"brown\"}", &out) == PC_OK);
    CHECK_EQ_UINT(uint_of(&out, "changed"), 1);
    pc_json_value_fini(&out);
    CHECK(strcmp(text_of(s, id), "The quick brown fox") == 0);
    /* add / remove dictionary */
    CHECK(ex(s, "edit.checkSpelling", "{\"action\":\"addToDictionary\",\"word\":\"Photocraft\"}", &out) == PC_OK);
    yyjson_mut_val *ud = yyjson_mut_obj_get(pc_json_value_root(&out), "userDictionary");
    CHECK(yyjson_mut_arr_size(ud) == 1);
    pc_json_value_fini(&out);
    CHECK(ex(s, "edit.checkSpelling", "{\"action\":\"removeFromDictionary\",\"word\":\"photocraft\"}", &out) == PC_OK);
    ud = yyjson_mut_obj_get(pc_json_value_root(&out), "userDictionary");
    CHECK(yyjson_mut_arr_size(ud) == 0);
    pc_json_value_fini(&out);
    CHECK(ex(s, "edit.checkSpelling", "{\"action\":\"suggest\",\"word\":\"recieve\"}", &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "edit.checkSpelling", "{\"action\":\"bogus\"}", &out) != PC_OK);
    if (out.doc) pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_insert_glyph(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":10,\"y\":50,\"text\":\"ab\",\"size\":12}");
    char p[160];
    pc_json_value out = PC_JSON_VALUE_INIT;
    snprintf(p, sizeof p, "{\"layer\":%llu,\"text\":\"\\u2192\",\"at\":1}", (unsigned long long)id);
    CHECK(ex(s, "type.insertText", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(strcmp(text_of(s, id), "a\xe2\x86\x92""b") == 0);
    snprintf(p, sizeof p, "{\"layer\":%llu,\"text\":\"\\u20ac\",\"range\":[0,2]}", (unsigned long long)id);
    CHECK(ex(s, "type.insertText", p, &out) == PC_OK);
    CHECK_EQ_UINT(uint_of(&out, "caret"), 1);
    pc_json_value_fini(&out);
    /* CRLF normalised; empty text rejected */
    snprintf(p, sizeof p, "{\"layer\":%llu,\"text\":\"\"}", (unsigned long long)id);
    CHECK(ex(s, "type.insertText", p, &out) != PC_OK);
    if (out.doc) pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static const char *const IDS[] = {"edit.checkSpelling", "type.insertText"};
static const char *const HOSTILE[] = {
    "{}", "null", "[]", "{\"action\":123}", "{\"action\":\"change\"}", "{\"action\":\"changeAll\"}",
    "{\"action\":\"change\",\"layer\":99999,\"start\":0,\"end\":1,\"replace\":\"x\"}", "{\"layer\":-1}",
    "{\"text\":\"\"}", "{\"range\":[9,0]}", "{\"at\":-1}", "{\"layer\":99999,\"text\":\"x\"}",
    "{\"action\":\"addToDictionary\",\"word\":\"  \"}", "{\"action\":\"suggest\"}",
};

static void test_adversarial(void)
{
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        pc_engine_session *bare = NULL;
        CHECK(pc_engine_session_new(&bare, NULL) == PC_OK);
        pc_engine_session *s = mk();
        (void)create(s, "{\"x\":10,\"y\":50,\"text\":\"Teh fox\",\"size\":12}");
        for (size_t j = 0; j < sizeof HOSTILE / sizeof HOSTILE[0]; j++) {
            pc_json_value o1 = PC_JSON_VALUE_INIT, o2 = PC_JSON_VALUE_INIT;
            if (ex(bare, IDS[i], HOSTILE[j], &o1) == PC_OK) {
                pc_json_value_fini(&o1);
            }
            if (ex(s, IDS[i], HOSTILE[j], &o2) == PC_OK) {
                pc_json_value_fini(&o2);
            }
        }
        pc_engine_session_free(bare);
        pc_engine_session_free(s);
    }
}

int main(int argc, char **argv)
{
    setenv("PHOTOCRAFT_NO_SYSTEM_FONTS", "1", 1);
    RUN(test_spell_list_change_learn);
    RUN(test_insert_glyph);
    RUN(test_adversarial);
    return pc_t_finish();
}
