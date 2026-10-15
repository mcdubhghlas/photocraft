/* E11 type styles (type_styles_cmds.c): character/paragraph style lifecycle + adversarial. */
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
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(400, 200), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                      pc_color_rgba(0, 0, 0, 0), NULL) == PC_OK);
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

static const pc_text_layer *tl(pc_engine_session *s, uint64_t id)
{
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), (pc_layer_id)id);
    return &l->content.u.text;
}

static void test_character_style_lifecycle(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":10,\"y\":50,\"text\":\"Hello world\",\"size\":12}");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.characterStyle.new",
             "{\"attrs\":{\"size\":30,\"underline\":true},\"fromSelection\":false}", &out) == PC_OK);
    uint64_t cs = uint_of(&out, "id");
    CHECK(cs != 0);
    pc_json_value_fini(&out);
    /* list: first entry is None */
    CHECK(ex(s, "type.characterStyle.list", "{}", &out) == PC_OK);
    yyjson_mut_val *styles = yyjson_mut_obj_get(pc_json_value_root(&out), "styles");
    CHECK(yyjson_mut_arr_size(styles) == 2);
    pc_json_value_fini(&out);
    /* apply to "world" */
    char p[160];
    snprintf(p, sizeof p, "{\"id\":%llu,\"layer\":%llu,\"range\":[6,11]}", (unsigned long long)cs,
             (unsigned long long)id);
    CHECK(ex(s, "type.characterStyle.apply", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    const pc_text_layer *t = tl(s, id);
    CHECK_EQ_UINT(t->nruns, 2);
    CHECK(t->runs[1].style.has_style_sheet && t->runs[1].style.style_sheet == (uint32_t)cs);
    CHECK(!t->runs[0].style.has_style_sheet);
    CHECK_NEAR(t->runs[1].style.size_pt, 30.0, 1e-4);
    /* redefine via set: size 40 */
    snprintf(p, sizeof p, "{\"id\":%llu,\"attrs\":{\"size_pt\":40.0}}", (unsigned long long)cs);
    CHECK(ex(s, "type.characterStyle.set", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK_NEAR(tl(s, id)->runs[1].style.size_pt, 40.0, 1e-4);
    /* rename, duplicate, delete */
    snprintf(p, sizeof p, "{\"id\":%llu,\"name\":\"Emphasis\"}", (unsigned long long)cs);
    CHECK(ex(s, "type.characterStyle.rename", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    snprintf(p, sizeof p, "{\"id\":%llu}", (unsigned long long)cs);
    CHECK(ex(s, "type.characterStyle.duplicate", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "type.characterStyle.delete", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    /* text keeps its size, loses the ref */
    CHECK_NEAR(tl(s, id)->runs[1].style.size_pt, 40.0, 1e-4);
    CHECK(!tl(s, id)->runs[1].style.has_style_sheet);
    pc_engine_session_free(s);
}

static void test_paragraph_basic(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":10,\"y\":50,\"text\":\"One\\nTwo\",\"size\":12}");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.paragraphStyle.new",
             "{\"name\":\"Head\",\"fromSelection\":false,\"attrs\":{\"align\":\"center\",\"size\":24}}", &out) ==
          PC_OK);
    uint64_t ps = uint_of(&out, "id");
    pc_json_value_fini(&out);
    char p[160];
    snprintf(p, sizeof p, "{\"id\":%llu,\"layer\":%llu,\"range\":[0,1]}", (unsigned long long)ps,
             (unsigned long long)id);
    CHECK(ex(s, "type.paragraphStyle.apply", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    const pc_text_layer *t = tl(s, id);
    CHECK(t->paragraphs[0].style.has_style_sheet && t->paragraphs[0].style.style_sheet == (uint32_t)ps);
    CHECK(t->paragraphs[0].style.align == PC_TEXT_ALIGN_CENTER);
    /* Basic Paragraph can't be deleted or renamed */
    CHECK(ex(s, "type.paragraphStyle.delete", "{\"id\":0}", &out) != PC_OK);
    if (out.doc) pc_json_value_fini(&out);
    CHECK(ex(s, "type.paragraphStyle.rename", "{\"id\":0,\"name\":\"x\"}", &out) != PC_OK);
    if (out.doc) pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static const char *const IDS[] = {
    "type.characterStyle.new",   "type.characterStyle.duplicate", "type.characterStyle.delete",
    "type.characterStyle.rename", "type.characterStyle.set",       "type.characterStyle.apply",
    "type.characterStyle.redefine", "type.characterStyle.clearOverride", "type.characterStyle.list",
    "type.paragraphStyle.new",   "type.paragraphStyle.duplicate", "type.paragraphStyle.delete",
    "type.paragraphStyle.rename", "type.paragraphStyle.set",       "type.paragraphStyle.apply",
    "type.paragraphStyle.redefine", "type.paragraphStyle.clearOverride", "type.paragraphStyle.list",
};
static const char *const HOSTILE[] = {
    "{}", "null", "[]", "{\"id\":-1}", "{\"id\":99999}", "{\"id\":\"no\"}", "{\"attrs\":5}",
    "{\"attrs\":{\"bogusKey\":1}}", "{\"attrs\":{\"size\":\"x\"}}", "{\"name\":\"\"}", "{\"range\":[9,0]}",
    "{\"layers\":[\"x\"]}", "{\"clear\":5}", "{\"id\":0}", "{\"replace\":\"x\"}",
};

static void test_adversarial(void)
{
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        pc_engine_session *bare = NULL;
        CHECK(pc_engine_session_new(&bare, NULL) == PC_OK);
        pc_engine_session *s = mk();
        (void)create(s, "{\"x\":10,\"y\":50,\"text\":\"Aa\\nBb\",\"size\":12}");
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
    RUN(test_character_style_lifecycle);
    RUN(test_paragraph_basic);
    RUN(test_adversarial);
    return pc_t_finish();
}
