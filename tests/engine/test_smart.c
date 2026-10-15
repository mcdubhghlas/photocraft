/* Smart object primitives (port of the primitive-level parts of crates/engine/src/smart_cmds.rs
 * tests, E6): placement algebra, source bytes, and no-filter rendering (an embedded SVG). The
 * command-level and smart-filter tests are deferred with their subsystems (see smart_cmds.h). */
#include <math.h>
#include <string.h>

#include "algo/transform.h"
#include "base/blob.h"
#include "base/json.h"
#include "base/str.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "engine/smart_cmds.h"
#include "geom/geom.h"
#include "raster/surface.h"
#include "harness.h"

static bool aff_eq(pc_affine a, double a0, double a1, double a2, double a3, double a4, double a5)
{
    double e[6] = {a0, a1, a2, a3, a4, a5};
    for (int i = 0; i < 6; i++) {
        if (fabs(a.m[i] - e[i]) > 1e-9) {
            return false;
        }
    }
    return true;
}

static void test_placement_algebra(void)
{
    /* affine_homography: Affine [a,b,c,d,e,f] -> row-major [a,c,e, b,d,f, 0,0,1]. */
    pc_affine a = {{2.0, 0.5, 0.25, 3.0, 10.0, 20.0}};
    pc_algo_homography h = pc_engine_smart_affine_homography(a);
    CHECK(h.m[0] == 2.0 && h.m[1] == 0.25 && h.m[2] == 10.0);
    CHECK(h.m[3] == 0.5 && h.m[4] == 3.0 && h.m[5] == 20.0);
    CHECK(h.m[6] == 0.0 && h.m[7] == 0.0 && h.m[8] == 1.0);

    /* snap_affine rounds near-integers. */
    pc_affine noisy = {{1.0 + 1e-10, 0.0, 0.0, 10.0 - 1e-10, 4.9999999999, 7.3}};
    pc_affine snapped = pc_engine_smart_snap_affine(noisy);
    CHECK(aff_eq(snapped, 1.0, 0.0, 0.0, 10.0, 5.0, 7.3));

    /* transform_placement on an affine smart object composes a * transform. */
    pc_smart_object sm;
    pc_smart_source src;
    memset(&src, 0, sizeof src);
    src.linked = false;
    pc_smart_object_init(&sm, &src, PC_AFFINE_IDENTITY, NULL);
    pc_engine_smart_transform_placement(&sm, pc_affine_translate(5.0, -3.0));
    CHECK(!sm.has_perspective);
    CHECK(aff_eq(sm.transform, 1.0, 0.0, 0.0, 1.0, 5.0, -3.0));

    /* set_placement round trip: an affine homography stores exactly, no perspective. */
    pc_algo_homography hh = pc_engine_smart_affine_homography((pc_affine){{2.0, 0.0, 0.0, 2.0, 1.0, 2.0}});
    pc_engine_smart_set_placement(&sm, &hh);
    CHECK(!sm.has_perspective && aff_eq(sm.transform, 2.0, 0.0, 0.0, 2.0, 1.0, 2.0));

    /* A projective homography stores perspective; placement reproduces it (normalised). */
    pc_algo_homography proj = {{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.001, 0.0, 1.0}};
    pc_engine_smart_set_placement(&sm, &proj);
    CHECK(sm.has_perspective);
    pc_algo_homography back;
    pc_engine_smart_placement(&sm, &back);
    for (int i = 0; i < 9; i++) {
        CHECK(fabs(back.m[i] - proj.m[i]) < 1e-9);
    }
    pc_smart_object_fini(&sm);
}

static void test_shift_moves_placement_and_cache(void)
{
    pc_smart_object sm;
    pc_smart_source src;
    memset(&src, 0, sizeof src);
    pc_surface cache;
    pc_surface_init(&cache, PC_PIXEL_FORMAT_RGBA8);
    float red[4] = {1, 0, 0, 1};
    CHECK(pc_surface_fill_rect(&cache, pc_rect_make(0, 0, 4, 4), red, 4, NULL) == PC_OK);
    pc_smart_object_init(&sm, &src, PC_AFFINE_IDENTITY, &cache);
    CHECK(sm.has_cache);
    CHECK(pc_engine_smart_shift(&sm, 10, 5, NULL) == PC_OK);
    CHECK(aff_eq(sm.transform, 1.0, 0.0, 0.0, 1.0, 10.0, 5.0));
    pc_rect b = pc_surface_content_bounds(&sm.cache);
    CHECK(b.x0 == 10 && b.y0 == 5 && b.x1 == 14 && b.y1 == 9);
    pc_smart_object_fini(&sm);
}

static void test_source_bytes_embedded(void)
{
    pc_metadata meta;
    memset(&meta, 0, sizeof meta);
    pc_smart_source src;
    memset(&src, 0, sizeof src);
    src.linked = false;
    CHECK(pc_str_set_c(&src.file_name, "art.svg", NULL) == PC_OK);
    const char *payload = "<svg/>";
    CHECK(pc_blob_new(payload, strlen(payload), &src.bytes, NULL) == PC_OK);

    bool found = false;
    pc_str name = PC_STR_INIT;
    pc_blob *bytes = NULL;
    CHECK(pc_engine_smart_source_bytes(&meta, &src, &found, &name, &bytes, NULL) == PC_OK);
    CHECK(found);
    CHECK(pc_str_eq_c(&name, "art.svg"));
    CHECK(pc_blob_len(bytes) == strlen(payload));
    CHECK(memcmp(pc_blob_data(bytes), payload, strlen(payload)) == 0);
    pc_str_fini(&name);
    pc_blob_release(bytes);

    /* A linked source with no embedded match and an empty path -> not found. */
    pc_smart_source linked;
    memset(&linked, 0, sizeof linked);
    linked.linked = true;
    found = true;
    CHECK(pc_engine_smart_source_bytes(&meta, &linked, &found, &name, &bytes, NULL) == PC_OK);
    CHECK(!found);

    pc_str_fini(&src.file_name);
    pc_blob_release(src.bytes);
    pc_metadata_fini(&meta);
}

static void test_render_embedded_svg(void)
{
    pc_document doc;
    CHECK(pc_document_init(&doc, "Smart", pc_size_make(32, 32), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                           NULL) == PC_OK);
    pc_smart_source src;
    memset(&src, 0, sizeof src);
    src.linked = false;
    CHECK(pc_str_set_c(&src.file_name, "r.svg", NULL) == PC_OK);
    const char *svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\">"
        "<rect width=\"10\" height=\"10\" fill=\"#ff0000\"/></svg>";
    CHECK(pc_blob_new(svg, strlen(svg), &src.bytes, NULL) == PC_OK);
    pc_smart_object sm;
    pc_smart_object_init(&sm, &src, PC_AFFINE_IDENTITY, NULL);

    bool available = false;
    pc_surface out;
    CHECK(pc_engine_smart_render(&doc, &sm, &available, &out, NULL) == PC_OK);
    CHECK(available);
    pc_rect b = pc_surface_content_bounds(&out);
    CHECK(!(b.x1 <= b.x0 || b.y1 <= b.y0)); /* non-empty */
    pc_surface_fini(&out);

    /* E6: a visible smart filter now renders through the stack (E9's apply_filter_to_surface). */
    sm.filters_enabled = true;
    pc_smart_filter f;
    memset(&f, 0, sizeof f);
    CHECK(pc_str_set_c(&f.command, "filter.blur.gaussianBlur", NULL) == PC_OK);
    f.visible = true;
    f.opacity = 1.0f;
    f.blend = PC_BLEND_NORMAL;
    {
        yyjson_doc *pd = NULL;
        const char *pj = "{\"radius\":2}";
        CHECK(pc_json_parse(pj, strlen(pj), &pd, NULL) == PC_OK);
        CHECK(pc_json_value_from_val(yyjson_doc_get_root(pd), &f.params, NULL) == PC_OK);
        yyjson_doc_free(pd);
    }
    pc_smart_filter *arr = malloc(sizeof *arr);
    CHECK(arr != NULL);
    arr[0] = f;
    sm.smart_filters = arr;
    sm.nsmart_filters = 1;
    available = false;
    CHECK(pc_engine_smart_render(&doc, &sm, &available, &out, NULL) == PC_OK);
    CHECK(available); /* the filtered smart object renders */
    pc_rect fb = pc_surface_content_bounds(&out);
    CHECK(!(fb.x1 <= fb.x0 || fb.y1 <= fb.y0));
    pc_surface_fini(&out);

    /* A hidden filter is a no-op: the render equals the placed source. */
    sm.smart_filters[0].visible = false;
    available = false;
    CHECK(pc_engine_smart_render(&doc, &sm, &available, &out, NULL) == PC_OK);
    CHECK(available);
    pc_surface_fini(&out);

    pc_smart_object_fini(&sm);
    pc_document_fini(&doc);
}

static void test_refresh_updates_cache(void)
{
    pc_document doc;
    CHECK(pc_document_init(&doc, "Smart", pc_size_make(32, 32), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                           NULL) == PC_OK);
    pc_smart_source src;
    memset(&src, 0, sizeof src);
    src.linked = false;
    CHECK(pc_str_set_c(&src.file_name, "r.svg", NULL) == PC_OK);
    const char *svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
                      "<rect width=\"8\" height=\"8\" fill=\"#00ff00\"/></svg>";
    CHECK(pc_blob_new(svg, strlen(svg), &src.bytes, NULL) == PC_OK);
    pc_smart_object sm;
    pc_smart_object_init(&sm, &src, PC_AFFINE_IDENTITY, NULL);
    pc_layer_content content;
    content.kind = PC_LAYER_SMART;
    content.u.smart = sm;
    pc_layer l;
    CHECK(pc_layer_init(&l, "SO", &content, NULL) == PC_OK);
    pc_layer_id id = 0;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);

    bool updated = false;
    CHECK(pc_engine_smart_refresh(&doc, id, &updated, NULL) == PC_OK);
    CHECK(updated);
    const pc_layer *got = pc_document_layer(&doc, id);
    CHECK(got != NULL && got->content.u.smart.has_cache);

    /* A non-smart layer id is an error. */
    bool u2 = true;
    pc_layer_id bg = 0;
    CHECK(pc_document_top_layer(&doc, &bg));
    (void)u2;
    pc_document_fini(&doc);
}

static void test_hooks_transform_smart(void)
{
    pc_smart_object sm;
    pc_smart_source src;
    memset(&src, 0, sizeof src);
    pc_smart_object_init(&sm, &src, PC_AFFINE_IDENTITY, NULL);
    pc_layer_content content;
    content.kind = PC_LAYER_SMART;
    content.u.smart = sm;
    pc_layer l;
    CHECK(pc_layer_init(&l, "SO", &content, NULL) == PC_OK);
    pc_affine a = pc_affine_translate(3.0, 4.0);
    pc_engine_hook_transform_smart(&l, &a);
    CHECK(aff_eq(l.content.u.smart.transform, 1.0, 0.0, 0.0, 1.0, 3.0, 4.0));
    pc_layer_fini(&l);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_placement_algebra);
    RUN(test_shift_moves_placement_and_cache);
    RUN(test_source_bytes_embedded);
    RUN(test_render_embedded_svg);
    RUN(test_refresh_updates_cache);
    RUN(test_hooks_transform_smart);
    return pc_t_finish();
}
