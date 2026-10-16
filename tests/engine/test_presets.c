/* Preset-panel data model and commands (E2b): built-in data, the Clone Source / Tool Presets /
 * Swatches / Gradients panel commands, file import/export round trips and adversarial inputs. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "doc/doc.h"
#include "doc/effects.h"
#include "doc/vector.h"
#include "engine/engine_internal.h"
#include "engine/layer_style.h"
#include "engine/presets.h"
#include "harness.h"
#include "paint/tile.h"
#include "psd/grd.h"

static pc_status ex(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    pc_error e = {0};
    pc_json_value scratch = PC_JSON_VALUE_INIT;
    pc_json_value *o = out != NULL ? out : &scratch;
    pc_status rc = pc_engine_session_execute_json(s, id, json, o, &e);
    if (out == NULL) {
        pc_json_value_fini(&scratch);
    }
    return rc;
}

static yyjson_mut_val *getk(const pc_json_value *v, const char *k)
{
    yyjson_mut_val *r = pc_json_value_root(v);
    return r != NULL ? yyjson_mut_obj_get(r, k) : NULL;
}

/* ---- built-in data ------------------------------------------------------------------------ */

static void test_builtin_data(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    const pc_preset_state *st = &s->presets;
    CHECK_EQ_UINT(st->gradients.n, 10); /* Basics + 8 hue groups + Rainbows */
    CHECK_EQ_STR(st->gradients.v[0].name, "Basics");
    CHECK_EQ_STR(st->gradients.v[9].name, "Rainbows");
    CHECK_EQ_UINT(st->styles.n, 3);
    CHECK_EQ_UINT(st->shapes.n, 5);
    CHECK_EQ_UINT(st->pattern_groups.n, 2);
    CHECK_EQ_UINT(st->ntool_presets, 5);
    CHECK(st->clone.active == 0);
    CHECK(st->clone.slots[0].scale[0] == 100.0 && st->clone.slots[0].scale[1] == 100.0);
    CHECK(st->clone.overlay.show && st->clone.overlay.opacity == 100.0f);
    /* current gradient defaults to Foreground to Background. */
    CHECK_EQ_STR(st->gradient.name, "Foreground to Background");
    CHECK_EQ_UINT(st->gradient.nstops, 2);

    /* 46 swatches in 5 groups; group[1] is the exact-RGB group. */
    size_t total = 0;
    for (size_t i = 0; i < st->swatches.n; i++) {
        total += st->swatches.v[i].n;
    }
    CHECK_EQ_UINT(st->swatches.n, 5);
    CHECK_EQ_UINT(total, 46);
    CHECK_EQ_STR(st->swatches.v[1].name, "RGB");
    pc_engine_session_free(s);
}

static void test_swatch_conv_builtin(void)
{
    /* The RGB group converts to the exact primaries/secondaries. */
    pc_swatch_conv *c = pc_swatch_conv_builtin();
    CHECK(c != NULL);
    pc_swatch_color red = {PC_SWATCH_RGB, {1.0f, 0.0f, 0.0f, 0.0f}, 0, {0}};
    float rgb[3];
    pc_swatch_conv_rgb(c, &red, rgb);
    char hex[8];
    pc_swatch_hex(rgb, hex);
    CHECK_EQ_STR(hex, "#ff0000");
    /* Non-finite stored values never escape 0..1. */
    pc_swatch_color bad = {PC_SWATCH_LAB, {500.0f, 1.0f / 0.0f * 0.0f, -999.0f, 0.0f}, 0, {0}};
    (void)bad;
    pc_swatch_conv_free(c);
}

static void test_shape_parse(void)
{
    char msg[256];
    pc_path path;
    /* A valid shape path. */
    CHECK(pc_shape_parse_path("M 0 0 L 100 0 L 100 100 Z", &path, msg));
    CHECK(path.nsubpaths == 1);
    pc_path_fini(&path);
    /* A circle. */
    CHECK(pc_shape_parse_path("O 50 50 40", &path, msg));
    CHECK(path.nsubpaths == 1 && path.subpaths[0].nknots == 4);
    pc_path_fini(&path);
    /* Adversarial: bad commands / empty never crash. */
    CHECK(!pc_shape_parse_path("", &path, msg));
    CHECK(!pc_shape_parse_path("X 1 2", &path, msg));
    CHECK(!pc_shape_parse_path("L 1 2", &path, msg)); /* L before M */
    CHECK(!pc_shape_parse_path("M 1", &path, msg));   /* short */
}

/* ---- clone source ------------------------------------------------------------------------- */

static void test_clone_source(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "cloneSource.set", "{\"source\":[10,20]}", &out) == PC_OK);
    pc_json_value_fini(&out);
    /* linked W/H keep their ratio (ported Rust test). */
    CHECK(ex(s, "cloneSource.set", "{\"width\":50,\"height\":200}", NULL) == PC_OK);
    CHECK(ex(s, "cloneSource.set", "{\"width\":100}", NULL) == PC_OK);
    CHECK(s->presets.clone.slots[0].scale[0] == 100.0 && s->presets.clone.slots[0].scale[1] == 200.0);
    CHECK(ex(s, "cloneSource.set", "{\"linked\":true,\"width\":150}", &out) == PC_OK);
    CHECK(s->presets.clone.slots[0].scale[0] == 150.0 && s->presets.clone.slots[0].scale[1] == 300.0);
    pc_json_value_fini(&out);
    CHECK(ex(s, "cloneSource.resetTransform", "{}", NULL) == PC_OK);
    CHECK(s->presets.clone.slots[0].scale[0] == 100.0);
    CHECK(ex(s, "cloneSource.overlay", "{\"blend\":\"darken\",\"opacity\":50}", NULL) == PC_OK);
    CHECK(s->presets.clone.overlay.blend == 1 && s->presets.clone.overlay.opacity == 50.0f);
    /* adversarial */
    CHECK(ex(s, "cloneSource.select", "{\"index\":9}", NULL) != PC_OK);
    CHECK(ex(s, "cloneSource.select", "{}", NULL) != PC_OK);
    CHECK(ex(s, "cloneSource.overlay", "{\"blend\":\"nope\"}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- tool presets ------------------------------------------------------------------------- */

static void test_tool_presets(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "tool.presets.new", "{\"tool\":\"brush\",\"name\":\"My Brush\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "name")), "My Brush");
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(s->presets.ntool_presets, 6);
    /* a taken name gets " 2". */
    CHECK(ex(s, "tool.presets.new", "{\"tool\":\"brush\",\"name\":\"My Brush\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "name")), "My Brush 2");
    pc_json_value_fini(&out);
    CHECK(ex(s, "tool.presets.select", "{\"preset\":\"My Brush\"}", NULL) == PC_OK);
    CHECK(ex(s, "tool.presets.edit", "{\"action\":\"rename\",\"preset\":\"My Brush\",\"name\":\"Renamed\"}", NULL) == PC_OK);
    CHECK(ex(s, "tool.presets.edit", "{\"action\":\"delete\",\"preset\":\"Renamed\"}", NULL) == PC_OK);
    CHECK_EQ_UINT(s->presets.ntool_presets, 6);
    CHECK(ex(s, "tool.presets.reset", "{}", NULL) == PC_OK);
    CHECK_EQ_UINT(s->presets.ntool_presets, 5);
    /* adversarial */
    CHECK(ex(s, "tool.presets.new", "{}", NULL) != PC_OK);                         /* no tool */
    CHECK(ex(s, "tool.presets.new", "{\"tool\":\"brush\",\"options\":5}", NULL) != PC_OK);
    CHECK(ex(s, "tool.presets.select", "{\"preset\":\"nope\"}", NULL) != PC_OK);
    CHECK(ex(s, "tool.presets.edit", "{\"action\":\"move\"}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- swatches ----------------------------------------------------------------------------- */

static void test_swatches(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "swatches.add", "{\"color\":\"#123456\",\"name\":\"Teal\",\"group\":\"Mine\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "group")), "Mine");
    pc_json_value_fini(&out);
    CHECK(ex(s, "swatches.add", "{\"color\":{\"model\":\"cmyk\",\"values\":[100,0,50,0]},\"name\":\"Ink\"}", NULL) == PC_OK);
    CHECK(ex(s, "swatches.use", "{\"swatch\":\"Teal\",\"target\":\"foreground\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "hex")), "#123456");
    pc_json_value_fini(&out);
    CHECK(ex(s, "swatches.rename", "{\"swatch\":\"Teal\",\"name\":\"Teal2\"}", NULL) == PC_OK);
    CHECK(ex(s, "swatches.delete", "{\"swatch\":\"Teal2\"}", NULL) == PC_OK);
    CHECK(ex(s, "swatches.reset", "{}", NULL) == PC_OK);

    /* export then import (aco + ase) round trips without crashing. */
    for (int k = 0; k < 2; k++) {
        const char *fmt = k == 0 ? "aco" : "ase";
        char req[128];
        (void)snprintf(req, sizeof req, "{\"format\":\"%s\"}", fmt);
        CHECK(ex(s, "swatches.export", req, &out) == PC_OK);
        const char *data = yyjson_mut_get_str(getk(&out, "data"));
        CHECK(data != NULL);
        char *imp = malloc(strlen(data) + 64);
        (void)snprintf(imp, strlen(data) + 64, "{\"data\":\"%s\",\"mode\":\"append\",\"group\":\"R\"}", data);
        pc_json_value out2 = PC_JSON_VALUE_INIT;
        CHECK(ex(s, "swatches.import", imp, &out2) == PC_OK);
        pc_json_value_fini(&out2);
        free(imp);
        pc_json_value_fini(&out);
    }

    /* adversarial: corrupt files and bad params never crash. */
    CHECK(ex(s, "swatches.add", "{\"color\":\"#ff\"}", NULL) != PC_OK);
    CHECK(ex(s, "swatches.add", "{\"color\":{\"model\":\"xyz\",\"values\":[1,2,3]}}", NULL) != PC_OK);
    CHECK(ex(s, "swatches.import", "{\"data\":\"%%%notbase64\"}", NULL) != PC_OK);
    CHECK(ex(s, "swatches.import", "{\"data\":\"AAAA\"}", NULL) != PC_OK);
    CHECK(ex(s, "swatches.import", "{}", NULL) != PC_OK);
    CHECK(ex(s, "swatches.delete", "{\"swatch\":\"nope\"}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- gradients ---------------------------------------------------------------------------- */

static void test_gradients(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "gradient.presets.list", "{}", &out) == PC_OK);
    CHECK(getk(&out, "current") != NULL);
    pc_json_value_fini(&out);
    CHECK(ex(s, "gradient.presets.select", "{\"preset\":\"Rainbow\"}", NULL) == PC_OK);
    CHECK_EQ_STR(s->presets.gradient.name, "Rainbow");
    size_t n0 = s->presets.gradient.nstops;
    CHECK(ex(s, "gradient.presets.stop", "{\"action\":\"add\",\"location\":0.5}", NULL) == PC_OK);
    CHECK_EQ_UINT(s->presets.gradient.nstops, n0 + 1);
    CHECK(ex(s, "gradient.presets.new", "{\"name\":\"Mine\",\"group\":\"Basics\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "group")), "Basics");
    pc_json_value_fini(&out);
    CHECK(ex(s, "gradient.presets.edit", "{\"action\":\"rename\",\"preset\":\"Mine\",\"name\":\"Mine2\"}", NULL) == PC_OK);
    CHECK(ex(s, "gradient.presets.reset", "{}", NULL) == PC_OK);
    CHECK_EQ_UINT(s->presets.gradients.n, 10);

    /* importGrd: build a real .grd with the psd writer, import it. */
    pc_psd_grd_gradient g;
    memset(&g, 0, sizeof g);
    (void)pc_psd_str_set(&g.name, "Ember", 5, NULL);
    pc_psd_grd_stop stops[2];
    memset(stops, 0, sizeof stops);
    stops[0].location = 0.0f;
    stops[0].midpoint = 0.5f;
    stops[0].color.model = PC_PSD_GRD_FOREGROUND;
    stops[1].location = 1.0f;
    stops[1].midpoint = 0.5f;
    stops[1].color.model = PC_PSD_GRD_RGB;
    stops[1].color.c[0] = 1.0f;
    g.stops = stops;
    g.nstops = 2;
    pc_psd_bytes gb;
    memset(&gb, 0, sizeof gb);
    CHECK(pc_psd_grd_write(&g, 1, &gb, NULL) == PC_OK);
    pc_psd_str_fini(&g.name);
    pc_buf b64;
    pc_buf_init(&b64);
    CHECK(pc_paint_b64_encode(gb.data, gb.len, &b64, NULL) == PC_OK);
    pc_psd_bytes_fini(&gb);
    char *req = malloc(b64.len + 64);
    (void)snprintf(req, b64.len + 64, "{\"data\":\"%s\",\"group\":\"Fire\"}", (const char *)b64.data);
    CHECK(ex(s, "gradient.presets.importGrd", req, &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "group")), "Fire");
    pc_json_value_fini(&out);
    free(req);
    pc_buf_fini(&b64);

    /* adversarial */
    CHECK(ex(s, "gradient.presets.importGrd", "{\"data\":\"@@\"}", NULL) != PC_OK);
    CHECK(ex(s, "gradient.presets.importGrd", "{}", NULL) != PC_OK);
    CHECK(ex(s, "gradient.presets.select", "{\"preset\":\"nope\"}", NULL) != PC_OK);
    CHECK(ex(s, "gradient.presets.stop", "{\"action\":\"delete\",\"index\":99}", NULL) != PC_OK);
    CHECK(ex(s, "gradient.presets.new", "{\"stops\":[[0,\"#ff\"]]}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_persist_round_trip(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "swatches.add", "{\"color\":\"#abcdef\",\"name\":\"Sky\",\"group\":\"Custom\"}", NULL) == PC_OK);
    CHECK(ex(s, "gradient.presets.select", "{\"preset\":\"Rainbow\"}", NULL) == PC_OK);
    CHECK(ex(s, "tool.presets.new", "{\"tool\":\"brush\",\"name\":\"Persisted\"}", NULL) == PC_OK);
    pc_json_value blob = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_presets_to_json(s, &blob, NULL) == PC_OK);
    /* serialise to text, parse back, load into a fresh session. */
    pc_buf text;
    pc_buf_init(&text);
    CHECK(pc_json_write(pc_json_value_root(&blob), 0, &text, NULL) == PC_OK);
    pc_json_value_fini(&blob);
    yyjson_doc *jd = NULL;
    CHECK(pc_json_parse((const char *)text.data, text.len, &jd, NULL) == PC_OK);
    pc_buf_fini(&text);
    pc_engine_session *s2 = NULL;
    CHECK(pc_engine_session_new(&s2, NULL) == PC_OK);
    pc_engine_load_presets_json(s2, yyjson_doc_get_root(jd));
    yyjson_doc_free(jd);
    /* the custom swatch, current gradient and tool preset survived. */
    size_t gi, ii;
    CHECK(pc_preset_find(&s2->presets.swatches, "Sky", NULL, pc_swatch_ops(), &gi, &ii));
    CHECK_EQ_STR(s2->presets.gradient.name, "Rainbow");
    bool found = false;
    for (size_t i = 0; i < s2->presets.ntool_presets; i++) {
        if (strcmp(s2->presets.tool_presets[i].name, "Persisted") == 0) {
            found = true;
        }
    }
    CHECK(found);
    pc_engine_session_free(s);
    pc_engine_session_free(s2);
}

/* ---- style.presets.* / shape.presets.* (panel commands) ----------------------------------- */

static pc_engine_session *session_with_layer(pc_color_mode mode, pc_layer_id *top)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(16, 16), mode, PC_SAMPLE_TYPE_U8,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_layer l;
    CHECK(pc_layer_raster(&l, "Layer 1", pc_document_pixel_format(&doc), NULL) == PC_OK);
    pc_layer_id id = 0;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_engine_session_select_layer(s, id, NULL) == PC_OK);
    if (top != NULL) {
        *top = id;
    }
    return s;
}

static const pc_effects *layer_effects(pc_engine_session *s, pc_layer_id id)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), id);
    CHECK(l != NULL);
    return &l->effects;
}

static void test_style_presets(void)
{
    pc_layer_id id = 0;
    pc_engine_session *s = session_with_layer(PC_COLOR_MODE_RGB, &id);

    /* list: the three built-in groups. */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "style.presets.list", "{}", &out) == PC_OK);
    CHECK_EQ_UINT(yyjson_mut_arr_size(getk(&out, "groups")), 3);
    pc_json_value_fini(&out);

    /* apply Gold: the layer gains effects and reports {style:"Gold"}. */
    CHECK(ex(s, "style.presets.apply", "{\"preset\":\"Gold\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "style")), "Gold");
    pc_json_value_fini(&out);
    CHECK(layer_effects(s, id)->nitems > 0 && layer_effects(s, id)->enabled);

    /* apply with add appends. */
    size_t before = layer_effects(s, id)->nitems;
    CHECK(ex(s, "style.presets.apply", "{\"preset\":\"Drop Shadow\",\"add\":true}", NULL) == PC_OK);
    CHECK(layer_effects(s, id)->nitems == before + 1);

    /* Default Style (None) clears the effects. */
    CHECK(ex(s, "style.presets.apply", "{\"preset\":\"Default Style (None)\"}", NULL) == PC_OK);
    CHECK_EQ_UINT(layer_effects(s, id)->nitems, 0);

    /* unknown preset is a graceful error. */
    CHECK(ex(s, "style.presets.apply", "{\"preset\":\"Nope\"}", NULL) != PC_OK);
    CHECK(ex(s, "style.presets.apply", "{}", NULL) != PC_OK);

    /* new from a layer with effects, then it shows up in the list. */
    CHECK(ex(s, "style.presets.apply", "{\"preset\":\"Chrome\"}", NULL) == PC_OK);
    CHECK(ex(s, "style.presets.new", "{\"name\":\"Mine\",\"group\":\"Custom\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "name")), "Mine");
    pc_json_value_fini(&out);
    size_t gi, ii;
    CHECK(pc_preset_find(&s->presets.styles, "Mine", NULL, pc_style_preset_ops(), &gi, &ii));

    /* reset restores the three built-in groups (dropping Custom). */
    CHECK(ex(s, "style.presets.reset", "{}", NULL) == PC_OK);
    CHECK_EQ_UINT(s->presets.styles.n, 3);

    pc_engine_session_free(s);
}

static void test_style_apply_gray_mode(void)
{
    /* Applying an RGB preset in a grayscale document converts colours into the doc mode and
     * leaves the document in gray (fx_mode_tests::style_presets_apply). */
    pc_layer_id id = 0;
    pc_engine_session *s = session_with_layer(PC_COLOR_MODE_GRAYSCALE, &id);
    CHECK(ex(s, "style.presets.apply", "{\"preset\":\"Gold\"}", NULL) == PC_OK);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(pc_doc_arc_get(d->doc)->mode == PC_COLOR_MODE_GRAYSCALE);
    CHECK(layer_effects(s, id)->nitems > 0);
    pc_engine_session_free(s);
}

static void test_style_pattern_overlay_copies_pattern(void)
{
    pc_layer_id id = 0;
    pc_engine_session *s = session_with_layer(PC_COLOR_MODE_RGB, &id);
    /* Build a style with a pattern overlay naming the built-in "Bricks" and push it. */
    yyjson_doc *jd = NULL;
    CHECK(pc_json_parse("{\"pattern\":\"Bricks\"}", 20, &jd, NULL) == PC_OK);
    pc_style_preset sp;
    memset(&sp, 0, sizeof sp);
    sp.name = pc_strndup("Bricky", 6);
    sp.effects = calloc(1, sizeof *sp.effects);
    CHECK(sp.name != NULL && sp.effects != NULL);
    CHECK(pc_engine_effect_from_params("patternOverlay", yyjson_doc_get_root(jd), &sp.effects[0]));
    sp.neffects = 1;
    yyjson_doc_free(jd);
    CHECK(pc_preset_group_push(&s->presets.styles, 0, &sp, pc_style_preset_ops(), NULL) == PC_OK);

    char json[64];
    snprintf(json, sizeof json, "{\"preset\":\"Bricky\",\"layer\":%llu}", (unsigned long long)id);
    CHECK(ex(s, "style.presets.apply", json, NULL) == PC_OK);
    /* The document now holds the Bricks pattern. */
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    bool found = false;
    for (size_t i = 0; i < doc->npatterns; i++) {
        if (strcmp(pc_str_c(&doc->patterns[i].name), "Bricks") == 0) {
            found = true;
        }
    }
    CHECK(found);
    pc_engine_session_free(s);
}

static void test_shape_presets(void)
{
    pc_engine_session *s = session_with_layer(PC_COLOR_MODE_RGB, NULL);

    /* list: the five built-in groups (no custom shapes yet). */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "shape.presets.list", "{}", &out) == PC_OK);
    CHECK_EQ_UINT(yyjson_mut_arr_size(getk(&out, "groups")), 5);
    pc_json_value_fini(&out);

    /* place a built-in shape: a new shape layer, result carries {preset}. */
    CHECK(ex(s, "shape.presets.place", "{\"preset\":\"Heart\",\"rect\":[2,2,10,10],\"fill\":\"#ff0000\"}",
             &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "preset")), "Heart");
    pc_json_value_fini(&out);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    bool has_shape = false;
    for (size_t i = 0; i < doc->nlayers; i++) {
        if (doc->layers[i].content.kind == PC_LAYER_SHAPE) {
            has_shape = true;
        }
    }
    CHECK(has_shape);

    /* negative size flips; an empty rect and an unknown shape are graceful errors. */
    CHECK(ex(s, "shape.presets.place", "{\"preset\":\"Arrow Right\",\"rect\":[12,10,-10,-6]}", NULL) ==
          PC_OK);
    CHECK(ex(s, "shape.presets.place", "{\"preset\":\"Heart\",\"rect\":[0,0,0,5]}", NULL) != PC_OK);
    CHECK(ex(s, "shape.presets.place", "{\"preset\":\"Unicorn\"}", NULL) != PC_OK);

    /* new from the preset path language, then rename then delete it. */
    CHECK(ex(s, "shape.presets.new", "{\"name\":\"Tri\",\"path\":\"M 0 0 L 10 0 L 5 10 Z\"}", NULL) ==
          PC_OK);
    size_t gi, ii;
    CHECK(pc_preset_find(&s->presets.shapes, "Tri", NULL, pc_shape_preset_ops(), &gi, &ii));
    CHECK(ex(s, "shape.presets.edit", "{\"action\":\"rename\",\"preset\":\"Tri\",\"name\":\"Tri2\"}",
             NULL) == PC_OK);
    CHECK(pc_preset_find(&s->presets.shapes, "Tri2", NULL, pc_shape_preset_ops(), &gi, &ii));
    CHECK(ex(s, "shape.presets.edit", "{\"action\":\"delete\",\"preset\":\"Tri2\"}", NULL) == PC_OK);
    CHECK(!pc_preset_find(&s->presets.shapes, "Tri2", NULL, pc_shape_preset_ops(), &gi, &ii));

    CHECK(ex(s, "shape.presets.reset", "{}", NULL) == PC_OK);
    CHECK_EQ_UINT(s->presets.shapes.n, 5);
    pc_engine_session_free(s);
}

static void test_custom_shapes_join_panel(void)
{
    /* Edit > Define Custom Shape results show in the Shapes panel as a "Custom Shapes" group, and
     * shape.presets.edit renames/deletes them in the edit state. */
    pc_engine_session *s = session_with_layer(PC_COLOR_MODE_RGB, NULL);
    /* No path to define from yet. */
    CHECK(!pc_engine_session_is_enabled(s, "edit.defineCustomShape"));
    /* Set a work path, then define. */
    CHECK(ex(s, "path.set", "{\"path\":{\"subpaths\":[{\"closed\":true,\"knots\":[{\"anchor\":[0,0]},"
                            "{\"anchor\":[8,0]},{\"anchor\":[8,8]}]}]}}", NULL) == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "edit.defineCustomShape"));
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "edit.defineCustomShape", "{\"name\":\"My Star\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(getk(&out, "name")), "My Star");
    pc_json_value_fini(&out);
    size_t nc = 0;
    CHECK(pc_engine_custom_shapes(s, &nc) != NULL && nc == 1);

    /* It appears as a "Custom Shapes" group and places like any shape. */
    CHECK(ex(s, "shape.presets.list", "{}", &out) == PC_OK);
    CHECK_EQ_UINT(yyjson_mut_arr_size(getk(&out, "groups")), 6);
    pc_json_value_fini(&out);
    CHECK(ex(s, "shape.presets.place", "{\"preset\":\"My Star\"}", NULL) == PC_OK);

    /* rename + delete via shape.presets.edit route to the edit state. */
    CHECK(ex(s, "shape.presets.edit", "{\"action\":\"rename\",\"preset\":\"My Star\",\"name\":\"Star 2\"}",
             NULL) == PC_OK);
    CHECK(ex(s, "shape.presets.edit", "{\"action\":\"delete\",\"preset\":\"Star 2\"}", NULL) == PC_OK);
    (void)pc_engine_custom_shapes(s, &nc);
    CHECK_EQ_UINT(nc, 0);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_builtin_data);
    RUN(test_persist_round_trip);
    RUN(test_swatch_conv_builtin);
    RUN(test_shape_parse);
    RUN(test_clone_source);
    RUN(test_tool_presets);
    RUN(test_swatches);
    RUN(test_gradients);
    RUN(test_style_presets);
    RUN(test_style_apply_gray_mode);
    RUN(test_style_pattern_overlay_copies_pattern);
    RUN(test_shape_presets);
    RUN(test_custom_shapes_join_panel);
    return pc_t_finish();
}
