/* Image menu commands (ported from the reachable parts of crates/engine/src/image_cmds/tests):
 * Image Size, Canvas Size, Crop, Trim, Mode (model + bit depth), Duplicate, Reveal All and the
 * right-angle Image Rotation / flips. Every registered command also gets an adversarial-params
 * sweep (REWRITE_PLAN rule 9: no crash, an Err instead).
 *
 * The exact resampled pixel values are golden-sensitive and exercised by the algo resample tests;
 * here we assert the structural outcomes (sizes, bounds, history, modes) the engine is responsible
 * for and that route through the same algo calls as the reference. */
#include <math.h>
#include <string.h>

#include "base/mem.h"
#include "color/color.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/surface.h"
#include "engine/engine_internal.h"
#include "harness.h"

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d != NULL ? pc_doc_arc_get(d->doc) : NULL;
}

static size_t past_len(pc_engine_session *s)
{
    return pc_history_past_len(pc_engine_session_active(s)->history);
}

static pc_status new_doc(pc_engine_session *s, const char *json)
{
    pc_json_value out;
    pc_status rc = pc_engine_session_execute_json(s, "file.new", json, &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    return rc;
}

/* An edit that appends a transparent raster layer with a red square at (10,5)-(20,15). */
static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ctx;
    pc_layer l;
    pc_status rc = pc_layer_raster(&l, "Layer 1", pc_document_pixel_format(doc), err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface *surf = pc_layer_surface_mut(&l);
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_from_rgba(pc_surface_format(surf), red, enc);
    rc = pc_surface_fill_rect(surf, pc_rect_make(10, 5, 20, 15), enc, n, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    pc_layer_mask_reveal_all(&l.mask);
    l.has_mask = true;
    pc_effect fx;
    pc_effect_default_drop_shadow(&fx);
    size_t ecap = l.effects.nitems;
    pc_effect *eg = pc_grow_array(l.effects.items, &ecap, l.effects.nitems + 1, sizeof(pc_effect));
    if (eg == NULL) {
        pc_effect_fini(&fx);
        pc_layer_fini(&l);
        return PC_ERR_NOMEM;
    }
    l.effects.items = eg;
    l.effects.items[l.effects.nitems] = fx;
    l.effects.nitems++;
    l.effects.enabled = true;
    size_t cap = doc->nlayers;
    pc_layer *grown = pc_grow_array(doc->layers, &cap, doc->nlayers + 1, sizeof(pc_layer));
    if (grown == NULL) {
        pc_layer_fini(&l);
        return PC_ERR_NOMEM;
    }
    doc->layers = grown;
    *active = l.id;
    *ha = true;
    doc->layers[doc->nlayers] = l;
    doc->nlayers++;
    return PC_OK;
}

/* A red square at (-5,5)-(15,15), partly off-canvas, on a new layer (for Reveal All). */
static pc_status paint_offcanvas_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx,
                                      pc_error *err)
{
    (void)ctx;
    pc_layer l;
    pc_status rc = pc_layer_raster(&l, "Off", pc_document_pixel_format(doc), err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface *surf = pc_layer_surface_mut(&l);
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_from_rgba(pc_surface_format(surf), red, enc);
    rc = pc_surface_fill_rect(surf, pc_rect_make(-5, 5, 15, 15), enc, n, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    size_t cap = doc->nlayers;
    pc_layer *grown = pc_grow_array(doc->layers, &cap, doc->nlayers + 1, sizeof(pc_layer));
    if (grown == NULL) {
        pc_layer_fini(&l);
        return PC_ERR_NOMEM;
    }
    doc->layers = grown;
    *active = l.id;
    *ha = true;
    doc->layers[doc->nlayers] = l;
    doc->nlayers++;
    return PC_OK;
}

/* 40x20 doc with a `bg` Background and a red-square layer on top (the Rust session()). */
static pc_engine_session *session_bg(const char *bg)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char p[96];
    snprintf(p, sizeof p, "{\"width\":40,\"height\":20,\"background\":\"%s\"}", bg);
    CHECK(new_doc(s, p) == PC_OK);
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    return s;
}

static pc_engine_session *session(void)
{
    return session_bg("white");
}

static pc_rect layer_bounds(pc_engine_session *s, size_t layer)
{
    const pc_surface *surf = pc_layer_surface(&doc_of(s)->layers[layer]);
    return pc_surface_content_bounds(surf);
}

static void layer_rgba(pc_engine_session *s, size_t layer, int32_t x, int32_t y, float out[4])
{
    pc_surface_rgba(pc_layer_surface(&doc_of(s)->layers[layer]), x, y, out);
}

/* ---- Image Size --------------------------------------------------------------------------- */

static void test_image_size_scales(void)
{
    const char *filters[] = {"bicubic",         "bicubicSmoother", "bicubicSharper",
                             "bicubicAutomatic", "automatic",       "bilinear",
                             "nearest",         "lanczos"};
    for (size_t i = 0; i < sizeof filters / sizeof filters[0]; i++) {
        pc_engine_session *s = session();
        char p[128];
        snprintf(p, sizeof p, "{\"width\":80,\"resample\":\"%s\"}", filters[i]);
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "image.imageSize", p, &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        pc_size sz = doc_of(s)->size;
        CHECK_EQ_UINT(sz.width, 80);
        CHECK_EQ_UINT(sz.height, 40);
        pc_rect b = layer_bounds(s, 1); /* red square scaled 2x: ~ (20,10)-(40,30) */
        CHECK(labs((long)b.x0 - 20) <= 1 && labs((long)b.x1 - 40) <= 1);
        pc_engine_session_free(s);
    }
}

static void test_image_size_resolution_only(void)
{
    pc_engine_session *s = session();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "image.imageSize", "{\"resolution\":300,\"resample\":\"none\",\"width\":999}", &out,
              NULL) == PC_OK);
    /* #1815: the result is the document's real size, not the ignored request. */
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "width")), 40);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "height")), 20);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 40);
    CHECK(fabsf(doc_of(s)->resolution_dpi - 300.0f) < 1e-3f);
    pc_engine_session_free(s);
}

static void test_image_size_refuses_budget(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(new_doc(s, "{\"width\":64,\"height\":48}") == PC_OK);
    pc_size before = doc_of(s)->size;
    size_t past = past_len(s);
    pc_error err = {0};
    pc_json_value out;
    pc_status rc = pc_engine_session_execute_json(
        s, "image.imageSize", "{\"width\":300000,\"height\":300000}", &out, &err);
    CHECK(rc != PC_OK);
    CHECK(strstr(err.msg, "megapixels") != NULL);
    CHECK(doc_of(s)->size.width == before.width && doc_of(s)->size.height == before.height);
    CHECK_EQ_UINT(past_len(s), past);
    /* Changing only the resolution past the budget still works. */
    CHECK(pc_engine_session_execute_json(
              s, "image.imageSize",
              "{\"width\":300000,\"height\":300000,\"resample\":\"none\",\"resolution\":300}", &out,
              NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "image.imageSize", "{\"width\":128,\"height\":96}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 128);
    pc_engine_session_free(s);
}

/* ---- Canvas Size -------------------------------------------------------------------------- */

static void test_canvas_size_anchor_and_extend(void)
{
    pc_engine_session *s = session();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "image.canvasSize",
              "{\"width\":60,\"height\":30,\"anchor\":\"topLeft\",\"extensionColor\":\"#00ff00\"}",
              &out, NULL) == PC_OK);
    yyjson_mut_val *off = yyjson_mut_obj_get(pc_json_value_root(&out), "offset");
    CHECK_EQ_INT(yyjson_mut_get_int(yyjson_mut_arr_get(off, 0)), 0);
    CHECK_EQ_INT(yyjson_mut_get_int(yyjson_mut_arr_get(off, 1)), 0);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 60);
    CHECK_EQ_UINT(doc_of(s)->size.height, 30);
    /* Background extension painted green, original white kept. */
    float bg[4];
    layer_rgba(s, 0, 50, 25, bg);
    CHECK(bg[1] > 0.9f && bg[0] < 0.1f && bg[2] < 0.1f);
    layer_rgba(s, 0, 5, 5, bg);
    CHECK(bg[0] > 0.9f && bg[1] > 0.9f && bg[2] > 0.9f);
    pc_engine_session_free(s);
}

static void test_canvas_size_anchor_offsets(void)
{
    const struct {
        const char *a;
        int ox, oy;
    } cases[] = {{"topLeft", 0, 0},   {"top", 18, 0},     {"topRight", 36, 0},
                 {"left", 0, 16},     {"center", 18, 16}, {"right", 36, 16},
                 {"bottomLeft", 0, 32}, {"bottom", 18, 32}, {"bottomRight", 36, 32}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_engine_session *s = NULL;
        CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
        CHECK(new_doc(s, "{\"width\":64,\"height\":48,\"background\":\"white\"}") == PC_OK);
        char p[128];
        snprintf(p, sizeof p, "{\"width\":100,\"height\":80,\"anchor\":\"%s\"}", cases[i].a);
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "image.canvasSize", p, &out, NULL) == PC_OK);
        yyjson_mut_val *off = yyjson_mut_obj_get(pc_json_value_root(&out), "offset");
        CHECK_EQ_INT(yyjson_mut_get_int(yyjson_mut_arr_get(off, 0)), cases[i].ox);
        CHECK_EQ_INT(yyjson_mut_get_int(yyjson_mut_arr_get(off, 1)), cases[i].oy);
        pc_json_value_fini(&out);
        pc_engine_session_free(s);
    }
    /* Misspelled / unknown anchors are a BadParams error naming `anchor`. */
    const char *bad[] = {"topleft", "TopLeft", "middle", "", "NOPE"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pc_engine_session *s = NULL;
        CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
        CHECK(new_doc(s, "{\"width\":64,\"height\":48}") == PC_OK);
        char p[128];
        snprintf(p, sizeof p, "{\"width\":100,\"height\":80,\"anchor\":\"%s\"}", bad[i]);
        pc_error err = {0};
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "image.canvasSize", p, &out, &err) != PC_OK);
        CHECK(strstr(err.msg, "anchor") != NULL);
        pc_engine_session_free(s);
    }
}

/* ---- Crop --------------------------------------------------------------------------------- */

static void test_crop_explicit_and_selection(void)
{
    /* Explicit shrink. */
    pc_engine_session *s = session();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "image.crop", "{\"x\":10,\"y\":5,\"width\":10,\"height\":10}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 10);
    CHECK_EQ_UINT(doc_of(s)->size.height, 10);
    float px[4];
    layer_rgba(s, 1, 0, 0, px);
    CHECK(px[0] > 0.9f && px[1] < 0.1f && px[3] > 0.9f);
    pc_engine_session_free(s);

    /* Explicit crop that extends the canvas. */
    s = session();
    CHECK(pc_engine_session_execute_json(
              s, "image.crop", "{\"x\":-10,\"y\":0,\"width\":60,\"height\":20}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 60);
    layer_rgba(s, 1, 20, 5, px);
    CHECK(px[0] > 0.9f && px[1] < 0.1f);
    pc_engine_session_free(s);

    /* Crop to a selection via select.rect (E7). */
    s = session();
    CHECK(pc_engine_session_execute_json(
              s, "select.rect", "{\"x\":5,\"y\":2,\"width\":20,\"height\":10}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "image.crop", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 20);
    CHECK_EQ_UINT(doc_of(s)->size.height, 10);
    CHECK(!doc_of(s)->has_selection);
    pc_engine_session_free(s);
}

static void test_crop_rejects_bad_rects(void)
{
    /* A rectangle that would wrap through the narrowing cast is rejected (#959). */
    pc_engine_session *s = session();
    pc_error err = {0};
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "image.crop", "{\"x\":4294967396,\"y\":0,\"width\":10,\"height\":10}", &out, &err) !=
          PC_OK);
    CHECK(strstr(err.msg, "32-bit") != NULL);
    /* The document is untouched. */
    CHECK_EQ_UINT(doc_of(s)->size.width, 40);
    /* An in-range rectangle past the canvas still works. */
    CHECK(pc_engine_session_execute_json(
              s, "image.crop", "{\"x\":-10,\"y\":-10,\"width\":1000,\"height\":1000}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);

    /* A side past the per-side limit is refused before anything changes (#960). */
    s = session();
    pc_size before = doc_of(s)->size;
    size_t past = past_len(s);
    err = (pc_error){0};
    CHECK(pc_engine_session_execute_json(
              s, "image.crop", "{\"x\":0,\"y\":0,\"width\":300001,\"height\":10}", &out, &err) !=
          PC_OK);
    CHECK(strstr(err.msg, "300000 px limit") != NULL);
    CHECK(doc_of(s)->size.width == before.width);
    CHECK_EQ_UINT(past_len(s), past);
    pc_engine_session_free(s);

    /* An origin at i32::MIN can't move to (0,0): rejected, nothing changes. */
    s = session();
    before = doc_of(s)->size;
    past = past_len(s);
    err = (pc_error){0};
    char p[128];
    snprintf(p, sizeof p, "{\"x\":%ld,\"y\":0,\"width\":1,\"height\":1}", (long)INT32_MIN);
    CHECK(pc_engine_session_execute_json(s, "image.crop", p, &out, &err) != PC_OK);
    CHECK(strstr(err.msg, "32-bit") != NULL);
    CHECK(doc_of(s)->size.width == before.width);
    CHECK_EQ_UINT(past_len(s), past);
    pc_engine_session_free(s);
}

static void test_crop_target_resample(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(new_doc(s, "{\"width\":400,\"height\":300,\"resolution\":72}") == PC_OK);
    size_t past = past_len(s);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "image.crop",
              "{\"x\":100,\"y\":100,\"width\":100,\"height\":125,\"targetWidth\":1200,"
              "\"targetHeight\":1500,\"resolution\":300}",
              &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "documentWidth")), 1200);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "documentHeight")), 1500);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 1200);
    CHECK(fabsf(doc_of(s)->resolution_dpi - 300.0f) < 1e-3f);
    CHECK_EQ_UINT(past_len(s), past + 1); /* one undo step */
    pc_engine_session_free(s);
}

static void test_crop_angle_deferred(void)
{
    /* A rotated crop is deferred to E4 transform_cmds: an UNSUPPORTED error, no crash, no change. */
    pc_engine_session *s = session();
    pc_size before = doc_of(s)->size;
    size_t past = past_len(s);
    pc_error err = {0};
    pc_json_value out;
    pc_status rc = pc_engine_session_execute_json(
        s, "image.crop", "{\"x\":10,\"y\":0,\"width\":10,\"height\":20,\"angle\":90}", &out, &err);
    CHECK(rc != PC_OK);
    CHECK(doc_of(s)->size.width == before.width && doc_of(s)->size.height == before.height);
    CHECK_EQ_UINT(past_len(s), past);
    pc_engine_session_free(s);
}

/* ---- Trim --------------------------------------------------------------------------------- */

static void test_trim(void)
{
    /* On a transparent canvas, trim to the red square plus its drop shadow. */
    pc_engine_session *s = session_bg("transparent");
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.trim", "{\"basedOn\":\"transparent\"}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_size sz = doc_of(s)->size;
    CHECK(sz.width >= 10 && sz.width < 30 && sz.height >= 10 && sz.height < 25);
    pc_engine_session_free(s);
}

/* ---- Mode (model + depth) ----------------------------------------------------------------- */

static void test_mode_and_depth(void)
{
    pc_engine_session *s = session();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.mode.grayscale", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->mode == PC_COLOR_MODE_GRAYSCALE);
    CHECK(pc_surface_format(pc_layer_surface(&doc_of(s)->layers[1])).mode == PC_COLOR_MODE_GRAYSCALE);
    CHECK(pc_engine_session_execute_json(s, "image.mode.bits16", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->depth == PC_SAMPLE_TYPE_U16);
    CHECK(pc_surface_format(pc_layer_surface(&doc_of(s)->layers[1])).sample == PC_SAMPLE_TYPE_U16);
    CHECK(doc_of(s)->layers[1].has_mask);
    CHECK(pc_surface_format(&doc_of(s)->layers[1].mask.surface).sample == PC_SAMPLE_TYPE_U16);
    CHECK(pc_engine_session_execute_json(s, "image.mode.bits32", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->depth == PC_SAMPLE_TYPE_F32);
    CHECK(pc_engine_session_execute_json(s, "image.mode.bits8", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->depth == PC_SAMPLE_TYPE_U8);
    /* Selecting the current depth preserves the history branch (no revision move). */
    uint64_t rev = pc_engine_session_active(s)->revision;
    CHECK(pc_engine_session_execute_json(s, "image.mode.bits8", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_active(s)->revision == rev);
    pc_engine_session_free(s);
}

static void test_bit_depth_linear_roundtrip(void)
{
    /* 32-bit documents hold linear light: an 8-bit sRGB grey widens and narrows back unchanged. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(new_doc(s, "{\"width\":4,\"height\":1,\"background\":\"white\"}") == PC_OK);
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.mode.bits32", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "image.mode.bits8", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->depth == PC_SAMPLE_TYPE_U8);
    /* The red square survived the round trip. */
    float px[4];
    layer_rgba(s, 1, 10, 0, px);
    (void)px;
    pc_engine_session_free(s);
}

/* ---- Duplicate ---------------------------------------------------------------------------- */

static void test_duplicate(void)
{
    pc_engine_session *s = session();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.duplicate", "{\"mergedOnly\":true}", &out, NULL) ==
          PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "document")), 1);
    const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(root, "name"));
    CHECK(name != NULL && strstr(name, "copy") != NULL);
    pc_json_value_fini(&out);
    size_t ndocs;
    (void)pc_engine_session_documents(s, &ndocs);
    CHECK_EQ_UINT(ndocs, 2);
    CHECK_EQ_UINT(doc_of(s)->nlayers, 1); /* merged copy has one layer */
    pc_engine_session_free(s);
}

/* ---- Reveal All --------------------------------------------------------------------------- */

static void test_reveal_all(void)
{
    pc_engine_session *s = session();
    /* The red layer is within the canvas, so reveal-all changes nothing (its content is inside). */
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.revealAll", "{}", &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "changed")) == false);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);

    /* A layer that reaches off-canvas makes reveal-all grow the canvas. */
    s = session();
    CHECK(pc_engine_session_edit(s, "offcanvas", paint_offcanvas_edit, NULL, NULL) == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "image.revealAll", "{}", &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "changed")) == true);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->size.width > 40);
    pc_engine_session_free(s);
}

/* ---- right-angle rotation / flips --------------------------------------------------------- */

static void test_rotation_and_flips(void)
{
    pc_engine_session *s = session();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.imageRotation.90cw", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    /* 90 degrees swaps the canvas dimensions. */
    CHECK_EQ_UINT(doc_of(s)->size.width, 20);
    CHECK_EQ_UINT(doc_of(s)->size.height, 40);
    CHECK(pc_engine_session_execute_json(s, "image.imageRotation.90ccw", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 40);
    CHECK(pc_engine_session_execute_json(s, "image.imageRotation.flipCanvasHorizontal", "{}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "image.imageRotation.flipCanvasVertical", "{}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "image.imageRotation.180", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(doc_of(s)->size.width, 40);
    CHECK_EQ_UINT(doc_of(s)->size.height, 20);
    pc_engine_session_free(s);
}

/* ---- adversarial params ------------------------------------------------------------------- */

static void run_adversarial(const char *id)
{
    const char *params[] = {
        "{}",
        "null",
        "{\"width\":\"big\",\"height\":[1,2]}",
        "{\"x\":true,\"y\":{},\"width\":-5,\"height\":0}",
        "{\"width\":-1,\"height\":-1,\"percent\":-5}",
        "{\"resolution\":\"x\",\"resample\":42,\"anchor\":99}",
        "{\"angle\":\"NaN\",\"targetWidth\":0,\"targetHeight\":1e308}",
        "{\"extensionColor\":[],\"mergedOnly\":\"yes\",\"name\":5}",
        "{\"width\":200,\"height\":150,\"relative\":false}",
    };
    for (size_t i = 0; i < sizeof params / sizeof params[0]; i++) {
        pc_engine_session *s = session();
        pc_json_value out;
        pc_error err = {0};
        /* Must never crash: either succeeds or returns an error (and the out is freed on ok). */
        pc_status rc = pc_engine_session_execute_json(s, id, params[i], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
        /* An empty session (no document) must also never crash. */
        pc_engine_session *empty = NULL;
        CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
        err = (pc_error){0};
        rc = pc_engine_session_execute_json(empty, id, params[i], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
        pc_engine_session_free(empty);
        pc_engine_session_free(s);
    }
}

static void test_adversarial(void)
{
    const char *ids[] = {"image.imageSize",
                         "image.canvasSize",
                         "image.crop",
                         "image.trim",
                         "image.mode.rgb",
                         "image.mode.grayscale",
                         "image.mode.cmyk",
                         "image.mode.lab",
                         "image.mode.bits8",
                         "image.mode.bits16",
                         "image.mode.bits32",
                         "image.duplicate",
                         "image.revealAll",
                         "image.imageRotation.flipCanvasHorizontal",
                         "image.imageRotation.flipCanvasVertical",
                         "image.imageRotation.180",
                         "image.imageRotation.90cw",
                         "image.imageRotation.90ccw"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        /* The command must be registered. */
        CHECK(pc_engine_command_find(ids[i]) != NULL);
        run_adversarial(ids[i]);
    }
}

int main(int argc, char **argv)
{
    RUN(test_image_size_scales);
    RUN(test_image_size_resolution_only);
    RUN(test_image_size_refuses_budget);
    RUN(test_canvas_size_anchor_and_extend);
    RUN(test_canvas_size_anchor_offsets);
    RUN(test_crop_explicit_and_selection);
    RUN(test_crop_rejects_bad_rects);
    RUN(test_crop_target_resample);
    RUN(test_crop_angle_deferred);
    RUN(test_trim);
    RUN(test_mode_and_depth);
    RUN(test_bit_depth_linear_roundtrip);
    RUN(test_duplicate);
    RUN(test_reveal_all);
    RUN(test_rotation_and_flips);
    RUN(test_adversarial);
    return pc_t_finish();
}
