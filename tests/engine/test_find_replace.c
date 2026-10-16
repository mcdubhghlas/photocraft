/* Edit menu: Define Custom Shape (edit.defineCustomShape) and Find and Replace Text
 * (edit.findAndReplaceText), plus the find_matches primitive (edit_menu_cmds.c). Ports
 * crates/engine/src/edit_menu_cmds/tests.rs. Never crashes on hostile params (rule 9). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "doc/doc.h"
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
    pc_json_value scratch = PC_JSON_VALUE_INIT;
    pc_json_value *o = out != NULL ? out : &scratch;
    pc_status rc = pc_engine_session_execute_json(s, id, json, o, NULL);
    if (out == NULL) {
        pc_json_value_fini(&scratch);
    }
    return rc;
}

static uint64_t create_text(pc_engine_session *s, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.create", json, &out) == PC_OK);
    yyjson_mut_val *v = yyjson_mut_obj_get(pc_json_value_root(&out), "layer");
    uint64_t id = v != NULL ? yyjson_mut_get_uint(v) : 0;
    pc_json_value_fini(&out);
    return id;
}

static const char *text_of(pc_engine_session *s, uint64_t id)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(st->doc), (pc_layer_id)id);
    CHECK(l != NULL && l->content.kind == PC_LAYER_TEXT);
    return pc_str_c(&l->content.u.text.text);
}

/* ---- find_matches primitive --------------------------------------------------------------- */

static size_t matches(const char *text, const char *find, bool cs, bool whole, pc_text_match *first)
{
    pc_text_match *m = NULL;
    size_t n = 0;
    CHECK(pc_engine_find_matches(text, strlen(text), find, strlen(find), cs, whole, &m, &n, NULL) ==
          PC_OK);
    if (first != NULL && n > 0) {
        *first = m[0];
    }
    free(m);
    return n;
}

static void test_find_matches_cases(void)
{
    pc_text_match f;
    CHECK_EQ_UINT(matches("Cat cat CAT", "cat", false, false, &f), 3);
    CHECK(f.a == 0 && f.b == 3);
    CHECK_EQ_UINT(matches("Cat cat CAT", "cat", true, false, &f), 1);
    CHECK(f.a == 4 && f.b == 7);
    CHECK_EQ_UINT(matches("cat catalog bobcat", "cat", false, true, &f), 1);
    CHECK(f.a == 0 && f.b == 3);
    /* ß folds to itself; STRASSE folds to "strasse" (not "straße"), so only the first matches. */
    CHECK_EQ_UINT(matches("Stra\xC3\x9F" "e STRASSE", "stra\xC3\x9F" "e", false, false, &f), 1);
    CHECK(f.a == 0 && f.b == 7);
    CHECK_EQ_UINT(matches("abc", "", false, false, NULL), 0);
}

/* ---- Define Custom Shape ------------------------------------------------------------------ */

static void test_define_custom_shape(void)
{
    pc_engine_session *s = mk(16, 16);
    /* No path: disabled, and a direct call is a graceful error. */
    CHECK(!pc_engine_session_is_enabled(s, "edit.defineCustomShape"));
    CHECK(ex(s, "edit.defineCustomShape", "{}", NULL) != PC_OK);
    /* Set a work path, then define (auto-named). */
    CHECK(ex(s, "path.set",
             "{\"path\":{\"subpaths\":[{\"closed\":true,\"knots\":[{\"anchor\":[0,0]},"
             "{\"anchor\":[8,0]},{\"anchor\":[8,8]}]}]}}",
             NULL) == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "edit.defineCustomShape"));
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "edit.defineCustomShape", "{}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "name")), "Shape 1");
    pc_json_value_fini(&out);
    /* A named define, then redefining the same name replaces (count stays). */
    CHECK(ex(s, "edit.defineCustomShape", "{\"name\":\"Box\"}", NULL) == PC_OK);
    size_t n = 0;
    (void)pc_engine_custom_shapes(s, &n);
    CHECK_EQ_UINT(n, 2);
    CHECK(ex(s, "edit.defineCustomShape", "{\"name\":\"Box\"}", NULL) == PC_OK);
    (void)pc_engine_custom_shapes(s, &n);
    CHECK_EQ_UINT(n, 2);
    pc_engine_session_free(s);
}

/* ---- Find and Replace Text ---------------------------------------------------------------- */

static void test_find_and_replace(void)
{
    pc_engine_session *s = mk(64, 64);
    /* No type layers yet: disabled. */
    CHECK(!pc_engine_session_is_enabled(s, "edit.findAndReplaceText"));
    uint64_t a = create_text(s, "{\"x\":2,\"y\":10,\"text\":\"hello world\"}");
    uint64_t b = create_text(s, "{\"x\":2,\"y\":30,\"text\":\"a world of worlds\"}");
    CHECK(a != 0 && b != 0);
    CHECK(pc_engine_session_is_enabled(s, "edit.findAndReplaceText"));

    /* changeAll replaces every "world" across both layers. */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "edit.findAndReplaceText", "{\"find\":\"world\",\"replace\":\"planet\"}", &out) == PC_OK);
    uint64_t count = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "count"));
    CHECK_EQ_UINT(count, 3); /* "world" x1 + "world"+"worlds" x2 */
    pc_json_value_fini(&out);
    CHECK(strstr(text_of(s, a), "planet") != NULL);
    CHECK(strstr(text_of(s, b), "planet") != NULL);
    CHECK(strstr(text_of(s, a), "world") == NULL);

    /* empty find is a graceful error; unknown action too. */
    CHECK(ex(s, "edit.findAndReplaceText", "{\"find\":\"\"}", NULL) != PC_OK);
    CHECK(ex(s, "edit.findAndReplaceText", "{\"find\":\"x\",\"action\":\"bogus\"}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_find_step_through(void)
{
    pc_engine_session *s = mk(64, 64);
    uint64_t a = create_text(s, "{\"x\":2,\"y\":10,\"text\":\"cat cat cat\"}");
    CHECK(a != 0);
    /* find moves the cursor; changeFind replaces at the cursor and finds the next. */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "edit.findAndReplaceText", "{\"find\":\"cat\",\"action\":\"find\"}", &out) == PC_OK);
    yyjson_mut_val *found = yyjson_mut_obj_get(pc_json_value_root(&out), "found");
    CHECK(found != NULL && yyjson_mut_is_obj(found));
    pc_json_value_fini(&out);
    CHECK(ex(s, "edit.findAndReplaceText",
             "{\"find\":\"cat\",\"replace\":\"dog\",\"action\":\"change\"}", &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(strncmp(text_of(s, a), "dog", 3) == 0);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_find_matches_cases);
    RUN(test_define_custom_shape);
    RUN(test_find_and_replace);
    RUN(test_find_step_through);
    return pc_t_finish();
}
