/* Channels: Save/Load Selection, channel management, Quick Mask, Split/Merge (port of a subset of
 * crates/engine/src/channel_cmds/tests.rs, E7). */
#include <stdlib.h>
#include <string.h>

#include "algo/selection.h"
#include "color/color.h"
#include "engine/channel_cmds.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h, pc_sample_type depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB, depth,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

struct rectctx {
    int x0, y0, x1, y1;
};

static pc_status rectsel(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *e)
{
    (void)ha;
    (void)a;
    struct rectctx *c = v;
    pc_rect area = pc_document_bounds(doc);
    uint32_t w = pc_rect_width(area), h = pc_rect_height(area);
    size_t npx = (size_t)w * (size_t)h;
    float *m = calloc(npx ? npx : 1, sizeof(float));
    if (m == NULL) {
        return pc_fail(e, PC_ERR_NOMEM, "m");
    }
    for (int y = c->y0; y < c->y1; y++) {
        for (int x = c->x0; x < c->x1; x++) {
            if (x >= 0 && y >= 0 && (uint32_t)x < w && (uint32_t)y < h) {
                m[(size_t)y * w + (size_t)x] = 1.0f;
            }
        }
    }
    pc_surface out;
    bool some = false;
    pc_status rc = pc_algo_selection_combine(NULL, m, npx, area, PC_ALGO_SELECTION_REPLACE, &out, &some, e);
    free(m);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    if (some) {
        doc->selection = out;
        doc->has_selection = true;
    } else {
        pc_surface_fini(&out);
    }
    return PC_OK;
}

static void sel_rect(pc_engine_session *s, int x, int y, int w, int h)
{
    struct rectctx c = {x, y, x + w, y + h};
    CHECK(pc_engine_session_edit(s, "sel", rectsel, &c, NULL) == PC_OK);
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

static float sel_at(pc_engine_session *s, int x, int y)
{
    const pc_document *doc = doc_of(s);
    return doc->has_selection ? pc_surface_sample_channel(&doc->selection, x, y, 0) : 0.0f;
}

static float chan_at(pc_engine_session *s, size_t i, int x, int y)
{
    const pc_document *doc = doc_of(s);
    if (i >= doc->nchannels) {
        return -1.0f;
    }
    return pc_surface_sample_channel(&doc->channels[i].surface, x, y, 0);
}

static pc_status run(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, json, out, NULL);
}

static void ok(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out;
    CHECK(run(s, id, json, &out) == PC_OK);
    pc_json_value_fini(&out);
}

/* ---- tests ------------------------------------------------------------------------------- */

static void test_save_and_operations(void)
{
    for (int di = 0; di < 3; di++) {
        pc_sample_type depth = di == 0 ? PC_SAMPLE_TYPE_U8 : (di == 1 ? PC_SAMPLE_TYPE_U16 : PC_SAMPLE_TYPE_F32);
        pc_engine_session *s = mk(40, 20, depth);
        pc_json_value out;
        CHECK(run(s, "select.saveSelection", "{}", &out) != PC_OK); /* no selection */

        sel_rect(s, 5, 5, 10, 10); /* x[5,15) y[5,15) */
        CHECK(run(s, "select.saveSelection", "{}", &out) == PC_OK);
        CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "channel")), 0);
        CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "name")), "Alpha 1");
        pc_json_value_fini(&out);
        CHECK_NEAR(chan_at(s, 0, 10, 10), 1.0f, 1e-3f);
        CHECK_NEAR(chan_at(s, 0, 20, 10), 0.0f, 1e-3f);

        sel_rect(s, 12, 5, 10, 10); /* x[12,22) */
        ok(s, "select.saveSelection", "{\"channel\": 0, \"operation\": \"add\"}");
        CHECK_NEAR(chan_at(s, 0, 6, 10), 1.0f, 1e-3f);
        CHECK_NEAR(chan_at(s, 0, 20, 10), 1.0f, 1e-3f);
        CHECK_NEAR(chan_at(s, 0, 30, 10), 0.0f, 1e-3f);

        sel_rect(s, 5, 5, 10, 10);
        ok(s, "select.saveSelection", "{\"channel\": 0, \"operation\": \"subtract\"}");
        CHECK_NEAR(chan_at(s, 0, 10, 10), 0.0f, 1e-3f);
        CHECK_NEAR(chan_at(s, 0, 18, 10), 1.0f, 1e-3f);

        sel_rect(s, 5, 5, 10, 10);
        ok(s, "select.saveSelection", "{\"channel\": \"Alpha 1\", \"operation\": \"intersect\"}");
        CHECK_NEAR(chan_at(s, 0, 18, 10), 0.0f, 1e-3f);

        ok(s, "select.saveSelection", "{\"channel\": 0, \"operation\": \"replace\", \"name\": \"Mine\"}");
        CHECK_EQ_STR(pc_str_c(&doc_of(s)->channels[0].name), "Mine");

        CHECK(run(s, "select.saveSelection", "{\"operation\": \"bogus\"}", &out) != PC_OK);
        CHECK(run(s, "select.saveSelection", "{\"channel\": 7, \"operation\": \"add\"}", &out) != PC_OK);
        pc_engine_session_free(s);
    }
}

static void test_load_operations_and_invert(void)
{
    pc_engine_session *s = mk(40, 20, PC_SAMPLE_TYPE_U8);
    sel_rect(s, 5, 5, 10, 10);
    ok(s, "select.saveSelection", "{}"); /* channel 0 covers x[5,15) */

    ok(s, "select.loadSelection", "{\"channel\": 0}");
    CHECK_NEAR(sel_at(s, 10, 10), 1.0f, 1e-3f);
    CHECK_NEAR(sel_at(s, 30, 10), 0.0f, 1e-3f);

    ok(s, "select.loadSelection", "{\"channel\": 0, \"invert\": true}");
    CHECK_NEAR(sel_at(s, 10, 10), 0.0f, 1e-3f);
    CHECK_NEAR(sel_at(s, 30, 10), 1.0f, 1e-3f);

    ok(s, "select.loadSelection", "{\"channel\": 0}"); /* back to x[5,15) */
    sel_rect(s, 12, 5, 10, 10);
    ok(s, "select.loadSelection", "{\"channel\": 0, \"operation\": \"add\"}");
    CHECK_NEAR(sel_at(s, 6, 10), 1.0f, 1e-3f);
    CHECK_NEAR(sel_at(s, 14, 10), 1.0f, 1e-3f);

    pc_json_value out;
    CHECK(run(s, "select.loadSelection", "{}", &out) != PC_OK);                                  /* missing channel */
    CHECK(run(s, "select.loadSelection", "{\"channel\": 0, \"operation\": \"xor\"}", &out) != PC_OK);
    pc_engine_session_free(s);
}

static void test_channel_management(void)
{
    pc_engine_session *s = mk(40, 20, PC_SAMPLE_TYPE_U8);
    pc_json_value out;
    CHECK(run(s, "channel.new", "{\"fill\": \"white\", \"name\": \"W\"}", &out) == PC_OK);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "channel")), 0);
    pc_json_value_fini(&out);
    CHECK_NEAR(chan_at(s, 0, 3, 3), 1.0f, 1e-3f);

    /* New channel targets and shows itself alone (composite off). */
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(pc_engine_channel_visible_colors(d, 3) == 0);
    CHECK(pc_engine_channel_alpha_shown(d, 0));

    ok(s, "channel.new", "{}");
    CHECK_EQ_STR(pc_str_c(&doc_of(s)->channels[1].name), "Alpha 1");
    ok(s, "channel.duplicate", "{\"channel\": 0}");
    CHECK_EQ_STR(pc_str_c(&doc_of(s)->channels[2].name), "W copy");
    ok(s, "channel.duplicate", "{\"channel\": \"red\", \"invert\": true}");
    CHECK_EQ_STR(pc_str_c(&doc_of(s)->channels[3].name), "Red copy");
    CHECK_NEAR(chan_at(s, 3, 1, 1), 0.0f, 1e-3f); /* white image inverted */

    ok(s, "channel.rename", "{\"channel\": 1, \"name\": \"Renamed\"}");
    CHECK(run(s, "channel.rename", "{\"channel\": 1, \"name\": \" \"}", &out) != PC_OK);
    ok(s, "channel.move", "{\"channel\": \"Renamed\", \"to\": 0}");
    CHECK_EQ_STR(pc_str_c(&doc_of(s)->channels[0].name), "Renamed");
    CHECK_EQ_STR(pc_str_c(&doc_of(s)->channels[1].name), "W");

    /* Target channel 1, inspect the panel state. */
    ok(s, "channel.target", "{\"channel\": 1}");
    CHECK(run(s, "channel.list", "{}", &out) == PC_OK);
    yyjson_mut_val *tgt = yyjson_mut_obj_get(pc_json_value_root(&out), "target");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(tgt, "kind")), "alpha");
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(tgt, "index")), 1);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "compositeVisible")));
    pc_json_value_fini(&out);

    /* Delete a channel (count drops). */
    size_t before = doc_of(s)->nchannels;
    ok(s, "channel.delete", "{\"channel\": 0}");
    CHECK(doc_of(s)->nchannels == before - 1);
    pc_engine_session_free(s);
}

static void test_inspect_channels(void)
{
    pc_engine_session *s = mk(16, 16, PC_SAMPLE_TYPE_U8);
    pc_json_value out;
    CHECK(run(s, "document.inspect", "{}", &out) == PC_OK);
    yyjson_mut_val *ch = yyjson_mut_obj_get(pc_json_value_root(&out), "channels");
    CHECK(ch != NULL);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(ch, "composite")), "RGB");
    yyjson_mut_val *colors = yyjson_mut_obj_get(ch, "colors");
    CHECK(yyjson_mut_arr_size(colors) == 3);
    CHECK(yyjson_mut_is_null(yyjson_mut_obj_get(ch, "quickMask")));
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_quick_mask(void)
{
    pc_engine_session *s = mk(40, 20, PC_SAMPLE_TYPE_U8);
    sel_rect(s, 5, 5, 10, 10);
    pc_json_value out;
    CHECK(run(s, "select.editInQuickMaskMode", "{}", &out) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "quickMask")));
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->has_quick_mask);
    CHECK(!doc_of(s)->has_selection); /* selection moved into the mask */

    /* Leaving restores a selection. */
    ok(s, "select.editInQuickMaskMode", "{}");
    CHECK(!doc_of(s)->has_quick_mask);
    CHECK_NEAR(sel_at(s, 10, 10), 1.0f, 1e-3f);
    CHECK_NEAR(sel_at(s, 30, 10), 0.0f, 1e-3f);
    pc_engine_session_free(s);
}

static void test_split(void)
{
    pc_engine_session *s = mk(16, 16, PC_SAMPLE_TYPE_U8);
    pc_json_value out;
    CHECK(run(s, "channel.split", "{}", &out) == PC_OK);
    yyjson_mut_val *docs = yyjson_mut_obj_get(pc_json_value_root(&out), "documents");
    CHECK(yyjson_mut_arr_size(docs) == 3); /* R, G, B */
    pc_json_value_fini(&out);
    /* Each resulting document is grayscale. */
    size_t n = 0;
    const pc_engine_doc_state *all = pc_engine_session_documents(s, &n);
    bool found_gray = false;
    for (size_t i = 0; i < n; i++) {
        if (pc_doc_arc_get(all[i].doc)->mode == PC_COLOR_MODE_GRAYSCALE) {
            found_gray = true;
        }
    }
    CHECK(found_gray);
    pc_engine_session_free(s);
}

static void test_merge(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    for (int i = 0; i < 3; i++) {
        pc_document g;
        CHECK(pc_document_with_background(&g, "Gray", pc_size_make(16, 16), PC_COLOR_MODE_GRAYSCALE,
                                          PC_SAMPLE_TYPE_U8, pc_color_gray(0.5f), NULL) == PC_OK);
        CHECK(pc_engine_session_add_document(s, &g, NULL, NULL, NULL) == PC_OK);
    }
    CHECK(pc_engine_session_is_enabled(s, "channel.merge"));
    pc_json_value out;
    CHECK(run(s, "channel.merge", "{\"mode\": \"rgb\"}", &out) == PC_OK);
    size_t idx = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "document"));
    pc_json_value_fini(&out);
    size_t n = 0;
    const pc_engine_doc_state *all = pc_engine_session_documents(s, &n);
    CHECK(idx < n);
    CHECK(pc_doc_arc_get(all[idx].doc)->mode == PC_COLOR_MODE_RGB);
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk(24, 16, PC_SAMPLE_TYPE_U8);
    const char *cases[][2] = {
        {"channel.new", "7"},
        {"channel.duplicate", "{}"},
        {"channel.duplicate", "{\"channel\": 99}"},
        {"channel.delete", "{\"channel\": 99}"},
        {"channel.rename", "{}"},
        {"channel.move", "{\"channel\": 0}"},
        {"channel.target", "{\"channel\": \"bogus\"}"},
        {"channel.setVisible", "{\"channel\": \"bogus\"}"},
        {"channel.options", "{\"channel\": \"quickMask\", \"indicates\": \"spot\"}"},
        {"channel.mergeSpot", "{}"},
        {"channel.merge", "{\"mode\": \"xyz\"}"},
        {"select.saveSelection", "{\"operation\": 3}"},
        {"select.loadSelection", "[1,2,3]"},
        {"select.loadSelection", "{\"channel\": [1]}"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_json_value out;
        pc_error err = {0};
        pc_status rc = pc_engine_session_execute_json(s, cases[i][0], cases[i][1], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
        /* The invariant is "no crash"; many of these are errors. */
    }
    CHECK(doc_of(s) != NULL);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_save_and_operations);
    RUN(test_load_operations_and_invert);
    RUN(test_channel_management);
    RUN(test_inspect_channels);
    RUN(test_quick_mask);
    RUN(test_split);
    RUN(test_merge);
    RUN(test_adversarial);
    return pc_t_finish();
}
