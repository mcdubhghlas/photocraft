/* Layers panel effects-list disclosure (port of fx_view_cmds tests, E9). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "doc/effects.h"
#include "engine/engine_internal.h"
#include "harness.h"

static void give_drop_shadow(pc_document *doc, pc_layer_id id)
{
    pc_layer *l = pc_document_layer_mut(doc, id);
    CHECK(l != NULL);
    pc_effect e;
    pc_effect_default_drop_shadow(&e);
    pc_effect *items = malloc(sizeof *items);
    CHECK(items != NULL);
    items[0] = e;
    l->effects.items = items;
    l->effects.nitems = 1;
    l->effects.enabled = true;
}

static pc_engine_session *session(pc_layer_id *a, pc_layer_id *b)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(32, 32), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    pc_layer la, lb;
    CHECK(pc_layer_raster(&la, "a", fmt, NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &la, a, NULL) == PC_OK);
    give_drop_shadow(&doc, *a);
    CHECK(pc_layer_raster(&lb, "b", fmt, NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &lb, b, NULL) == PC_OK);
    give_drop_shadow(&doc, *b);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static bool open_fx(pc_engine_session *s, pc_layer_id id)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    for (size_t i = 0; i < d->nfx_collapsed; i++) {
        if (d->fx_collapsed[i] == id) {
            return false;
        }
    }
    return true;
}

static bool exec_ok(pc_engine_session *s, const char *json)
{
    pc_json_value out;
    pc_status rc = pc_engine_session_execute_json(s, "layer.setEffectsExpanded", json, &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    return rc == PC_OK;
}

static void test_toggles_one_or_all_without_history(void)
{
    pc_layer_id a, b;
    pc_engine_session *s = session(&a, &b);
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    st->saved_revision = st->revision;
    size_t steps = pc_history_entry_count(st->history);

    CHECK(open_fx(s, a) && open_fx(s, b)); /* lists start open */

    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)a);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.setEffectsExpanded", buf, &out, NULL) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "expanded")));
    pc_json_value_fini(&out);
    CHECK(!open_fx(s, a) && open_fx(s, b));

    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK_EQ_UINT(pc_history_entry_count(d->history), steps); /* no history step */
    CHECK(d->revision == d->saved_revision);                  /* still clean */

    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"all\":true}", (unsigned long long)a);
    CHECK(exec_ok(s, buf));
    CHECK(open_fx(s, a) && open_fx(s, b)); /* a closed, b open -> toggle opens all */

    CHECK(exec_ok(s, "{\"all\":true,\"expanded\":false}"));
    CHECK(!open_fx(s, a) && !open_fx(s, b));

    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"expanded\":true}", (unsigned long long)b);
    CHECK(exec_ok(s, buf));
    CHECK(!open_fx(s, a) && open_fx(s, b));
    CHECK_EQ_UINT(pc_engine_session_active(s)->nfx_collapsed, (size_t)1); /* no duplicates */

    pc_engine_session_free(s);
}

static void test_bad_params_fail_gracefully(void)
{
    pc_layer_id a, b;
    pc_engine_session *s = session(&a, &b);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    unsigned long long bg = (unsigned long long)pc_document_background(doc)->id;
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", bg);
    CHECK(!exec_ok(s, buf)); /* background has no effects */
    CHECK(!exec_ok(s, "{\"layer\":9999}"));
    CHECK(!exec_ok(s, "{\"layer\":\"x\"}"));
    CHECK(!exec_ok(s, "{\"layer\":-1}"));
    CHECK(!exec_ok(s, "{\"expanded\":\"yes\",\"all\":true}"));
    CHECK(!exec_ok(s, "{\"all\":3}"));
    pc_engine_session_free(s);

    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(!exec_ok(empty, "{}")); /* no document */
    pc_document doc2;
    CHECK(pc_document_with_background(&doc2, "Untitled", pc_size_make(8, 8), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(empty, &doc2, NULL, NULL, NULL) == PC_OK);
    CHECK(!exec_ok(empty, "{\"all\":true}")); /* no effects: disabled */
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    RUN(test_toggles_one_or_all_without_history);
    RUN(test_bad_params_fail_gracefully);
    return pc_t_finish();
}
