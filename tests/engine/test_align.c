/* Edit > Auto-Align / Auto-Blend Layers (port of crates/engine/src/align_cmds.rs tests, E9). These
 * exercise the photo_cmds.c panorama primitives (register / register_direct / warp_placed /
 * plane_matrix / seam_blend / seam_order) end to end. The layers are painted directly, then
 * selected through layer.select, so the test needs E1 + E5 + E9 + algo. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/str.h"
#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *new_doc(int w, int h, pc_sample_type depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_init(&doc, "Untitled", pc_size_make((uint32_t)w, (uint32_t)h), PC_COLOR_MODE_RGB,
                           depth, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

/* An empty raster layer, returning its id (inserted at the top of the root). */
static pc_layer_id add_layer(pc_engine_session *s)
{
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d->active_layer;
}

/* Paint through a surface callback into a layer's surface, committed as one edit. */
struct paint {
    void (*fn)(pc_surface *surf, void *ud);
    void *ud;
    pc_layer_id id;
};
static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    struct paint *p = vctx;
    pc_surface *surf = pc_layer_surface_mut(pc_document_layer_mut(doc, p->id));
    p->fn(surf, p->ud);
    return PC_OK;
}
static void paint(pc_engine_session *s, pc_layer_id id, void (*fn)(pc_surface *, void *), void *ud)
{
    struct paint p = {fn, ud, id};
    CHECK(pc_engine_session_edit(s, "tex", paint_edit, &p, NULL) == PC_OK);
}

static void fill(pc_surface *surf, pc_rect r, float c0, float c1, float c2, float c3)
{
    float px[4] = {c0, c1, c2, c3};
    (void)pc_surface_fill_rect(surf, r, px, 4, NULL);
}

/* A single filled rectangle (for the Auto-Blend fixtures). */
struct rectcol {
    pc_rect r;
    float c[4];
};
static void fill_rect_fn(pc_surface *surf, void *ud)
{
    struct rectcol *rc = ud;
    (void)pc_surface_fill_rect(surf, rc->r, rc->c, 4, NULL);
}

/* ---- deterministic texture (align_cmds.rs texture_layer) ---------------------------------- */

struct tex {
    int dx, dy;
};
static double tex_rnd(uint64_t *seed)
{
    *seed = *seed * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(*seed >> 33) / (double)(1u << 31);
}
static void tex_fn(pc_surface *surf, void *ud)
{
    struct tex *t = ud;
    int dx = t->dx, dy = t->dy;
    uint64_t seed = 99u;
    fill(surf, pc_rect_make(dx, dy, dx + 240, dy + 180), 0.4f, 0.4f, 0.4f, 1.0f);
    for (int i = 0; i < 120; i++) {
        int x = (int)(tex_rnd(&seed) * 220.0), y = (int)(tex_rnd(&seed) * 160.0);
        int sw = 4 + (int)(tex_rnd(&seed) * 14.0), sh = 4 + (int)(tex_rnd(&seed) * 10.0);
        float v = (float)tex_rnd(&seed);
        fill(surf, pc_rect_make(dx + x, dy + y, dx + x + sw, dy + y + sh), v, 1.0f - v, v * 0.5f, 1.0f);
    }
}
static pc_layer_id texture_layer(pc_engine_session *s, int dx, int dy)
{
    pc_layer_id id = add_layer(s);
    struct tex t = {dx, dy};
    paint(s, id, tex_fn, &t);
    return id;
}

/* A smooth layer (blobs on a gradient) shifted by (dx, dy) (align_cmds.rs smooth_layer). */
struct smooth {
    int dx, dy;
};
static void smooth_fn(pc_surface *surf, void *ud)
{
    struct smooth *sm = ud;
    for (int y = 0; y < 150; y++) {
        for (int x = 0; x < 200; x++) {
            float u = (float)(x - sm->dx), v = (float)(y - sm->dy);
            float blob =
                expf(-((u - 90.0f) * (u - 90.0f) + (v - 70.0f) * (v - 70.0f)) / (2.0f * 25.0f * 25.0f));
            float blob2 =
                expf(-((u - 150.0f) * (u - 150.0f) + (v - 40.0f) * (v - 40.0f)) / (2.0f * 15.0f * 15.0f));
            float g = 0.2f + 0.5f * blob + 0.25f * blob2 + 0.001f * u;
            fill(surf, pc_rect_make(x, y, x + 1, y + 1), g, g * 0.8f, g * 0.6f, 1.0f);
        }
    }
}
static pc_layer_id smooth_layer(pc_engine_session *s, int dx, int dy)
{
    pc_layer_id id = add_layer(s);
    struct smooth sm = {dx, dy};
    paint(s, id, smooth_fn, &sm);
    return id;
}

static void stripes_fn(pc_surface *surf, void *ud)
{
    (void)ud;
    for (int y = 0; y < 150; y++) {
        for (int x = 0; x < 200; x++) {
            float g = ((x / 7 + y / 11) % 2 == 0) ? 0.9f : 0.1f;
            fill(surf, pc_rect_make(x, y, x + 1, y + 1), g, g, g, 1.0f);
        }
    }
}

static void checker_fn(pc_surface *surf, void *ud)
{
    (void)ud;
    for (int y = 0; y < 64; y += 2) {
        for (int x = 0; x < 64; x += 2) {
            float v = ((x / 2 + y / 2) % 2 == 0) ? 0.1f : 0.9f;
            fill(surf, pc_rect_make(x, y, x + 2, y + 2), v, v, v, 1.0f);
        }
    }
}

static void select_two(pc_engine_session *s, pc_layer_id a, pc_layer_id b)
{
    char buf[64];
    pc_json_value out;
    snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)a);
    CHECK(pc_engine_session_execute_json(s, "layer.select", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    snprintf(buf, sizeof buf, "{\"layer\":%llu,\"mode\":\"add\"}", (unsigned long long)b);
    CHECK(pc_engine_session_execute_json(s, "layer.select", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}

static pc_rect active_bounds(pc_engine_session *s, pc_layer_id id)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    return pc_surface_content_bounds(pc_layer_surface(pc_document_layer(doc, id)));
}

static float mask_value(pc_engine_session *s, pc_layer_id id, int x, int y)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    return l->has_mask ? pc_layer_mask_value(&l->mask, x, y) : -1.0f;
}

static void pixel_at(pc_engine_session *s, pc_layer_id id, int x, int y, float out[4])
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_surface *surf = pc_layer_surface(pc_document_layer(doc, id));
    float px[4] = {0};
    pc_surface_read_pixel(surf, x, y, px);
    pc_to_rgba(pc_surface_format(surf), px, pc_surface_channels(surf), out);
}

/* ---- tests -------------------------------------------------------------------------------- */

static void test_auto_align_repositions_at_all_depths(void)
{
    pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t di = 0; di < 3; di++) {
        pc_engine_session *s = new_doc(300, 240, depths[di]);
        pc_layer_id a = texture_layer(s, 20, 20);
        pc_layer_id b = texture_layer(s, 31, 13);
        CHECK(!pc_engine_session_is_enabled(s, "edit.autoAlignLayers"));
        select_two(s, a, b);
        CHECK(pc_engine_session_is_enabled(s, "edit.autoAlignLayers"));
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "edit.autoAlignLayers",
                                             "{\"projection\":\"reposition\"}", &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        pc_rect bb = active_bounds(s, b);
        CHECK(labs(bb.x0 - 20) <= 1 && labs(bb.y0 - 20) <= 1);
        CHECK(pc_engine_session_undo(s));
        bb = active_bounds(s, b);
        CHECK_EQ_INT(bb.x0, 31);
        CHECK_EQ_INT(bb.y0, 13);
        pc_engine_session_free(s);
    }
}

static void test_auto_align_auto_projection_and_errors(void)
{
    pc_engine_session *s = new_doc(300, 240, PC_SAMPLE_TYPE_U8);
    pc_layer_id a = texture_layer(s, 20, 20);
    pc_layer_id b = texture_layer(s, 26, 25);
    select_two(s, a, b);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "edit.autoAlignLayers", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "edit.autoAlignLayers", "{\"projection\":\"sideways\"}", &out,
                                         NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "edit.autoAlignLayers", "{\"reference\":9999}", &out, NULL) !=
          PC_OK);
    pc_engine_session_free(s);
}

static void test_auto_blend_panorama_masks_and_seamless_layer(void)
{
    pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t di = 0; di < 3; di++) {
        pc_engine_session *s = new_doc(200, 60, depths[di]);
        pc_layer_id left = add_layer(s);
        struct rectcol lr = {pc_rect_make(0, 0, 130, 60), {0.2f, 0.3f, 0.8f, 1.0f}};
        paint(s, left, fill_rect_fn, &lr);
        pc_layer_id right = add_layer(s);
        struct rectcol rr = {pc_rect_make(70, 0, 200, 60), {0.9f, 0.6f, 0.2f, 1.0f}};
        paint(s, right, fill_rect_fn, &rr);
        select_two(s, left, right);
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "edit.autoBlendLayers", "{\"method\":\"panorama\"}", &out,
                                             NULL) == PC_OK);
        pc_json_value_fini(&out);
        CHECK(fabsf(mask_value(s, left, 10, 30) - 1.0f) < 1e-6f);
        CHECK(fabsf(mask_value(s, right, 10, 30)) < 1e-6f);
        CHECK(fabsf(mask_value(s, left, 190, 30)) < 1e-6f);
        CHECK(fabsf(mask_value(s, right, 190, 30) - 1.0f) < 1e-6f);
        CHECK(pc_engine_session_undo(s));
        CHECK(mask_value(s, left, 10, 30) < 0.0f);
        pc_engine_session_free(s);
    }
}

static void test_auto_align_registers_a_smooth_bracket_by_intensity(void)
{
    pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t di = 0; di < 3; di++) {
        pc_engine_session *s = new_doc(200, 150, depths[di]);
        pc_layer_id a = smooth_layer(s, 0, 0);
        pc_layer_id b = smooth_layer(s, 4, 3);
        select_two(s, a, b);
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "edit.autoAlignLayers", "{\"registration\":\"sideways\"}",
                                             &out, NULL) != PC_OK);
        CHECK(pc_engine_session_execute_json(
                  s, "edit.autoAlignLayers",
                  "{\"projection\":\"cylindrical\",\"registration\":\"intensity\"}", &out, NULL) != PC_OK);
        const char *cases[] = {"{\"projection\":\"reposition\",\"registration\":\"intensity\"}",
                               "{\"registration\":\"intensity\"}",
                               "{\"projection\":\"perspective\",\"registration\":\"intensity\"}"};
        for (size_t c = 0; c < 3; c++) {
            CHECK(pc_engine_session_execute_json(s, "edit.autoAlignLayers", cases[c], &out, NULL) == PC_OK);
            pc_json_value_fini(&out);
            float pa[4], pb[4];
            pixel_at(s, a, 90, 70, pa);
            pixel_at(s, b, 90, 70, pb);
            CHECK(fabsf(pa[0] - pb[0]) < 0.03f);
            CHECK(pc_engine_session_undo(s));
        }
        pc_engine_session_free(s);
    }
}

static void test_auto_align_rejects_unrelated_layers(void)
{
    pc_engine_session *s = new_doc(200, 150, PC_SAMPLE_TYPE_U8);
    pc_layer_id a = smooth_layer(s, 0, 0);
    pc_layer_id b = add_layer(s);
    paint(s, b, stripes_fn, NULL);
    select_two(s, a, b);
    pc_json_value out;
    const char *regs[] = {"{\"registration\":\"intensity\"}", "{\"registration\":\"auto\"}"};
    for (size_t i = 0; i < 2; i++) {
        pc_error e;
        pc_error_clear(&e);
        CHECK(pc_engine_session_execute_json(s, "edit.autoAlignLayers", regs[i], &out, &e) != PC_OK);
        CHECK_CONTAINS(e.msg, "matching detail");
    }
    pc_engine_session_free(s);
}

static void test_auto_blend_stack_without_seamless(void)
{
    pc_engine_session *s = new_doc(64, 64, PC_SAMPLE_TYPE_U8);
    pc_layer_id a = add_layer(s);
    paint(s, a, checker_fn, NULL);
    pc_layer_id flat = add_layer(s);
    struct rectcol fr = {pc_rect_make(0, 0, 64, 64), {0.5f, 0.5f, 0.5f, 1.0f}};
    paint(s, flat, fill_rect_fn, &fr);
    select_two(s, a, flat);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "edit.autoBlendLayers",
                                         "{\"method\":\"stack\",\"seamlessTones\":false}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    int shown = 0;
    for (int x = 0; x < 64; x++) {
        if (mask_value(s, a, x, 32) > 0.5f) {
            shown++;
        }
    }
    CHECK_EQ_INT(shown, 64);
    CHECK(fabsf(mask_value(s, flat, 10, 10)) < 1e-6f);
    CHECK(pc_engine_session_execute_json(s, "edit.autoBlendLayers", "{\"method\":\"sideways\"}", &out,
                                         NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_hostile_params(void)
{
    pc_engine_session *s = new_doc(64, 48, PC_SAMPLE_TYPE_U8);
    pc_layer_id a = texture_layer(s, 0, 0);
    pc_layer_id b = texture_layer(s, 3, 2);
    select_two(s, a, b);
    const char *ids[] = {"edit.autoAlignLayers", "edit.autoBlendLayers"};
    const char *cases[] = {"null",
                           "[]",
                           "{\"projection\":123}",
                           "{\"reference\":-1}",
                           "{\"reference\":99999999999999}",
                           "{\"method\":true}",
                           "{\"haloControl\":1e308}",
                           "{\"haloControl\":\"hard\"}",
                           "{\"interpolation\":42}"};
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < sizeof cases / sizeof cases[0]; j++) {
            pc_json_value out;
            if (pc_engine_session_execute_json(s, ids[i], cases[j], &out, NULL) == PC_OK) {
                pc_json_value_fini(&out);
            }
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_auto_align_repositions_at_all_depths);
    RUN(test_auto_align_auto_projection_and_errors);
    RUN(test_auto_blend_panorama_masks_and_seamless_layer);
    RUN(test_auto_align_registers_a_smooth_bracket_by_intensity);
    RUN(test_auto_align_rejects_unrelated_layers);
    RUN(test_auto_blend_stack_without_seamless);
    RUN(test_hostile_params);
    return pc_t_finish();
}
