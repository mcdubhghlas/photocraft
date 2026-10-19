/* Layer styles (port of crates/engine/src/layer_style.rs tests, E6).
 *
 * Covers the effect-building primitives (contours, factory defaults, gradient styles) and the
 * registered layer.layerStyle.* commands at the data level (reads the layer's pc_effects). The
 * reference's rendering tests (over the compositor) and the makeDefault/defaultFor tests (preset
 * store, E2b) are not ported here. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/json.h"
#include "base/str.h"
#include "color/color.h"
#include "doc/doc.h"
#include "doc/effects.h"
#include "doc/json.h"
#include "engine/engine_internal.h"
#include "engine/layer_style.h"
#include "harness.h"

#include <yyjson.h>

/* A session with a white Background and one transparent layer on top (active). */
static pc_engine_session *session(pc_layer_id *top)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(32, 32), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
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

static bool exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    return rc == PC_OK;
}

static const pc_effects *active_effects(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL && d->has_active_layer);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    CHECK(l != NULL);
    return &l->effects;
}

/* ---- primitives ---- */

static void test_gradient_styles_parse(void)
{
    CHECK(pc_engine_gradient_style("Radial") == PC_GRADIENT_RADIAL);
    CHECK(pc_engine_gradient_style("diamond") == PC_GRADIENT_DIAMOND);
    CHECK(pc_engine_gradient_style("angle") == PC_GRADIENT_ANGLE);
    CHECK(pc_engine_gradient_style("reflected") == PC_GRADIENT_REFLECTED);
    CHECK(pc_engine_gradient_style("?") == PC_GRADIENT_LINEAR);
    CHECK(pc_engine_gradient_style(NULL) == PC_GRADIENT_LINEAR);
}

static void test_factory_defaults(void)
{
    const char *kinds[] = {"dropShadow",   "innerShadow",     "outerGlow",      "innerGlow", "stroke",
                           "colorOverlay", "gradientOverlay", "patternOverlay", "satin",     "bevelEmboss"};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        CHECK(d != NULL);
        yyjson_mut_val *o = NULL;
        CHECK(pc_engine_effect_defaults(kinds[i], d, &o));
        CHECK(o != NULL && yyjson_mut_obj_size(o) > 0);
        yyjson_mut_doc_free(d);
        pc_effect e;
        CHECK(pc_engine_effect_from_params(kinds[i], NULL, &e));
        pc_effect_fini(&e);
    }
    pc_effect bad;
    CHECK(!pc_engine_effect_from_params("nope", NULL, &bad));
    yyjson_mut_doc *d2 = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o2 = NULL;
    CHECK(!pc_engine_effect_defaults("nope", d2, &o2));
    yyjson_mut_doc_free(d2);
}

static void test_builtin_contours(void)
{
    pc_contour c;
    CHECK(pc_engine_builtin_contour("Linear", &c));
    CHECK(!c.custom);
    pc_contour_fini(&c);

    CHECK(pc_engine_builtin_contour("Cone", &c));
    CHECK(c.custom);
    CHECK(c.points.len == 18);
    CHECK(pc_contour_is_corner(&c, 1));
    /* contour_param of a known preset is its name. */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v = NULL;
    CHECK(pc_engine_contour_param(&c, d, &v, NULL) == PC_OK);
    CHECK(yyjson_mut_is_str(v) && strcmp(yyjson_mut_get_str(v), "Cone") == 0);
    yyjson_mut_doc_free(d);
    pc_contour_fini(&c);

    CHECK(!pc_engine_builtin_contour("No Such Contour", &c));
    CHECK(!pc_engine_builtin_contour(NULL, &c));
}

/* Parse a JSON literal into an owned yyjson_doc for the from-param tests. */
static yyjson_doc *parse(const char *json)
{
    return yyjson_read(json, strlen(json), 0);
}

static void test_custom_contour_roundtrip(void)
{
    const char *curve = "{\"name\":\"Mine\",\"points\":[[0.0,0.2],[0.4,0.9],[1.0,0.1]],"
                        "\"corners\":[false,true,false]}";
    yyjson_doc *jd = parse(curve);
    CHECK(jd != NULL);
    pc_contour c;
    CHECK(pc_engine_contour_from_param(yyjson_doc_get_root(jd), &c));
    CHECK(c.custom && c.points.len == 3);
    CHECK(!pc_contour_is_corner(&c, 0) && pc_contour_is_corner(&c, 1) && !pc_contour_is_corner(&c, 2));
    CHECK(fabsf(c.points.pts[1].input - 0.4f) < 1e-6f && fabsf(c.points.pts[1].output - 0.9f) < 1e-6f);
    yyjson_doc_free(jd);

    /* .shc round trip keeps the corner and the middle point. */
    pc_buf shc;
    CHECK(pc_engine_contour_to_shc(&c, &shc, NULL) == PC_OK);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    CHECK(pc_engine_contours_from_shc(shc.data, shc.len, d, arr, NULL) == PC_OK);
    CHECK(yyjson_mut_arr_size(arr) == 1);
    /* Re-parse the emitted param through an immutable doc. */
    yyjson_mut_doc_set_root(d, yyjson_mut_arr_get(arr, 0));
    char *text = yyjson_mut_write(d, 0, NULL);
    CHECK(text != NULL);
    yyjson_doc *back = parse(text);
    free(text);
    pc_contour c2;
    CHECK(pc_engine_contour_from_param(yyjson_doc_get_root(back), &c2));
    CHECK(c2.points.len == 3);
    CHECK(!pc_contour_is_corner(&c2, 0) && pc_contour_is_corner(&c2, 1) && !pc_contour_is_corner(&c2, 2));
    CHECK(fabsf(c2.points.pts[1].input - 0.4f) < 0.003f && fabsf(c2.points.pts[1].output - 0.9f) < 0.003f);
    yyjson_doc_free(back);
    yyjson_mut_doc_free(d);
    pc_buf_fini(&shc);
    pc_contour_fini(&c);
    pc_contour_fini(&c2);

    /* Malformed curves are rejected (effect keeps its contour). */
    const char *bad[] = {"{\"points\":[[0.5,0.5]]}", "{\"points\":[[0.0,\"x\"],[1.0,1.0]]}",
                         "{\"points\":\"no\"}", "7"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        yyjson_doc *b = parse(bad[i]);
        pc_contour junk;
        CHECK(!pc_engine_contour_from_param(yyjson_doc_get_root(b), &junk));
        yyjson_doc_free(b);
    }
}

static void test_overlay_contour_carries_unknown_names(void)
{
    /* A known preset applies; noise is a percentage. */
    pc_effect fx;
    CHECK(pc_engine_effect_from_params("dropShadow", NULL, &fx));
    yyjson_doc *p = parse("{\"contour\":\"Cone\",\"noise\":30}");
    pc_engine_overlay_effect(&fx, yyjson_doc_get_root(p));
    yyjson_doc_free(p);
    pc_contour cone;
    CHECK(pc_engine_builtin_contour("Cone", &cone));
    CHECK(pc_contour_eq(&fx.u.shadow.contour, &cone));
    CHECK(fabsf(fx.u.shadow.noise - 0.3f) < 1e-6f);
    pc_contour_fini(&cone);
    pc_effect_fini(&fx);

    /* An imported contour with an unknown name survives overlaying its own name. */
    pc_effect carried;
    CHECK(pc_engine_effect_from_params("dropShadow", NULL, &carried));
    pc_contour_fini(&carried.u.shadow.contour);
    const char *imp = "{\"name\":\"Photoshop contour\",\"points\":[[0.0,1.0],[1.0,0.0]]}";
    yyjson_doc *ij = parse(imp);
    CHECK(pc_engine_contour_from_param(yyjson_doc_get_root(ij), &carried.u.shadow.contour));
    yyjson_doc_free(ij);
    yyjson_doc *q = parse("{\"contour\":\"Photoshop contour\"}");
    pc_engine_overlay_effect(&carried, yyjson_doc_get_root(q));
    yyjson_doc_free(q);
    CHECK(fabsf(carried.u.shadow.contour.points.pts[0].output - 1.0f) < 1e-6f); /* unchanged */
    /* Linear applies by name. */
    yyjson_doc *lin = parse("{\"contour\":\"Linear\"}");
    pc_engine_overlay_effect(&carried, yyjson_doc_get_root(lin));
    yyjson_doc_free(lin);
    CHECK(!carried.u.shadow.contour.custom);
    pc_effect_fini(&carried);
}

/* ---- commands ---- */

static void test_every_style_command_adds_its_effect(void)
{
    const char *kinds[] = {"dropShadow",   "innerShadow",     "outerGlow",   "innerGlow", "stroke",
                           "colorOverlay", "gradientOverlay", "bevelEmboss", "satin"};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        pc_engine_session *s = session(NULL);
        char id[64];
        (void)snprintf(id, sizeof id, "layer.layerStyle.%s", kinds[i]);
        CHECK(exec(s, id, "{}"));
        const pc_effects *fx = active_effects(s);
        CHECK(fx->nitems == 1);
        CHECK(pc_effect_enabled(&fx->items[0]));
        pc_engine_session_free(s);
    }
}

static void test_params_applied_replace_add(void)
{
    pc_engine_session *s = session(NULL);
    CHECK(exec(s, "layer.layerStyle.stroke",
               "{\"size\":7,\"position\":\"inside\",\"color\":\"#00ff00\",\"opacity\":50}"));
    const pc_effects *fx = active_effects(s);
    CHECK(fx->nitems == 1 && fx->items[0].kind == PC_FX_STROKE);
    CHECK(fx->items[0].u.stroke.size == 7.0f);
    CHECK(fx->items[0].u.stroke.position == PC_STROKE_INSIDE);
    CHECK(fabsf(fx->items[0].u.stroke.common.opacity - 0.5f) < 1e-6f);
    CHECK(fx->items[0].u.stroke.paint.kind == PC_FX_PAINT_COLOR);
    /* Replaces (same kind). */
    CHECK(exec(s, "layer.layerStyle.stroke", "{\"size\":2}"));
    CHECK(active_effects(s)->nitems == 1);
    /* Adds a second instance. */
    CHECK(exec(s, "layer.layerStyle.stroke", "{\"size\":4,\"add\":true}"));
    CHECK(active_effects(s)->nitems == 2);
    CHECK(exec(s, "layer.layerStyle.dropShadow", "{\"blend\":\"normal\",\"distance\":12}"));
    fx = active_effects(s);
    CHECK(fx->nitems == 3 && fx->items[2].kind == PC_FX_DROP_SHADOW);
    CHECK(fx->items[2].u.shadow.common.blend == PC_BLEND_NORMAL && fx->items[2].u.shadow.distance == 12.0f);
    pc_engine_session_free(s);
}

static void test_clear_and_undo(void)
{
    pc_engine_session *s = session(NULL);
    CHECK(exec(s, "layer.layerStyle.colorOverlay", "{\"color\":\"#ff0000\"}"));
    CHECK(exec(s, "layer.layerStyle.clear", "{}"));
    CHECK(active_effects(s)->nitems == 0);
    CHECK(pc_engine_session_undo(s));
    CHECK(active_effects(s)->nitems == 1);
    pc_engine_session_free(s);
}

static void test_clear_one_effect_by_index(void)
{
    pc_layer_id top;
    pc_engine_session *s = session(&top);
    CHECK(exec(s, "layer.layerStyle.dropShadow", "{}"));
    CHECK(exec(s, "layer.layerStyle.colorOverlay", "{\"color\":\"#ff0000\"}"));
    char p[96];
    (void)snprintf(p, sizeof p, "{\"layer\":%llu,\"effect\":0}", (unsigned long long)top);
    CHECK(exec(s, "layer.layerStyle.clear", p));
    const pc_effects *fx = active_effects(s);
    CHECK(fx->nitems == 1 && fx->items[0].kind == PC_FX_COLOR_OVERLAY);
    /* Bad params change nothing. */
    CHECK(!exec(s, "layer.layerStyle.clear", "{\"effect\":5}"));
    CHECK(!exec(s, "layer.layerStyle.clear", "{\"effect\":-1}"));
    CHECK(!exec(s, "layer.layerStyle.clear", "{\"effect\":\"x\"}"));
    CHECK(!exec(s, "layer.layerStyle.clear", "{\"effect\":0,\"layer\":999999}"));
    CHECK(active_effects(s)->nitems == 1);
    CHECK(pc_engine_session_undo(s));
    CHECK(active_effects(s)->nitems == 2);
    pc_engine_session_free(s);
}

static void test_bevel_sets_technique_contour_texture(void)
{
    pc_engine_session *s = session(NULL);
    CHECK(exec(s, "layer.layerStyle.bevelEmboss",
               "{\"style\":\"pillow\",\"technique\":\"chiselHard\",\"contour\":true,\"contourRange\":70,"
               "\"texture\":\"Bubbles\",\"textureDepth\":-200,\"textureInvert\":true}"));
    const pc_effects *fx = active_effects(s);
    CHECK(fx->nitems == 1 && fx->items[0].kind == PC_FX_BEVEL_EMBOSS);
    const pc_bevel *b = &fx->items[0].u.bevel;
    CHECK(b->style == PC_BEVEL_PILLOW_EMBOSS && b->technique == PC_BEVEL_CHISEL_HARD);
    CHECK(b->has_contour && fabsf(b->contour.range - 0.7f) < 1e-6f);
    CHECK(b->has_texture);
    CHECK(pc_str_eq_c(&b->texture.name, "Bubbles") && b->texture.depth == -2.0f && b->texture.invert);
    pc_engine_session_free(s);
}

static void test_replace_rejects_bad_entries(void)
{
    pc_engine_session *s = session(NULL);
    const char *bad[] = {
        "{}",
        "{\"effects\":\"nope\"}",
        "{\"effects\":[{\"params\":{}}]}",
        "{\"effects\":[{\"kind\":\"nope\"}]}",
        "{\"effects\":[{\"kind\":\"stroke\",\"fx\":{\"Bogus\":{}}}]}",
        "{\"effects\":[{\"kind\":\"stroke\",\"fx\":3}]}",
        "{\"effects\":[{\"kind\":\"stroke\",\"params\":\"nope\"}]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!exec(s, "layer.layerStyle.replace", bad[i]));
    }
    CHECK(active_effects(s)->nitems == 0);
    /* A too-long list is refused, not run. */
    pc_buf b;
    pc_buf_init(&b);
    CHECK(pc_buf_append(&b, "{\"effects\":[", 12, NULL) == PC_OK);
    for (int i = 0; i < 257; i++) {
        CHECK(pc_buf_append(&b, i == 0 ? "{\"kind\":\"stroke\"}" : ",{\"kind\":\"stroke\"}",
                            (size_t)(i == 0 ? 17 : 18), NULL) == PC_OK);
    }
    CHECK(pc_buf_append(&b, "]}", 3, NULL) == PC_OK); /* include NUL */
    CHECK(!exec(s, "layer.layerStyle.replace", (const char *)b.data));
    CHECK(active_effects(s)->nitems == 0);
    pc_buf_fini(&b);
    pc_engine_session_free(s);
}

static void test_replace_builds_and_edits_snapshots(void)
{
    pc_layer_id top;
    pc_engine_session *s = session(&top);
    /* Build a stroke snapshot (size 7), then replace: one edits the snapshot with params, one is
     * fresh. */
    pc_effect stroke;
    CHECK(pc_engine_effect_from_params("stroke", NULL, &stroke));
    stroke.u.stroke.size = 7.0f;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *effects = yyjson_mut_arr(d);
    yyjson_mut_val *e1 = yyjson_mut_obj(d);
    (void)yyjson_mut_obj_add_str(d, e1, "kind", "stroke");
    (void)yyjson_mut_obj_add_val(d, e1, "fx", pc_effect_to_json(d, &stroke, 0));
    yyjson_mut_val *p1 = yyjson_mut_obj(d);
    (void)yyjson_mut_obj_add_int(d, p1, "opacity", 50);
    (void)yyjson_mut_obj_add_val(d, e1, "params", p1);
    (void)yyjson_mut_arr_append(effects, e1);
    yyjson_mut_val *e2 = yyjson_mut_obj(d);
    (void)yyjson_mut_obj_add_str(d, e2, "kind", "stroke");
    yyjson_mut_val *p2 = yyjson_mut_obj(d);
    (void)yyjson_mut_obj_add_int(d, p2, "size", 2);
    (void)yyjson_mut_obj_add_str(d, p2, "color", "#ffffff");
    (void)yyjson_mut_obj_add_val(d, e2, "params", p2);
    (void)yyjson_mut_arr_append(effects, e2);
    (void)yyjson_mut_obj_add_val(d, root, "effects", effects);
    yyjson_mut_doc_set_root(d, root);
    char *text = yyjson_mut_write(d, 0, NULL);
    yyjson_mut_doc_free(d);
    pc_effect_fini(&stroke);
    CHECK(text != NULL);
    CHECK(exec(s, "layer.layerStyle.replace", text));
    free(text);
    const pc_effects *fx = active_effects(s);
    CHECK(fx->nitems == 2);
    CHECK(fx->items[0].kind == PC_FX_STROKE && fx->items[0].u.stroke.size == 7.0f); /* snapshot kept */
    CHECK(fabsf(fx->items[0].u.stroke.common.opacity - 0.5f) < 1e-6f);              /* overlaid */
    CHECK(fx->items[1].u.stroke.size == 2.0f);
    /* Empty list clears. */
    CHECK(exec(s, "layer.layerStyle.replace", "{\"effects\":[]}"));
    CHECK(active_effects(s)->nitems == 0);
    pc_engine_session_free(s);
}

static void test_gradient_overlay_method(void)
{
    pc_engine_session *s = session(NULL);
    CHECK(exec(s, "layer.layerStyle.gradientOverlay",
               "{\"from\":\"#000000\",\"to\":\"#ffffff\",\"method\":\"classic\"}"));
    const pc_effects *fx = active_effects(s);
    CHECK(fx->nitems == 1 && fx->items[0].kind == PC_FX_GRADIENT_OVERLAY);
    const pc_gradient *g = &fx->items[0].u.gradient_overlay.gradient;
    CHECK(g->nstops > 2); /* baked to dense stops */
    /* Interpolate the red channel at t; Photoshop's Classic: ~0.037 at 1/16, 0.5 at 0.5. */
    for (int k = 0; k < 2; k++) {
        float t = (k == 0) ? 1.0f / 16.0f : 0.5f;
        float want = (k == 0) ? 0.037f : 0.5f;
        float r = 0.0f;
        for (size_t i = 0; i + 1 < g->nstops; i++) {
            if (t >= g->stops[i].pos && t <= g->stops[i + 1].pos) {
                float span = g->stops[i + 1].pos - g->stops[i].pos;
                float u = span > 1e-6f ? (t - g->stops[i].pos) / span : 0.0f;
                float r0[3], r1[3];
                pc_color_to_rgb(&g->stops[i].color, r0);
                pc_color_to_rgb(&g->stops[i + 1].color, r1);
                r = r0[0] + (r1[0] - r0[0]) * u;
                break;
            }
        }
        CHECK(fabsf(r - want) < 0.01f);
    }
    /* Without a method the two stops stay as they are. */
    CHECK(exec(s, "layer.layerStyle.gradientOverlay",
               "{\"from\":\"#000000\",\"to\":\"#ffffff\",\"add\":true}"));
    fx = active_effects(s);
    bool found_two = false;
    for (size_t i = 0; i < fx->nitems; i++) {
        if (fx->items[i].kind == PC_FX_GRADIENT_OVERLAY &&
            fx->items[i].u.gradient_overlay.gradient.nstops == 2) {
            found_two = true;
        }
    }
    CHECK(found_two);
    pc_engine_session_free(s);
}

static void test_patternoverlay_library_and_bad_layer(void)
{
    pc_engine_session *s = session(NULL);
    CHECK(pc_engine_command_find("layer.layerStyle.patternOverlay") != NULL);
    CHECK(exec(s, "layer.layerStyle.patternOverlay", "{\"pattern\":\"Bricks\",\"scale\":37}"));
    CHECK(active_effects(s)->nitems == 1 && active_effects(s)->items[0].kind == PC_FX_PATTERN_OVERLAY);
    CHECK(active_effects(s)->items[0].u.pattern_overlay.scale == .37f);
    CHECK(pc_doc_arc_get(pc_engine_session_active(s)->doc)->npatterns == 1);
    CHECK(pc_engine_session_undo(s));
    CHECK(active_effects(s)->nitems == 0 && pc_doc_arc_get(pc_engine_session_active(s)->doc)->npatterns == 0);
    CHECK(exec(s, "layer.layerStyle.replace",
               "{\"effects\":[{\"kind\":\"patternOverlay\",\"params\":{\"pattern\":\"Bricks\"}}]}"));
    CHECK(active_effects(s)->nitems == 1);
    CHECK(!exec(s, "layer.layerStyle.patternOverlay", "{\"pattern\":\"missing-pattern-name\"}"));
    CHECK(active_effects(s)->nitems == 1);
    pc_engine_session_free(s);

    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(!exec(empty, "layer.layerStyle.dropShadow", "{}")); /* no document */
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_gradient_styles_parse);
    RUN(test_factory_defaults);
    RUN(test_builtin_contours);
    RUN(test_custom_contour_roundtrip);
    RUN(test_overlay_contour_carries_unknown_names);
    RUN(test_every_style_command_adds_its_effect);
    RUN(test_params_applied_replace_add);
    RUN(test_clear_and_undo);
    RUN(test_clear_one_effect_by_index);
    RUN(test_bevel_sets_technique_contour_texture);
    RUN(test_replace_rejects_bad_entries);
    RUN(test_replace_builds_and_edits_snapshots);
    RUN(test_gradient_overlay_method);
    RUN(test_patternoverlay_library_and_bad_layer);
    return pc_t_finish();
}
