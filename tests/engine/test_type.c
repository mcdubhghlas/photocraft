/* E11 type command specs (type_cmds.c): type.create/edit/setStyle/rasterize/info/fonts and the
 * size keys, plus an adversarial-params sweep (no command crashes on hostile input, rule 9). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(w, h), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                      pc_color_rgba(0, 0, 0, 0), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status ex(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, json, out, NULL);
}

static uint64_t uint_of(pc_json_value *out, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(pc_json_value_root(out), key);
    return v != NULL ? yyjson_mut_get_uint(v) : 0;
}

static const char *str_of(pc_json_value *out, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(pc_json_value_root(out), key);
    return v != NULL ? yyjson_mut_get_str(v) : NULL;
}

static uint64_t create(pc_engine_session *s, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.create", json, &out) == PC_OK);
    uint64_t id = uint_of(&out, "layer");
    pc_json_value_fini(&out);
    return id;
}

static const pc_text_layer *text_layer(pc_engine_session *s, uint64_t id)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(st->doc), (pc_layer_id)id);
    CHECK(l != NULL && l->content.kind == PC_LAYER_TEXT);
    return &l->content.u.text;
}

static void test_create_and_info(void)
{
    pc_engine_session *s = mk(200, 100);
    uint64_t id = create(s, "{\"x\":10,\"y\":50,\"text\":\"Hello\",\"size\":24,\"color\":\"#ff0000\"}");
    CHECK(id != 0);
    const pc_text_layer *t = text_layer(s, id);
    CHECK_EQ_STR(pc_str_c(&t->text), "Hello");
    CHECK(t->has_cache);
    CHECK(t->psd_raw != NULL);
    CHECK_EQ_UINT(t->runs[0].len, 5);
    CHECK_NEAR(t->runs[0].style.size_pt, 24.0, 1e-4);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), (pc_layer_id)id);
    CHECK_EQ_STR(pc_str_c(&l->name), "Hello");

    pc_json_value info = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.info", "{\"layer\":100000}", &info) != PC_OK); /* no such layer */
    pc_json_value_fini(&info);
    char idp[64];
    snprintf(idp, sizeof idp, "{\"layer\":%llu}", (unsigned long long)id);
    CHECK(ex(s, "type.info", idp, &info) == PC_OK);
    CHECK_EQ_STR(str_of(&info, "text"), "Hello");
    pc_json_value_fini(&info);

    CHECK(pc_engine_session_undo(s));
    CHECK(pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), (pc_layer_id)id) == NULL);
    pc_engine_session_free(s);
}

static void test_edit_keeps_styles(void)
{
    pc_engine_session *s = mk(200, 100);
    uint64_t id = create(s, "{\"x\":0,\"y\":40,\"text\":\"Hello world\",\"size\":12}");
    char p[256];
    snprintf(p, sizeof p, "{\"layer\":%llu,\"range\":[6,11],\"fauxBold\":true}", (unsigned long long)id);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.setStyle", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    const pc_text_layer *t = text_layer(s, id);
    CHECK_EQ_UINT(t->nruns, 2);
    CHECK(t->runs[1].style.faux_bold);
    /* insert inside the bold word inherits bold */
    snprintf(p, sizeof p, "{\"layer\":%llu,\"replace\":{\"start\":8,\"end\":8,\"text\":\"XX\"}}", (unsigned long long)id);
    CHECK(ex(s, "type.edit", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    t = text_layer(s, id);
    CHECK_EQ_STR(pc_str_c(&t->text), "Hello woXXrld");
    /* whole-text multibyte replace lays out two lines */
    snprintf(p, sizeof p, "{\"layer\":%llu,\"text\":\"Gr\\u00f6\\u00dfe \\u2713\\nzwei\"}", (unsigned long long)id);
    CHECK(ex(s, "type.edit", p, &out) == PC_OK);
    yyjson_mut_val *lines = yyjson_mut_obj_get(pc_json_value_root(&out), "lines");
    CHECK_EQ_UINT(yyjson_mut_arr_size(lines), 2);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_size_keys(void)
{
    pc_engine_session *s = mk(200, 100);
    uint64_t id = create(s, "{\"x\":0,\"y\":40,\"text\":\"Hello world\",\"size\":12}");
    char p[256];
    snprintf(p, sizeof p, "{\"layer\":%llu,\"range\":[6,11],\"size\":20}", (unsigned long long)id);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.setStyle", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "type.increaseSize", "{}", &out) == PC_OK);
    pc_json_value_fini(&out);
    const pc_text_layer *t = text_layer(s, id);
    CHECK_NEAR(t->runs[0].style.size_pt, 14.0, 1e-4);
    CHECK_NEAR(t->runs[1].style.size_pt, 22.0, 1e-4);
    /* bad `by` falls back / clamps, never crashes */
    CHECK(ex(s, "type.increaseSize", "{\"by\":\"big\"}", &out) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_rasterize_and_fonts(void)
{
    pc_engine_session *s = mk(200, 100);
    uint64_t id = create(s, "{\"x\":5,\"y\":30,\"text\":\"Raster\"}");
    char p[64];
    snprintf(p, sizeof p, "{\"layer\":%llu}", (unsigned long long)id);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.rasterize", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), (pc_layer_id)id);
    CHECK(l->content.kind == PC_LAYER_RASTER);
    CHECK(ex(s, "type.edit", p, &out) != PC_OK); /* no longer a type layer */

    CHECK(ex(s, "type.fonts", "{}", &out) == PC_OK);
    yyjson_mut_val *fams = yyjson_mut_obj_get(pc_json_value_root(&out), "families");
    bool has_inter = false;
    size_t idx, mx;
    yyjson_mut_val *v;
    yyjson_mut_arr_foreach(fams, idx, mx, v) {
        const char *f = yyjson_mut_get_str(v);
        if (f != NULL && strcmp(f, "Inter") == 0) {
            has_inter = true;
        }
    }
    CHECK(has_inter);
    pc_json_value_fini(&out);
    CHECK(ex(s, "type.fonts", "{\"family\":\"Inter\"}", &out) == PC_OK);
    yyjson_mut_val *faces = yyjson_mut_obj_get(pc_json_value_root(&out), "faces");
    CHECK(yyjson_mut_arr_size(faces) > 0);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_missing_font_warning(void)
{
    pc_engine_session *s = mk(200, 100);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.create", "{\"x\":10,\"y\":20,\"text\":\"Hi\",\"font\":\"NoSuchFamily123\"}", &out) == PC_OK);
    yyjson_mut_val *warn = yyjson_mut_obj_get(pc_json_value_root(&out), "warnings");
    CHECK(warn != NULL && yyjson_mut_arr_size(warn) == 1);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_reject_bad_params(void)
{
    pc_engine_session *s = mk(200, 100);
    pc_json_value out = PC_JSON_VALUE_INIT;
    /* absurd size / orientation / tracking rejected, no layer created */
    CHECK(ex(s, "type.create", "{\"text\":\"a\",\"size\":1e30}", &out) != PC_OK);
    if (out.doc) pc_json_value_fini(&out);
    CHECK(ex(s, "type.create", "{\"text\":\"a\",\"orientation\":\"diagonal\"}", &out) != PC_OK);
    if (out.doc) pc_json_value_fini(&out);
    uint64_t id = create(s, "{\"text\":\"abc\",\"size\":12}");
    char p[128];
    snprintf(p, sizeof p, "{\"layer\":%llu,\"tracking\":1e30}", (unsigned long long)id);
    CHECK(ex(s, "type.edit", p, &out) != PC_OK);
    if (out.doc) pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static const char *const TYPE_IDS[] = {
    "type.create", "type.edit", "type.setStyle", "type.rasterize", "type.info", "type.fonts",
    "type.increaseSize", "type.decreaseSize", "type.increaseSizeMore", "type.decreaseSizeMore",
};

static const char *const HOSTILE[] = {
    "{}", "null", "[]", "\"x\"", "{\"layer\":-1}", "{\"layer\":999999}", "{\"layer\":\"no\"}",
    "{\"text\":123}", "{\"size\":\"big\"}", "{\"range\":[9,0]}", "{\"range\":[-1,-2]}",
    "{\"runs\":[{\"start\":5,\"end\":2}]}", "{\"kernPair\":{\"at\":-1,\"by\":1e99}}", "{\"transform\":[1]}",
    "{\"box\":[0,0,-1,-1]}", "{\"weight\":1e99}", "{\"color\":[1,2]}", "{\"features\":{\"ss01\":true}}",
    "{\"variations\":{\"wght\":\"x\"}}", "{\"leadingBy\":[1]}", "{\"metricsAsShown\":true,\"size\":1e308}",
};

static void test_adversarial(void)
{
    for (size_t i = 0; i < sizeof TYPE_IDS / sizeof TYPE_IDS[0]; i++) {
        pc_engine_session *empty = NULL;
        CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
        pc_engine_session *doc = mk(80, 60);
        (void)create(doc, "{\"text\":\"Aa\\nBb\",\"size\":12}");
        for (size_t j = 0; j < sizeof HOSTILE / sizeof HOSTILE[0]; j++) {
            pc_json_value o1 = PC_JSON_VALUE_INIT, o2 = PC_JSON_VALUE_INIT;
            pc_status r1 = ex(empty, TYPE_IDS[i], HOSTILE[j], &o1);
            if (r1 == PC_OK) {
                pc_json_value_fini(&o1);
            }
            pc_status r2 = ex(doc, TYPE_IDS[i], HOSTILE[j], &o2);
            CHECK(r2 == PC_OK || r2 != PC_OK); /* reached here = no crash */
            if (r2 == PC_OK) {
                pc_json_value_fini(&o2);
            }
        }
        pc_engine_session_free(empty);
        pc_engine_session_free(doc);
    }
}

int main(int argc, char **argv)
{
    setenv("PHOTOCRAFT_NO_SYSTEM_FONTS", "1", 1);
    RUN(test_create_and_info);
    RUN(test_edit_keeps_styles);
    RUN(test_size_keys);
    RUN(test_rasterize_and_fonts);
    RUN(test_missing_font_warning);
    RUN(test_reject_bad_params);
    RUN(test_adversarial);
    return pc_t_finish();
}
