/* edit.transform (Free Transform) and the frame-based presets (port of the transform_cmds.rs and
 * extra_cmds.rs transform tests, plus adversarial-params coverage per command, E-tail ET1). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "engine/transform_cmds.h"
#include "raster/surface.h"
#include "harness.h"

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

static pc_layer_id active_id(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d->active_layer;
}

/* Execute expecting success, freeing the result. */
static void exj(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, &e);
    if (rc != PC_OK) {
        PC_T_FAIL("%s failed: %s", id, e.msg);
    } else {
        pc_json_value_fini(&o);
    }
}

/* Execute expecting failure (no crash); returns true when it errored. */
static bool exj_err(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, &e);
    if (rc == PC_OK) {
        pc_json_value_fini(&o);
        return false;
    }
    return true;
}

struct paint {
    pc_rect r;
    float color[4];
};

static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ha;
    struct paint *p = ctx;
    pc_layer *l = pc_document_layer_mut(doc, *active);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, *active);
    }
    pc_surface *surf = pc_layer_surface_mut(l);
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t n = pc_from_rgba(pc_surface_format(surf), p->color, enc);
    return pc_surface_fill_rect(surf, p->r, enc, n, err);
}

/* 100x100 doc, an empty layer above the Background, a red rect at (10,10)-(30,20). */
static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    exj(s, "file.new", "{\"width\":100,\"height\":100}");
    exj(s, "layer.new.layer", "{}");
    struct paint pt = {pc_rect_make(10, 10, 30, 20), {1.0f, 0.0f, 0.0f, 1.0f}};
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, &pt, NULL) == PC_OK);
    return s;
}

static pc_rect active_bounds(pc_engine_session *s)
{
    const pc_document *doc = doc_of(s);
    const pc_layer *l = pc_document_layer(doc, active_id(s));
    return pc_surface_content_bounds(pc_layer_surface(l));
}

static float alpha_at(pc_engine_session *s, int32_t x, int32_t y)
{
    const pc_document *doc = doc_of(s);
    const pc_layer *l = pc_document_layer(doc, active_id(s));
    return pc_surface_sample_channel(pc_layer_surface(l), x, y, 3);
}

/* ---- affine + quad ------------------------------------------------------------------------ */

static void test_affine_matrix_translate(void)
{
    pc_engine_session *s = session();
    exj(s, "edit.transform", "{\"matrix\":[1,0,0,1,40,50]}");
    pc_rect b = active_bounds(s);
    CHECK_EQ_INT(b.x0, 50);
    CHECK_EQ_INT(b.y0, 60);
    CHECK_EQ_INT(b.x1, 70);
    CHECK_EQ_INT(b.y1, 70);
    CHECK(pc_engine_session_undo(s));
    b = active_bounds(s);
    CHECK_EQ_INT(b.x0, 10);
    CHECK_EQ_INT(b.y1, 20);
    pc_engine_session_free(s);
}

static void test_scale_via_quad(void)
{
    pc_engine_session *s = session();
    exj(s, "edit.transform", "{\"quad\":[[10,10],[50,10],[50,30],[10,30]]}");
    pc_rect b = active_bounds(s);
    CHECK((uint32_t)(b.x1 - b.x0) >= 38 && (uint32_t)(b.x1 - b.x0) <= 42);
    CHECK((uint32_t)(b.y1 - b.y0) >= 18 && (uint32_t)(b.y1 - b.y0) <= 22);
    CHECK(pc_engine_session_undo(s));
    b = active_bounds(s);
    CHECK_EQ_INT(b.x0, 10);
    CHECK_EQ_INT(b.x1, 30);
    pc_engine_session_free(s);
}

static void test_perspective_quad(void)
{
    pc_engine_session *s = session();
    exj(s, "edit.transform", "{\"matrix\":[1,0,0,1,40,50]}");
    /* Perspective: a wider bottom than top. */
    exj(s, "edit.transform", "{\"quad\":[[55,60],[65,60],[75,70],[45,70]]}");
    pc_rect b = active_bounds(s);
    CHECK(b.x0 <= 46 && b.x1 >= 74);
    /* A collapsed quad is refused, not a crash. */
    CHECK(exj_err(s, "edit.transform", "{\"quad\":[[0,0],[0,0],[0,0],[0,0]]}"));
    pc_engine_session_free(s);
}

static void test_background_becomes_layer_0(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    exj(s, "file.new", "{\"width\":40,\"height\":40}");
    /* The Background is the active layer; transform it. */
    exj(s, "edit.transform", "{\"quad\":[[0,0],[20,0],[20,20],[0,20]]}");
    const pc_document *doc = doc_of(s);
    const pc_layer *l = &doc->layers[0];
    CHECK(l->name.len == 7 && memcmp(l->name.ptr, "Layer 0", 7) == 0);
    CHECK(pc_surface_format(pc_layer_surface(l)).alpha);
    /* The revealed area is transparent. */
    CHECK(pc_surface_sample_channel(pc_layer_surface(l), 30, 30, 3) == 0.0f);
    pc_engine_session_free(s);
}

/* ---- selection-only ----------------------------------------------------------------------- */

static void test_selection_only_moves_selected(void)
{
    pc_engine_session *s = session();
    exj(s, "select.rect", "{\"x\":10,\"y\":10,\"width\":10,\"height\":10}");
    exj(s, "edit.transform", "{\"matrix\":[1,0,0,1,0,50]}");
    CHECK(alpha_at(s, 15, 15) == 0.0f); /* lifted area cleared */
    CHECK(alpha_at(s, 25, 15) == 1.0f); /* unselected part stays */
    CHECK(alpha_at(s, 15, 65) == 1.0f); /* moved pixels land */
    const pc_document *doc = doc_of(s);
    CHECK(doc->has_selection);
    pc_rect sel = pc_surface_content_bounds(&doc->selection);
    CHECK_EQ_INT(sel.y0, 60);
    CHECK_EQ_INT(sel.y1, 70);
    pc_engine_session_free(s);
}

static void test_selection_bounds_drop_vacated(void)
{
    pc_engine_session *s = session();
    exj(s, "select.rect", "{\"x\":10,\"y\":10,\"width\":20,\"height\":10}");
    exj(s, "edit.transform", "{\"matrix\":[1,0,0,1,50,0]}");
    pc_rect b = active_bounds(s);
    CHECK_EQ_INT(b.x0, 60);
    CHECK_EQ_INT(b.x1, 80);
    CHECK_EQ_INT(b.y0, 10);
    CHECK_EQ_INT(b.y1, 20);
    pc_engine_session_free(s);
}

/* ---- presets ------------------------------------------------------------------------------ */

static void test_presets_exact(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    exj(s, "file.new", "{\"width\":100,\"height\":100}");
    exj(s, "layer.new.layer", "{}");
    struct paint big = {pc_rect_make(10, 10, 20, 14), {1.0f, 0.0f, 0.0f, 1.0f}};
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, &big, NULL) == PC_OK);
    struct paint dot = {pc_rect_make(10, 10, 11, 11), {0.0f, 1.0f, 0.0f, 1.0f}};
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, &dot, NULL) == PC_OK);

    const pc_document *doc;
    const pc_layer *l;
    float px[4];

    exj(s, "edit.transform.flipHorizontal", "{}");
    pc_rect b = active_bounds(s);
    CHECK_EQ_INT(b.x0, 10);
    CHECK_EQ_INT(b.x1, 20);
    doc = doc_of(s);
    l = pc_document_layer(doc, active_id(s));
    pc_surface_rgba(pc_layer_surface(l), 19, 10, px);
    CHECK(px[1] > 0.99f && px[0] < 0.01f); /* green went to the right edge */

    exj(s, "edit.transform.flipVertical", "{}");
    doc = doc_of(s);
    l = pc_document_layer(doc, active_id(s));
    pc_surface_rgba(pc_layer_surface(l), 19, 13, px);
    CHECK(px[1] > 0.99f);

    exj(s, "edit.transform.rotate180", "{}");
    doc = doc_of(s);
    l = pc_document_layer(doc, active_id(s));
    pc_surface_rgba(pc_layer_surface(l), 10, 10, px);
    CHECK(px[1] > 0.99f);

    exj(s, "edit.transform.rotate90Cw", "{}");
    b = active_bounds(s);
    CHECK_EQ_UINT((uint32_t)(b.x1 - b.x0), 4u);
    CHECK_EQ_UINT((uint32_t)(b.y1 - b.y0), 10u);

    exj(s, "edit.transform.rotate90Ccw", "{}");
    b = active_bounds(s);
    CHECK_EQ_INT(b.x0, 10);
    CHECK_EQ_INT(b.x1, 20);
    CHECK_EQ_INT(b.y0, 10);
    CHECK_EQ_INT(b.y1, 14);
    pc_engine_session_free(s);
}

/* ---- lone targets ------------------------------------------------------------------------- */

static pc_status paint_channel_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx,
                                    pc_error *err)
{
    (void)ha;
    (void)active;
    (void)ctx;
    if (doc->nchannels == 0) {
        return pc_engine_err_other(err, "no channel");
    }
    float one = 1.0f;
    return pc_surface_fill_rect(&doc->channels[0].surface, pc_rect_make(10, 10, 20, 20), &one, 1, err);
}

static void test_lone_alpha_channel(void)
{
    pc_engine_session *s = session();
    exj(s, "channel.new", "{}");
    CHECK(pc_engine_session_edit(s, "paint channel", paint_channel_edit, NULL, NULL) == PC_OK);
    pc_rect layer_before = active_bounds(s);
    exj(s, "edit.transform", "{\"matrix\":[1,0,0,1,40,50],\"target\":{\"channel\":0}}");
    const pc_document *doc = doc_of(s);
    const pc_surface *ch = &doc->channels[0].surface;
    CHECK(pc_surface_sample_channel(ch, 55, 65, 0) > 0.99f);
    CHECK(pc_surface_sample_channel(ch, 15, 15, 0) < 0.01f);
    pc_rect layer_after = active_bounds(s);
    CHECK(pc_rect_eq(layer_before, layer_after)); /* the layer is untouched */
    pc_engine_session_free(s);
}

static pc_status mask_hide_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx,
                                pc_error *err)
{
    (void)ha;
    (void)err;
    bool linked = *(bool *)ctx;
    pc_layer *l = pc_document_layer_mut(doc, *active);
    if (l == NULL || !l->has_mask) {
        return PC_ERR_INVALID;
    }
    float black = 0.0f;
    (void)pc_surface_fill_rect(&l->mask.surface, pc_rect_make(10, 10, 20, 20), &black, 1, NULL);
    l->mask.linked = linked;
    return PC_OK;
}

static void test_lone_unlinked_mask(void)
{
    pc_engine_session *s = session();
    exj(s, "layer.layerMask.revealAll", "{}");
    bool linked = false;
    CHECK(pc_engine_session_edit(s, "mask", mask_hide_edit, &linked, NULL) == PC_OK);
    exj(s, "edit.transform", "{\"matrix\":[1,0,0,1,40,50],\"target\":\"mask\"}");
    /* Pixels untouched. */
    pc_rect b = active_bounds(s);
    CHECK_EQ_INT(b.x0, 10);
    CHECK_EQ_INT(b.x1, 30);
    const pc_document *doc = doc_of(s);
    const pc_layer *l = pc_document_layer(doc, active_id(s));
    CHECK(pc_surface_sample_channel(&l->mask.surface, 55, 65, 0) < 0.01f); /* mask moved */
    CHECK(pc_surface_sample_channel(&l->mask.surface, 15, 15, 0) > 0.99f); /* vacated reads default */
    pc_engine_session_free(s);
}

static void test_linked_mask_moves_with_pixels(void)
{
    pc_engine_session *s = session();
    exj(s, "layer.layerMask.revealAll", "{}");
    bool linked = true;
    CHECK(pc_engine_session_edit(s, "mask", mask_hide_edit, &linked, NULL) == PC_OK);
    exj(s, "edit.transform", "{\"matrix\":[1,0,0,1,40,50]}");
    pc_rect b = active_bounds(s);
    CHECK_EQ_INT(b.x0, 50);
    CHECK_EQ_INT(b.y1, 70);
    const pc_document *doc = doc_of(s);
    const pc_layer *l = pc_document_layer(doc, active_id(s));
    CHECK(pc_surface_sample_channel(&l->mask.surface, 55, 65, 0) < 0.01f); /* hidden half moved */
    CHECK(pc_surface_sample_channel(&l->mask.surface, 15, 15, 0) > 0.99f); /* nothing left behind */
    pc_engine_session_free(s);
}

/* ---- shapes transform as vectors ---------------------------------------------------------- */

static void test_shape_transforms_as_vector(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    exj(s, "file.new", "{\"width\":200,\"height\":200}");
    exj(s, "shape.create", "{\"kind\":\"rect\",\"rect\":[10,10,40,20],\"fill\":\"#ff0000\"}");
    exj(s, "edit.transform", "{\"matrix\":[2,0,0,2,-10,-10]}");
    const pc_document *doc = doc_of(s);
    const pc_layer *l = pc_document_layer(doc, active_id(s));
    CHECK(l->content.kind == PC_LAYER_SHAPE); /* still a live/vector shape layer */
    CHECK(!pc_rect_is_empty(active_bounds(s)));
    pc_engine_session_free(s);
}

/* ---- budget + adversarial params ---------------------------------------------------------- */

static void test_huge_scale_refused_unchanged(void)
{
    pc_engine_session *s = session();
    const pc_document *before = doc_of(s);
    uint64_t rev0 = pc_engine_session_active(s)->revision;
    /* A 10000x scale about the frame centre: ~200000 x 100000 px of output, far past the 2^30 px
     * budget, but each side stays under the +-2^20 coordinate window. */
    CHECK(exj_err(s, "edit.transform", "{\"scaleX\":1000000,\"scaleY\":1000000,\"interpolation\":\"nearest\"}"));
    CHECK(doc_of(s) == before);                                  /* same Arc: nothing committed */
    CHECK_EQ_UINT(pc_engine_session_active(s)->revision, rev0);  /* no history step */
    pc_rect b = active_bounds(s);
    CHECK_EQ_INT(b.x0, 10); /* still where it was */
    CHECK_EQ_INT(b.x1, 30);
    pc_engine_session_free(s);
}

static void test_adversarial_edit_transform(void)
{
    const char *bad[] = {
        "{\"matrix\":[1,0,0,1,0,0,0]}",                  /* wrong length */
        "{\"matrix\":[1,0,0,1,0]}",                      /* wrong length */
        "{\"matrix\":\"x\"}",                            /* wrong type */
        "{\"quad\":[[0,0],[0,0],[0,0],[0,0]]}",          /* degenerate */
        "{\"quad\":[[0,0],[1,0]]}",                      /* too few corners -> treated absent -> no transform */
        "{\"matrix\":[1e300,0,0,1e300,0,0]}",            /* overflow scale */
        "{\"matrix\":[0,0,0,0,0,0]}",                    /* singular */
        "{\"scaleX\":0,\"scaleY\":0}",                   /* zero scale */
        "{\"rotate\":\"x\"}",                            /* non-number */
        "{\"scaleX\":1e308,\"dx\":1e308}",               /* huge relative */
        "{}",                                            /* nothing to do */
        "{\"matrix\":[1,0,0,1,0,0],\"target\":\"mask\"}",   /* no mask */
        "{\"matrix\":[1,0,0,1,0,0],\"target\":{\"channel\":9}}", /* no such channel */
        "{\"matrix\":[1,0,0,1,0,0],\"target\":\"quickMask\"}",   /* not in quick mask */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pc_engine_session *s = session();
        pc_json_value o = PC_JSON_VALUE_INIT;
        pc_error e = {0};
        /* Must never crash; most error, a couple are harmless no-ops. */
        pc_status rc = pc_engine_session_execute_json(s, "edit.transform", bad[i], &o, &e);
        pc_json_value_fini(&o);
        (void)rc;
        pc_engine_session_free(s);
    }
    /* NaN/Inf numbers via explicitly built JSON. */
    pc_engine_session *s = session();
    CHECK(exj_err(s, "edit.transform", "{\"dx\":1e999}")); /* parses to inf -> rejected */
    pc_engine_session_free(s);
}

static void test_adversarial_presets(void)
{
    const char *ids[] = {"edit.transform.rotate180", "edit.transform.rotate90Cw",
                         "edit.transform.rotate90Ccw", "edit.transform.flipHorizontal",
                         "edit.transform.flipVertical"};
    const char *params[] = {"{}", "{\"target\":\"mask\"}", "{\"target\":{\"channel\":9}}",
                            "{\"target\":\"quickMask\"}", "{\"target\":7}", "{\"layer\":999999}"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            pc_engine_session *s = session();
            pc_json_value o = PC_JSON_VALUE_INIT;
            pc_error e = {0};
            pc_status rc = pc_engine_session_execute_json(s, ids[i], params[j], &o, &e);
            pc_json_value_fini(&o);
            (void)rc; /* never crashes */
            pc_engine_session_free(s);
        }
    }
    /* A plain preset on a painted layer succeeds (no document missing). */
    pc_engine_session *s = session();
    exj(s, "edit.transform.rotate90Cw", "{}");
    pc_engine_session_free(s);
}

static void test_locked_for_transform_export(void)
{
    pc_engine_session *s = session();
    pc_layer_id id = active_id(s);
    CHECK(!pc_engine_transform_locked(doc_of(s), id));
    /* No crash on a stale id. */
    CHECK(!pc_engine_transform_locked(doc_of(s), 987654321u));
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_affine_matrix_translate);
    RUN(test_scale_via_quad);
    RUN(test_perspective_quad);
    RUN(test_background_becomes_layer_0);
    RUN(test_selection_only_moves_selected);
    RUN(test_selection_bounds_drop_vacated);
    RUN(test_presets_exact);
    RUN(test_lone_alpha_channel);
    RUN(test_lone_unlinked_mask);
    RUN(test_linked_mask_moves_with_pixels);
    RUN(test_shape_transforms_as_vector);
    RUN(test_huge_scale_refused_unchanged);
    RUN(test_adversarial_edit_transform);
    RUN(test_adversarial_presets);
    RUN(test_locked_for_transform_export);
    return pc_t_finish();
}
