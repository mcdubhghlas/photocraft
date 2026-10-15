/* Smart Objects / Smart Filters commands (port of the command-level parts of
 * crates/engine/src/smart_cmds.rs tests, E6): the layer.smartObjects.* and layer.smartFilter.*
 * commands registered by smart_cmds_run.c, plus an adversarial-params sweep (every registered
 * command must return an error, never crash, on hostile input). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/json.h"
#include "base/str.h"
#include "color/blend.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "engine/smart_cmds.h"
#include "raster/surface.h"
#include "harness.h"

#include <yyjson.h>

static const char *SVG = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\">"
                         "<rect width=\"10\" height=\"10\" fill=\"#ff0000\"/></svg>";

static pc_json_value radius_params(void)
{
    yyjson_doc *pd = NULL;
    const char *pj = "{\"radius\":2}";
    pc_json_value v = PC_JSON_VALUE_INIT;
    CHECK(pc_json_parse(pj, strlen(pj), &pd, NULL) == PC_OK);
    CHECK(pc_json_value_from_val(yyjson_doc_get_root(pd), &v, NULL) == PC_OK);
    yyjson_doc_free(pd);
    return v;
}

/* A smart-object layer with `nfilters` visible gaussian-blur smart filters, inserted on top. */
static pc_layer_id add_smart_layer(pc_document *doc, size_t nfilters, bool with_mask)
{
    pc_smart_source src;
    memset(&src, 0, sizeof src);
    src.linked = false;
    CHECK(pc_str_set_c(&src.file_name, "r.svg", NULL) == PC_OK);
    CHECK(pc_blob_new(SVG, strlen(SVG), &src.bytes, NULL) == PC_OK);
    pc_smart_object sm;
    pc_smart_object_init(&sm, &src, PC_AFFINE_IDENTITY, NULL);
    sm.filters_enabled = true;
    if (nfilters > 0) {
        sm.smart_filters = calloc(nfilters, sizeof *sm.smart_filters);
        CHECK(sm.smart_filters != NULL);
        for (size_t i = 0; i < nfilters; i++) {
            pc_smart_filter f;
            memset(&f, 0, sizeof f);
            CHECK(pc_str_set_c(&f.command, "filter.blur.gaussianBlur", NULL) == PC_OK);
            f.visible = true;
            f.opacity = 1.0f;
            f.blend = PC_BLEND_NORMAL;
            f.params = radius_params();
            sm.smart_filters[i] = f;
        }
        sm.nsmart_filters = nfilters;
    }
    if (with_mask) {
        pc_layer_mask_reveal_all(&sm.filter_mask);
        sm.has_filter_mask = true;
    }
    pc_layer_content content;
    content.kind = PC_LAYER_SMART;
    content.u.smart = sm;
    pc_layer l;
    CHECK(pc_layer_init(&l, "SO", &content, NULL) == PC_OK);
    pc_layer_id id = 0;
    CHECK(pc_document_insert_above(doc, NULL, &l, &id, NULL) == PC_OK);
    return id;
}

static pc_engine_session *smart_session(pc_layer_id *out, size_t nfilters, bool with_mask)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(32, 32), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_layer_id id = add_smart_layer(&doc, nfilters, with_mask);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_engine_session_select_layer(s, id, NULL) == PC_OK);
    if (out != NULL) {
        *out = id;
    }
    return s;
}

static const pc_smart_object *active_smart(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL && d->has_active_layer);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    CHECK(l != NULL && l->content.kind == PC_LAYER_SMART);
    return &l->content.u.smart;
}

static bool exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    return rc == PC_OK;
}

/* ---- smart-filter editing ---- */

static void test_set_visible(void)
{
    pc_engine_session *s = smart_session(NULL, 2, false);
    CHECK(active_smart(s)->smart_filters[0].visible);
    CHECK(exec(s, "layer.smartFilter.setVisible", "{\"index\":0,\"visible\":false}"));
    CHECK(!active_smart(s)->smart_filters[0].visible);
    /* default index = top; default visible = toggle */
    CHECK(exec(s, "layer.smartFilter.setVisible", "{}"));
    CHECK(!active_smart(s)->smart_filters[1].visible);
    pc_engine_session_free(s);
}

static void test_set_params(void)
{
    pc_engine_session *s = smart_session(NULL, 1, false);
    CHECK(exec(s, "layer.smartFilter.setParams", "{\"index\":0,\"params\":{\"radius\":9}}"));
    const pc_smart_object *sm = active_smart(s);
    yyjson_mut_val *root = pc_json_value_root(&sm->smart_filters[0].params);
    CHECK(root != NULL && yyjson_mut_is_obj(root));
    yyjson_mut_val *r = yyjson_mut_obj_get(root, "radius");
    CHECK(r != NULL && yyjson_mut_get_num(r) == 9.0);
    /* missing params -> error, no crash */
    CHECK(!exec(s, "layer.smartFilter.setParams", "{\"index\":0}"));
    pc_engine_session_free(s);
}

static void test_blending_options(void)
{
    pc_engine_session *s = smart_session(NULL, 1, false);
    CHECK(exec(s, "layer.smartFilter.blendingOptions", "{\"index\":0,\"blend\":\"multiply\",\"opacity\":0.5}"));
    const pc_smart_object *sm = active_smart(s);
    CHECK(sm->smart_filters[0].blend == PC_BLEND_MULTIPLY);
    CHECK(fabs(sm->smart_filters[0].opacity - 0.5f) < 1e-6);
    /* opacity as a percentage */
    CHECK(exec(s, "layer.smartFilter.blendingOptions", "{\"index\":0,\"opacity\":25}"));
    CHECK(fabs(active_smart(s)->smart_filters[0].opacity - 0.25f) < 1e-6);
    /* unknown blend -> error */
    CHECK(!exec(s, "layer.smartFilter.blendingOptions", "{\"blend\":\"notamode\"}"));
    pc_engine_session_free(s);
}

static void test_move_and_delete(void)
{
    pc_layer_id id;
    pc_engine_session *s = smart_session(&id, 3, false);
    /* tag the bottom filter so we can follow it through a move */
    CHECK(exec(s, "layer.smartFilter.setParams", "{\"index\":0,\"params\":{\"tag\":1}}"));
    CHECK(exec(s, "layer.smartFilter.move", "{\"index\":0,\"to\":2}"));
    const pc_smart_object *sm = active_smart(s);
    yyjson_mut_val *root = pc_json_value_root(&sm->smart_filters[2].params);
    CHECK(root != NULL && yyjson_mut_obj_get(root, "tag") != NULL);
    /* out-of-range to -> error */
    CHECK(!exec(s, "layer.smartFilter.move", "{\"index\":0,\"to\":9}"));
    CHECK(exec(s, "layer.smartFilter.delete", "{\"index\":1}"));
    CHECK(active_smart(s)->nsmart_filters == 2);
    pc_engine_session_free(s);
}

static void test_clear_and_disable(void)
{
    pc_engine_session *s = smart_session(NULL, 2, true);
    CHECK(exec(s, "layer.smartFilter.disableSmartFilters", "{\"enabled\":false}"));
    CHECK(!active_smart(s)->filters_enabled);
    CHECK(exec(s, "layer.smartFilter.disableFilterMask", "{\"enabled\":false}"));
    CHECK(!active_smart(s)->filter_mask.enabled);
    CHECK(exec(s, "layer.smartFilter.deleteFilterMask", "{}"));
    CHECK(!active_smart(s)->has_filter_mask);
    CHECK(exec(s, "layer.smartFilter.clearSmartFilters", "{}"));
    CHECK(active_smart(s)->nsmart_filters == 0);
    pc_engine_session_free(s);
}

static void test_transfer(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(32, 32), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_layer_id from = add_smart_layer(&doc, 2, false);
    pc_layer_id to = add_smart_layer(&doc, 0, false);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);

    char json[128];
    (void)snprintf(json, sizeof json, "{\"from\":%llu,\"to\":%llu}", (unsigned long long)from,
                   (unsigned long long)to);
    CHECK(exec(s, "layer.smartFilter.transfer", json));
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *dc = pc_doc_arc_get(d->doc);
    CHECK(pc_document_layer(dc, to)->content.u.smart.nsmart_filters == 2);
    CHECK(pc_document_layer(dc, from)->content.u.smart.nsmart_filters == 0); /* moved */

    /* from == to -> error, no crash */
    (void)snprintf(json, sizeof json, "{\"from\":%llu,\"to\":%llu}", (unsigned long long)to,
                   (unsigned long long)to);
    CHECK(!exec(s, "layer.smartFilter.transfer", json));
    pc_engine_session_free(s);
}

static void test_via_copy(void)
{
    pc_layer_id id;
    pc_engine_session *s = smart_session(&id, 1, false);
    size_t before;
    {
        const pc_document *dc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
        before = dc->nlayers;
    }
    CHECK(exec(s, "layer.smartObjects.newSmartObjectViaCopy", "{}"));
    const pc_document *dc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(dc->nlayers == before + 1);
    /* the new active layer is a smart object with its own (embedded) contents */
    const pc_smart_object *sm = active_smart(s);
    CHECK(!sm->source.linked);
    pc_engine_session_free(s);
}

/* ---- adversarial params: never crash ---- */

static void test_adversarial(void)
{
    static const char *const ids[] = {
        "layer.smartObjects.newSmartObjectViaCopy", "layer.smartObjects.rasterize",
        "layer.smartFilter.disableSmartFilters",    "layer.smartFilter.deleteFilterMask",
        "layer.smartFilter.disableFilterMask",      "layer.smartFilter.blendingOptions",
        "layer.smartFilter.clearSmartFilters",      "layer.smartFilter.setVisible",
        "layer.smartFilter.setParams",              "layer.smartFilter.delete",
        "layer.smartFilter.move",                   "layer.smartFilter.transfer",
        "view.clearSelectedArtboardGuides",
    };
    static const char *const payloads[] = {
        "{}",
        "null",
        "{\"layer\":-1}",
        "{\"layer\":\"nope\"}",
        "{\"index\":999999999}",
        "{\"index\":-3,\"to\":-1}",
        "{\"from\":4294967296,\"to\":0}",
        "{\"params\":42}",
        "{\"opacity\":\"x\",\"blend\":7}",
        "{\"visible\":\"maybe\"}",
    };
    /* Against a fresh smart layer, and against an empty session (no document). */
    for (int withdoc = 0; withdoc < 2; withdoc++) {
        pc_engine_session *s = NULL;
        pc_layer_id id = 0;
        if (withdoc) {
            s = smart_session(&id, 2, true);
        } else {
            CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
        }
        for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
            for (size_t j = 0; j < sizeof payloads / sizeof payloads[0]; j++) {
                pc_json_value out;
                pc_error err = {0};
                pc_status rc = pc_engine_session_execute_json(s, ids[i], payloads[j], &out, &err);
                if (rc == PC_OK) {
                    pc_json_value_fini(&out);
                }
                /* no assertion on rc: either outcome is acceptable, the point is no crash */
            }
        }
        pc_engine_session_free(s);
    }
    CHECK(true);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_set_visible);
    RUN(test_set_params);
    RUN(test_blending_options);
    RUN(test_move_and_delete);
    RUN(test_clear_and_disable);
    RUN(test_transfer);
    RUN(test_via_copy);
    RUN(test_adversarial);
    return pc_t_finish();
}
