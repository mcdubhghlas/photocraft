/* E11 type caret queries (type_caret_cmds.c): hitTest/caret/navigate round-trips + adversarial. */
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
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(400, 300), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
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

static void test_caret_round_trip(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":40,\"y\":70,\"text\":\"AaB\",\"size\":28}");
    for (int i = 0; i <= 3; i++) {
        char p[96];
        snprintf(p, sizeof p, "{\"layer\":%llu,\"index\":%d}", (unsigned long long)id, i);
        pc_json_value c = PC_JSON_VALUE_INIT;
        CHECK(ex(s, "type.caret", p, &c) == PC_OK);
        CHECK_EQ_UINT(uint_of(&c, "index"), (uint64_t)i);
        yyjson_mut_val *seg = yyjson_mut_obj_get(pc_json_value_root(&c), "segment");
        yyjson_mut_val *a = yyjson_mut_arr_get(seg, 0), *b = yyjson_mut_arr_get(seg, 1);
        double x = (yyjson_mut_get_real(yyjson_mut_arr_get(a, 0)) + yyjson_mut_get_real(yyjson_mut_arr_get(b, 0))) / 2;
        double y = (yyjson_mut_get_real(yyjson_mut_arr_get(a, 1)) + yyjson_mut_get_real(yyjson_mut_arr_get(b, 1))) / 2;
        pc_json_value_fini(&c);
        char hp[128];
        snprintf(hp, sizeof hp, "{\"layer\":%llu,\"x\":%g,\"y\":%g}", (unsigned long long)id, x, y);
        pc_json_value h = PC_JSON_VALUE_INIT;
        CHECK(ex(s, "type.hitTest", hp, &h) == PC_OK);
        CHECK_EQ_UINT(uint_of(&h, "index"), (uint64_t)i);
        CHECK_EQ_UINT(uint_of(&h, "layer"), id);
        pc_json_value_fini(&h);
    }
    pc_engine_session_free(s);
}

static uint64_t go(pc_engine_session *s, uint64_t id, int index, const char *mov)
{
    char p[128];
    snprintf(p, sizeof p, "{\"layer\":%llu,\"index\":%d,\"move\":\"%s\"}", (unsigned long long)id, index, mov);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.navigate", p, &out) == PC_OK);
    uint64_t r = uint_of(&out, "index");
    pc_json_value_fini(&out);
    return r;
}

static void test_navigate(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":20,\"y\":60,\"text\":\"hello big world\",\"size\":18}");
    CHECK_EQ_UINT(go(s, id, 0, "wordNext"), 5);
    CHECK_EQ_UINT(go(s, id, 5, "wordNext"), 9);
    CHECK_EQ_UINT(go(s, id, 15, "wordPrev"), 10);
    CHECK_EQ_UINT(go(s, id, 4, "start"), 0);
    CHECK_EQ_UINT(go(s, id, 4, "end"), 15);
    CHECK_EQ_UINT(go(s, id, 4, "lineStart"), 0);
    CHECK_EQ_UINT(go(s, id, 4, "lineEnd"), 15);
    /* empty text: every move returns 0 */
    uint64_t e = create(s, "{\"x\":10,\"y\":40,\"text\":\"\",\"size\":18}");
    const char *moves[] = {"wordNext", "wordPrev", "lineNext", "linePrev", "lineStart", "lineEnd", "start", "end"};
    for (size_t i = 0; i < sizeof moves / sizeof moves[0]; i++) {
        CHECK_EQ_UINT(go(s, e, 0, moves[i]), 0);
    }
    pc_engine_session_free(s);
}

static const char *const IDS[] = {"type.hitTest", "type.caret", "type.navigate"};
static const char *const HOSTILE[] = {
    "{}", "null", "[]", "{\"x\":\"1\",\"y\":2}", "{\"layer\":\"no\",\"x\":1,\"y\":2}", "{\"layer\":99999,\"index\":0}",
    "{\"index\":-1}", "{\"index\":1.5}", "{\"index\":99}", "{\"index\":0,\"move\":\"left\"}",
    "{\"index\":0,\"move\":\"lineNext\",\"x\":\"no\"}", "{\"x\":-40,\"y\":-40}",
};

static void test_adversarial(void)
{
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        pc_engine_session *bare = NULL;
        CHECK(pc_engine_session_new(&bare, NULL) == PC_OK);
        pc_engine_session *s = mk();
        uint64_t id = create(s, "{\"x\":10,\"y\":40,\"text\":\"Hi\",\"size\":20}");
        (void)id;
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
    RUN(test_caret_round_trip);
    RUN(test_navigate);
    RUN(test_adversarial);
    return pc_t_finish();
}
