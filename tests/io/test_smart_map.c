/* Smart objects and the PSD writer's units: the #[test]s of crates/io/src/smart_map.rs (filters,
 * placed-layer blocks, warps, perspective, the FEid mask, hex and uuids) and of
 * crates/io/src/psd_export.rs (four-byte Photo Filter blocks, the size limit and PSB choice), plus
 * never-crash checks of the exporter: absurd and hostile documents, corrupted preserved blocks,
 * smart objects nested past the limit, and exports on several threads at once. */
#include <pthread.h>

#include "harness.h"

#include "common.h"
#include "algo/transform.h"
#include "base/bytes.h"
#include "format/format.h"
#include "io/adjust_map.h"
#include "io/blocks.h"
#include "io/psd_common.h"
#include "io/smart_internal.h"
#include "io/smart_map.h"

static pc_smart_filter sf(const char *command, const char *params)
{
    pc_smart_filter s;
    yyjson_doc *jd;
    memset(&s, 0, sizeof s);
    must(pc_str_set_c(&s.command, command, NULL));
    must(pc_json_parse(params, strlen(params), &jd, NULL));
    must(pc_json_value_from_val(yyjson_doc_get_root(jd), &s.params, NULL));
    yyjson_doc_free(jd);
    s.blend = PC_BLEND_NORMAL;
    s.opacity = 1.0f;
    s.visible = true;
    return s;
}

static pc_psd_desc desc_bytes_roundtrip(const pc_psd_desc *d)
{
    pc_psd_bytes b = {NULL, 0};
    pc_psd_desc back;
    must(pc_psd_desc_to_bytes(d, &b, NULL));
    must(pc_psd_desc_parse(b.data, b.len, &back, NULL));
    pc_psd_bytes_fini(&b);
    return back;
}

static void test_modelled_filters_round_trip(void)
{
    static const char *const F[][2] = {
        {"filter.blur.gaussianBlur", "{\"radius\": 4.0}"},
        {"filter.blur.boxBlur", "{\"radius\": 5.0}"},
        {"filter.blur.motionBlur", "{\"angle\": -20, \"distance\": 10.0}"},
        {"filter.sharpen.unsharpMask", "{\"amount\": 150.0, \"radius\": 2.0, \"threshold\": 0}"},
        {"filter.noise.addNoise",
         "{\"amount\": 12.0, \"distribution\": \"uniform\", \"monochromatic\": false, \"seed\": 1234}"},
        {"filter.noise.median", "{\"radius\": 3.0}"},
        {"filter.other.highPass", "{\"radius\": 3.0}"},
        {"filter.other.maximum", "{\"radius\": 2.0}"},
        {"filter.other.minimum", "{\"radius\": 2.0}"},
        {"filter.pixelate.mosaic", "{\"cellSize\": 8.0}"},
        {"filter.stylize.emboss", "{\"angle\": 135, \"height\": 3, \"amount\": 100}"},
        {"image.adjustments.levels",
         "{\"inBlack\": 20, \"inWhite\": 220, \"gamma\": 1.4, \"red\": {\"inBlack\": 0, \"inWhite\": 255, "
         "\"gamma\": 0.8}}"},
        {"image.adjustments.curves",
         "{\"points\": [[0.0, 0.0], [64.0, 40.0], [255.0, 255.0]], \"blue\": [[0.0, 20.0], [255.0, 235.0]]}"},
        {"image.adjustments.shadowsHighlights", "{}"},
    };
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
        pc_smart_filter f = sf(F[i][0], F[i][1]);
        pc_smart_filter g;
        pc_psd_desc d;
        pc_psd_desc back;
        f.blend = PC_BLEND_OVERLAY;
        f.opacity = 0.75f;
        f.visible = false;
        must(pc_io_item_for_filter(&f, &d, NULL));
        back = desc_bytes_roundtrip(&d);
        must(pc_io_filter_from_item(&back, &g, NULL));
        if (!pc_smart_filter_eq(&g, &f)) {
            PC_T_FAIL("%s does not round-trip", F[i][0]);
        }
        pc_smart_filter_fini(&g);
        pc_psd_desc_fini(&back);
        pc_psd_desc_fini(&d);
        pc_smart_filter_fini(&f);
    }
}

static void add_val(pc_psd_desc *d, const char *key, pc_psd_value v)
{
    must(pc_psd_desc_add(d, key, &v, NULL));
}

static void add_obj(pc_psd_desc *d, const char *key, pc_psd_desc *sub)
{
    pc_psd_value v;
    must(pc_psd_value_object(sub, &v, NULL));
    must(pc_psd_desc_add(d, key, &v, NULL));
}

static pc_psd_desc twirl_item(void)
{
    pc_psd_desc item;
    pc_psd_desc bo;
    pc_psd_desc fl;
    pc_psd_value v;
    must(pc_psd_desc_init(&item, "filterFX", NULL));
    must(pc_psd_value_text("Twirl...", true, &v, NULL));
    add_val(&item, "Nm  ", v);
    must(pc_psd_desc_init(&bo, "blendOptions", NULL));
    add_val(&bo, "Opct", pc_io3_unit("#Prc", 100.0));
    must(pc_psd_value_enum("BlnM", "normal", &v, NULL));
    add_val(&bo, "Md  ", v);
    add_obj(&item, "blendOptions", &bo);
    add_val(&item, "enab", pc_psd_value_bool(true));
    must(pc_psd_desc_init(&fl, "Twrl", NULL));
    add_val(&fl, "Angl", pc_psd_value_int(50));
    add_obj(&item, "Fltr", &fl);
    add_val(&item, "filterID", pc_psd_value_int((int32_t)pc_load_be32((const uint8_t *)"Twrl")));
    return item;
}

static void test_unknown_filters_keep_their_descriptor(void)
{
    pc_psd_desc item = twirl_item();
    pc_smart_filter f;
    pc_smart_filter g;
    pc_smart_filter back;
    pc_psd_desc out;
    pc_psd_desc odd;
    pc_psd_desc fl;
    yyjson_mut_val *name;
    must(pc_io_filter_from_item(&item, &f, NULL));
    CHECK(pc_str_eq_c(&f.command, PC_IO_UNSUPPORTED_FILTER));
    name = yyjson_mut_obj_get(pc_json_value_root(&f.params), "name");
    CHECK(name != NULL && yyjson_mut_equals_str(name, "Twirl..."));
    must(pc_io_item_for_filter(&f, &out, NULL));
    CHECK(pc_psd_desc_eq(&out, &item));
    pc_psd_desc_fini(&out);
    /* Hidden and re-blended: only those keys change. */
    must(pc_smart_filter_clone(&f, &g, NULL));
    g.visible = false;
    g.blend = PC_BLEND_SCREEN;
    must(pc_io_item_for_filter(&g, &out, NULL));
    must(pc_io_filter_from_item(&out, &back, NULL));
    CHECK(!back.visible);
    CHECK(back.blend == PC_BLEND_SCREEN);
    CHECK(pc_json_value_eq(&back.params, &f.params));
    pc_smart_filter_fini(&back);
    pc_psd_desc_fini(&out);
    pc_smart_filter_fini(&g);
    pc_smart_filter_fini(&f);
    /* A known filter with a setting we don't model is kept verbatim too. */
    must(pc_psd_desc_init(&odd, "filterFX", NULL));
    must(pc_psd_desc_init(&fl, "GsnB", NULL));
    add_obj(&odd, "Fltr", &fl);
    add_val(&odd, "filterID", pc_psd_value_int((int32_t)pc_load_be32((const uint8_t *)"GsnB")));
    must(pc_io_filter_from_item(&odd, &f, NULL));
    CHECK(pc_str_eq_c(&f.command, PC_IO_UNSUPPORTED_FILTER));
    pc_smart_filter_fini(&f);
    pc_psd_desc_fini(&odd);
    pc_psd_desc_fini(&item);
}

static void stack_with(pc_io_filter_stack *st, pc_smart_filter *f, size_t n)
{
    pc_io_filter_stack_init(st);
    st->filters = xmalloc(n * sizeof *f);
    memcpy(st->filters, f, n * sizeof *f);
    st->nfilters = n;
}

static void test_unwritable_filters_are_reported_not_fatal(void)
{
    pc_smart_filter f[5];
    pc_io_filter_stack st;
    pc_io_filter_stack back;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_psd_desc d;
    f[0] = sf("filter.distort.twirl", "{\"angle\": 50}");
    f[1] = sf("filter.blur.gaussianBlur", "{\"radius\": 2.0}");
    f[2] = sf(PC_IO_UNSUPPORTED_FILTER, "{\"psd\": \"zz\"}");
    f[3] = sf("image.adjustments.levels", "{\"cyan\": {\"gamma\": 2.0}}");
    f[4] = sf("image.adjustments.shadowsHighlights", "{\"shadowAmount\": 50}");
    stack_with(&st, f, 5);
    must(pc_io_filter_fx(&st, "L", &w, &d, NULL));
    CHECK_EQ_UINT(w.n, 4);
    must(pc_io_filter_stack_read(&d, &back, NULL));
    CHECK_EQ_UINT(back.nfilters, 1);
    CHECK(back.nfilters == 1 && pc_smart_filter_eq(&back.filters[0], &st.filters[1]));
    pc_io_filter_stack_fini(&back);
    pc_psd_desc_fini(&d);
    pc_io_warnings_fini(&w);
    pc_io_filter_stack_fini(&st);
}

static pc_io_placed_spec spec(const char *idnt, const char *placed, pc_affine t, double w, double h)
{
    pc_io_placed_spec s;
    memset(&s, 0, sizeof s);
    s.idnt = idnt;
    s.placed = placed;
    s.transform = t;
    s.size[0] = w;
    s.size[1] = h;
    s.dpi = 72.0;
    return s;
}

static void test_placed_blocks_parse_back(void)
{
    const pc_affine t = {{0.5, 0.1, -0.1, 0.5, 10.0, 20.0}};
    pc_smart_filter f = sf("filter.blur.gaussianBlur", "{\"radius\": 3.0}");
    pc_io_filter_stack st;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_psd_desc fx;
    pc_io_placed_spec s;
    pc_buf sold = PC_BUF_INIT;
    pc_buf plld = PC_BUF_INIT;
    pc_buf again = PC_BUF_INIT;
    pc_io_placed p;
    bool found = false;
    pc_psd_block tb;
    pc_psd_desc tmpl;
    pc_psd_desc d;
    stack_with(&st, &f, 1);
    st.enabled = false;
    st.mask_enabled = false;
    st.mask_linked = true;
    must(pc_io_filter_fx(&st, "L", &w, &fx, NULL));
    s = spec("id-1", "pl-2", t, 64.0, 32.0);
    s.filter_fx = &fx;
    must(pc_io_sold_bytes(NULL, &s, &w, &sold, NULL));
    CHECK_EQ_UINT(w.n, 0);
    CHECK_EQ_UINT(sold.len % 4, 0);
    must(pc_io_parse_sold(sold.data, sold.len, &found, &p, NULL));
    CHECK(found);
    if (found) {
        CHECK(pc_str_eq_c(&p.idnt, "id-1") && pc_str_eq_c(&p.placed, "pl-2"));
        for (size_t i = 0; i < 6; i++) {
            CHECK_NEAR(p.transform.m[i], t.m[i], 1e-9);
        }
        CHECK(p.has_stack && pc_io_filter_stack_eq(&p.stack, &st));
        pc_io_placed_fini(&p);
    }
    must(pc_psd_block_init(&tb, "SoLd", sold.data, sold.len, NULL));
    CHECK(pc_psd_block_check_structure(&tb, NULL) == PC_OK);
    pc_psd_block_fini(&tb);
    must(pc_io_plld_bytes(&s, &w, &plld, NULL));
    must(pc_psd_block_init(&tb, "PlLd", plld.data, plld.len, NULL));
    CHECK(pc_psd_block_check_structure(&tb, NULL) == PC_OK);
    pc_psd_block_fini(&tb);
    /* A template keeps its other keys; an unmoved transform keeps its exact quad. */
    must(pc_io_sold_descriptor(sold.data, sold.len, &found, &tmpl, NULL));
    CHECK(found);
    add_val(&tmpl, "extraKey", pc_psd_value_int(7));
    s.filter_fx = NULL;
    must(pc_io_sold_bytes(&tmpl, &s, &w, &again, NULL));
    must(pc_io_sold_descriptor(again.data, again.len, &found, &d, NULL));
    CHECK(found);
    {
        const pc_psd_value *x = pc_psd_desc_get(&d, "extraKey");
        CHECK(x != NULL && x->kind == PC_PSD_V_INTEGER && x->i == 7);
        CHECK(pc_psd_desc_get(&d, "filterFX") == NULL);
        CHECK(pc_psd_value_eq(pc_psd_desc_get(&d, "Trnf"), pc_psd_desc_get(&tmpl, "Trnf")));
    }
    pc_psd_desc_fini(&d);
    pc_psd_desc_fini(&tmpl);
    pc_buf_fini(&again);
    pc_buf_fini(&plld);
    pc_buf_fini(&sold);
    pc_psd_desc_fini(&fx);
    pc_io_warnings_fini(&w);
    pc_io_filter_stack_fini(&st);
}

/* A Distort / Perspective placement goes out as its four corners and comes back as the same
 * projective map; an affine one reads back as no perspective. */
static void test_perspective_placement_round_trips_through_sold(void)
{
    const double r[4] = {0.0, 0.0, 64.0, 32.0};
    const double q[4][2] = {{10.0, 5.0}, {80.0, 8.0}, {70.0, 50.0}, {12.0, 40.0}};
    const pc_affine id = {{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};
    pc_algo_homography h;
    pc_io_placed_spec s = spec("id", "pl", id, 64.0, 32.0);
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_buf sold = PC_BUF_INIT;
    double back[9];
    bool found = false;
    CHECK(pc_algo_homography_rect_to_quad(r, q, &h));
    s.has_perspective = true;
    memcpy(s.perspective, h.m, sizeof h.m);
    must(pc_io_sold_bytes(NULL, &s, &w, &sold, NULL));
    must(pc_io_parse_smart_perspective((const uint8_t *)"SoLd", sold.data, sold.len, &found, back, NULL));
    CHECK(found);
    for (size_t i = 0; found && i < 9; i++) {
        double a = back[i] / back[8];
        double b = h.m[i] / h.m[8];
        CHECK(fabs(a - b) < 1e-9 * (1.0 + fabs(b)));
    }
    /* Affine placements carry no perspective. */
    s.has_perspective = false;
    s.transform = (pc_affine){{2.0, 0.0, 0.0, 2.0, 5.0, 7.0}};
    must(pc_io_sold_bytes(NULL, &s, &w, &sold, NULL));
    must(pc_io_parse_smart_perspective((const uint8_t *)"SoLd", sold.data, sold.len, &found, back, NULL));
    CHECK(!found);
    must(pc_io_parse_smart_perspective((const uint8_t *)"PlLd", sold.data, sold.len, &found, back, NULL));
    CHECK(!found);
    pc_buf_fini(&sold);
    pc_io_warnings_fini(&w);
}

static void test_quad_moved_detects_corner_changes(void)
{
    const pc_affine t = {{1.0, 0.0, 0.0, 1.0, 3.0, 4.0}};
    pc_io_placed_spec s = spec("id", "pl", t, 20.0, 10.0);
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_buf sold = PC_BUF_INIT;
    pc_io_placed p;
    bool found = false;
    pc_psd_desc empty;
    const double r[4] = {0.0, 0.0, 20.0, 10.0};
    const double q[4][2] = {{3.0, 4.0}, {23.0, 4.0}, {20.0, 12.0}, {3.0, 14.0}};
    pc_algo_homography h;
    must(pc_io_sold_bytes(NULL, &s, &w, &sold, NULL));
    must(pc_io_parse_sold(sold.data, sold.len, &found, &p, NULL));
    CHECK(found);
    if (!found) {
        pc_buf_fini(&sold);
        return;
    }
    CHECK(!pc_io_quad_moved(&p.descriptor, &s)); /* same placement */
    s.transform = (pc_affine){{1.0, 0.0, 0.0, 1.0, 3.5, 4.0}};
    CHECK(pc_io_quad_moved(&p.descriptor, &s)); /* moved half a pixel */
    s.transform = t;
    CHECK(pc_algo_homography_rect_to_quad(r, q, &h));
    s.has_perspective = true;
    memcpy(s.perspective, h.m, sizeof h.m);
    CHECK(pc_io_quad_moved(&p.descriptor, &s)); /* one corner moved (Distort) */
    must(pc_psd_desc_init(&empty, "null", NULL));
    CHECK(pc_io_quad_moved(&empty, &s)); /* no stored quad */
    pc_psd_desc_fini(&empty);
    pc_io_placed_fini(&p);
    pc_buf_fini(&sold);
    pc_io_warnings_fini(&w);
}

/* Corrupted placed-layer data and filter descriptors never crash: they parse to something
 * (unknown filters stay verbatim) or to nothing. */
static void test_mutated_placed_data_never_panics(void)
{
    pc_smart_filter f[3];
    pc_io_filter_stack st;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_psd_desc fx;
    const pc_affine id = {{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};
    pc_io_placed_spec s = spec("id", "pl", id, 8.0, 8.0);
    pc_buf sold = PC_BUF_INIT;
    uint64_t state = 0x2545F4914F6CDD1Du;
    uint8_t *b;
    f[0] = sf("filter.blur.gaussianBlur", "{\"radius\": 3.0}");
    f[1] = sf("image.adjustments.curves", "{\"points\": [[0.0, 0.0], [255.0, 255.0]]}");
    f[2] = sf("image.adjustments.levels", "{\"inBlack\": 3, \"inWhite\": 250, \"gamma\": 1.1}");
    stack_with(&st, f, 3);
    must(pc_io_filter_fx(&st, "L", &w, &fx, NULL));
    s.filter_fx = &fx;
    must(pc_io_sold_bytes(NULL, &s, &w, &sold, NULL));
    b = xmalloc(sold.len);
    for (int k = 0; k < 3000; k++) {
        pc_io_placed p;
        bool found = false;
        uint64_t m;
        memcpy(b, sold.data, sold.len);
#define NEXT() (state ^= state << 13, state ^= state >> 7, state ^= state << 17, state)
        m = 1 + NEXT() % 3;
        for (uint64_t j = 0; j < m; j++) {
            size_t i = (size_t)(NEXT() % sold.len);
            b[i] = (uint8_t)NEXT();
        }
#undef NEXT
        if (pc_io_parse_sold(b, sold.len, &found, &p, NULL) == PC_OK && found) {
            pc_buf out = PC_BUF_INIT;
            for (size_t i = 0; p.has_stack && i < p.stack.nfilters; i++) {
                pc_psd_desc d;
                if (pc_io_item_for_filter(&p.stack.filters[i], &d, NULL) == PC_OK) {
                    pc_psd_desc_fini(&d);
                }
            }
            (void)pc_io_sold_bytes(&p.descriptor, &s, &w, &out, NULL);
            pc_buf_fini(&out);
            pc_io_placed_fini(&p);
        }
    }
    CHECK(true);
    free(b);
    pc_buf_fini(&sold);
    pc_psd_desc_fini(&fx);
    pc_io_warnings_fini(&w);
    pc_io_filter_stack_fini(&st);
}

static void test_warps_are_written_and_read_back(void)
{
    const double bounds[4] = {0.0, 0.0, 60.0, 30.0};
    const pc_affine id = {{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};
    pc_warp arc;
    pc_warp c;
    pc_warp multi;
    pc_mesh m;
    const pc_warp *warps[4];
    const pc_warp *back[4];
    pc_warp_preset(&arc, PC_WARP_ARC, 40.0, bounds);
    arc.vertical = true;
    arc.h_distort = 10.0;
    must(pc_mesh_identity(bounds, 1, 1, &m, NULL));
    m.points[15] = pc_point_make(67.0, 30.0);
    pc_warp_custom(&c, &m, bounds);
    must(pc_mesh_identity(bounds, 2, 2, &m, NULL));
    pc_warp_custom(&multi, &m, bounds);
    warps[0] = &arc;
    back[0] = &arc;
    warps[1] = &c;
    back[1] = &c;
    warps[2] = &multi;
    back[2] = NULL;
    warps[3] = NULL;
    back[3] = NULL;
    for (size_t i = 0; i < 4; i++) {
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_io_placed_spec s = spec("i", "p", id, 60.0, 30.0);
        pc_buf sold = PC_BUF_INIT;
        pc_warp got;
        bool found = false;
        s.warp = warps[i];
        must(pc_io_sold_bytes(NULL, &s, &w, &sold, NULL));
        must(pc_io_parse_placed_warp((const uint8_t *)"SoLd", sold.data, sold.len, &found, &got, NULL));
        CHECK(found == (back[i] != NULL));
        if (found) {
            CHECK(back[i] != NULL && pc_warp_eq(&got, back[i]));
            pc_warp_fini(&got);
        }
        CHECK_EQ_UINT(w.n, warps[i] != NULL && back[i] == NULL ? 1 : 0);
        pc_buf_fini(&sold);
        pc_io_warnings_fini(&w);
    }
    pc_warp_fini(&arc);
    pc_warp_fini(&c);
    pc_warp_fini(&multi);
}

static void test_hex_and_uuid(void)
{
    static const uint8_t B[4] = {0, 1, 0xab, 0xff};
    char *hex = NULL;
    uint8_t *back = NULL;
    size_t n = 0;
    bool ok = false;
    char u[37];
    char u2[37];
    must(pc_io3_to_hex(B, 4, &hex, NULL));
    CHECK_EQ_STR(hex, "0001abff");
    must(pc_io3_from_hex(hex, strlen(hex), &ok, &back, &n, NULL));
    CHECK(ok && n == 4 && memcmp(back, B, 4) == 0);
    free(back);
    free(hex);
    must(pc_io3_from_hex("abc", 3, &ok, &back, &n, NULL));
    CHECK(!ok);
    must(pc_io3_from_hex("zz", 2, &ok, &back, &n, NULL));
    CHECK(!ok);
    pc_io_uuid_from("x", 1, u);
    CHECK_EQ_UINT(strlen(u), 36);
    pc_io_uuid_from("x", 1, u2);
    CHECK_EQ_STR(u, u2);
    pc_io_uuid_from("y", 1, u2);
    CHECK(strcmp(u, u2) != 0);
}

/* The filter mask of an FEid item: decoded in document coordinates, white beyond its bounds;
 * a depth that doesn't match the document is an error, an all-white mask is no mask. */
static void test_filter_mask_items(void)
{
    pc_pixel_format df = PC_PIXEL_FORMAT_RGBA8;
    pc_surface src = t_pattern(df, pc_rect_make(1, 1, 7, 5), 12, true);
    pc_layer_mask lm;
    const float one = 1.0f;
    const float half = 0.5f;
    pc_psd_fx_item it;
    pc_io_filter_stack st;
    pc_layer_mask got;
    bool has = false;
    pc_error e;
    pc_surface_init_default(&lm.surface, PC_PIXEL_FORMAT_GRAY8, &one, 1);
    must(pc_surface_fill_rect(&lm.surface, pc_rect_make(2, 2, 4, 4), &half, 1, NULL));
    lm.enabled = true;
    lm.linked = false;
    lm.density = 1.0f;
    lm.feather = 0.0f;
    pc_io_filter_stack_init(&st);
    must(pc_io_feid_item("p", &src, &lm, pc_rect_make(0, 0, 9, 6), df, &has, &it, NULL));
    CHECK(has);
    must(pc_io_mask_from_item(&it, PC_SAMPLE_TYPE_U8, &st, &has, &got, NULL));
    CHECK(has);
    if (has) {
        CHECK(pc_surface_eq(&got.surface, &lm.surface) ||
              pc_rect_eq(pc_surface_content_bounds(&got.surface), pc_rect_make(2, 2, 4, 4)));
        pc_layer_mask_fini(&got);
    }
    CHECK(pc_io_mask_from_item(&it, PC_SAMPLE_TYPE_U16, &st, &has, &got, &e) == PC_ERR_FORMAT);
    CHECK_CONTAINS(e.msg, "8-bit in a U16 document");
    /* A truncated mask plane can't be decoded. */
    it.mask.data.len = it.mask.data.len / 2;
    CHECK(pc_io_mask_from_item(&it, PC_SAMPLE_TYPE_U8, &st, &has, &got, &e) == PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, "the smart filter mask could not be decoded");
    pc_psd_fx_item_fini(&it);
    /* No mask: all white. */
    must(pc_io_feid_item("p", &src, NULL, pc_rect_make(0, 0, 9, 6), df, &has, &it, NULL));
    must(pc_io_mask_from_item(&it, PC_SAMPLE_TYPE_U8, &st, &has, &got, NULL));
    CHECK(!has);
    pc_psd_fx_item_fini(&it);
    pc_layer_mask_fini(&lm);
    pc_surface_fini(&src);
}

/* ---- psd_export.rs ---- */

static const pc_psd_block *find_block(const pc_psd_file *f, const char key[4])
{
    for (size_t i = 0; f->has_layer_info && i < f->layer_info.n; i++) {
        const pc_psd_block *b = pc_psd_layer_block(&f->layer_info.layers[i], key);
        if (b != NULL) {
            return b;
        }
    }
    return NULL;
}

static void check_phfl(const pc_psd_file *f)
{
    const pc_psd_block *b = find_block(f, "phfl");
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    CHECK_EQ_UINT(b->data.len, 20); /* PSD layer block length includes four-byte padding */
    CHECK(b->data.data[0] == 0 && b->data.data[1] == 2);
    CHECK(b->data.data[17] == 0 && b->data.data[18] == 0 && b->data.data[19] == 0);
}

static void test_photo_filter_psd_is_four_byte_aligned_and_round_trips(void)
{
    pc_adjustment adj;
    pc_document doc;
    pc_layer layer;
    pc_layer_content c;
    pc_psd_file f;
    pc_psd_file parsed;
    pc_psd_bytes bytes = {NULL, 0};
    pc_document back;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    bool found = false;
    pc_io_raw_blocks raw = {NULL, 0, 0};
    memset(&adj, 0, sizeof adj);
    adj.kind = PC_ADJ_PHOTO_FILTER;
    adj.u.photo_filter.color[0] = 60000.0f / 65535.0f;
    adj.u.photo_filter.color[1] = 30000.0f / 65535.0f;
    adj.u.photo_filter.color[2] = 0.0f;
    adj.u.photo_filter.density = 0.14f;
    adj.u.photo_filter.preserve_luminosity = true;
    must(pc_document_with_background(&doc, "Photo Filter PSD", pc_size_make(32, 24), PC_COLOR_MODE_RGB,
                                     PC_SAMPLE_TYPE_U8, PC_COLOR_WHITE, NULL));
    c.kind = PC_LAYER_ADJUSTMENT;
    must(pc_adjustment_clone(&adj, &c.u.adjustment, NULL));
    must(pc_layer_init(&layer, "Warming Filter", &c, NULL));
    {
        pc_layer copy;
        must(pc_layer_clone(&layer, &copy, NULL));
        t_push(&doc, &copy);
    }
    must(pc_io_document_to_psd(&doc, &f, NULL));
    check_phfl(&f);
    must(pc_psd_file_to_bytes(&f, &bytes, NULL));
    must(pc_psd_file_parse(bytes.data, bytes.len, &parsed, NULL));
    check_phfl(&parsed);
    must(pc_io_psd_to_document(&parsed, &back, &w, NULL));
    for (size_t i = 0; i < back.nlayers; i++) {
        found = found || (back.layers[i].content.kind == PC_LAYER_ADJUSTMENT &&
                          pc_adjustment_eq(&back.layers[i].content.u.adjustment, &adj));
    }
    CHECK(found);
    pc_document_fini(&back);
    pc_psd_file_fini(&parsed);
    pc_psd_file_fini(&f);
    pc_psd_bytes_fini(&bytes);
    /* A legacy 18-byte block still imports; saving repairs it even when the values are unchanged
     * (normally raw blocks are reused). */
    must(pc_io_adjust_write(&adj, &raw, NULL));
    CHECK(raw.n >= 1 && raw.v[0].len >= 18);
    layer.psd_blocks = xmalloc(sizeof *layer.psd_blocks);
    memcpy(layer.psd_blocks[0].key, "phfl", 4);
    must(pc_blob_new(raw.v[0].data, 18, &layer.psd_blocks[0].data, NULL));
    layer.npsd_blocks = 1;
    pc_io_raw_blocks_fini(&raw);
    {
        pc_layer top;
        must(pc_document_remove(&doc, doc.layers[doc.nlayers - 1].id, &top, NULL));
        pc_layer_fini(&top);
    }
    t_push(&doc, &layer);
    must(pc_io_document_to_psd(&doc, &f, NULL));
    check_phfl(&f);
    must(pc_psd_file_to_bytes(&f, &bytes, NULL));
    must(pc_psd_file_parse(bytes.data, bytes.len, &parsed, NULL));
    check_phfl(&parsed);
    pc_psd_file_fini(&parsed);
    pc_psd_file_fini(&f);
    pc_psd_bytes_fini(&bytes);
    pc_io_warnings_fini(&w);
    pc_adjustment_fini(&adj);
    pc_document_fini(&doc);
}

static void test_size_limit_and_psb_choice(void)
{
    pc_document big = t_doc("size estimate", 30000, 30000, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document small = t_doc("size estimate", 1, 1, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document huge = t_doc("size estimate", UINT32_MAX, UINT32_MAX, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    bool ok = false;
    bool exceeds = false;
    uint64_t size = 0;
    must(pc_io_estimate_psd_size(&big, &ok, &size, NULL));
    CHECK(ok && size > PC_IO_PSD_MAX_FILE_SIZE);
    must(pc_io_psd_size_exceeds_limit(&big, &exceeds, NULL));
    CHECK(exceeds);
    CHECK(pc_io_psd_version(&big, false, exceeds) == PC_PSD_V2);
    must(pc_io_psd_size_exceeds_limit(&small, &exceeds, NULL));
    CHECK(!exceeds);
    CHECK(pc_io_psd_version(&small, false, exceeds) == PC_PSD_V1);
    CHECK(pc_io_psd_version(&small, true, exceeds) == PC_PSD_V2);
    /* Arithmetic overflow counts as too large. */
    must(pc_io_psd_size_exceeds_limit(&huge, &exceeds, NULL));
    CHECK(exceeds);
    pc_document_fini(&big);
    pc_document_fini(&small);
    pc_document_fini(&huge);
}

/* ---- never crash ---- */

/* Absurd documents export to an error or a file, never a crash. */
static void test_absurd_documents(void)
{
    pc_document huge = t_doc("h", 4000000000u, 4000000000u, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32);
    pc_document zero = t_doc("z", 0, 3, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document deep = t_doc("d", 4, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_psd_file f;
    pc_io_export_result r;
    pc_error e;
    pc_status st;
    st = pc_io_document_to_psd(&huge, &f, &e);
    CHECK(st == PC_ERR_LIMIT);
    st = pc_io_export(&zero, "x.psd", NULL, &r, &e);
    CHECK(st != PC_OK);
    if (st == PC_OK) {
        pc_io_export_result_fini(&r);
    }
    /* Groups nested past the compositor's limit: a limit error. */
    {
        pc_layer cur;
        must(pc_layer_raster(&cur, "leaf", PC_PIXEL_FORMAT_RGBA8, NULL));
        for (int i = 0; i < 200; i++) {
            pc_layer *kid = xmalloc(sizeof *kid);
            pc_layer g;
            *kid = cur;
            must(pc_layer_group(&g, "g", kid, 1, NULL));
            cur = g;
        }
        t_push(&deep, &cur);
        st = pc_io_document_to_psd(&deep, &f, &e);
        CHECK(st == PC_ERR_LIMIT);
        if (st == PC_OK) {
            pc_psd_file_fini(&f);
        }
    }
    pc_document_fini(&huge);
    pc_document_fini(&zero);
    pc_document_fini(&deep);
}

static pc_status export_psd(const pc_document *d, pc_io_warnings *w)
{
    pc_psd_file f;
    pc_error e;
    pc_status st = pc_io_document_to_psd_with(d, NULL, &f, w, &e);
    if (st == PC_OK) {
        pc_psd_bytes b = {NULL, 0};
        st = pc_psd_file_to_bytes(&f, &b, &e);
        pc_psd_bytes_fini(&b);
        pc_psd_file_fini(&f);
    }
    return st;
}

static void set_blocks(pc_layer *l, const char *const *keys, size_t n, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < l->npsd_blocks; i++) {
        pc_blob_release(l->psd_blocks[i].data);
    }
    free(l->psd_blocks);
    l->psd_blocks = xmalloc(n * sizeof *l->psd_blocks);
    for (size_t i = 0; i < n; i++) {
        memcpy(l->psd_blocks[i].key, keys[i], 4);
        must(pc_blob_new(data, len, &l->psd_blocks[i].data, NULL));
    }
    l->npsd_blocks = n;
}

/* Non-finite and out-of-range fields, and preserved blocks full of garbage, still export. */
static void test_hostile_fields(void)
{
    static const char *const KEYS[] = {"lmfx", "levl", "curv", "SoCo", "GdFl", "PtFl",
                                       "vmsk", "vsms", "TySh", "PlLd", "SoLd", "SoLE",
                                       "CgEd", "phfl", "fxrp", "brst", "cmls", "artb"};
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    uint64_t state = 88172645463325252u;
    d.layers[1].opacity = NAN;
    d.layers[2].fill_opacity = -INFINITY;
    d.layers[3].effects.has_reference = true;
    d.layers[3].effects.reference[0] = NAN;
    d.layers[4].has_link_group = true;
    d.layers[4].link_group = UINT64_MAX;
    d.global_light.angle = NAN;
    d.global_light.altitude = 1e30f;
    d.resolution_dpi = -5.0f;
    d.guides.horizontal = xmalloc(2 * sizeof *d.guides.horizontal);
    d.guides.horizontal[0] = NAN;
    d.guides.horizontal[1] = 1e30f;
    d.guides.nhorizontal = 2;
    CHECK(export_psd(&d, &w) == PC_OK);
    pc_io_warnings_fini(&w);
    /* Garbage in every preserved block the exporter reads back (adjustment, fill, effects, text,
     * placed-layer, vector mask and comps blocks). */
    for (int round = 0; round < 40; round++) {
        uint8_t junk[64];
        size_t len = (size_t)(round * 7 % 64);
        for (size_t i = 0; i < sizeof junk; i++) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            junk[i] = (uint8_t)state;
        }
        if (round % 5 == 0) {
            memcpy(junk, "soLD\0\0\0\x04", 8);
        }
        set_blocks(&d.layers[1], KEYS, sizeof KEYS / sizeof KEYS[0], junk, len);
        set_blocks(&d.layers[2], KEYS + 3, 6, junk, len);
        CHECK(export_psd(&d, &w) == PC_OK);
        pc_io_warnings_fini(&w);
    }
    pc_document_fini(&d);
}

/* A smart object whose .pcraft source nests more than 8 deep is written as pixels. */
static void test_nesting_limit(void)
{
    pc_buf bytes = PC_BUF_INIT;
    pc_document d = t_doc("n", 4, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer l = t_raster("px", PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 4, 4), 1, true);
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    t_push(&d, &l);
    for (int level = 0; level < 11; level++) {
        pc_document outer = t_doc("n", 4, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        pc_smart_source src;
        pc_layer_content c;
        pc_surface cache = t_pattern(PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 4, 4), 2, true);
        pc_layer sl;
        must(pc_format_save_to_bytes(&d, NULL, &bytes, NULL));
        memset(&src, 0, sizeof src);
        must(pc_str_set_c(&src.file_name, "level.pcraft", NULL));
        must(pc_blob_new(bytes.data, bytes.len, &src.bytes, NULL));
        c.kind = PC_LAYER_SMART;
        pc_smart_object_init(&c.u.smart, &src, (pc_affine){{1, 0, 0, 1, 0, 0}}, &cache);
        must(pc_layer_init(&sl, "smart", &c, NULL));
        t_push(&outer, &sl);
        pc_document_fini(&d);
        d = outer;
    }
    CHECK(export_psd(&d, &w) == PC_OK);
    CHECK(pc_io_warnings_any(&w, "smart objects nest more than 8 deep"));
    pc_io_warnings_fini(&w);
    pc_buf_fini(&bytes);
    pc_document_fini(&d);
}

static void *export_thread(void *arg)
{
    const pc_document *d = arg;
    for (int i = 0; i < 3; i++) {
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        if (export_psd(d, &w) != PC_OK) {
            pc_io_warnings_fini(&w);
            return (void *)1;
        }
        pc_io_warnings_fini(&w);
    }
    return NULL;
}

/* Exports of one shared document on several threads at once (run under TSan too). */
static void test_exports_on_several_threads(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U16);
    pthread_t t[4];
    int started = 0;
    for (int i = 0; i < 4; i++) {
        started += pthread_create(&t[i], NULL, export_thread, &d) == 0 ? 1 : 0;
    }
    for (int i = 0; i < started; i++) {
        void *r = NULL;
        CHECK(pthread_join(t[i], &r) == 0 && r == NULL);
    }
    CHECK_EQ_INT(started, 4);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_modelled_filters_round_trip);
    RUN(test_unknown_filters_keep_their_descriptor);
    RUN(test_unwritable_filters_are_reported_not_fatal);
    RUN(test_placed_blocks_parse_back);
    RUN(test_perspective_placement_round_trips_through_sold);
    RUN(test_quad_moved_detects_corner_changes);
    RUN(test_mutated_placed_data_never_panics);
    RUN(test_warps_are_written_and_read_back);
    RUN(test_hex_and_uuid);
    RUN(test_filter_mask_items);
    RUN(test_photo_filter_psd_is_four_byte_aligned_and_round_trips);
    RUN(test_size_limit_and_psb_choice);
    RUN(test_absurd_documents);
    RUN(test_hostile_fields);
    RUN(test_nesting_limit);
    RUN(test_exports_on_several_threads);
    return pc_t_finish();
}
